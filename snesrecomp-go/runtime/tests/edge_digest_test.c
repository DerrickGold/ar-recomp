#include "edge_digest.h"
#include "runner_internal.h"
#include "snes/snes.h"
#include "support/sha256.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef EDGE_DIGEST_TEST_DIR
#define EDGE_DIGEST_TEST_DIR "."
#endif

#ifdef _WIN32
#define set_environment(name, value) _putenv_s(name, value)
#else
#define set_environment(name, value) setenv(name, value, 1)
#endif

static int failures;

static void check(bool condition, const char *message) {
    if (condition) return;
    fprintf(stderr, "edge digest failed: %s\n", message);
    ++failures;
}

static void emit(Snes *runner, SrEventMask mask, SrRunnerEvent event) {
    sr_runner_emit_event(runner, mask, &event);
}

static void emit_frame(Snes *runner, uint32_t flags, uint64_t frame) {
    SrRunnerEvent event = {0};
    event.type = SR_EVENT_FRAME_BOUNDARY;
    event.flags = flags;
    event.frame_counter = frame;
    event.label = "ignored-frame-label";
    emit(runner, SR_EVENT_MASK_FRAME, event);
}

static void emit_block(Snes *runner, const char *label, uint32_t pc24,
                       uint32_t cpu_flags, uint16_t x, uint16_t s) {
    SrRunnerEvent event = {0};
    event.type = SR_EVENT_EXECUTION_BLOCK;
    event.frame_counter = 99u; /* implied by the frame records */
    event.pc24 = pc24;
    event.cpu_flags = cpu_flags;
    event.register_x = x;
    event.stack_pointer = s;
    event.label = label;
    emit(runner, SR_EVENT_MASK_EXECUTION_BLOCK, event);
}

/* One host frame: blocks under two labels, a dispatch, an interrupt and an
 * error. first_label is "alpha" but may live at any address; swap exchanges
 * the order of the first two blocks. */
static void emit_scenario(Snes *runner, const char *first_label, bool swap) {
    char alpha_copy[] = "alpha";
    SrRunnerEvent dispatch = {0};
    SrRunnerEvent interrupt = {0};
    SrRunnerEvent error = {0};
    emit_frame(runner, SR_EVENT_FRAME_BEGIN | SR_EVENT_FRAME_HOST_TICK, 41u);
    emit_block(runner, first_label, swap ? 0x008329u : 0x008325u,
               SR_CPU_STATE_M_FLAG | SR_CPU_STATE_X_FLAG |
                   SR_CPU_STATE_HOST_RETURN_VALID |
                   SR_CPU_STATE_EXECUTION_PC_VALID,
               0x1234u, 0x01f0u);
    /* Same label content at another address records no label change. */
    emit_block(runner, alpha_copy, swap ? 0x008325u : 0x008329u,
               SR_CPU_STATE_M_FLAG, 0x0002u, 0x01eeu);
    dispatch.type = SR_EVENT_DYNAMIC_DISPATCH;
    dispatch.pc24 = 0x03a027u;
    dispatch.source_pc24 = 0x039ef3u;
    dispatch.flags = SR_EVENT_DISPATCH_FOUND | SR_EVENT_DISPATCH_CONTINUATION;
    dispatch.cpu_flags = SR_CPU_STATE_M_FLAG | SR_CPU_STATE_X_FLAG;
    dispatch.register_x = 0x0010u;
    dispatch.stack_pointer = 0x01fdu;
    emit(runner, SR_EVENT_MASK_DYNAMIC_DISPATCH, dispatch);
    interrupt.type = SR_EVENT_INTERRUPT;
    interrupt.interrupt_kind = SR_INTERRUPT_NMI;
    interrupt.flags = SR_EVENT_INTERRUPT_ENTER;
    interrupt.interrupt_vector = 0xffeau;
    interrupt.interrupt_scanline = 225;
    interrupt.pc24 = 0x008000u;
    emit(runner, SR_EVENT_MASK_INTERRUPT, interrupt);
    error.type = SR_EVENT_ERROR;
    error.error_code = SR_RUNNER_ERROR_DISPATCH_MISS;
    error.flags = SR_EVENT_ERROR_RECOVERABLE;
    error.pc24 = 0x05c000u;
    error.source_pc24 = 0x05b123u;
    emit(runner, SR_EVENT_MASK_ERROR, error);
    emit_block(runner, "beta", 0x7e2000u, SR_CPU_STATE_EMULATION, 0xffffu,
               0x01ffu);
    emit_frame(runner, SR_EVENT_FRAME_END | SR_EVENT_FRAME_HOST_TICK, 41u);
}

/* The documented record stream of emit_scenario(swap = false). */
static void expected_scenario_digest(char text[65]) {
    static const uint8_t stream[] = {
        'F', 0x09, 41, 0, 0, 0, 0, 0, 0, 0,
        'L', 5, 0, 'a', 'l', 'p', 'h', 'a',
        'B', 0x25, 0x83, 0x00, 0x0b, 0x34, 0x12, 0xf0, 0x01,
        'B', 0x29, 0x83, 0x00, 0x01, 0x02, 0x00, 0xee, 0x01,
        'D', 0x27, 0xa0, 0x03, 0xf3, 0x9e, 0x03, 0x05, 0x03, 0x10, 0x00,
        0xfd, 0x01,
        'I', 0x01, 0x01, 0xea, 0xff, 0xe1, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00,
        'E', 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0xc0, 0x05, 0x23, 0xb1, 0x05,
        'L', 4, 0, 'b', 'e', 't', 'a',
        'B', 0x00, 0x20, 0x7e, 0x04, 0xff, 0xff, 0xff, 0x01,
        'F', 0x0a, 41, 0, 0, 0, 0, 0, 0, 0,
    };
    static const char hex[] = "0123456789abcdef";
    uint8_t digest[32];
    unsigned index;
    sha256_compute(stream, sizeof(stream), digest);
    for (index = 0u; index < 32u; ++index) {
        text[index * 2u] = hex[digest[index] >> 4];
        text[index * 2u + 1u] = hex[digest[index] & 15u];
    }
    text[64] = '\0';
}

