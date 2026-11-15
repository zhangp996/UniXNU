/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* IN2020 stock s6sy791@48 panel coordinates, inclusive. */
enum {SmTouchMaxX = 1439, SmTouchMaxY = 3168, SmTouchFingers = 10};
enum {SmTouchDown = 1, SmTouchMove = 2, SmTouchUp = 3};

typedef struct {
    unsigned id, action, x, y;
} SmTouchContact;

typedef struct {
    unsigned active;
    SmTouchContact contacts[SmTouchFingers];
} SmTouchFrame;

static inline bool
sm8250_touch_decode(const uint8_t event[8], SmTouchContact *contact)
{
    contact->id = (event[0] >> 2) & 0xf;
    contact->action = event[0] >> 6;
    if ((event[0] & 3) != 0 || contact->id == 0 ||
        contact->id > SmTouchFingers || contact->action == 0) {
        return false;
    }
    contact->x = (event[1] << 4) | (event[3] >> 4);
    contact->y = (event[2] << 4) | (event[3] & 0xf);
    return true;
}

static inline bool
sm8250_touch_apply(SmTouchFrame *frame, const SmTouchContact *contact)
{
    const unsigned slot = contact->id - 1;
    const unsigned bit = 1u << slot;
    if (contact->action != SmTouchDown && !(frame->active & bit)) {
        return false;
    }
    if (contact->action == SmTouchUp) {
        frame->active &= ~bit;
    } else {
        frame->contacts[slot] = *contact;
        frame->active |= bit;
    }
    return true;
}
