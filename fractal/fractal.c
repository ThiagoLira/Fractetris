/* Fractetris: the main game plays at 160x144, and EVERY pixel of that
 * screen is itself a live micro-game — simulated entirely on the GPU.
 *
 * The micro-games are a FULL PORT of the C game loop to GLSL (Doom-in-a-
 * shader style): the entire game state lives in textures and each frame is
 * a pure step function, executed identically in three passes (registers,
 * logic board, display board). Faithful details:
 *
 *  - dual board layers per game, like the original hardware: logic reads
 *    the "shadow" (WRAM) layer while the clear-blink, curtain and wipe
 *    animations write the "display" (VRAM) layer
 *  - the AI is a virtual player: it emits held/pressed button bytes that
 *    run through the real input code — DAS 23/9 auto-shift, tap-rotation
 *    with revert-on-collision, genuine soft drop with its scoring
 *  - exact per-frame call order and stage timings from the C port:
 *    rotate -> drop -> scan -> merge -> shift -> timers -> blink -> wipe;
 *    2-frame ARE, 7x10-frame blink (grey/restore from shadow, then clear),
 *    13-frame settle, 18-frame progressive wipe redraw, line score applied
 *    at wipe step 5, level-up check at wipe step 16
 *  - the real piece RNG (16-bit LCG -> mod-7 wrap -> 2-reroll anti-repeat),
 *    spawn-lock top-out rule, and the row-per-frame game-over curtain
 *
 * Keyboard plays the big game; wheel zooms at cursor, drag pans,
 * =/- zoom, Home resets. Single translation unit: includes the whole port.
 */

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
#define GAMES_X GB_W
#define GAMES_Y GB_H
#define MB_W 10
#define MB_H 18
#define REG_TEXELS 5

static SDL_Window *win;
static SDL_GLContext glctx;
static int win_w = 1280, win_h = 1152;
static int running = 1;

static uint32_t fb[GB_W * GB_H];

static GLuint prog_reg, prog_shadow, prog_disp, prog_show;
static GLuint tex_big, tex_tiles, tex_menus;
static GLuint tex_reg[2], tex_shadow[2], tex_disp[2];
static GLuint fbo;
static GLuint vao;
static int cur;
static int frame_no;

static double cam_x = GAMES_X / 2.0, cam_y = GAMES_Y / 2.0, cam_scale = 8.0;
static int dragging;

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

/* Register layout, 5 RGBA8 texels per game:
 * T0: R=(x+1)|rot<<4  G=y+2  B=id|prev<<3|armed<<6|failed<<7
 *     A=nxt|lock<<3|blink<<5
 * T1: R=gravtimer  G=das|t2<<5  B=timer1  A=wipe|kind<<5
 * T2: R,G=rng  B=(tgt+1)|drot<<4  A=prev held buttons
 * T3: R=lines  G=level|mode<<5 (0 play, 1 curtain, 2 over,
 *        3 title, 4 mode-select, 5 level-select)
 *     B=softdrop count  A=curtain row
 * T4: R,G,B=score
 *
 * All three passes execute the same step() on the same old state, so their
 * outputs can never disagree. */
