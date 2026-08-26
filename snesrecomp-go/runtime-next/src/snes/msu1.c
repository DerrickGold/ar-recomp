#include "msu1.h"

#include "../apu_sync.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#endif

enum {
    kPathCapacity = 1024,
    kStatusAudioRepeat = 0x20,
    kStatusAudioPlaying = 0x10,
    kStatusAudioError = 0x08,
    kRevision = 1
};

typedef struct Msu1State {
    bool armed;
    bool auto_base;
    bool have_base;
    char base[kPathCapacity];
    FILE *data_file;
    bool data_open_attempted;
    uint8_t seek_bytes[4];
    FILE *track_file;
    uint16_t selected_track;
    uint8_t selected_track_low;
    uint32_t loop_frame;
    long audio_data_offset;
    uint8_t volume;
    bool playing;
    bool repeating;
    bool audio_error;
    double source_phase;
    int16_t current[2];
    int16_t next[2];
    bool have_current;
    bool have_next;
} Msu1State;

static Msu1State s_msu;
static const uint8_t kIdentity[6] = {'S', '-', 'M', 'S', 'U', '1'};

static bool text_equals_ignore_case(const char *left, const char *right) {
    while (*left != '\0' && *right != '\0') {
        if (tolower((unsigned char)*left) != tolower((unsigned char)*right)) {
            return false;
        }
        ++left;
        ++right;
    }
    return *left == *right;
}

static void close_files(void) {
    if (s_msu.data_file != NULL) fclose(s_msu.data_file);
    if (s_msu.track_file != NULL) fclose(s_msu.track_file);
    s_msu.data_file = NULL;
    s_msu.track_file = NULL;
}

void msu1_shutdown(void) {
    close_files();
    memset(&s_msu, 0, sizeof(s_msu));
}

static bool track_filename_base(const char *name, char *base,
                                size_t base_capacity) {
    const size_t length = strlen(name);
    if (length < 7u || name[length - 4u] != '.' ||
        tolower((unsigned char)name[length - 3u]) != 'p' ||
        tolower((unsigned char)name[length - 2u]) != 'c' ||
        tolower((unsigned char)name[length - 1u]) != 'm') {
        return false;
    }
    size_t cursor = length - 4u;
    if (cursor == 0u || !isdigit((unsigned char)name[cursor - 1u])) return false;
    while (cursor > 0u && isdigit((unsigned char)name[cursor - 1u])) --cursor;
    if (cursor < 2u || name[cursor - 1u] != '-') return false;
    const size_t base_length = cursor - 1u;
    if (base_length >= base_capacity) return false;
    memcpy(base, name, base_length);
    base[base_length] = '\0';
    return true;
}

static bool path_is_directory(const char *path) {
    struct stat information;
    if (stat(path, &information) != 0) return false;
#ifdef _WIN32
    return (information.st_mode & _S_IFMT) == _S_IFDIR;
#else
    return S_ISDIR(information.st_mode);
#endif
}

typedef struct BaseCandidate {
    char name[kPathCapacity];
    unsigned tracks;
} BaseCandidate;

static void count_candidate(BaseCandidate *candidates, unsigned *count,
                            const char *filename) {
    char base[kPathCapacity];
    if (!track_filename_base(filename, base, sizeof(base))) return;
    for (unsigned index = 0; index < *count; ++index) {
        if (strcmp(candidates[index].name, base) == 0) {
            ++candidates[index].tracks;
            return;
        }
    }
    if (*count >= 16u) return;
    snprintf(candidates[*count].name, sizeof(candidates[*count].name), "%s",
             base);
    candidates[*count].tracks = 1u;
    ++*count;
}

static void resolve_directory_base(void) {
    if (!path_is_directory(s_msu.base)) return;
    char directory[kPathCapacity];
    snprintf(directory, sizeof(directory), "%s", s_msu.base);
    size_t length = strlen(directory);
    while (length > 0u &&
           (directory[length - 1u] == '/' || directory[length - 1u] == '\\')) {
        directory[--length] = '\0';
    }
    BaseCandidate *candidates =
        (BaseCandidate *)calloc(16u, sizeof(*candidates));
    if (candidates == NULL) return;
    unsigned candidate_count = 0u;
#ifdef _WIN32
    char pattern[kPathCapacity];
    if (snprintf(pattern, sizeof(pattern), "%s\\*", directory) >=
        (int)sizeof(pattern)) {
        free(candidates);
        return;
    }
    WIN32_FIND_DATAA entry;
    HANDLE search = FindFirstFileA(pattern, &entry);
    if (search == INVALID_HANDLE_VALUE) {
        free(candidates);
        return;
    }
    do {
        count_candidate(candidates, &candidate_count, entry.cFileName);
    } while (FindNextFileA(search, &entry));
    FindClose(search);
#else
    DIR *stream = opendir(directory);
    if (stream == NULL) {
        free(candidates);
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(stream)) != NULL) {
        count_candidate(candidates, &candidate_count, entry->d_name);
    }
    closedir(stream);
