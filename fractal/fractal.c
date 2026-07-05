/* Fractal Tetris: the real port plays at 160x144, and EVERY pixel of that
 * screen is itself a live micro-Tetris played by a dumb AI — simulated
 * entirely on the GPU, ~23k concurrent games in two fragment-shader passes
 * over ping-ponged state textures.
 *
 * The micro-games run the PORTED RULES, not an approximation: 10x18 board,
 * the real 21-level gravity table, the original RNG (16-bit LCG standing in
 * for rDIV + the mod-7 wrap loop + the 2-reroll anti-repeat quirk), piece
 * pipeline (active/preview/hidden), lock -> scan -> blink (7 phases x 10
 * frames) -> settle -> wipe timing, level = lines/10 progression, top-out on
 * the 2nd spawn lock, and the game-over curtain filling the board row by
 * row before the game restarts. Menus are skipped: every game starts on the
 * board. Each game plays grayscale, tinted by its main-game pixel color.
 *
 * Keyboard plays the big game; wheel zooms at cursor, drag pans,
 * =/- zoom, Home resets.
 *
 * Single translation unit: includes the whole port below. */

#define TETRIS_NO_MAIN
#include "../src/assets.c"
#include "../src/video.c"
#include "../src/audio.c"
#include "../src/input.c"
#include "../src/game.c"

#include <SDL.h>
#include <GLES3/gl3.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define FRAME_HZ 59.7275
#define GAMES_X GB_W /* one micro-game per big-game pixel */
#define GAMES_Y GB_H
#define MB_W 10 /* the real playfield */
#define MB_H 18
#define REG_TEXELS 4 /* register texels per game */

static SDL_Window *win;
static SDL_GLContext glctx;
static int win_w = 1280, win_h = 1152;
static int running = 1;

static uint32_t fb[GB_W * GB_H];

static GLuint prog_reg, prog_board, prog_show;
static GLuint tex_big, tex_board[2], tex_reg[2], tex_tiles;
static GLuint fbo;
static GLuint vao;
static int cur;
static int frame_no;

static double cam_x = GAMES_X / 2.0, cam_y = GAMES_Y / 2.0, cam_scale = 8.0;
static int dragging;

/* fit the whole 160x144 wall of games in the window */
static void fit_view(void)
{
    cam_x = GAMES_X / 2.0;
    cam_y = GAMES_Y / 2.0;
    cam_scale = win_w / (double)GAMES_X;
    if (win_h / (double)GAMES_Y < cam_scale)
        cam_scale = win_h / (double)GAMES_Y;
    if (cam_scale < 2.0)
        cam_scale = 2.0;
}

/* ---------------- shaders ---------------- */

static const char *VS =
    "#version 300 es\n"
    "void main(){"
    "vec2 p=vec2((gl_VertexID<<1&2),(gl_VertexID&2));"
    "gl_Position=vec4(p*2.0-1.0,0.0,1.0);}";

/* Register layout, 3 RGBA8 texels per game at (g.x*3+k, g.y):
 * T0: R = (x+1) | rot<<4      x: piece col -1..9
 *     G = y+2                 y: piece row -2..17
 *     B = id | prev<<3        piece ids 0..6
 *     A = gravity timer 0..52
 * T1: R = lines (caps 250)
 *     G = level | stage<<5    stage: 0 fall, 2 post-lock wait, 3 blink,
 *                                    4 settle, 5 wipe, 6 curtain, 7 dead
 *     B = counter | next<<5   counter: blink phase / wipe step / curtain row
 *     A = timer | failed<<7
 * T2: R,G = 16-bit LCG state
 *     B = (tgt+1) | drot<<4   AI target column and desired rotation
 * T3: R,G,B = 24-bit score (real formula, capped 999999)
 *
 * Both sim passes recompute the SAME deterministic step; the board pass
 * turns the step's action into cell writes. */