#define SIM_COMMON \
    "#version 300 es\n" \
    "precision highp float; precision highp int;\n" \
    "uniform sampler2D uShadow; uniform sampler2D uDisp;" \
    "uniform sampler2D uReg; uniform int uFrame; uniform int uPass;\n" \
    "const uint P[28]=uint[28](" \
    "0x1700u,0x6220u,0x0740u,0x2230u,0x4700u,0x2260u,0x0710u,0x3220u," \
    "0x0F00u,0x2222u,0x0F00u,0x2222u,0x6600u,0x6600u,0x6600u,0x6600u," \
    "0x6300u,0x1320u,0x6300u,0x1320u,0x3600u,0x2310u,0x3600u,0x2310u," \
    "0x2700u,0x2620u,0x0720u,0x2320u);\n" \
    "const int GRAV[21]=int[21](52,48,44,40,36,32,27,21,16,10,9,8,7,6,5," \
    "5,4,4,3,3,2);\n" \
    "const int TILE[7]=int[7](20,17,0,19,18,22,21);\n" \
    "int tileFor(int id,int rot,int r,int c){" \
    "if(id==2){if((rot&1)==0)return c==0?26:(c==3?31:27);" \
    "return r==0?16:(r==3?25:24);}return TILE[id];}\n" \
    "uint hsh(uint x){x^=x>>16u;x*=0x7feb352du;x^=x>>15u;x*=0x846ca68bu;" \
    "x^=x>>16u;return x;}\n" \
    "bool pcell(uint m,int r,int c){return (m>>uint(r*4+c)&1u)!=0u;}\n" \
    "bool sfill(ivec2 g,int r,int c){" \
    "return texelFetch(uShadow,ivec2(g.x*10+c,g.y*18+r),0).r>0.5;}\n" \
    "vec4 scell(ivec2 g,int r,int c){" \
    "return texelFetch(uShadow,ivec2(g.x*10+c,g.y*18+r),0);}\n" \
    "bool hit(ivec2 g,uint m,int x,int y){" \
    "for(int r=0;r<4;r++)for(int c=0;c<4;c++){if(!pcell(m,r,c))continue;" \
    "int br=y+r,bc=x+c;" \
    "if(br<0||br>17||bc<0||bc>9)return true;" \
    "if(sfill(g,br,bc))return true;}return false;}\n" \
    /* faithful bug: full-line scan covers rows 2..17 only */ \
    "bool rowfull(ivec2 g,int r){if(r<2)return false;" \
    "for(int c=0;c<10;c++)if(!sfill(g,r,c))return false;return true;}\n" \
    "int countfull(ivec2 g){int n=0;" \
    "for(int r=2;r<18;r++)if(rowfull(g,r))n++;return n;}\n" \
    /* shadow row content after removing full rows (compaction) */ \
    "vec4 shifted(ivec2 g,int dr,int c){" \
    "int d=17;" \
    "for(int s=17;s>=0;s--){if(rowfull(g,s))continue;" \
    "if(d==dr)return scell(g,s,c);d--;}" \
    "return vec4(0.0);}\n" \
    "struct GS{int x;int rot;int y;int id;int prev;int nxt;int armed;" \
    "int failed;int lockst;int blink;int grav;int das;int t2;int t1;" \
    "int wipe;int kind;uint rng;int tgt;int drot;int held0;int lines;" \
    "int level;int mode;int sd;int crow;int score;};\n" \
    "GS load(ivec2 g){GS s;" \
    "ivec4 a=ivec4(texelFetch(uReg,ivec2(g.x*5,g.y),0)*255.0+0.5);" \
    "ivec4 b=ivec4(texelFetch(uReg,ivec2(g.x*5+1,g.y),0)*255.0+0.5);" \
    "ivec4 c=ivec4(texelFetch(uReg,ivec2(g.x*5+2,g.y),0)*255.0+0.5);" \
    "ivec4 d=ivec4(texelFetch(uReg,ivec2(g.x*5+3,g.y),0)*255.0+0.5);" \
    "ivec4 e=ivec4(texelFetch(uReg,ivec2(g.x*5+4,g.y),0)*255.0+0.5);" \
    "s.x=(a.r&15)-1;s.rot=a.r>>4;s.y=a.g-2;" \
    "s.id=a.b&7;s.prev=a.b>>3&7;s.armed=a.b>>6&1;s.failed=a.b>>7;" \
    "s.nxt=a.a&7;s.lockst=a.a>>3&3;s.blink=a.a>>5;" \
    "s.grav=b.r;s.das=b.g&31;s.t2=b.g>>5;s.t1=b.b;" \
    "s.wipe=b.a&31;s.kind=b.a>>5;" \
    "s.rng=uint(c.r)|uint(c.g)<<8;s.tgt=(c.b&15)-1;s.drot=c.b>>4;" \
    "s.held0=c.a;" \
    "s.lines=d.r;s.level=d.g&31;s.mode=d.g>>5;s.sd=d.b;s.crow=d.a;" \
    "s.score=e.r|e.g<<8|e.b<<16;" \
    "return s;}\n" \
    /* the ported RNG chain */ \
    "int rndpiece(inout uint rng){" \
    "rng=(rng*25173u+13849u)&0xFFFFu;" \
    "uint b=rng>>8u;int a=0;" \
    "for(int i=0;i<256;i++){b=(b-1u)&255u;if(b==0u)break;" \
    "a++;if(a==7)a=0;}return a;}\n" \
    /* placement search, register pass only */ \
    "void choose(ivec2 g,int id,inout uint rng,out int bt,out int br2){" \
    "bt=2;br2=0;int best=-1000000;" \
    "rng=(rng*25173u+13849u)&0xFFFFu;" \
    "for(int rot=0;rot<4;rot++){uint m=P[id*4+rot];" \
    "for(int x=-1;x<=8;x++){" \
    "int y=-2;if(hit(g,m,x,y))continue;" \
    "for(int i=0;i<20;i++){if(hit(g,m,x,y+1))break;y++;}" \
    "int sc=y*3;" \
    "for(int r=0;r<4;r++){int rr=y+r;if(rr<0||rr>17)continue;" \
    "bool has=false,full=true;" \
    "for(int c=0;c<10;c++){bool f=sfill(g,rr,c);" \
    "int pc=c-x;" \
    "if(!f&&pc>=0&&pc<4&&pcell(m,r,pc)){f=true;has=true;}" \
    "if(!f){full=false;break;}}" \
    "if(full&&has)sc+=120;}" \
    "for(int c=0;c<4;c++){int low=-1;" \
    "for(int r=0;r<4;r++)if(pcell(m,r,c))low=r;" \
    "if(low<0)continue;int bc=x+c;" \
    "for(int rr=y+low+1;rr<18;rr++){" \
    "if(sfill(g,rr,bc))break;sc-=12;}}" \
    "if(sc>best||(sc==best&&(rng>>uint(4+rot)&1u)==1u))" \
    "{best=sc;bt=x;br2=rot;}}}}\n" \
    /* NextPiece: exact pipeline + reroll quirk */ \
    "void nextpiece(ivec2 g,inout GS s){" \
    "s.id=s.prev;s.prev=s.nxt;" \
    "int d=0;int e=s.prev;int c=s.id;" \
    "for(int h=3;h>0;h--){d=rndpiece(s.rng);" \
    "if(h==1)break;if((d|e|c)!=c)break;}" \
    "s.nxt=d;s.x=2;s.y=-2;s.rot=0;" \
    "s.armed=0;s.sd=0;s.grav=GRAV[min(s.level,20)];" \
    "if(uPass==0)choose(g,s.id,s.rng,s.tgt,s.drot);" \
    "else{s.rng=(s.rng*25173u+13849u)&0xFFFFu;s.tgt=2;s.drot=0;}}\n" \
    /* board-pass action flags */ \
    "const int A_MERGE=1,A_GREY=2,A_RESTORE=4,A_CLEAR=8,A_SHIFT=16," \
    "A_WIPEROW=32,A_CURTAIN=64,A_RESET=128;\n" \
    "const int B_A=1,B_B=2,B_R=16,B_L=32,B_D=128;\n" \
    /* ==== one frame, ported line-for-line from game.c ST_PLAY ==== */ \
    "int gstep(ivec2 g,inout GS s,out int wiperow,out uint lockm," \
    "out int lx,out int ly,out int lid,out int lrot){" \
    "int act=0;wiperow=0;lockm=0u;lx=0;ly=0;lid=0;lrot=0;" \
    "uint gid=uint(g.y*512+g.x);" \
    /* --- virtual player: buttons through the real input path --- */ \
    "int held=0;" \
    "if(s.mode==0&&s.lockst==0&&s.wipe==0){" \
    "if(s.x>s.tgt)held|=B_L;else if(s.x<s.tgt)held|=B_R;" \
    "else if(s.rot!=s.drot){if(((uFrame+int(gid))&7)<1)held|=B_B;}" \
    "else held|=B_D;}" \
    "int pressed=held&~s.held0;s.held0=held;" \
    /* --- game over: curtain, wait, restart --- */ \
    "if(s.mode==1){" \
    "wiperow=17-s.crow;s.crow++;" \
    "if(s.crow>=18){s.mode=2;s.t1=70;}" \
    "return A_CURTAIN;}" \
    "if(s.mode==2){if(s.t1>0){s.t1--;return 0;}" \
    /* back to the title screen (board cleared under the menus) */ \
    "s.mode=3;s.t1=40+int(hsh(gid^uint(uFrame))%80u);" \
    "return A_RESET;}" \
    /* --- boot flow: title -> mode select -> level select -> play. \
     * The virtual player presses Start on defaults: 1P, A-type, level 0 */ \
    "if(s.mode==3){if(s.t1>0){s.t1--;return 0;}" \
    "s.mode=4;s.t1=20+int(hsh(gid^uint(uFrame)^7u)%25u);return 0;}" \
    "if(s.mode==4){if(s.t1>0){s.t1--;return 0;}" \
    "s.mode=5;s.t1=20+int(hsh(gid^uint(uFrame)^13u)%25u);return 0;}" \
    "if(s.mode==5){if(s.t1>0){s.t1--;return 0;}" \
    "s.rng=(hsh(gid^uint(uFrame))&0xFFFFu)|1u;" \
    "s.lines=0;s.failed=0;s.score=0;s.crow=0;" \
    "s.level=0;" /* default settings: first level */ \
    "s.lockst=0;s.blink=0;s.wipe=0;s.kind=0;s.t1=0;s.t2=0;s.das=0;" \
    "s.prev=rndpiece(s.rng);s.nxt=rndpiece(s.rng);" \
    "nextpiece(g,s);s.mode=0;" \
    "return 0;}" \
    /* --- rotate_and_shift: rotation + DAS, real timings --- */ \
    "if(s.lockst==0&&s.wipe==0){" \
    "int old=s.rot;" \
    "if((pressed&B_A)!=0)s.rot=(s.rot-1)&3;" \
    "else if((pressed&B_B)!=0)s.rot=(s.rot+1)&3;" \
    "if(s.rot!=old&&hit(g,P[s.id*4+s.rot],s.x,s.y))s.rot=old;" \
    "int dir=0;" \
    "if((held&B_R)!=0){" \
    "if((pressed&B_R)!=0){dir=1;s.das=23;}" \
    "else{s.das--;if(s.das<=0){dir=1;s.das=9;}}}" \
    "else if((held&B_L)!=0){" \
    "if((pressed&B_L)!=0){dir=-1;s.das=23;}" \
    "else{s.das--;if(s.das<=0){dir=-1;s.das=9;}}}" \
    "if(dir!=0){" \
    "if(hit(g,P[s.id*4+s.rot],s.x+dir,s.y))s.das=1;" \
    "else s.x+=dir;}}" \
    /* --- drop_piece: soft drop + independent gravity --- */ \
    "if(s.lockst==0&&s.wipe==0){" \
    "int dodrop=0;" \
    "int dlr=held&(B_D|B_L|B_R);" \
    "if((held&B_D)==0)s.armed=1;" \
    "if(dlr==B_D&&s.armed==1){" \
    "if(s.t2==0){s.t2=3;s.sd=min(s.sd+1,255);dodrop=1;}}" \
    "else s.sd=0;" \
    "if(dodrop==0){s.grav--;" \
    "if(s.grav<=0){s.grav=GRAV[min(s.level,20)];dodrop=1;}}" \
    "else if(false){}" \
    "if(dodrop==1){" \
    "if(hit(g,P[s.id*4+s.rot],s.x,s.y+1)){" \
    /* lock_current_piece: soft-drop points, spawn-lock top-out */ \
    "if(s.sd>1)s.score=min(s.score+s.sd-1,999999);" \
    "s.sd=0;s.lockst=1;s.armed=0;" \
    "if(s.x==2&&s.y==-2){" \
    "if(s.failed==1){s.mode=1;s.crow=0;return act;}" \
    "s.failed=1;}" \
    "}else s.y++;}}" \
    /* --- check_completed_rows (lockst 2, frame after merge) --- */ \
    "if(s.lockst==2){" \
    "int n=countfull(g);" \
    "s.kind=n;s.lockst=3;s.blink=0;s.t1=2;" \
    "if(n>0)s.lines=min(s.lines+n,250);}" \
    /* --- lock_piece_into_bg (lockst 1, same frame as the failed drop) --- */ \
    "if(s.lockst==1){" \
    "act|=A_MERGE;lockm=P[s.id*4+s.rot];lx=s.x;ly=s.y;" \
    "lid=s.id;lrot=s.rot;s.lockst=2;}" \
    /* --- move_blocks_down --- */ \
    "if(s.wipe==1&&s.t1==0){act|=A_SHIFT;s.wipe=2;}" \
    /* --- main-loop timers --- */ \
    "if(s.t1>0)s.t1--;if(s.t2>0)s.t2--;" \
    /* --- vblank: animate_line_clear (blink 7x10, then settle 13) --- */ \
    "if(s.lockst==3&&s.t1==0){" \
    "if(s.kind==0){s.lockst=0;nextpiece(g,s);}" \
    "else if(s.blink==6){act|=A_CLEAR;" \
    "s.blink=0;s.t1=13;s.wipe=1;s.lockst=0;}" \
    "else{act|=((s.blink&1)==0)?A_GREY:A_RESTORE;" \
    "s.blink++;s.t1=10;}}" \
    /* --- vblank: wipe chain (row/frame; score@5, level-up@16) --- */ \
    "if(s.wipe>=2&&s.t1==0){" \
    "wiperow=17-(s.wipe-2);act|=A_WIPEROW;" \
    "if(s.wipe==5&&s.kind>0){" \
    "int base=s.kind==1?40:s.kind==2?100:s.kind==3?300:1200;" \
    "s.score=min(s.score+base*(s.level+1),999999);}" \
    "if(s.wipe==16&&s.level<20&&s.lines/10>s.level)s.level++;" \
    "s.wipe++;" \
    "if(s.wipe>19){s.wipe=0;s.kind=0;nextpiece(g,s);}}" \
    "return act;}\n"