#endif
    if (candidate_count == 0u) {
        free(candidates);
        return;
    }
    unsigned best = 0u;
    for (unsigned index = 1u; index < candidate_count; ++index) {
        if (candidates[index].tracks > candidates[best].tracks) best = index;
    }
    char resolved[kPathCapacity];
    if (snprintf(resolved, sizeof(resolved), "%s/%s", directory,
                 candidates[best].name) >= (int)sizeof(resolved)) {
        free(candidates);
        return;
    }
    snprintf(s_msu.base, sizeof(s_msu.base), "%s", resolved);
    free(candidates);
}

bool msu1_configure_base(const char *path_prefix) {
    close_files();
    memset(&s_msu, 0, sizeof(s_msu));
    s_msu.volume = 255u;
    if (path_prefix == NULL || path_prefix[0] == '\0') return false;
    if (strlen(path_prefix) >= sizeof(s_msu.base)) return false;
    snprintf(s_msu.base, sizeof(s_msu.base), "%s", path_prefix);
    s_msu.armed = true;
    s_msu.have_base = true;
    resolve_directory_base();
    return true;
}

void msu1_init(void) {
    const char *setting = getenv("SNESRECOMP_MSU1");
    msu1_shutdown();
    s_msu.volume = 255u;
    if (setting == NULL || setting[0] == '\0') return;
    s_msu.armed = true;
    if (text_equals_ignore_case(setting, "1") ||
        text_equals_ignore_case(setting, "on") ||
        text_equals_ignore_case(setting, "auto") ||
        text_equals_ignore_case(setting, "true")) {
        s_msu.auto_base = true;
        return;
    }
    (void)msu1_configure_base(setting);
}

void msu1_set_rom_path(const char *rom_path) {
    if (!s_msu.armed || !s_msu.auto_base || s_msu.have_base ||
        rom_path == NULL || rom_path[0] == '\0' ||
        strlen(rom_path) >= sizeof(s_msu.base)) {
        return;
    }
    snprintf(s_msu.base, sizeof(s_msu.base), "%s", rom_path);
    char *dot = strrchr(s_msu.base, '.');
    char *slash = strrchr(s_msu.base, '/');
    char *backslash = strrchr(s_msu.base, '\\');
    char *separator = slash;
    if (backslash != NULL && (separator == NULL || backslash > separator)) {
        separator = backslash;
    }
    if (dot != NULL && (separator == NULL || dot > separator)) *dot = '\0';
    s_msu.have_base = true;
}

bool msu1_enabled(void) {
    return s_msu.armed && s_msu.have_base;
}

static void open_data_file(void) {
    if (s_msu.data_open_attempted) return;
    s_msu.data_open_attempted = true;
    char path[kPathCapacity + 8];
    if (snprintf(path, sizeof(path), "%s.msu", s_msu.base) >=
        (int)sizeof(path)) return;
    s_msu.data_file = fopen(path, "rb");
}