#define SIM_COMMON \
    "#version 300 es\n" \
    "precision highp float; precision highp int;\n" \
    "uniform sampler2D uBoard; uniform sampler2D uReg; uniform int uFrame;\n" \
    "uniform int uPass;\n" /* 0 = registers, 1 = board */ \
    "const uint P[28]=uint[28](" \
    "0x1700u,0x6220u,0x0740u,0x2230u," /* L */ \
    "0x4700u,0x2260u,0x0710u,0x3220u," /* J */ \
    "0x0F00u,0x2222u,0x0F00u,0x2222u," /* I */ \
    "0x6600u,0x6600u,0x6600u,0x6600u," /* O */ \
    "0x6300u,0x1320u,0x6300u,0x1320u," /* Z */ \
    "0x3600u,0x2310u,0x3600u,0x2310u," /* S */ \
    "0x2700u,0x2620u,0x0720u,0x2320u);" /* T */ \
    "const int GRAV[21]=int[21](52,48,44,40,36,32,27,21,16,10,9,8,7,6,5," \
    "5,4,4,3,3,2);\n" \
    "uint hsh(uint x){x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;" \
    "x^=x>>16u;return x;}\n" \
    "bool pcell(uint m,int r,int c){return (m>>uint(r*4+c)&1u)!=0u;}\n" \
    "bool bfill(ivec2 g,int r,int c){" \
    "return texelFetch(uBoard,ivec2(g.x*10+c,g.y*18+r),0).r>0.5;}\n" \
    /* real collision rule incl. the above-field-is-solid quirk */ \
    "bool hit(ivec2 g,uint m,int x,int y){" \
    "for(int r=0;r<4;r++)for(int c=0;c<4;c++){if(!pcell(m,r,c))continue;" \
    "int br=y+r,bc=x+c;" \
    "if(br<0||br>17||bc<0||bc>9)return true;" \
    "if(bfill(g,br,bc))return true;}return false;}\n" \
    "struct GS{int x;int rot;int id;int prev;int nxt;int grav;" \
    "int lines;int level;int stage;int cntr;int timer;int failed;" \
    "int y;uint rng;int tgt;int drot;int score;};\n" \
    "GS load(ivec2 g){GS s;" \
    "ivec4 a=ivec4(texelFetch(uReg,ivec2(g.x*4,g.y),0)*255.0+0.5);" \
    "ivec4 b=ivec4(texelFetch(uReg,ivec2(g.x*4+1,g.y),0)*255.0+0.5);" \
    "ivec4 c=ivec4(texelFetch(uReg,ivec2(g.x*4+2,g.y),0)*255.0+0.5);" \
    "ivec4 d=ivec4(texelFetch(uReg,ivec2(g.x*4+3,g.y),0)*255.0+0.5);" \
    "s.x=(a.r&15)-1;s.rot=a.r>>4;s.y=a.g-2;s.id=a.b&7;s.prev=a.b>>3;" \
    "s.grav=a.a;s.lines=b.r;s.level=b.g&31;s.stage=b.g>>5;" \
    "s.cntr=b.b&31;s.nxt=b.b>>5;s.timer=b.a&127;s.failed=b.a>>7;" \
    "s.rng=uint(c.r)|uint(c.g)<<8;s.tgt=(c.b&15)-1;s.drot=c.b>>4;" \
    "s.score=d.r|d.g<<8|d.b<<16;" \
    "return s;}\n" \
    /* the ported RNG: LCG -> DIV byte -> mod-7 wrap loop */ \
    "int rndpiece(inout uint rng){" \
    "rng=(rng*25173u+13849u)&0xFFFFu;" \
    "uint b=rng>>8u;int a=0;" \
    "for(int i=0;i<256;i++){b=(b-1u)&255u;if(b==0u)break;" \
    "a++;if(a==7)a=0;}return a;}\n" \
    /* placement search: simulate every (rotation, column), score the
     * landing. Runs only in the register pass — the board pass never
     * stores tgt/drot, so it skips the expensive part. */ \
    "void choose(ivec2 g,int id,inout uint rng,out int btgt,out int brot){" \
    "btgt=2;brot=0;int best=-1000000;" \
    "rng=(rng*25173u+13849u)&0xFFFFu;" \
    "for(int rot=0;rot<4;rot++){uint m=P[id*4+rot];" \
    "for(int x=-1;x<=8;x++){" \
    "int y=-2;if(hit(g,m,x,y))continue;" \
    "for(int i=0;i<20;i++){if(hit(g,m,x,y+1))break;y++;}" \
    "int sc=y*3;" /* land low */ \
    "for(int r=0;r<4;r++){int br=y+r;if(br<0||br>17)continue;" \
    "bool has=false,full=true;" \
    "for(int c=0;c<10;c++){bool f=bfill(g,br,c);" \
    "int pc=c-x;" \
    "if(!f&&pc>=0&&pc<4&&pcell(m,r,pc)){f=true;has=true;}" \
    "if(!f){full=false;break;}}" \
    "if(full&&has)sc+=120;}" /* completing lines is the point */ \
    "for(int c=0;c<4;c++){int low=-1;" \
    "for(int r=0;r<4;r++)if(pcell(m,r,c))low=r;" \
    "if(low<0)continue;int bc=x+c;" \
    "for(int br=y+low+1;br<18;br++){" \
    "if(bfill(g,br,bc))break;sc-=12;}}" /* buried holes hurt */ \
    "if(sc>best||(sc==best&&(rng>>uint(4+rot)&1u)==1u))" \
    "{best=sc;btgt=x;brot=rot;}}}}\n" \
    /* NextPiece with the original 2-reroll anti-repeat quirk */ \
    "void nextpiece(inout GS s,ivec2 g){" \
    "s.id=s.prev;s.prev=s.nxt;" \
    "int d=0;int e=s.prev;int c=s.id;" \
    "for(int h=3;h>0;h--){d=rndpiece(s.rng);" \
    "if(h==1)break;if((d|e|c)!=c)break;}" \
    "s.nxt=d;" \
    "s.x=2;s.y=-2;s.rot=0;" \
    "s.grav=GRAV[min(s.level,20)];" \
    "if(uPass==0)choose(g,s.id,s.rng,s.tgt,s.drot);" \
    "else{s.rng=(s.rng*25173u+13849u)&0xFFFFu;s.tgt=2;s.drot=0;}}\n" \
    /* actions the board pass must apply this frame */ \
    "const int ACT_NONE=0,ACT_MERGE=1,ACT_REMOVE=2,ACT_CURTAIN=3," \
    "ACT_CLEAR=4;\n" \
    /* real per-cell tile ids (atlas index = tile id - 0x70); the I piece \
     * uses its composite end/middle tiles like the original */ \
    "const int TILE[7]=int[7](20,17,0,19,18,22,21);\n" \
    "int tileFor(int id,int rot,int r,int c){" \
    "if(id==2){if((rot&1)==0)return c==0?26:(c==3?31:27);" \
    "return r==0?16:(r==3?25:24);}return TILE[id];}\n" \
    "int fullrows(ivec2 g){int n=0;" \
    "for(int r=0;r<18;r++){bool f=true;" \
    "for(int c=0;c<10;c++)if(!bfill(g,r,c)){f=false;break;}" \
    "if(f)n++;}return n;}\n" \
    /* one frame of the ported rules; identical in both passes */ \
    "int gstep(ivec2 g,inout GS s,out int actrow){" \
    "actrow=0;uint gid=uint(g.y*512+g.x);" \
    "if(s.stage==0){" \
    /* dumb AI on the real movement rules (rotate = B-button dir) */ \
    "if((uFrame+int(gid))%3==0){" \
    "uint m2=P[s.id*4+((s.rot+1)&3)];" \
    "if(s.rot!=s.drot){if(!hit(g,m2,s.x,s.y))s.rot=(s.rot+1)&3;}" \
    "else if(s.x<s.tgt){if(!hit(g,P[s.id*4+s.rot],s.x+1,s.y))s.x++;}" \
    "else if(s.x>s.tgt){if(!hit(g,P[s.id*4+s.rot],s.x-1,s.y))s.x--;}}" \
    "s.grav--;" \
    "if(s.grav<=0){s.grav=GRAV[min(s.level,20)];" \
    "if(hit(g,P[s.id*4+s.rot],s.x,s.y+1)){" \
    /* lock; real top-out rule: 2nd lock at the spawn position */ \
    "bool atspawn=(s.x==2&&s.y==-2);" \
    "if(atspawn&&s.failed==1){s.stage=6;s.cntr=0;return ACT_MERGE;}" \
    "if(atspawn)s.failed=1;" \
    "s.stage=2;s.timer=2;return ACT_MERGE;}" \
    "else s.y++;}" \
    "return ACT_NONE;}" \
    "if(s.stage==2){s.timer--;if(s.timer>0)return ACT_NONE;" \
    "int n=fullrows(g);" \
    "if(n==0){nextpiece(s,g);s.stage=0;return ACT_NONE;}" \
    "s.lines=min(s.lines+n,250);" \
    "s.stage=3;s.cntr=0;s.timer=10;return ACT_NONE;}" \
    "if(s.stage==3){s.timer--;if(s.timer>0)return ACT_NONE;" \
    "s.cntr++;s.timer=10;" \
    "if(s.cntr<7)return ACT_NONE;" \
    /* blink over: score with the real formula, remove rows, settle 13, \
     * wipe 18 (real cadence) */ \
    "int n=fullrows(g);" \
    "if(n>0){int base=n==1?40:n==2?100:n==3?300:1200;" \
    "s.score=min(s.score+base*(s.level+1),999999);}" \
    "s.stage=4;s.timer=13;return ACT_REMOVE;}" \
    "if(s.stage==4){s.timer--;if(s.timer>0)return ACT_NONE;" \
    "s.stage=5;s.cntr=0;return ACT_NONE;}" \
    "if(s.stage==5){s.cntr++;" \
    "if(s.cntr==16&&s.level<20&&s.lines/10>s.level)s.level++;" \
    "if(s.cntr>=18){nextpiece(s,g);s.stage=0;}" \
    "return ACT_NONE;}" \
    "if(s.stage==6){" /* curtain: one row per frame, bottom-up */ \
    "actrow=17-s.cntr;s.cntr++;" \
    "if(s.cntr>=18){s.stage=7;s.timer=70;}" \
    "return ACT_CURTAIN;}" \
    "if(s.stage==7){s.timer--;" \
    "if(s.timer>0)return ACT_NONE;" \
    /* restart: fresh game, new random start level 0-9 */ \
    "s.rng=(hsh(gid^uint(uFrame))&0xFFFFu)|1u;" \
    "s.lines=0;s.failed=0;s.score=0;" \
    "s.level=int(hsh(gid^uint(uFrame)^0xBEEFu)%10u);" \
    "s.prev=rndpiece(s.rng);s.nxt=rndpiece(s.rng);" \
    "nextpiece(s,g);s.stage=0;" \
    "return ACT_CLEAR;}" \
    "return ACT_NONE;}\n"