static const char *FS_REG = SIM_COMMON
    "out vec4 O;\n"
    "void main(){"
    "ivec2 p=ivec2(gl_FragCoord.xy);"
    "ivec2 g=ivec2(p.x/5,p.y);int k=p.x-g.x*5;"
    "GS s=load(g);int wr;uint lm;int lx,ly,lid,lrot;"
    "gstep(g,s,wr,lm,lx,ly,lid,lrot);"
    "if(k==0)O=vec4(float((s.x+1)|s.rot<<4),float(s.y+2),"
    "float(s.id|s.prev<<3|s.armed<<6|s.failed<<7),"
    "float(s.nxt|s.lockst<<3|s.blink<<5))/255.0;"
    "else if(k==1)O=vec4(float(s.grav),float(s.das|s.t2<<5),"
    "float(s.t1),float(s.wipe|s.kind<<5))/255.0;"
    "else if(k==2)O=vec4(float(s.rng&255u),float(s.rng>>8u),"
    "float((s.tgt+1)|s.drot<<4),float(s.held0))/255.0;"
    "else if(k==3)O=vec4(float(s.lines),float(s.level|s.mode<<5),"
    "float(s.sd),float(s.crow))/255.0;"
    "else O=vec4(float(s.score&255),float(s.score>>8&255),"
    "float(s.score>>16&255),0.0)/255.0;}";