static uint32_t little_u32(const uint8_t bytes[4]) {
    return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
           (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void load_track(uint16_t track) {
    if (s_msu.track_file != NULL) fclose(s_msu.track_file);
    s_msu.track_file = NULL;
    s_msu.selected_track = track;
    s_msu.playing = false;
    s_msu.audio_error = false;
    s_msu.source_phase = 0.0;
    s_msu.have_current = false;
    s_msu.have_next = false;
    char path[kPathCapacity + 32];
    if (snprintf(path, sizeof(path), "%s-%u.pcm", s_msu.base,
                 (unsigned)track) >= (int)sizeof(path)) {
        s_msu.audio_error = true;
        return;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        s_msu.audio_error = true;
        return;
    }
    uint8_t header[8];
    const size_t read = fread(header, 1u, sizeof(header), file);
    if (read == sizeof(header) && memcmp(header, "MSU1", 4u) == 0) {
        s_msu.loop_frame = little_u32(header + 4u);
        s_msu.audio_data_offset = 8;
    } else {
        s_msu.loop_frame = 0u;
        s_msu.audio_data_offset = 0;
        (void)fseek(file, 0, SEEK_SET);
    }
    s_msu.track_file = file;
}

uint8_t msu1_read(uint16_t register_address) {
    if (!msu1_enabled()) return 0u;
    RtlApuLock();
    uint8_t result = 0u;
    const unsigned reg = register_address & 7u;
    if (reg == 0u) {
        result = (uint8_t)(kRevision |
            (s_msu.repeating ? kStatusAudioRepeat : 0) |
            (s_msu.playing ? kStatusAudioPlaying : 0) |
            (s_msu.audio_error ? kStatusAudioError : 0));
    } else if (reg == 1u) {
        open_data_file();
        if (s_msu.data_file != NULL) {
            const int byte = fgetc(s_msu.data_file);
            if (byte != EOF) result = (uint8_t)byte;
        }
    } else {
        result = kIdentity[reg - 2u];
    }
    RtlApuUnlock();
    return result;
}

void msu1_write(uint16_t register_address, uint8_t value) {
    if (!msu1_enabled()) return;
    RtlApuLock();
    const unsigned reg = register_address & 7u;
    switch (reg) {
        case 0: case 1: case 2:
            s_msu.seek_bytes[reg] = value;
            break;
        case 3:
            s_msu.seek_bytes[3] = value;
            open_data_file();
            if (s_msu.data_file != NULL) {
                (void)fseek(s_msu.data_file,
                            (long)little_u32(s_msu.seek_bytes), SEEK_SET);
            }
            break;
        case 4: s_msu.selected_track_low = value; break;
        case 5:
            load_track((uint16_t)(s_msu.selected_track_low |
                                  (uint16_t)value << 8));
            break;
        case 6: s_msu.volume = value; break;
        case 7:
            s_msu.repeating = (value & 2u) != 0u;
            s_msu.playing = (value & 1u) != 0u &&
                            s_msu.track_file != NULL && !s_msu.audio_error;
            break;
        default: break;
    }
    RtlApuUnlock();
}

static bool read_track_frame(int16_t output[2]) {
    uint8_t bytes[4];
    if (s_msu.track_file == NULL) return false;
    if (fread(bytes, 1u, sizeof(bytes), s_msu.track_file) != sizeof(bytes)) {
        if (!s_msu.repeating ||
            fseek(s_msu.track_file,
                  s_msu.audio_data_offset + (long)s_msu.loop_frame * 4L,
                  SEEK_SET) != 0 ||
            fread(bytes, 1u, sizeof(bytes), s_msu.track_file) != sizeof(bytes)) {
            return false;
        }
    }
    output[0] = (int16_t)(uint16_t)(bytes[0] | (uint16_t)bytes[1] << 8);
    output[1] = (int16_t)(uint16_t)(bytes[2] | (uint16_t)bytes[3] << 8);
    return true;
}

static bool prime_audio(void) {
    if (!s_msu.have_current) {
        if (!read_track_frame(s_msu.current)) return false;
        s_msu.have_current = true;
    }
    if (!s_msu.have_next) {
        s_msu.have_next = read_track_frame(s_msu.next);
    }
    return true;
}

static int16_t clamp_sample(int value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return (int16_t)value;
}

void msu1_mix(int16_t *output, int output_frames, int output_rate) {
    if (!msu1_enabled() || !s_msu.playing || output == NULL ||
        output_frames <= 0 || output_rate <= 0 || !prime_audio()) {
        return;
    }
    const double step = 44100.0 / output_rate;
    for (int frame = 0; frame < output_frames && s_msu.playing; ++frame) {
        for (int side = 0; side < 2; ++side) {
            const int current = s_msu.current[side];
            const int next = s_msu.have_next ? s_msu.next[side] : current;
            int sample = current +
                (int)((next - current) * s_msu.source_phase);
            sample = sample * s_msu.volume / 255;
            output[frame * 2 + side] =
                clamp_sample(output[frame * 2 + side] + sample);
        }
        s_msu.source_phase += step;
        while (s_msu.source_phase >= 1.0 && s_msu.playing) {
            s_msu.source_phase -= 1.0;
            if (!s_msu.have_next) {
                s_msu.playing = false;
                break;
            }
            s_msu.current[0] = s_msu.next[0];
            s_msu.current[1] = s_msu.next[1];
            s_msu.have_next = read_track_frame(s_msu.next);
        }
    }
}