static const char *FS_REG = SIM_COMMON
    "out vec4 O;\n"
    "void main(){"
    "ivec2 p=ivec2(gl_FragCoord.xy);"
    "ivec2 g=ivec2(p.x/4,p.y);int k=p.x-g.x*4;"
    "GS s=load(g);int ar;gstep(g,s,ar);"
    "if(k==0)O=vec4(float((s.x+1)|s.rot<<4),float(s.y+2),"
    "float(s.id|s.prev<<3),float(s.grav))/255.0;"
    "else if(k==1)O=vec4(float(s.lines),float(s.level|s.stage<<5),"
    "float(s.cntr|s.nxt<<5),float(s.timer|s.failed<<7))/255.0;"
    "else if(k==2)O=vec4(float(s.rng&255u),float(s.rng>>8u),"
    "float((s.tgt+1)|s.drot<<4),0.0)/255.0;"
    "else O=vec4(float(s.score&255),float(s.score>>8&255),"
    "float(s.score>>16&255),0.0)/255.0;}";

static const char *FS_BOARD = SIM_COMMON
    "out vec4 O;\n"
    "void main(){"
    "ivec2 t=ivec2(gl_FragCoord.xy);"
    "ivec2 g=ivec2(t.x/10,t.y/18);"
    "int mr=t.y-g.y*18,mc=t.x-g.x*10;"
    "GS s=load(g);GS s0=s;int ar;int act=gstep(g,s,ar);"
    "vec4 self=texelFetch(uBoard,t,0);"
    "if(act==ACT_NONE){O=self;return;}"
    "if(act==ACT_CLEAR){O=vec4(0.0);return;}"
    "if(act==ACT_CURTAIN){O=(mr==ar)?vec4(1.0,23.0/255.0,0.0,1.0):self;"
    "return;}" /* curtain fills with tile $87 like the original */
    "if(act==ACT_MERGE){"
    "uint m=P[s0.id*4+s0.rot];int pr=mr-s0.y,pc=mc-s0.x;"
    "if(pr>=0&&pr<4&&pc>=0&&pc<4&&pcell(m,pr,pc))"
    "O=vec4(1.0,float(tileFor(s0.id,s0.rot,pr,pc))/255.0,0.0,1.0);"
    "else O=self;return;}"
    /* ACT_REMOVE: drop full rows, walking sources bottom-up */
    "int d=17;int src=-1;"
    "for(int r=17;r>=0;r--){"
    "bool full=true;"
    "for(int c=0;c<10;c++)if(!bfill(g,r,c)){full=false;break;}"
    "if(full)continue;"
    "if(d==mr){src=r;break;}d--;}"
    "if(src<0){O=vec4(0.0);return;}"
    "O=texelFetch(uBoard,ivec2(g.x*10+mc,g.y*18+src),0);}";