/* logic (shadow/WRAM) board: merge, shift, curtain, reset */
static const char *FS_SHADOW = SIM_COMMON
    "out vec4 O;\n"
    "void main(){"
    "ivec2 t=ivec2(gl_FragCoord.xy);"
    "ivec2 g=ivec2(t.x/10,t.y/18);"
    "int mr=t.y-g.y*18,mc=t.x-g.x*10;"
    "GS s=load(g);int wr;uint lm;int lx,ly,lid,lrot;"
    "int act=gstep(g,s,wr,lm,lx,ly,lid,lrot);"
    "vec4 self=texelFetch(uShadow,t,0);"
    "if((act&A_RESET)!=0){O=vec4(0.0);return;}"
    "if((act&A_CURTAIN)!=0){O=(mr==wr)?vec4(1.0,23.0/255.0,0.0,1.0):self;"
    "return;}"
    "if((act&A_MERGE)!=0){int pr=mr-ly,pc=mc-lx;"
    "if(pr>=0&&pr<4&&pc>=0&&pc<4&&pcell(lm,pr,pc))"
    "{O=vec4(1.0,float(tileFor(lid,lrot,pr,pc))/255.0,0.0,1.0);return;}}"
    "if((act&A_SHIFT)!=0){O=shifted(g,mr,mc);return;}"
    "O=self;}";

