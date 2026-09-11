# Serial input and native SPC boot

## Serial pads and mice

ABI v2 appends `submit_input_device`, gated by `SR_RUNNER_CAP_INPUT_DEVICES`
and `SNES_RUNNER_API_INPUT_DEVICES_SIZE`. It has no SDL or platform dependency.
Include `snesrecomp/runner/input.h` for the request type. Ports are zero-based;
the default device on both ports remains a gamepad. Pad buttons still come
from the established packed `RtlRunFrame` input.

At an emulation-thread safe point, before the next game-frame slice:

```c
const SnesRunnerApi *api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
if (api && api->struct_size >= SNES_RUNNER_API_INPUT_DEVICES_SIZE &&
    (api->capabilities & SR_RUNNER_CAP_INPUT_DEVICES)) {
    SrGenerationSnapshot generation = {.struct_size = SR_GENERATION_SNAPSHOT_V2_SIZE};
    if (api->query_generations(runner, &generation) == SR_RESULT_OK) {
        SrInputDeviceRequest input = {
            .struct_size = SR_INPUT_DEVICE_REQUEST_V2_SIZE,
            .lifetime_generation = generation.lifetime_generation,
            .port = 0, .device = SR_INPUT_DEVICE_MOUSE,
            .buttons = mouse_buttons, .delta_x = dx, .delta_y = dy,
        };
        SrResult result = api->submit_input_device(runner, &input);
        /* Handle result; stale lifetimes require re-querying generations. */
    }
}
```

Positive movement is right/down, in signed relative counts, not screen
coordinates. Buttons use `SR_MOUSE_BUTTON_LEFT/RIGHT`. An unchanged mouse
accumulates deltas and replaces held buttons; changing the device clears its
serial state. Requests for pads/absent devices must have zero mouse fields.
The runner never retains host pointers. Do not submit from audio/observation
callbacks or concurrently with the emulation thread.

The hardware path supports manual `$4016/$4017` reads, latch writes, mouse
identification, sign/magnitude movement, overread bits, and sensitivity cycling.
At `SR_GAME_TIMING_BEGIN_FRAME_SLICE`, enabled automatic joypad reading
consumes the first sixteen bits; manual reads continue with the mouse's
remaining motion bits. Scanout's final beam positioning does **not** consume
a second packet. Existing live automatic-pad results remain compatible.

This is a deterministic functional protocol model, not a cycle-exact
controller wire simulation. Automatic reads are completed at the frame
boundary, not clocked over hardware scanlines. Mouse sensitivity uses the
published measured lookup mapping; it is not a calibration of a particular
host mouse or a reproduction of third-party mouse timing quirks. Large host
deltas are consumed in bounded steps; pending signed accumulation saturates.

An SDL3 frontend can poll
[`SDL_GetRelativeMouseState`](https://wiki.libsdl.org/SDL3/SDL_GetRelativeMouseState)
after pumping events, map left/right button flags, retain fractional motion
until converting to integer counts, and submit exactly once per emulated tick.
Do not re-submit the same relative delta for every catch-up tick. That adapter
belongs in the frontend; SDL is not a runner dependency. No mouse events are
silently added to the existing pad-only replay format: a host recording mouse
input must record device/buttons/deltas and their tick explicitly.

Protocol references: [SNESdev Mouse](https://snes.nesdev.org/wiki/Mouse) and
[controller reading](https://snes.nesdev.org/wiki/Controller_reading).

## Native SPC upload

The original 64-byte recomp bootstrap now accepts the conventional ready,
command, byte-transfer, next-block, and execute handshake instead of idling
after the ready marker. It executes as ordinary SPC700 instructions. Uploads
use the normal ARAM/MMIO writes and write-provenance path, including uploads
to DSP registers. There is no 65816 interpreter fallback, title-specific
shortcut, externally downloaded firmware, or copied vendor IPL bytecode.

The synthetic boot contract covers initialization, invalid initial commands,
counter/page wrapping, multiple blocks, executing a redistributable SPC
fixture, reentry, and MMIO transfers. Existing direct host-bootstrap
transactions remain available. This firmware implements the documented port
protocol; it does not promise the vendor firmware's internal PCs or exact
instruction timing. Uploads which replace the pointer scratch `$00/$01` or
unmap the executing ROM still have the ordinary SPC memory side effects.

Sources: [SPC boot protocol](https://snes.nesdev.org/wiki/Booting_the_SPC700)
and [S-SMP initialization](https://snes.nesdev.org/wiki/S-SMP).

## Save states and deterministic hashes

The input update advanced portable snapshots from version 12 to 13 to include device type,
buttons, pending motion, sensitivity, sign, packet, serial cursor, automatic
results, and latch. The subsequent audio update uses version 14 and rejects
older quick states because their DSP/timeline layouts differ. Current snapshots
retain the exact input, bootstrap, and mid-upload state.

Input advanced semantic digest schema from 2 to 3; the current audio update
advances it to 4. Compare hashes only within the same schema. The golden
fixture is updated deliberately; WRAM, SRAM, CPU, and semantic dispatch hashes
can still be compared independently across this change.
