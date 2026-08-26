#include "ar_trace.h"

#include "common_cpu_infra.h"
#include "common_rtl.h"
#include "cpu_state.h"
#include "runner_next.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    kBlockRingMask = 1023,
    kGameFrameOffset = 0x88,
    kDefaultChannels = 0x3fff & ~(AR_TR_WRAM | AR_TR_STACK),
    kLineCapacity = 768,
    kDefaultRingLines = 4096,
    kMaximumDedupeKeys = 512
};

static bool s_initialized;
static FILE *s_file;
static long s_frame_low = -1;
static long s_frame_high = -1;
static int s_channels = kDefaultChannels;
static unsigned s_vram_low;
static unsigned s_vram_high = 0x7fffu;
static unsigned s_wram_low;
static unsigned s_wram_high = 0x1ffffu;
static char s_function_filter[64];
static uint64_t s_sequence;

static bool s_watch;
static char s_watch_prefix[256];
static char (*s_ring)[kLineCapacity];
static unsigned s_ring_capacity;
static unsigned s_ring_head;
static unsigned s_ring_count;
static FILE *s_capture;
static int s_post_remaining;
static int s_post_lines = 400;
static unsigned s_capture_number;
static const char *s_pending_anomaly;
static uint32_t s_dedupe_keys[kMaximumDedupeKeys];
static unsigned s_dedupe_count;
static const SnesRunnerApi *s_runner_api;
static SrRunnerHandle *s_runner;
static uint64_t s_memory_subscription;
static uint64_t s_register_subscription;
static uint64_t s_dma_subscription;
static uint64_t s_lifecycle_subscription;

static void unsubscribe_runner(void) {
    if (s_runner_api != NULL && s_runner != NULL) {
        if (s_memory_subscription != 0u)
            (void)s_runner_api->unsubscribe_events(
                s_runner, s_memory_subscription);
        if (s_register_subscription != 0u)
            (void)s_runner_api->unsubscribe_events(
                s_runner, s_register_subscription);
        if (s_dma_subscription != 0u)
            (void)s_runner_api->unsubscribe_events(
                s_runner, s_dma_subscription);
        if (s_lifecycle_subscription != 0u)
            (void)s_runner_api->unsubscribe_events(
                s_runner, s_lifecycle_subscription);
    }
    s_memory_subscription = 0u;
    s_register_subscription = 0u;
    s_dma_subscription = 0u;
    s_lifecycle_subscription = 0u;
    s_runner_api = NULL;
    s_runner = NULL;
}

static void escape_json(const char *source, char *destination,
                        size_t capacity) {
    if (capacity == 0u) return;
    size_t output = 0u;
    if (source == NULL) source = "?";
    while (*source != '\0' && output + 1u < capacity) {
        const unsigned char value = (unsigned char)*source++;
        if ((value == '\\' || value == '"') && output + 2u < capacity) {
            destination[output++] = '\\';
            destination[output++] = (char)value;
        } else if (value >= 0x20u) {
            destination[output++] = (char)value;
        }
    }
    destination[output] = '\0';
}

void ar_trace_close(void) {
    unsubscribe_runner();
    if (s_file != NULL) fclose(s_file);
    if (s_capture != NULL) fclose(s_capture);
    free(s_ring);
    s_file = NULL;
    s_capture = NULL;
    s_ring = NULL;
    s_initialized = false;
    s_watch = false;
    s_ring_capacity = s_ring_head = s_ring_count = 0u;
    s_post_remaining = 0;
    s_sequence = 0u;
    s_dedupe_count = 0u;
    s_pending_anomaly = NULL;
}

int ar_trace_open_file(const char *path, int channel_mask,
                       long host_frame_low, long host_frame_high) {
    Snes *runner = (Snes *)(void *)s_runner;
    ar_trace_close();
    s_initialized = true;
    s_channels = channel_mask;
    s_frame_low = host_frame_low;
    s_frame_high = host_frame_high;
    if (path == NULL || path[0] == '\0') return 0;
    s_file = fopen(path, "w");
    if (runner != NULL) ar_trace_bind_runner(runner, 1);
    return s_file != NULL;
}