/* display (VRAM) board: blink grey/restore/clear + progressive wipe redraw,
 * deferred from the shadow exactly like the original's VRAM writes */
static const char *FS_DISP = SIM_COMMON
    "out vec4 O;\n"
    "void main(){"
    "ivec2 t=ivec2(gl_FragCoord.xy);"
    "ivec2 g=ivec2(t.x/10,t.y/18);"
    "int mr=t.y-g.y*18,mc=t.x-g.x*10;"
    "GS s=load(g);int wr;uint lm;int lx,ly,lid,lrot;"
    "int act=gstep(g,s,wr,lm,lx,ly,lid,lrot);"
    "vec4 self=texelFetch(uDisp,t,0);"
    "if((act&A_RESET)!=0){O=vec4(0.0);return;}"
    "if((act&A_CURTAIN)!=0){O=(mr==wr)?vec4(1.0,23.0/255.0,0.0,1.0):self;"
    "return;}"
    "if((act&A_MERGE)!=0){int pr=mr-ly,pc=mc-lx;"
    "if(pr>=0&&pr<4&&pc>=0&&pc<4&&pcell(lm,pr,pc))"
    "{O=vec4(1.0,float(tileFor(lid,lrot,pr,pc))/255.0,0.0,1.0);return;}}"
    "bool full=rowfull(g,mr);"
    "if((act&A_GREY)!=0&&full){O=vec4(1.0,28.0/255.0,0.0,1.0);return;}"
    "if((act&A_RESTORE)!=0&&full){O=scell(g,mr,mc);return;}"
    "if((act&A_CLEAR)!=0&&full){O=vec4(0.0);return;}"
    "if((act&A_WIPEROW)!=0&&mr==wr){"
    "O=((act&A_SHIFT)!=0)?shifted(g,mr,mc):scell(g,mr,mc);return;}"
    "O=self;}";