static size_t read_file(const char *path, char *buffer, size_t capacity) {
    FILE *file = fopen(path, "rb");
    size_t length;
    if (file == NULL) return 0u;
    length = fread(buffer, 1u, capacity - 1u, file);
    fclose(file);
    buffer[length] = '\0';
    return length;
}

static void run_scenario(Snes *runner, const char *path,
                         const char *first_label, bool swap) {
    check(sr_edge_digest_open(path) == 1, "open digest file");
    check(sr_runner_event_enabled(SR_EVENT_MASK_EXECUTION_BLOCK) &&
              sr_runner_event_enabled(SR_EVENT_MASK_DYNAMIC_DISPATCH) &&
              sr_runner_event_enabled(SR_EVENT_MASK_INTERRUPT) &&
              sr_runner_event_enabled(SR_EVENT_MASK_ERROR) &&
              sr_runner_event_enabled(SR_EVENT_MASK_FRAME),
          "an open digest observes the bound runner");
    emit_scenario(runner, first_label, swap);
    sr_edge_digest_close();
    check(g_sr_runner_event_mask == 0u, "close releases the subscription");
}

int main(void) {
    static char first[4096], second[4096], swapped[4096], environment[4096];
    char expected[65], line[256];
    const char *heap_label;
    char *copy;
    Snes runner = {0};
    const char *env_path = EDGE_DIGEST_TEST_DIR "/edge-digest-env.txt";
    const char *first_path = EDGE_DIGEST_TEST_DIR "/edge-digest-first.txt";
    const char *second_path = EDGE_DIGEST_TEST_DIR "/edge-digest-second.txt";
    const char *swapped_path = EDGE_DIGEST_TEST_DIR "/edge-digest-swapped.txt";

    /* The environment names the file at the first bind. */
    check(set_environment("SNESRECOMP_EDGE_DIGEST", env_path) == 0,
          "set digest environment");
    sr_edge_digest_bind_runner(&runner, 1);
    check(sr_runner_event_enabled(SR_EVENT_MASK_EXECUTION_BLOCK),
          "environment enables the digest");
    emit_frame(&runner, SR_EVENT_FRAME_END | SR_EVENT_FRAME_HOST_TICK, 1u);
    sr_edge_digest_bind_runner(&runner, 0);
    check(g_sr_runner_event_mask == 0u, "unbind releases the subscription");
    sr_edge_digest_close();
    read_file(env_path, environment, sizeof(environment));
    check(strncmp(environment, "snesrecomp-edge-digest v1\nframe=1 blocks=0 "
                               "dispatches=0 interrupts=0 errors=0 "
                               "boundaries=1 sha256=",
                  94) == 0,
          "environment digest header and frame checkpoint");
    check(strstr(environment, "\nunbind blocks=0 ") != NULL &&
              strstr(environment, "\nfinal blocks=0 ") != NULL,
          "unbind and final checkpoints");

    /* Nothing subscribes, so nothing is paid, without an open file. */
    sr_edge_digest_bind_runner(&runner, 1);
    check(g_sr_runner_event_mask == 0u, "no file, no subscription");

    run_scenario(&runner, first_path, "alpha", false);
    read_file(first_path, first, sizeof(first));
    expected_scenario_digest(expected);
    snprintf(line, sizeof(line),
             "frame=41 blocks=3 dispatches=1 interrupts=1 errors=1 "
             "boundaries=2 sha256=%s\n",
             expected);
    check(strstr(first, line) != NULL,
          "checkpoint digests the documented record stream");
    snprintf(line, sizeof(line),
             "final blocks=3 dispatches=1 interrupts=1 errors=1 "
             "boundaries=2 sha256=%s\n",
             expected);
    check(strstr(first, line) != NULL, "final line repeats the digest");

    /* A label at another address with the same content changes nothing. */
    copy = (char *)malloc(6u);
    check(copy != NULL, "allocate label copy");
    if (copy != NULL) {
        memcpy(copy, "alpha", 6u);
        heap_label = copy;
        run_scenario(&runner, second_path, heap_label, false);
        read_file(second_path, second, sizeof(second));
        check(strcmp(first, second) == 0,
              "digest is independent of label addresses");
        free(copy);
    }

    /* Reordering two blocks changes the digest. */
    run_scenario(&runner, swapped_path, "alpha", true);
    read_file(swapped_path, swapped, sizeof(swapped));
    check(strstr(swapped, "frame=41 blocks=3 ") != NULL &&
              strcmp(first, swapped) != 0,
          "digest is order sensitive");

    sr_edge_digest_bind_runner(&runner, 0);
    if (failures != 0) return 1;
    puts("edge digest ok");
    return 0;
}
