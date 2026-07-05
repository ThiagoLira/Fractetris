#ifndef INPUT_H
#define INPUT_H

#include <stdint.h>

/* GB joypad bit layout (matches the original's hJoyHeld convention) */
#define BTN_A      0x01
#define BTN_B      0x02
#define BTN_SELECT 0x04
#define BTN_START  0x08
#define BTN_RIGHT  0x10
#define BTN_LEFT   0x20
#define BTN_UP     0x40
#define BTN_DOWN   0x80

typedef struct {
    uint8_t held;    /* buttons currently down */
    uint8_t pressed; /* newly down this frame */
} Input;

/* Feed SDL keyboard state each frame; returns held+pressed. */
Input input_poll(void);
/* Joystick lifecycle: call once after SDL_Init, and per SDL_Event. */
void input_init(void);
union SDL_Event;
void input_handle_event(const union SDL_Event *ev);

#endif
