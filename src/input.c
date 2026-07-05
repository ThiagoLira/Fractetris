#include <SDL.h>
#include "input.h"

/* Keyboard: X/Space = A, Z = B, arrows/WASD = d-pad, Enter = Start,
 * Tab/RShift = Select.
 *
 * Joysticks use the classic SDL_Joystick API (no mapping database, works
 * with any HID pad): axis 0/1 + hat 0 = d-pad; even buttons = A, odd = B,
 * except the two highest-numbered buttons which act as Select/Start
 * (start/select are nearly always the last pair on basic pads). */

#define AXIS_DEADZONE 16384

static SDL_Joystick *sticks[4];
static int stick_count;

void input_init(void)
{
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
    SDL_JoystickEventState(SDL_ENABLE);
}

void input_handle_event(const union SDL_Event *uev)
{
    const SDL_Event *ev = (const SDL_Event *)uev;
    if (ev->type == SDL_JOYDEVICEADDED && stick_count < 4) {
        SDL_Joystick *j = SDL_JoystickOpen(ev->jdevice.which);
        if (j)
            sticks[stick_count++] = j;
    } else if (ev->type == SDL_JOYDEVICEREMOVED) {
        for (int i = 0; i < stick_count; i++) {
            if (sticks[i] &&
                SDL_JoystickInstanceID(sticks[i]) == ev->jdevice.which) {
                SDL_JoystickClose(sticks[i]);
                sticks[i] = sticks[--stick_count];
                sticks[stick_count] = NULL;
            }
        }
    }
}

static uint8_t joystick_held(void)
{
    uint8_t held = 0;
    for (int i = 0; i < stick_count; i++) {
        SDL_Joystick *j = sticks[i];
        if (!j)
            continue;

        if (SDL_JoystickNumAxes(j) >= 2) {
            Sint16 x = SDL_JoystickGetAxis(j, 0);
            Sint16 y = SDL_JoystickGetAxis(j, 1);
            if (x < -AXIS_DEADZONE) held |= BTN_LEFT;
            if (x > AXIS_DEADZONE)  held |= BTN_RIGHT;
            if (y < -AXIS_DEADZONE) held |= BTN_UP;
            if (y > AXIS_DEADZONE)  held |= BTN_DOWN;
        }
        if (SDL_JoystickNumHats(j) >= 1) {
            Uint8 hat = SDL_JoystickGetHat(j, 0);
            if (hat & SDL_HAT_LEFT)  held |= BTN_LEFT;
            if (hat & SDL_HAT_RIGHT) held |= BTN_RIGHT;
            if (hat & SDL_HAT_UP)    held |= BTN_UP;
            if (hat & SDL_HAT_DOWN)  held |= BTN_DOWN;
        }

        int nb = SDL_JoystickNumButtons(j);
        for (int b = 0; b < nb; b++) {
            if (!SDL_JoystickGetButton(j, b))
                continue;
            if (nb >= 4 && b == nb - 1)
                held |= BTN_START;
            else if (nb >= 4 && b == nb - 2)
                held |= BTN_SELECT;
            else if (b & 1)
                held |= BTN_B;
            else
                held |= BTN_A;
        }
    }
    return held;
}

Input input_poll(void)
{
    static uint8_t prev;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint8_t held = 0;

    if (k[SDL_SCANCODE_X] || k[SDL_SCANCODE_SPACE])  held |= BTN_A;
    if (k[SDL_SCANCODE_Z])                           held |= BTN_B;
    if (k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_TAB]) held |= BTN_SELECT;
    if (k[SDL_SCANCODE_RETURN])                      held |= BTN_START;
    if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D])  held |= BTN_RIGHT;
    if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A])   held |= BTN_LEFT;
    if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W])     held |= BTN_UP;
    if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S])   held |= BTN_DOWN;

    held |= joystick_held();

    Input in = { held, (uint8_t)(held & ~prev) };
    prev = held;
    return in;
}