static void add_channel_name(const char *name) {
    struct Entry { const char *name; int bit; };
    static const struct Entry entries[] = {
        {"func", AR_TR_FUNC}, {"vram", AR_TR_VRAM},
        {"vmadd", AR_TR_VMADD}, {"reg", AR_TR_REG},
        {"dma", AR_TR_DMA}, {"mx", AR_TR_MX}, {"call", AR_TR_CALL},
        {"dispmiss", AR_TR_DISPMISS}, {"garbage", AR_TR_GARBAGE},
        {"wram", AR_TR_WRAM}, {"stack", AR_TR_STACK},
        {"hwread", AR_TR_HWREAD}, {"ppumem", AR_TR_PPUMEM},
        {"frame", AR_TR_FRAME}
    };
    for (unsigned index = 0; index < sizeof(entries) / sizeof(entries[0]);
         ++index) {
        if (strcmp(name, entries[index].name) == 0) {
            s_channels |= entries[index].bit;
            return;
        }
    }
}

static void parse_channels(const char *text) {
    if (text == NULL || text[0] == '\0') {
        s_channels = kDefaultChannels;
        return;
    }
    s_channels = 0;
    char token[32];
    size_t length = 0u;
    for (;;) {
        const char value = *text++;
        if (value == ',' || value == '\0') {
            token[length] = '\0';
            add_channel_name(token);
            length = 0u;
            if (value == '\0') break;
        } else if (value != ' ' && value != '\t' &&
                   length + 1u < sizeof(token)) {
            token[length++] = value;
        }
    }
}

static unsigned env_unsigned(const char *name, unsigned fallback) {
    const char *value = getenv(name);
    return value == NULL ? fallback : (unsigned)strtoul(value, NULL, 0);
}

static void initialize_from_environment(void) {
    s_initialized = true;
    s_channels = kDefaultChannels;
    const char *path = getenv("AR_TRACE");
    const char *watch = getenv("AR_TRACE_WATCH");
    if (path != NULL && path[0] != '\0') watch = NULL;
    parse_channels(getenv("AR_TRACE_CH"));
    s_vram_low = env_unsigned("AR_TRACE_VLO", 0u);
    s_vram_high = env_unsigned("AR_TRACE_VHI", 0x7fffu);
    s_wram_low = env_unsigned("AR_TRACE_WLO", 0u);
    s_wram_high = env_unsigned("AR_TRACE_WHI", 0x1ffffu);
    const char *filter = getenv("AR_TRACE_FUNC");
    if (filter != NULL) {
        snprintf(s_function_filter, sizeof(s_function_filter), "%s", filter);
    }
    const char *low = getenv("AR_TRACE_HF_LO");
    const char *high = getenv("AR_TRACE_HF_HI");
    s_frame_low = low == NULL ? -1 : strtol(low, NULL, 0);
    s_frame_high = high == NULL ? -1 : strtol(high, NULL, 0);
    if (path != NULL && path[0] != '\0') {
        s_file = fopen(path, "w");
        if (s_file == NULL) fprintf(stderr, "[ar_trace] cannot open %s\n", path);
        return;
    }
    if (watch == NULL || watch[0] == '\0') return;
    snprintf(s_watch_prefix, sizeof(s_watch_prefix), "%s", watch);
    s_ring_capacity = env_unsigned("AR_TRACE_RING", kDefaultRingLines);
    if (s_ring_capacity < 16u) s_ring_capacity = 16u;
    s_post_lines = (int)env_unsigned("AR_TRACE_POST", 400u);
    s_ring = calloc(s_ring_capacity, sizeof(*s_ring));
    if (s_ring == NULL) {
        fprintf(stderr, "[ar_trace] watch ring allocation failed\n");
        return;
    }
    s_watch = true;
}

int ar_trace_active(void) {
    if (!s_initialized) initialize_from_environment();
    if (s_file == NULL && !s_watch) return 0;
    if (s_watch) return 1;
    if (s_frame_low >= 0 && snes_frame_counter < s_frame_low) return 0;
    if (s_frame_high >= 0 && snes_frame_counter > s_frame_high) return 0;
    return 1;
}