static const char *FS_SHOW =
    "#version 300 es\n"
    "precision highp float; precision highp int;\n"
    "uniform sampler2D uBoard; uniform sampler2D uReg;"
    "uniform sampler2D uBig; uniform sampler2D uTiles;\n"
    "uniform vec2 uRes; uniform vec2 uCam; uniform float uScale;\n"
    "const uint P[28]=uint[28]("
    "0x1700u,0x6220u,0x0740u,0x2230u,0x4700u,0x2260u,0x0710u,0x3220u,"
    "0x0F00u,0x2222u,0x0F00u,0x2222u,0x6600u,0x6600u,0x6600u,0x6600u,"
    "0x6300u,0x1320u,0x6300u,0x1320u,0x3600u,0x2310u,0x3600u,0x2310u,"
    "0x2700u,0x2620u,0x0720u,0x2320u);\n"
    /* atlas: font tiles $00-$26 at 0..38, gameplay $70-$8F at 39..70 */
    "const int GB=39;\n"
    "const int TILE[7]=int[7](20,17,0,19,18,22,21);\n"
    "int tileFor(int id,int rot,int r,int c){"
    "if(id==2){if((rot&1)==0)return c==0?26:(c==3?31:27);"
    "return r==0?16:(r==3?25:24);}return TILE[id];}\n"
    "float tilepx(int t,vec2 tv){"
    "return texelFetch(uTiles,ivec2(t*8+int(tv.x*8.0),int(tv.y*8.0)),0).r;}\n"
    /* labels in font tile ids: SCORE LEVEL LINES */
    "const int LSCORE[5]=int[5](0x1C,0x0C,0x18,0x1B,0x0E);\n"
    "const int LLEVEL[5]=int[5](0x15,0x0E,0x1F,0x0E,0x15);\n"
    "const int LLINES[5]=int[5](0x15,0x12,0x17,0x0E,0x1C);\n"
    /* digit i of an n-digit right-aligned number, -1 = leading blank */
    "int dig(int v,int i,int n){"
    "int p=1;for(int k=1;k<n-i;k++)p*=10;"
    "if(i<n-1&&v<p)return -1;return (v/p)%10;}\n"
    "out vec4 O;\n"
    "void main(){"
    "vec2 w=uCam+vec2(gl_FragCoord.x-uRes.x*0.5,"
    "uRes.y*0.5-gl_FragCoord.y)/uScale;"
    "if(w.x<0.0||w.y<0.0||w.x>=160.0||w.y>=144.0)"
    "{O=vec4(0.02,0.02,0.03,1.0);return;}"
    "ivec2 g=ivec2(w);vec2 fr=fract(w);"
    "vec3 big=texelFetch(uBig,g,0).rgb;"
    /* each game is a 20x18-tile virtual GB screen, like the real layout:
     * border, wall, 10-wide field, wall, then the SCORE/LEVEL/LINES panel */
    "int vc=int(fr.x*20.0),vr=int(fr.y*18.0);"
    "vec2 tv=vec2(fract(fr.x*20.0),fract(fr.y*18.0));"
    "ivec4 a=ivec4(texelFetch(uReg,ivec2(g.x*4,g.y),0)*255.0+0.5);"
    "ivec4 b=ivec4(texelFetch(uReg,ivec2(g.x*4+1,g.y),0)*255.0+0.5);"
    "ivec4 d=ivec4(texelFetch(uReg,ivec2(g.x*4+3,g.y),0)*255.0+0.5);"
    "int stage=b.g>>5,phase=b.b&31;"
    "float grey=1.0;int tile=-1;"
    "if(vc==1||vc==12){tile=GB+11;}"
    "else if(vc==0){grey=0.75;}"
    "else if(vc>=2&&vc<=11){"
    "int mc=vc-2,mr=vr;"
    "vec4 cell=texelFetch(uBoard,ivec2(g.x*10+mc,g.y*18+mr),0);"
    "if(cell.r>0.5)tile=GB+int(cell.g*255.0+0.5);"
    "int px=(a.r&15)-1,rot=a.r>>4,py=a.g-2,id=a.b&7;"
    "if(stage==0){int pr=mr-py,pc=mc-px;"
    "if(pr>=0&&pr<4&&pc>=0&&pc<4&&(P[id*4+rot]>>uint(pr*4+pc)&1u)!=0u)"
    "tile=GB+tileFor(id,rot,pr,pc);}"
    "if(stage==3&&(phase&1)==0){bool full=true;"
    "for(int c=0;c<10;c++)"
    "if(texelFetch(uBoard,ivec2(g.x*10+c,g.y*18+mr),0).r<0.5)"
    "{full=false;break;}"
    "if(full)tile=GB+28;}"
    "}else{"
    /* right panel, coords echoing the real A-type screen */
    "int score=d.r|d.g<<8|d.b<<16;"
    "int level=b.g&31,lines=b.r;"
    "if(vr==1&&vc>=13&&vc<=17)tile=LSCORE[vc-13];"
    "else if(vr==3&&vc>=13&&vc<=18){int t=dig(score,vc-13,6);"
    "if(t>=0)tile=t;}"
    "else if(vr==6&&vc>=13&&vc<=17)tile=LLEVEL[vc-13];"
    "else if(vr==7&&(vc==16||vc==17)){int t=dig(level,vc-16,2);"
    "if(t>=0)tile=t;}"
    "else if(vr==9&&vc>=13&&vc<=17)tile=LLINES[vc-13];"
    "else if(vr==10&&vc>=14&&vc<=17){int t=dig(lines,vc-14,4);"
    "if(t>=0)tile=t;}"
    "}"
    "if(tile>=0)grey=tilepx(tile,tv);"
    "O=vec4(big*grey,1.0);}";

