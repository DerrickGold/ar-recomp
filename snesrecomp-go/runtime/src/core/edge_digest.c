#include "snesrecomp/support/utf8_fs.h"

#include "edge_digest.h"

#include "snesrecomp/runner.h"
#include "support/sha256.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Digest records. Every field is little-endian and fixed width, so the digest
 * depends only on the reported event sequence, never on struct layout,
 * pointers, or the host:
 *   'B' pc24:3 cpu:1 x:2 s:2                     executed generated block
 *   'D' pc24:3 source:3 flags:1 cpu:1 x:2 s:2    dynamic dispatch
 *   'I' kind:1 flags:1 vector:2 scanline:4 pc:3  interrupt
 *   'E' code:4 flags:1 pc24:3 source:3           runtime error
 *   'F' flags:1 frame:8                          frame boundary
 *   'L' length:2 bytes                           label change
 * A block or dispatch whose label differs in content from the last label seen
 * is preceded by an 'L' record, so the digest also records which generated
 * function executed each block. cpu keeps the M, X, E and host-return bits. */

enum {
    kBufferCapacity = 64 * 1024,
    kCpuFlagMask = 0x0f
};

static bool s_initialized;
static FILE *s_file;
static Sha256Context s_context;
static uint8_t s_buffer[kBufferCapacity];
static size_t s_buffered;
static const char *s_label_pointer;
static char *s_label;
static size_t s_label_length;
static size_t s_label_capacity;
static uint64_t s_blocks;
static uint64_t s_dispatches;
static uint64_t s_interrupts;
static uint64_t s_errors;
static uint64_t s_boundaries;
static const SnesRunnerApi *s_runner_api;
static SrRunnerHandle *s_runner;
static uint64_t s_subscription;
static bool s_exit_registered;

static void flush_buffer(void) {
    if (s_buffered == 0u) return;
    sha256_update(&s_context, s_buffer, s_buffered);
    s_buffered = 0u;
}

static void append(const void *data, size_t length) {
    if (length > sizeof(s_buffer) - s_buffered) {
        flush_buffer();
        if (length >= sizeof(s_buffer)) {
            sha256_update(&s_context, (const uint8_t *)data, length);
            return;
        }
    }
    memcpy(s_buffer + s_buffered, data, length);
    s_buffered += length;
}

/* Reserves one fixed-size record in the buffer; length is at most 16. */
static uint8_t *reserve(size_t length) {
    uint8_t *record;
    if (length > sizeof(s_buffer) - s_buffered) flush_buffer();
    record = s_buffer + s_buffered;
    s_buffered += length;
    return record;
}