int ar_trace_ch(int channel_bit) {
    if (!s_initialized) initialize_from_environment();
    return (s_channels & channel_bit) != 0;
}

static bool remember_key(uint32_t key) {
    for (unsigned index = 0; index < s_dedupe_count; ++index) {
        if (s_dedupe_keys[index] == key) return false;
    }
    if (s_dedupe_count < kMaximumDedupeKeys) {
        s_dedupe_keys[s_dedupe_count++] = key;
    }
    return true;
}

static void safe_component(const char *source, char *output, size_t capacity) {
    size_t index = 0u;
    if (source == NULL) source = "trace";
    while (*source != '\0' && index + 1u < capacity) {
        const char value = *source++;
        output[index++] = (value >= 'a' && value <= 'z') ||
                          (value >= 'A' && value <= 'Z') ||
                          (value >= '0' && value <= '9') || value == '-'
                              ? value : '_';
    }
    output[index] = '\0';
}

static void begin_watch_capture(void) {
    if (!s_watch || s_pending_anomaly == NULL || s_capture != NULL) return;
    char kind[48];
    safe_component(s_pending_anomaly, kind, sizeof(kind));
    char path[360];
    if (snprintf(path, sizeof(path), "%s_hf%d_%s%u.jsonl",
                 s_watch_prefix, snes_frame_counter, kind,
                 s_capture_number++) >= (int)sizeof(path)) {
        return;
    }
    s_capture = fopen(path, "w");
    if (s_capture == NULL) return;
    const unsigned oldest =
        (s_ring_head + s_ring_capacity - s_ring_count) % s_ring_capacity;
    for (unsigned index = 0; index < s_ring_count; ++index) {
        fputs(s_ring[(oldest + index) % s_ring_capacity], s_capture);
    }
    s_post_remaining = s_post_lines;
}

static void output_line(const char *line) {
    if (s_file != NULL) fputs(line, s_file);
    if (!s_watch) return;
    if (s_capture != NULL && s_pending_anomaly == NULL) {
        fputs(line, s_capture);
        if (--s_post_remaining <= 0) {
            fclose(s_capture);
            s_capture = NULL;
        }
    }
    snprintf(s_ring[s_ring_head], kLineCapacity, "%s", line);
    s_ring_head = (s_ring_head + 1u) % s_ring_capacity;
    if (s_ring_count < s_ring_capacity) ++s_ring_count;
    begin_watch_capture();
    s_pending_anomaly = NULL;
}

static void write_event(const char *channel, const char *format, ...) {
    char escaped_function[160];
    escape_json(g_last_recomp_func, escaped_function, sizeof(escaped_function));
    const unsigned game_frame = g_ram[kGameFrameOffset] |
                                (unsigned)g_ram[kGameFrameOffset + 1] << 8;
    const uint32_t block =
        g_ar_blk_ring[(g_ar_blk_idx - 1u) & kBlockRingMask];
    char line[kLineCapacity];
    int length = snprintf(
        line, sizeof(line),
        "{\"seq\":%llu,\"hf\":%d,\"gf\":%u,\"ch\":\"%s\","
        "\"blk\":\"%06X\",\"mnow\":%u,\"xnow\":%u,"
        "\"S\":\"%04X\",\"DB\":\"%02X\",\"PB\":\"%02X\","
        "\"fn\":\"%s\"",
        (unsigned long long)s_sequence++, snes_frame_counter, game_frame,
        channel, block, g_cpu.m_flag & 1u, g_cpu.x_flag & 1u, g_cpu.S,
        g_cpu.DB, g_cpu.PB, escaped_function);
    if (length < 0) return;
    size_t offset = (size_t)length;
    if (offset >= sizeof(line)) offset = sizeof(line) - 1u;
    va_list arguments;
    va_start(arguments, format);
    (void)vsnprintf(line + offset, sizeof(line) - offset, format, arguments);
    va_end(arguments);
    output_line(line);
}