/* ---------------- GL helpers ---------------- */

static GLuint compile(GLenum kind, const char *src)
{
    GLuint s = glCreateShader(kind);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "shader error:\n%s\n", log);
        exit(1);
    }
    return s;
}

static GLuint link2(const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        fprintf(stderr, "link error:\n%s\n", log);
        exit(1);
    }
    return p;
}

static GLuint make_tex(int w, int h, const void *data)
{
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, data);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

/* grayscale atlas of the REAL tiles, so zooming in shows the same
 * graphics: font $00-$26 (digits + letters, atlas 0..38) followed by
 * gameplay $70-$8F (walls, blocks, flash, curtain, atlas 39..70) */
static void tiles_init(void)
{
    /* ts_font / ts_gameplay live in game.c — same translation unit */
    enum { NFONT = 39, NGP = 32, N = NFONT + NGP,
           GP_FIRST = 0x70, VBASE = 48 /* GAMEPLAY_BASE */ };
    uint8_t *px = calloc(1, (size_t)N * 8 * 8 * 4);
    for (int t = 0; t < N; t++) {
        const Tile *tile = (t < NFONT)
            ? &ts_font.tiles[t]
            : &ts_gameplay.tiles[GP_FIRST + (t - NFONT) - VBASE];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint32_t c = tile->px[y * 8 + x];
                /* luminance works for both palette-mapped GB tiles and
                 * full-color custom tilesets (whose alpha carries no shade) */
                uint32_t r = c & 0xFF, gg = c >> 8 & 0xFF, b = c >> 16 & 0xFF;
                uint8_t grey = (c >> 24)
                    ? (uint8_t)((r * 77 + gg * 150 + b * 29) >> 8)
                    : 255; /* transparent = shade 0 = lightest */
                size_t o = ((size_t)y * N * 8 + (size_t)t * 8 + x) * 4;
                px[o] = px[o + 1] = px[o + 2] = grey;
                px[o + 3] = 255;
            }
        }
    }
    tex_tiles = make_tex(N * 8, 8, px);
    free(px);
}

