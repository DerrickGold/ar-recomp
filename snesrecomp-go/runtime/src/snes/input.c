/* Original protocol implementation from published controller specifications.
 * https://snes.nesdev.org/wiki/Mouse
 * https://snes.nesdev.org/wiki/Controller_reading
 * No game ROM addresses, host APIs, or third-party controller code. */
#include "input.h"
#include "snes.h"
#include "saveload.h"
#include "snesrecomp/runner/input.h"
#include <limits.h>
#include <string.h>

void snes_input_reset(Snes *snes) {
    unsigned port;
    snes->inputLatch = false;
    for (port = 0; port < 2u; ++port) {
        uint8_t device = snes->inputPorts[port].device;
        memset(&snes->inputPorts[port], 0, sizeof(SnesInputPort));
        snes->inputPorts[port].device = device;
    }
}

static int32_t add_delta(int32_t current, int32_t delta) {
    int64_t sum = (int64_t)current + delta;
    return sum < INT32_MIN ? INT32_MIN : sum > INT32_MAX ? INT32_MAX : (int32_t)sum;
}

void snes_input_submit(Snes *snes, unsigned port, unsigned device,
                       unsigned buttons, int32_t dx, int32_t dy) {
    SnesInputPort *input = &snes->inputPorts[port];
    if (input->device != device) {
        memset(input, 0, sizeof(*input));
        input->device = (uint8_t)device;
    }
    input->buttons = (uint8_t)buttons;
    input->pending_x = add_delta(input->pending_x, dx);
    input->pending_y = add_delta(input->pending_y, dy);
}

static uint8_t motion(int32_t *pending, uint8_t *sign, uint8_t sensitivity) {
    static const uint8_t accelerated[2][8] = {
        {0, 1, 2, 3, 8, 10, 12, 21}, {0, 1, 4, 9, 12, 20, 24, 28}
    };
    int32_t delta = *pending;
    unsigned magnitude;
    if (delta < -127) delta = -127;
    if (delta > 127) delta = 127;
    *pending -= delta;
    if (delta != 0) *sign = delta < 0 ? 0x80u : 0u;
    magnitude = (unsigned)(delta < 0 ? -delta : delta);
    if (sensitivity >= 1u && sensitivity <= 2u)
        magnitude = accelerated[sensitivity - 1u][magnitude < 7u ? magnitude : 7u];
    return (uint8_t)(*sign | magnitude);
}

static void capture(Snes *snes, unsigned port) {
    SnesInputPort *input = &snes->inputPorts[port];
    input->cursor = 0;
    switch (input->device) {
        case SR_INPUT_DEVICE_GAMEPAD:
            input->packet = (uint32_t)SwapInputBits(port == 0u ?
                snes->input1_currentState : snes->input2_currentState) << 16;
            break;
        case SR_INPUT_DEVICE_MOUSE: {
            const uint8_t x = motion(&input->pending_x, &input->sign_x, input->sensitivity);
            const uint8_t y = motion(&input->pending_y, &input->sign_y, input->sensitivity);
            const unsigned header = 1u | (unsigned)input->sensitivity << 4 |
                ((input->buttons & SR_MOUSE_BUTTON_LEFT) ? 0x40u : 0u) |
                ((input->buttons & SR_MOUSE_BUTTON_RIGHT) ? 0x80u : 0u);
            input->packet = (uint32_t)header << 16 | (uint32_t)y << 8 | x;
            break;
        }
        default: input->packet = 0; break;
    }
}

void snes_input_latch(Snes *snes, bool high) {
    unsigned port;
    if (snes->inputLatch && !high)
        for (port = 0; port < 2u; ++port) capture(snes, port);
    snes->inputLatch = high;
}

uint8_t snes_input_read(Snes *snes, unsigned port) {
    SnesInputPort *input = &snes->inputPorts[port];
    unsigned bits;
    if (input->device == SR_INPUT_DEVICE_NONE) return 0;
    if (snes->inputLatch) {
        if (input->device == SR_INPUT_DEVICE_MOUSE) {
            input->sensitivity = (uint8_t)((input->sensitivity + 1u) % 3u);
            return 0;
        }
        return (uint8_t)((port == 0u ? snes->input1_currentState : snes->input2_currentState) & 1u);
    }
    bits = input->device == SR_INPUT_DEVICE_MOUSE ? 32u : 16u;
    if (input->cursor >= bits) return 1u;
    return (uint8_t)((input->packet >> (31u - input->cursor++)) & 1u);
}

void snes_input_auto_read(Snes *snes) {
    unsigned port, bit;
    /* Same latch and clocks as software. In particular the mouse's last
     * sixteen motion bits remain available to subsequent manual reads. */
    snes_input_latch(snes, true);
    snes_input_latch(snes, false);
    for (port = 0; port < 2u; ++port) {
        uint16_t result = 0;
        for (bit = 0; bit < 16u; ++bit)
            result = (uint16_t)((result << 1) | snes_input_read(snes, port));
        snes->inputPorts[port].auto_result = result;
    }
}

uint16_t snes_input_auto_result(const Snes *snes, unsigned port) {
    /* Preserve the established live-pad adapter contract. Mouse and absent
     * ports use the automatic hardware transaction result instead. */
    if (snes->inputPorts[port].device == SR_INPUT_DEVICE_GAMEPAD)
        return SwapInputBits(port == 0u ? snes->input1_currentState : snes->input2_currentState);
    return snes->inputPorts[port].auto_result;
}

void snes_input_saveload(Snes *snes, SaveLoadInfo *info) {
    unsigned port;
    saveload_bool(info, &snes->inputLatch);
    for (port = 0; port < 2u; ++port) {
        SnesInputPort *input = &snes->inputPorts[port];
        saveload_u32(info, &input->packet);
        saveload_i32(info, &input->pending_x);
        saveload_i32(info, &input->pending_y);
        saveload_u16(info, &input->auto_result);
        saveload_u8(info, &input->device);
        saveload_u8(info, &input->buttons);
        saveload_u8(info, &input->sensitivity);
        saveload_u8(info, &input->cursor);
        saveload_u8(info, &input->sign_x);
        saveload_u8(info, &input->sign_y);
        if (input->device > SR_INPUT_DEVICE_MOUSE || input->buttons > 3u ||
            input->sensitivity > 2u || input->cursor > 32u ||
            (input->sign_x != 0u && input->sign_x != 0x80u) ||
            (input->sign_y != 0u && input->sign_y != 0x80u)) info->failed = true;
    }
}