static bool function_matches(const char *name) {
    return s_function_filter[0] == '\0' ||
           (name != NULL && strstr(name, s_function_filter) != NULL);
}

void ar_trace_func(uint32_t pc24, const char *name, int m, int x,
                   int expected_m, int expected_x) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_FUNC) ||
        !function_matches(name)) return;
    const int mismatch = (m & 1) != (expected_m & 1) ||
                         (x & 1) != (expected_x & 1);
    write_event("func", ",\"pc\":\"%06X\",\"m\":%d,\"x\":%d,"
                "\"em\":%d,\"ex\":%d,\"misdecode\":%d}\n",
                pc24, m & 1, x & 1, expected_m & 1, expected_x & 1, mismatch);
}

void ar_trace_call(uint32_t pc24, const char *name, int m, int x,
                   int expected_m, int expected_x) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_CALL) ||
        !function_matches(name)) return;
    const int mismatch = (m & 1) != (expected_m & 1) ||
                         (x & 1) != (expected_x & 1);
    if (s_watch && mismatch && (name == NULL || strstr(name, "Handler") == NULL) &&
        remember_key(0x1ea00000u ^ pc24)) s_pending_anomaly = "leak";
    write_event("call", ",\"site\":\"%06X\",\"m\":%d,\"x\":%d,"
                "\"em\":%d,\"ex\":%d,\"leak\":%d}\n",
                pc24, m & 1, x & 1, expected_m & 1, expected_x & 1, mismatch);
}

void ar_trace_vram(uint16_t address, uint16_t value, const char *path) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_VRAM)) return;
    address &= 0x7fffu;
    if (address < s_vram_low || address > s_vram_high) return;
    char escaped[80]; escape_json(path, escaped, sizeof(escaped));
    write_event("vram", ",\"va\":\"%04X\",\"val\":\"%04X\","
                "\"path\":\"%s\"}\n", address, value, escaped);
}

void ar_trace_vmadd(uint16_t address, const char *source) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_VMADD)) return;
    char escaped[80]; escape_json(source, escaped, sizeof(escaped));
    write_event("vmadd", ",\"vmadd\":\"%04X\",\"how\":\"%s\"}\n",
                address, escaped);
}

void ar_trace_reg(uint16_t address, uint8_t value) {
    if (ar_trace_active() && ar_trace_ch(AR_TR_REG))
        write_event("reg", ",\"reg\":\"%04X\",\"val\":\"%02X\"}\n",
                    address, value);
}

void ar_trace_dma(int channel, uint8_t b_address, uint8_t a_bank,
                  uint16_t a_address, uint32_t size, int from_b_bus) {
    if (ar_trace_active() && ar_trace_ch(AR_TR_DMA))
        write_event("dma", ",\"dch\":%d,\"bAdr\":\"%02X\","
                    "\"src\":\"%02X:%04X\",\"size\":%u,\"fromB\":%d,"
                    "\"hdma\":0}\n",
                    channel, b_address, a_bank, a_address, size,
                    from_b_bus != 0);
}

static void ar_trace_dma_event(const SrRunnerEvent *event) {
    if (ar_trace_active() && ar_trace_ch(AR_TR_DMA))
        write_event("dma", ",\"dch\":%u,\"bAdr\":\"%02X\","
                    "\"src\":\"%02X:%04X\",\"size\":%u,\"fromB\":%d,"
                    "\"hdma\":%d}\n",
                    event->dma_channel, event->dma_b_address,
                    (uint8_t)(event->dma_a_address24 >> 16),
                    (uint16_t)event->dma_a_address24,
                    event->dma_transfer_bytes,
                    (event->flags & SR_EVENT_DMA_FROM_B_BUS) != 0u,
                    (event->flags & SR_EVENT_DMA_HDMA) != 0u);
}

void ar_trace_dispmiss(uint32_t from_pc, uint32_t to_pc) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_DISPMISS)) return;
    if (s_watch && g_cpu.S < 0x0200u &&
        remember_key(0xd1500000u ^ to_pc)) s_pending_anomaly = "dispmiss";
    write_event("dispmiss", ",\"from\":\"%06X\",\"to\":\"%06X\"}\n",
                from_pc, to_pc);
}