static void sim_init(void)
{
    /* every game starts ON THE BOARD: fresh piece pipeline, random start
     * level 0-9, empty field (like picking a level and pressing start) */
    uint8_t *rg = calloc(1, (size_t)GAMES_X * REG_TEXELS * GAMES_Y * 4);
    static const int grav[21] = { 52,48,44,40,36,32,27,21,16,10,9,8,7,6,5,
                                  5,4,4,3,3,2 };
    for (int gy = 0; gy < GAMES_Y; gy++) {
        for (int gx = 0; gx < GAMES_X; gx++) {
            size_t o = ((size_t)gy * GAMES_X * REG_TEXELS
                        + (size_t)gx * REG_TEXELS) * 4;
            int level = rand() % 10;
            int id = rand() % 7, prev = rand() % 7, nxt = rand() % 7;
            rg[o + 0] = (uint8_t)((2 + 1) | 0 << 4);   /* x=2 rot=0 */
            rg[o + 1] = (uint8_t)(-2 + 2);             /* y=-2 */
            rg[o + 2] = (uint8_t)(id | prev << 3);
            rg[o + 3] = (uint8_t)(1 + rand() % grav[level]); /* stagger */
            rg[o + 4] = 0;                             /* lines */
            rg[o + 5] = (uint8_t)level;                /* stage 0 */
            rg[o + 6] = (uint8_t)(0 | nxt << 5);
            rg[o + 7] = 0;
            unsigned seed = (unsigned)(rand() & 0xFFFF) | 1u;
            rg[o + 8] = (uint8_t)(seed & 255);
            rg[o + 9] = (uint8_t)(seed >> 8);
            rg[o + 10] = (uint8_t)((rand() % 9 + 1) | (rand() & 3) << 4);
            rg[o + 11] = 0;
        }
    }
    tex_board[0] = make_tex(GAMES_X * MB_W, GAMES_Y * MB_H, NULL);
    {   /* explicit zero board */
        size_t bn = (size_t)GAMES_X * MB_W * GAMES_Y * MB_H * 4;
        uint8_t *bd = calloc(1, bn);
        glBindTexture(GL_TEXTURE_2D, tex_board[0]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GAMES_X * MB_W,
                        GAMES_Y * MB_H, GL_RGBA, GL_UNSIGNED_BYTE, bd);
        free(bd);
    }
    tex_board[1] = make_tex(GAMES_X * MB_W, GAMES_Y * MB_H, NULL);
    tex_reg[0] = make_tex(GAMES_X * REG_TEXELS, GAMES_Y, rg);
    tex_reg[1] = make_tex(GAMES_X * REG_TEXELS, GAMES_Y, NULL);
    free(rg);
}