static const char *FS_SHOW =
    "#version 300 es\n"
    "precision highp float; precision highp int;\n"
    "uniform sampler2D uDisp; uniform sampler2D uReg;"
    "uniform sampler2D uBig; uniform sampler2D uTiles;"
    "uniform sampler2D uMenus;\n"
    "uniform vec2 uRes; uniform vec2 uCam; uniform float uScale;"
    "uniform int uFrame;\n"
    "const uint P[28]=uint[28]("
    "0x1700u,0x6220u,0x0740u,0x2230u,0x4700u,0x2260u,0x0710u,0x3220u,"
    "0x0F00u,0x2222u,0x0F00u,0x2222u,0x6600u,0x6600u,0x6600u,0x6600u,"
    "0x6300u,0x1320u,0x6300u,0x1320u,0x3600u,0x2310u,0x3600u,0x2310u,"
    "0x2700u,0x2620u,0x0720u,0x2320u);\n"
    "const int GB=39;\n"
    "const int TILE[7]=int[7](20,17,0,19,18,22,21);\n"
    "int tileFor(int id,int rot,int r,int c){"
    "if(id==2){if((rot&1)==0)return c==0?26:(c==3?31:27);"
    "return r==0?16:(r==3?25:24);}return TILE[id];}\n"
    "float tilepx(int t,vec2 tv){"
    "return texelFetch(uTiles,ivec2(t*8+int(tv.x*8.0),int(tv.y*8.0)),0).r;}\n"
    "const int LSCORE[5]=int[5](0x1C,0x0C,0x18,0x1B,0x0E);\n"
    "const int LLEVEL[5]=int[5](0x15,0x0E,0x1F,0x0E,0x15);\n"
    "const int LLINES[5]=int[5](0x15,0x12,0x17,0x0E,0x1C);\n"
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
    "int vc=int(fr.x*20.0),vr=int(fr.y*18.0);"
    "vec2 tv=vec2(fract(fr.x*20.0),fract(fr.y*18.0));"
    "ivec4 a=ivec4(texelFetch(uReg,ivec2(g.x*5,g.y),0)*255.0+0.5);"
    "ivec4 b=ivec4(texelFetch(uReg,ivec2(g.x*5+1,g.y),0)*255.0+0.5);"
    "ivec4 d3=ivec4(texelFetch(uReg,ivec2(g.x*5+3,g.y),0)*255.0+0.5);"
    "ivec4 e=ivec4(texelFetch(uReg,ivec2(g.x*5+4,g.y),0)*255.0+0.5);"
    "int lockst=a.a>>3&3,wipe=b.a&31,mode=d3.g>>5;"
    /* booting games show the real menu screens (pre-rendered by the C
     * engine), with the blinking selection overlaid by the shader */
    "if(mode>=3){"
    "ivec2 sp=ivec2(int(fr.x*160.0),int(fr.y*144.0)+(mode-3)*144);"
    "vec3 mpx=texelFetch(uMenus,sp,0).rgb;"
    "float lum=dot(mpx,vec3(0.30,0.59,0.11));"
    "bool blink=((uFrame>>4)&1)==0;"
    "int mvc=int(fr.x*20.0),mvr=int(fr.y*18.0);"
    "if(mode==4&&blink&&mvr==5&&mvc>=3&&mvc<=8)lum=1.0-lum;"
    "if(mode==5&&blink&&mvr==6&&mvc==5)lum=1.0-lum;"
    "O=vec4(big*lum,1.0);return;}"
    "float grey=1.0;int tile=-1;"
    "if(vc==1||vc==12){tile=GB+11;}"
    "else if(vc==0){grey=0.75;}"
    "else if(vc>=2&&vc<=11){"
    "int mc=vc-2,mr=vr;"
    "vec4 cell=texelFetch(uDisp,ivec2(g.x*10+mc,g.y*18+mr),0);"
    "if(cell.r>0.5)tile=GB+int(cell.g*255.0+0.5);"
    /* active piece: hidden while locked/wiping, like the OAM hide */
    "if(mode==0&&lockst==0&&wipe==0){"
    "int px=(a.r&15)-1,rot=a.r>>4,py=a.g-2,id=a.b&7;"
    "int pr=mr-py,pc=mc-px;"
    "if(pr>=0&&pr<4&&pc>=0&&pc<4&&(P[id*4+rot]>>uint(pr*4+pc)&1u)!=0u)"
    "tile=GB+tileFor(id,rot,pr,pc);}"
    "}else{"
    "int score=e.r|e.g<<8|e.b<<16;"
    "int level=d3.g&31,lines=d3.r;"
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