void ar_trace_garbage(uint32_t pc24, const char *name, int m, int x) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_GARBAGE)) return;
    if (s_watch && remember_key(0x6a160000u ^ pc24))
        s_pending_anomaly = "garbage";
    char escaped[160]; escape_json(name, escaped, sizeof(escaped));
    write_event("garbage", ",\"pc\":\"%06X\",\"m\":%d,\"x\":%d,"
                "\"variant\":\"%s\"}\n", pc24, m & 1, x & 1, escaped);
}

void ar_trace_wram(uint32_t offset, uint16_t old_value, uint16_t value,
                   int width) {
    if (!ar_trace_active()) return;
    if (ar_trace_ch(AR_TR_STACK) && offset >= 0x100u && offset < 0x200u) {
        write_event("stack", ",\"off\":\"%05X\",\"old\":\"%04X\","
                    "\"val\":\"%04X\",\"w\":%d}\n",
                    offset, old_value, value, width);
    }
    if (ar_trace_ch(AR_TR_WRAM) && offset >= s_wram_low &&
        offset <= s_wram_high) {
        write_event("wram", ",\"off\":\"%05X\",\"old\":\"%04X\","
                    "\"val\":\"%04X\",\"w\":%d}\n",
                    offset, old_value, value, width);
    }
}

void ar_trace_hwread(uint16_t address, uint8_t value) {
    if (ar_trace_active() && ar_trace_ch(AR_TR_HWREAD))
        write_event("hwread", ",\"reg\":\"%04X\",\"val\":\"%02X\"}\n",
                    address, value);
}

void ar_trace_ppumem(const char *memory, uint16_t address, uint16_t value) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_PPUMEM)) return;
    char escaped[80]; escape_json(memory, escaped, sizeof(escaped));
    write_event("ppumem", ",\"mem\":\"%s\",\"addr\":\"%04X\","
                "\"val\":\"%04X\"}\n", escaped, address, value);
}

void ar_trace_frame(const char *event) {
    if (!ar_trace_active() || !ar_trace_ch(AR_TR_FRAME)) return;
    char escaped[80]; escape_json(event, escaped, sizeof(escaped));
    write_event("frame", ",\"what\":\"%s\"}\n", escaped);
}

static void observe_runner_event(void *user_data, SrRunnerHandle *runner,
                                 const SrRunnerEvent *event) {
    (void)user_data;
    (void)runner;
    if (event == NULL) return;
    if (event->type == SR_EVENT_MEMORY_WRITE) {
        if (event->memory_region == SR_MEMORY_WRAM) {
            ar_trace_wram(event->address, (uint16_t)event->previous_value,
                          (uint16_t)event->value,
                          (int)event->width_bytes);
        } else if (ar_trace_active() && ar_trace_ch(AR_TR_PPUMEM)) {
            const char *name = NULL;
            switch (event->memory_region) {
                case SR_MEMORY_SRAM: name = "sram"; break;
                case SR_MEMORY_VRAM: name = "vram"; break;
                case SR_MEMORY_CGRAM: name = "cgram"; break;
                case SR_MEMORY_OAM: name = "oam"; break;
                case SR_MEMORY_HIGH_OAM: name = "high_oam"; break;
                default: break;
            }
            if (name != NULL)
                ar_trace_ppumem(name, (uint16_t)event->address,
                                (uint16_t)event->value);
        }
    } else if (event->type == SR_EVENT_REGISTER_WRITE) {
        ar_trace_reg((uint16_t)event->address, (uint8_t)event->value);
    } else if (event->type == SR_EVENT_REGISTER_READ) {
        ar_trace_hwread((uint16_t)event->address, (uint8_t)event->value);
    } else if (event->type == SR_EVENT_DMA_BEGIN) {
        ar_trace_dma_event(event);
    } else if (event->type == SR_EVENT_FRAME_BOUNDARY &&
               (event->flags & SR_EVENT_FRAME_BEGIN) != 0u) {
        ar_trace_frame(event->label);
    } else if (event->type == SR_EVENT_INTERRUPT &&
               event->interrupt_kind == SR_INTERRUPT_NMI &&
               (event->flags & SR_EVENT_INTERRUPT_ENTER) != 0u) {
        ar_trace_frame(event->label);
    } else if (event->type == SR_EVENT_ERROR &&
               event->error_code == SR_RUNNER_ERROR_DISPATCH_MISS) {
        ar_trace_dispmiss(event->source_pc24, event->pc24);
    }
}