static void bind_sim_inputs(GLuint prog, int pass)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_board[cur]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_reg[cur]);
    glUniform1i(glGetUniformLocation(prog, "uBoard"), 0);
    glUniform1i(glGetUniformLocation(prog, "uReg"), 1);
    glUniform1i(glGetUniformLocation(prog, "uFrame"), frame_no);
    glUniform1i(glGetUniformLocation(prog, "uPass"), pass);
}

static void sim_tick(void)
{
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glUseProgram(prog_reg);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex_reg[cur ^ 1], 0);
    glViewport(0, 0, GAMES_X * REG_TEXELS, GAMES_Y);
    bind_sim_inputs(prog_reg, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(prog_board);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex_board[cur ^ 1], 0);
    glViewport(0, 0, GAMES_X * MB_W, GAMES_Y * MB_H);
    bind_sim_inputs(prog_board, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    cur ^= 1;
    frame_no++;
}

static void draw(void)
{
    glViewport(0, 0, win_w, win_h);
    glUseProgram(prog_show);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_board[cur]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_reg[cur]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_big);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, tex_tiles);
    glUniform1i(glGetUniformLocation(prog_show, "uBoard"), 0);
    glUniform1i(glGetUniformLocation(prog_show, "uReg"), 1);
    glUniform1i(glGetUniformLocation(prog_show, "uBig"), 2);
    glUniform1i(glGetUniformLocation(prog_show, "uTiles"), 3);
    glUniform2f(glGetUniformLocation(prog_show, "uRes"),
                (float)win_w, (float)win_h);
    glUniform2f(glGetUniformLocation(prog_show, "uCam"),
                (float)cam_x, (float)cam_y);
    glUniform1f(glGetUniformLocation(prog_show, "uScale"),
                (float)cam_scale);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/* ---------------- main loop ---------------- */

static double sim_accum;
static Uint64 last_counter;

static void frame(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_QUIT:
            running = 0;
            break;
        case SDL_KEYDOWN:
            if (ev.key.keysym.sym == SDLK_ESCAPE)
                running = 0;
            break;
        case SDL_MOUSEWHEEL: {
            int mx, my;
            SDL_GetMouseState(&mx, &my);
            double wx = cam_x + (mx - win_w * 0.5) / cam_scale;
            double wy = cam_y + (my - win_h * 0.5) / cam_scale;
            double f = ev.wheel.y > 0 ? 1.2 : 1.0 / 1.2;
            cam_scale *= f;
            if (cam_scale < 2.0) cam_scale = 2.0;
            if (cam_scale > 4000.0) cam_scale = 4000.0;
            cam_x = wx - (mx - win_w * 0.5) / cam_scale;
            cam_y = wy - (my - win_h * 0.5) / cam_scale;
            break;
        }
        case SDL_MOUSEBUTTONDOWN: dragging = 1; break;
        case SDL_MOUSEBUTTONUP: dragging = 0; break;
        case SDL_MOUSEMOTION:
            if (dragging) {
                cam_x -= ev.motion.xrel / cam_scale;
                cam_y -= ev.motion.yrel / cam_scale;
            }
            break;
        case SDL_WINDOWEVENT:
            if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                win_w = ev.window.data1;
                win_h = ev.window.data2;
            }
            break;
        }
        input_handle_event(&ev);
    }

    /* keyboard zoom: =/+ in, - out, Home resets (smooth while held) */
    {
        const Uint8 *k = SDL_GetKeyboardState(NULL);
        double f = 1.0;
        if (k[SDL_SCANCODE_EQUALS] || k[SDL_SCANCODE_KP_PLUS])
            f = 1.04;
        if (k[SDL_SCANCODE_MINUS] || k[SDL_SCANCODE_KP_MINUS])
            f = 1.0 / 1.04;
        if (f != 1.0) {
            cam_scale *= f;
            if (cam_scale < 2.0) cam_scale = 2.0;
            if (cam_scale > 4000.0) cam_scale = 4000.0;
        }
        if (k[SDL_SCANCODE_HOME])
            fit_view();
    }

    /* big game: fixed 59.73Hz steps */
    Uint64 now = SDL_GetPerformanceCounter();
    if (last_counter)
        sim_accum += (double)(now - last_counter) /
                     (double)SDL_GetPerformanceFrequency();
    last_counter = now;
    const double step = 1.0 / FRAME_HZ;
    if (sim_accum > 5 * step)
        sim_accum = 5 * step;
    int ticked = 0;
    while (sim_accum >= step) {
        game_update(input_poll());
        sim_accum -= step;
        ticked = 1;
    }
    if (ticked || frame_no == 0) {
        video_render(fb);
        glBindTexture(GL_TEXTURE_2D, tex_big);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GB_W, GB_H, GL_RGBA,
                        GL_UNSIGNED_BYTE, fb);
    }

    sim_tick();
    draw();
    SDL_GL_SwapWindow(win);
}