/* grayscale atlas: font $00-$26 (0..38) + gameplay $70-$8F (39..70) */
static void tiles_init(void)
{
    enum { NFONT = 39, NGP = 32, N = NFONT + NGP,
           GP_FIRST = 0x70, VBASE = 48 };
    uint8_t *px = calloc(1, (size_t)N * 8 * 8 * 4);
    for (int t = 0; t < N; t++) {
        const Tile *tile = (t < NFONT)
            ? &ts_font.tiles[t]
            : &ts_gameplay.tiles[GP_FIRST + (t - NFONT) - VBASE];
        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                uint32_t c = tile->px[y * 8 + x];
                uint32_t r = c & 0xFF, gg = c >> 8 & 0xFF, b = c >> 16 & 0xFF;
                uint8_t grey = (c >> 24)
                    ? (uint8_t)((r * 77 + gg * 150 + b * 29) >> 8)
                    : 255;
                size_t o = ((size_t)y * N * 8 + (size_t)t * 8 + x) * 4;
                px[o] = px[o + 1] = px[o + 2] = grey;
                px[o + 3] = 255;
            }
        }
    }
    tex_tiles = make_tex(N * 8, 8, px);
    free(px);
}

/* pre-render the three boot screens with the actual C engine so the
 * micro-games' menus are pixel-identical to the big game's */
static void menus_init(void)
{
    uint8_t *buf = malloc((size_t)GB_W * GB_H * 3 * 4);
    static uint32_t tmp[GB_W * GB_H]; /* static: exceeds the wasm stack */
    Input none = { 0, 0 };

    enter_title();
    game_update(none);
    video_render(tmp);
    memcpy(buf, tmp, sizeof tmp);

    load_menu_screen(&tm_modeselect, ST_MODE_SELECT);
    game_update(none);
    video_render(tmp);
    memcpy(buf + sizeof tmp, tmp, sizeof tmp);

    menu_cursor = 0;
    load_menu_screen(&tm_adiff, ST_A_LEVEL);
    game_update(none);
    video_render(tmp);
    memcpy(buf + 2 * sizeof tmp, tmp, sizeof tmp);

    tex_menus = make_tex(GB_W, GB_H * 3, buf);
    free(buf);
    enter_title(); /* reset the big game to its own boot state */
}

static void sim_init(void)
{
    static const int grav[21] = { 52,48,44,40,36,32,27,21,16,10,9,8,7,6,5,
                                  5,4,4,3,3,2 };
    uint8_t *rg = calloc(1, (size_t)GAMES_X * REG_TEXELS * GAMES_Y * 4);
    for (int gy = 0; gy < GAMES_Y; gy++) {
        for (int gx = 0; gx < GAMES_X; gx++) {
            size_t o = ((size_t)gy * GAMES_X * REG_TEXELS
                        + (size_t)gx * REG_TEXELS) * 4;
            (void)grav;
            rg[o + 0] = (uint8_t)(2 + 1);           /* x=2 rot=0 */
            rg[o + 1] = (uint8_t)(-2 + 2);          /* y=-2 */
            rg[o + 2] = (uint8_t)(rand() % 7 | (rand() % 7) << 3 | 1 << 6);
            rg[o + 3] = (uint8_t)(rand() % 7);      /* nxt, lock=0 */
            rg[o + 4] = 1;
            rg[o + 6] = (uint8_t)(1 + rand() % 120); /* t1: boot stagger */
            rg[o + 8] = (uint8_t)(rand() & 255);    /* rng lo */
            rg[o + 9] = (uint8_t)(rand() & 255);    /* rng hi */
            rg[o + 10] = (uint8_t)((rand() % 9 + 1) | (rand() & 3) << 4);
            rg[o + 12] = 0;                         /* lines */
            rg[o + 13] = (uint8_t)(0 | 3 << 5);     /* level 0, mode 3 */
        }
    }
    size_t bn = (size_t)GAMES_X * MB_W * GAMES_Y * MB_H * 4;
    uint8_t *zero = calloc(1, bn);
    for (int i = 0; i < 2; i++) {
        tex_shadow[i] = make_tex(GAMES_X * MB_W, GAMES_Y * MB_H, zero);
        tex_disp[i] = make_tex(GAMES_X * MB_W, GAMES_Y * MB_H, zero);
    }
    free(zero);
    tex_reg[0] = make_tex(GAMES_X * REG_TEXELS, GAMES_Y, rg);
    tex_reg[1] = make_tex(GAMES_X * REG_TEXELS, GAMES_Y, NULL);
    free(rg);
}