static void put16(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void put24(uint8_t *out, uint32_t value) {
    put16(out, value);
    out[2] = (uint8_t)(value >> 16);
}

static void put32(uint8_t *out, uint32_t value) {
    put24(out, value);
    out[3] = (uint8_t)(value >> 24);
}

/* Labels are runner-owned and immutable while bound, so an unchanged pointer
 * is an unchanged label; a changed pointer is compared by content, because
 * two builds may place identical names at different addresses. */
static void note_label(const char *label) {
    size_t length;
    uint8_t header[3];
    if (label == NULL || label == s_label_pointer) return;
    s_label_pointer = label;
    length = strlen(label);
    if (length > 0xffffu) length = 0xffffu;
    if (s_label != NULL && length == s_label_length &&
        memcmp(label, s_label, length) == 0)
        return;
    if (length + 1u > s_label_capacity) {
        size_t capacity = s_label_capacity != 0u ? s_label_capacity : 64u;
        char *grown;
        while (capacity < length + 1u) capacity *= 2u;
        grown = (char *)realloc(s_label, capacity);
        if (grown == NULL) {
            /* Keep hashing: the label is recorded, only the change check
             * against it is lost, which costs a repeated record at most. */
            s_label_length = 0u;
            header[0] = 'L';
            put16(header + 1, (uint32_t)length);
            append(header, sizeof(header));
            append(label, length);
            return;
        }
        s_label = grown;
        s_label_capacity = capacity;
    }
    memcpy(s_label, label, length);
    s_label[length] = '\0';
    s_label_length = length;
    header[0] = 'L';
    put16(header + 1, (uint32_t)length);
    append(header, sizeof(header));
    append(label, length);
}

static void write_checkpoint(const char *kind) {
    static const char hex[] = "0123456789abcdef";
    Sha256Context snapshot;
    uint8_t digest[32];
    char text[65];
    unsigned index;
    if (s_file == NULL) return;
    flush_buffer();
    snapshot = s_context;
    sha256_final(&snapshot, digest);
    for (index = 0u; index < 32u; ++index) {
        text[index * 2u] = hex[digest[index] >> 4];
        text[index * 2u + 1u] = hex[digest[index] & 15u];
    }
    text[64] = '\0';
    fprintf(s_file,
            "%s blocks=%llu dispatches=%llu interrupts=%llu errors=%llu "
            "boundaries=%llu sha256=%s\n",
            kind, (unsigned long long)s_blocks,
            (unsigned long long)s_dispatches,
            (unsigned long long)s_interrupts, (unsigned long long)s_errors,
            (unsigned long long)s_boundaries, text);
    fflush(s_file);
}

static void observe_event(void *user_data, SrRunnerHandle *runner,
                          const SrRunnerEvent *event) {
    uint8_t *out;
    (void)user_data;
    (void)runner;
    if (s_file == NULL || event == NULL) return;
    switch (event->type) {
    case SR_EVENT_EXECUTION_BLOCK:
        note_label(event->label);
        out = reserve(9u);
        out[0] = 'B';
        put24(out + 1, event->pc24);
        out[4] = (uint8_t)(event->cpu_flags & kCpuFlagMask);
        put16(out + 5, event->register_x);
        put16(out + 7, event->stack_pointer);
        ++s_blocks;
        break;
    case SR_EVENT_DYNAMIC_DISPATCH:
        note_label(event->label);
        out = reserve(13u);
        out[0] = 'D';
        put24(out + 1, event->pc24);
        put24(out + 4, event->source_pc24);
        out[7] = (uint8_t)event->flags;
        out[8] = (uint8_t)(event->cpu_flags & kCpuFlagMask);
        put16(out + 9, event->register_x);
        put16(out + 11, event->stack_pointer);
        ++s_dispatches;
        break;
    case SR_EVENT_INTERRUPT:
        out = reserve(12u);
        out[0] = 'I';
        out[1] = (uint8_t)event->interrupt_kind;
        out[2] = (uint8_t)event->flags;
        put16(out + 3, event->interrupt_vector);
        put32(out + 5, (uint32_t)event->interrupt_scanline);
        put24(out + 9, event->pc24);
        ++s_interrupts;
        break;
    case SR_EVENT_ERROR:
        out = reserve(12u);
        out[0] = 'E';
        put32(out + 1, event->error_code);
        out[5] = (uint8_t)event->flags;
        put24(out + 6, event->pc24);
        put24(out + 9, event->source_pc24);
        ++s_errors;
        break;
    case SR_EVENT_FRAME_BOUNDARY: {
        char kind[40];
        out = reserve(10u);
        out[0] = 'F';
        out[1] = (uint8_t)event->flags;
        put32(out + 2, (uint32_t)event->frame_counter);
        put32(out + 6, (uint32_t)(event->frame_counter >> 32));
        ++s_boundaries;
        if ((event->flags & (SR_EVENT_FRAME_END | SR_EVENT_FRAME_HOST_TICK)) ==
            (SR_EVENT_FRAME_END | SR_EVENT_FRAME_HOST_TICK)) {
            snprintf(kind, sizeof(kind), "frame=%llu",
                     (unsigned long long)event->frame_counter);
            write_checkpoint(kind);
        }
        break;
    }
    default:
        break;
    }
}

static void unsubscribe(void) {
    if (s_runner_api != NULL && s_runner != NULL && s_subscription != 0u)
        (void)s_runner_api->unsubscribe_events(s_runner, s_subscription);
    s_subscription = 0u;
}

static void subscribe(void) {
    SrEventSubscription subscription = {0};
    if (s_file == NULL || s_runner_api == NULL || s_runner == NULL) return;
    subscription.struct_size = sizeof(subscription);
    subscription.event_mask =
        SR_EVENT_MASK_EXECUTION_BLOCK | SR_EVENT_MASK_DYNAMIC_DISPATCH |
        SR_EVENT_MASK_INTERRUPT | SR_EVENT_MASK_ERROR | SR_EVENT_MASK_FRAME;
    subscription.callback = observe_event;
    if (s_runner_api->subscribe_events(s_runner, &subscription,
                                       &s_subscription) != SR_RESULT_OK) {
        s_subscription = 0u;
        /* The file then records no events at all; say so rather than let
         * an empty digest pass for agreement. */
        fprintf(s_file, "error subscribe-failed\n");
        fflush(s_file);
        fprintf(stderr, "[edge-digest] runner event subscription failed\n");
    }
}

static void close_file(void) {
    if (s_file == NULL) return;
    write_checkpoint("final");
    fclose(s_file);
    s_file = NULL;
}

static void close_at_exit(void) {
    /* The runner may already be gone at exit; only the file is finished. */
    close_file();
}

static int open_file(const char *path) {
    s_file = sr_fopen(path, "w");
    if (s_file == NULL) {
        fprintf(stderr, "[edge-digest] cannot open %s\n", path);
        return 0;
    }
    sha256_init(&s_context);
    s_buffered = 0u;
    s_label_pointer = NULL;
    s_label_length = 0u;
    if (s_label != NULL) s_label[0] = '\0';
    s_blocks = s_dispatches = s_interrupts = s_errors = s_boundaries = 0u;
    fputs("snesrecomp-edge-digest v1\n", s_file);
    fflush(s_file);
    if (!s_exit_registered) s_exit_registered = atexit(close_at_exit) == 0;
    return 1;
}

int sr_edge_digest_open(const char *path) {
    int opened;
    sr_edge_digest_close();
    s_initialized = true;
    if (path == NULL || path[0] == '\0') return 0;
    opened = open_file(path);
    subscribe();
    return opened;
}

void sr_edge_digest_close(void) {
    unsubscribe();
    close_file();
}

void sr_edge_digest_bind_runner(Snes *runner, int enabled) {
    const SnesRunnerApi *api;
    unsubscribe();
    if (s_runner != NULL && s_file != NULL) write_checkpoint("unbind");
    s_runner_api = NULL;
    s_runner = NULL;
    if (!enabled || runner == NULL) return;
    if (!s_initialized) {
        const char *path = getenv("SNESRECOMP_EDGE_DIGEST");
        s_initialized = true;
        if (path != NULL && path[0] != '\0') (void)open_file(path);
    }
    api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    if (api == NULL ||
        api->struct_size < SNES_RUNNER_API_EVENT_OBSERVER_SIZE ||
        (api->capabilities & SR_RUNNER_CAP_EVENT_OBSERVERS) == 0u)
        return;
    /* Keep the association with no file open, so an embedder can open the
     * digest after runner initialization. */
    s_runner_api = api;
    s_runner = (SrRunnerHandle *)(void *)runner;
    subscribe();
}