#ifdef __EMSCRIPTEN__
static void em_frame(void)
{
    if (!running)
        emscripten_cancel_main_loop();
    frame();
}
#endif

int main(int argc, char **argv)
{
    if (argc > 1)
        assets_set_root(argv[1]);

    SDL_Init(SDL_INIT_VIDEO);
    input_init();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    win = SDL_CreateWindow("Fractetris",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    glctx = SDL_GL_CreateContext(win);
    if (!glctx) {
        fprintf(stderr, "GL context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    SDL_GL_GetDrawableSize(win, &win_w, &win_h);
    fit_view();

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenFramebuffers(1, &fbo);

    prog_reg = link2(FS_REG);
    prog_board = link2(FS_BOARD);
    prog_show = link2(FS_SHOW);

    game_seed((unsigned)SDL_GetPerformanceCounter());
    srand((unsigned)SDL_GetPerformanceCounter());
    if (game_init())
        return 1;
    video_render(fb);
    tex_big = make_tex(GB_W, GB_H, fb);
    tiles_init();
    sim_init();

    /* FRACTAL_FRAMES + FRACTAL_SHOT (+FRACTAL_ZOOM/CX/CY): headless test */
    const char *shot = getenv("FRACTAL_SHOT");
    if (shot) {
        int n = 120;
        if (getenv("FRACTAL_FRAMES"))
            n = atoi(getenv("FRACTAL_FRAMES"));
        if (getenv("FRACTAL_ZOOM"))
            cam_scale = atof(getenv("FRACTAL_ZOOM"));
        if (getenv("FRACTAL_CX"))
            cam_x = atof(getenv("FRACTAL_CX"));
        if (getenv("FRACTAL_CY"))
            cam_y = atof(getenv("FRACTAL_CY"));
        for (int i = 0; i < n; i++) {
            Input in = { 0, 0 };
            game_update(in);
            video_render(fb);
            glBindTexture(GL_TEXTURE_2D, tex_big);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GB_W, GB_H, GL_RGBA,
                            GL_UNSIGNED_BYTE, fb);
            sim_tick();
        }

        /* FRACTAL_DUMP="gx,gy": print that game's board+regs as ASCII */
        const char *dump = getenv("FRACTAL_DUMP");
        if (dump) {
            int gx = 0, gy = 0;
            sscanf(dump, "%d,%d", &gx, &gy);
            uint8_t bpx[MB_W * MB_H * 4], rpx[REG_TEXELS * 4];
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex_board[cur], 0);
            glReadPixels(gx * MB_W, gy * MB_H, MB_W, MB_H, GL_RGBA,
                         GL_UNSIGNED_BYTE, bpx);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex_reg[cur], 0);
            glReadPixels(gx * REG_TEXELS, gy, REG_TEXELS, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, rpx);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            printf("x=%d rot=%d y=%d id=%d prev=%d grav=%d | lines=%d "
                   "level=%d stage=%d cntr=%d nxt=%d timer=%d failed=%d "
                   "score=%d\n",
                   (rpx[0] & 15) - 1, rpx[0] >> 4, rpx[1] - 2,
                   rpx[2] & 7, rpx[2] >> 3, rpx[3],
                   rpx[4], rpx[5] & 31, rpx[5] >> 5,
                   rpx[6] & 31, rpx[6] >> 5, rpx[7] & 127, rpx[7] >> 7,
                   rpx[12] | rpx[13] << 8 | rpx[14] << 16);
            for (int r = 0; r < MB_H; r++) {
                for (int c = 0; c < MB_W; c++)
                    putchar(bpx[(r * MB_W + c) * 4] > 127 ? '#' : '.');
                printf("  row %d\n", r);
            }
        }

        draw();
        glFinish();
        uint8_t *px = malloc((size_t)win_w * win_h * 4);
        glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        FILE *f = fopen(shot, "wb");
        fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
        for (int y = win_h - 1; y >= 0; y--)
            for (int x = 0; x < win_w; x++)
                fwrite(&px[((size_t)y * win_w + x) * 4], 1, 3, f);
        fclose(f);
        free(px);
        return 0;
    }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(em_frame, 0, 1);
#else
    while (running)
        frame();
#endif
    SDL_Quit();
    return 0;
}