static void bind_sim_inputs(GLuint prog, int pass)
{
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex_shadow[cur]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_disp[cur]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_reg[cur]);
    glUniform1i(glGetUniformLocation(prog, "uShadow"), 0);
    glUniform1i(glGetUniformLocation(prog, "uDisp"), 1);
    glUniform1i(glGetUniformLocation(prog, "uReg"), 2);
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

    glUseProgram(prog_shadow);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex_shadow[cur ^ 1], 0);
    glViewport(0, 0, GAMES_X * MB_W, GAMES_Y * MB_H);
    bind_sim_inputs(prog_shadow, 1);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glUseProgram(prog_disp);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, tex_disp[cur ^ 1], 0);
    glViewport(0, 0, GAMES_X * MB_W, GAMES_Y * MB_H);
    bind_sim_inputs(prog_disp, 2);
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
    glBindTexture(GL_TEXTURE_2D, tex_disp[cur]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, tex_reg[cur]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, tex_big);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, tex_tiles);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, tex_menus);
    glUniform1i(glGetUniformLocation(prog_show, "uDisp"), 0);
    glUniform1i(glGetUniformLocation(prog_show, "uReg"), 1);
    glUniform1i(glGetUniformLocation(prog_show, "uBig"), 2);
    glUniform1i(glGetUniformLocation(prog_show, "uTiles"), 3);
    glUniform1i(glGetUniformLocation(prog_show, "uMenus"), 4);
    glUniform1i(glGetUniformLocation(prog_show, "uFrame"), frame_no);
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
        /* micro-games step at the same fixed 59.73Hz as the big game */
        sim_tick();
        ticked = 1;
    }
    if (ticked || frame_no == 0) {
        video_render(fb);
        glBindTexture(GL_TEXTURE_2D, tex_big);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, GB_W, GB_H, GL_RGBA,
                        GL_UNSIGNED_BYTE, fb);
    }

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
    prog_shadow = link2(FS_SHADOW);
    prog_disp = link2(FS_DISP);
    prog_show = link2(FS_SHOW);

    game_seed((unsigned)SDL_GetPerformanceCounter());
    srand((unsigned)SDL_GetPerformanceCounter());
    if (game_init())
        return 1;
    video_render(fb);
    tex_big = make_tex(GB_W, GB_H, fb);
    tiles_init();
    menus_init();
    sim_init();

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

        const char *dump = getenv("FRACTAL_DUMP");
        if (dump) {
            int gx = 0, gy = 0;
            sscanf(dump, "%d,%d", &gx, &gy);
            uint8_t bpx[MB_W * MB_H * 4], rpx[REG_TEXELS * 4];
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex_shadow[cur], 0);
            glReadPixels(gx * MB_W, gy * MB_H, MB_W, MB_H, GL_RGBA,
                         GL_UNSIGNED_BYTE, bpx);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex_reg[cur], 0);
            glReadPixels(gx * REG_TEXELS, gy, REG_TEXELS, 1, GL_RGBA,
                         GL_UNSIGNED_BYTE, rpx);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            printf("x=%d rot=%d y=%d id=%d prev=%d armed=%d failed=%d "
                   "nxt=%d lock=%d blink=%d | grav=%d das=%d t2=%d t1=%d "
                   "wipe=%d kind=%d | lines=%d level=%d mode=%d sd=%d "
                   "| score=%d\n",
                   (rpx[0] & 15) - 1, rpx[0] >> 4, rpx[1] - 2,
                   rpx[2] & 7, rpx[2] >> 3 & 7, rpx[2] >> 6 & 1, rpx[2] >> 7,
                   rpx[3] & 7, rpx[3] >> 3 & 3, rpx[3] >> 5,
                   rpx[4], rpx[5] & 31, rpx[5] >> 5, rpx[6],
                   rpx[7] & 31, rpx[7] >> 5,
                   rpx[12], rpx[13] & 31, rpx[13] >> 5, rpx[14],
                   rpx[16] | rpx[17] << 8 | rpx[18] << 16);
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