void ar_trace_bind_runner(Snes *runner, int enabled) {
    SrEventSubscription subscription = {0};
    const SnesRunnerApi *api;
    unsubscribe_runner();
    if (!enabled || runner == NULL) return;
    if (!s_initialized) initialize_from_environment();
    api = sr_runner_get_api(SR_RUNNER_ABI_VERSION);
    if (api == NULL ||
        api->struct_size < SNES_RUNNER_API_EVENT_OBSERVER_SIZE ||
        (api->capabilities & SR_RUNNER_CAP_EVENT_OBSERVERS) == 0u)
        return;
    s_runner_api = api;
    s_runner = (SrRunnerHandle *)(void *)runner;
    /* Retain the association even with no active sink so an embedder can
     * enable deterministic file tracing after runner initialization. */
    if (s_file == NULL && !s_watch) return;
    subscription.struct_size = sizeof(subscription);
    subscription.callback = observe_runner_event;
    if ((s_channels & (AR_TR_WRAM | AR_TR_STACK | AR_TR_PPUMEM)) != 0) {
        subscription.event_mask = SR_EVENT_MASK_MEMORY_WRITE;
        if (api->subscribe_events(s_runner, &subscription,
                                  &s_memory_subscription) != SR_RESULT_OK)
            s_memory_subscription = 0u;
    }
    if ((s_channels & (AR_TR_REG | AR_TR_HWREAD)) != 0) {
        subscription.event_mask = SR_EVENT_MASK_REGISTER_ACCESS;
        if (api->subscribe_events(s_runner, &subscription,
                                  &s_register_subscription) != SR_RESULT_OK)
            s_register_subscription = 0u;
    }
    if ((s_channels & AR_TR_DMA) != 0) {
        subscription.event_mask = SR_EVENT_MASK_DMA;
        if (api->subscribe_events(s_runner, &subscription,
                                  &s_dma_subscription) != SR_RESULT_OK)
            s_dma_subscription = 0u;
    }
    if ((s_channels & (AR_TR_FRAME | AR_TR_DISPMISS)) != 0) {
        subscription.event_mask = 0u;
        if ((s_channels & AR_TR_FRAME) != 0)
            subscription.event_mask |=
                SR_EVENT_MASK_FRAME | SR_EVENT_MASK_INTERRUPT;
        if ((s_channels & AR_TR_DISPMISS) != 0)
            subscription.event_mask |= SR_EVENT_MASK_ERROR;
        if (api->subscribe_events(s_runner, &subscription,
                                  &s_lifecycle_subscription) != SR_RESULT_OK)
            s_lifecycle_subscription = 0u;
    }
}

void ar_trace_flush(const char *reason) {
    if (s_file != NULL) fflush(s_file);
    if (!s_watch || s_ring == NULL || s_ring_count == 0u) return;
    char safe_reason[48]; safe_component(reason, safe_reason, sizeof(safe_reason));
    char path[360];
    if (snprintf(path, sizeof(path), "%s_hf%d_%s%u.jsonl", s_watch_prefix,
                 snes_frame_counter, safe_reason, s_capture_number++) >=
        (int)sizeof(path)) return;
    FILE *file = fopen(path, "w");
    if (file == NULL) return;
    const unsigned oldest =
        (s_ring_head + s_ring_capacity - s_ring_count) % s_ring_capacity;
    for (unsigned index = 0; index < s_ring_count; ++index) {
        fputs(s_ring[(oldest + index) % s_ring_capacity], file);
    }
    fclose(file);
}
