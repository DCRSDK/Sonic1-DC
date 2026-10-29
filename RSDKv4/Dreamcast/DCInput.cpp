// RSDKv4 Dreamcast input backend: maple controller -> engine input slots
//
// Mapping:
//   D-pad / analog stick -> Up/Down/Left/Right
//   A -> Button A (jump)     B -> Button B
//   X -> Button C            Y -> Button Y
//   L trigger -> Button L    R trigger -> Button R
//   Start -> Start
//   L+R+Start held together -> Select (dev menu access)

#include "../RetroEngine.hpp"

#if RETRO_USING_KOS

#include <kos.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#include "DCCommon.hpp"

#define DC_ANALOG_DEADZONE (64)
#define DC_TRIGGER_PRESS   (64)

static inline void DC_SetButton(int id, bool held)
{
    if (held)
        inputDevice[id].setHeld();
    else if (inputDevice[id].hold)
        inputDevice[id].setReleased();
}

void DC_ProcessInput()
{
    // Keep the Sonic 2 emblem on any connected VMU LCD. This lives on the input
    // path purely because it is the one DC-side hook that runs every frame from
    // the very first one, which is the point — the icon needs to be up during
    // the boot logos, long before any stage loads.
    DC_VmuTick();

    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);

    if (!dev) {
        for (int i = 0; i < INPUT_BUTTONCOUNT; ++i) {
            if (inputDevice[i].hold)
                inputDevice[i].setReleased();
        }
        return;
    }

    cont_state_t *state = (cont_state_t *)maple_dev_status(dev);
    if (!state)
        return;

    const bool up    = (state->buttons & CONT_DPAD_UP) || state->joyy < -DC_ANALOG_DEADZONE;
    const bool down  = (state->buttons & CONT_DPAD_DOWN) || state->joyy > DC_ANALOG_DEADZONE;
    const bool left  = (state->buttons & CONT_DPAD_LEFT) || state->joyx < -DC_ANALOG_DEADZONE;
    const bool right = (state->buttons & CONT_DPAD_RIGHT) || state->joyx > DC_ANALOG_DEADZONE;

    const bool trigL = state->ltrig > DC_TRIGGER_PRESS;
    const bool trigR = state->rtrig > DC_TRIGGER_PRESS;
    const bool start = (state->buttons & CONT_START) != 0;

    DC_SetButton(INPUT_UP, up);
    DC_SetButton(INPUT_DOWN, down);
    DC_SetButton(INPUT_LEFT, left);
    DC_SetButton(INPUT_RIGHT, right);

    DC_SetButton(INPUT_BUTTONA, (state->buttons & CONT_A) != 0);
    DC_SetButton(INPUT_BUTTONB, (state->buttons & CONT_B) != 0);
    DC_SetButton(INPUT_BUTTONC, (state->buttons & CONT_X) != 0);
    DC_SetButton(INPUT_BUTTONY, (state->buttons & CONT_Y) != 0);

    DC_SetButton(INPUT_BUTTONL, trigL);
    DC_SetButton(INPUT_BUTTONR, trigR);

    DC_SetButton(INPUT_START, start && !(trigL && trigR));
    DC_SetButton(INPUT_SELECT, start && trigL && trigR);

    bool anyHold = false;
    for (int i = 0; i < INPUT_ANY; ++i) {
        if (inputDevice[i].hold) {
            anyHold = true;
            break;
        }
    }
    DC_SetButton(INPUT_ANY, anyHold);

    if (anyHold)
        inputType = 1; // controller
}

#endif // RETRO_USING_KOS
