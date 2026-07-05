#ifndef AUDIO_H
#define AUDIO_H

/* Stubs until the APU synth lands (task: sound engine port). */

typedef enum {
    SFX_CURSOR,
    SFX_MOVE,
    SFX_ROTATE,
    SFX_LOCK,
    SFX_LINECLEAR,
    SFX_TETRIS,
    SFX_LEVELUP,
    SFX_PAUSE,
    SFX_GAMEOVER,
} SfxId;

typedef enum {
    MUSIC_OFF = 3, /* music_sel: 0=A 1=B 2=C 3=off */
    MUSIC_TITLE = 100,
    MUSIC_GAMEOVER,
} MusicId;

void audio_sfx(int id);
void audio_music(int id);

#endif
