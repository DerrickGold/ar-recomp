#include "keybinds.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum {
    kScancodeUnknown = 0,
    kScancodeA = 4,
    kScancode1 = 30,
    kScancodeReturn = 40,
    kScancodeEscape = 41,
    kScancodeBackspace = 42,
    kScancodeTab = 43,
    kScancodeSpace = 44,
    kScancodeMinus = 45,
    kScancodeEquals = 46,
    kScancodeLeftBracket = 47,
    kScancodeRightBracket = 48,
    kScancodeBackslash = 49,
    kScancodeSemicolon = 51,
    kScancodeApostrophe = 52,
    kScancodeGrave = 53,
    kScancodeComma = 54,
    kScancodePeriod = 55,
    kScancodeSlash = 56,
    kScancodeF1 = 58,
    kScancodeRight = 79,
    kScancodeLeft = 80,
    kScancodeDown = 81,
    kScancodeUp = 82,
    kScancodeLeftCtrl = 224,
    kScancodeLeftShift = 225,
    kScancodeLeftAlt = 226,
    kScancodeLeftGui = 227,
    kScancodeRightCtrl = 228,
    kScancodeRightShift = 229,
    kScancodeRightAlt = 230,
    kScancodeRightGui = 231,
};

typedef struct ButtonDefinition {
    const char *name;
    size_t offset;
} ButtonDefinition;

#define BUTTON(name) {#name, offsetof(PlayerBinds, name)}
static const ButtonDefinition kButtons[] = {
    BUTTON(a), BUTTON(b), BUTTON(x), BUTTON(y), BUTTON(l), BUTTON(r),
    BUTTON(start), BUTTON(select), BUTTON(up), BUTTON(down), BUTTON(left),
    BUTTON(right),
};
#undef BUTTON

static KeyBinds g_bindings;
static char g_config_path[4096];

static void reset_defaults(void) {
    memset(&g_bindings, 0, sizeof(g_bindings));
    g_bindings.p1.a = kScancodeA + ('X' - 'A');
    g_bindings.p1.b = kScancodeA + ('Z' - 'A');
    g_bindings.p1.x = kScancodeA + ('S' - 'A');
    g_bindings.p1.y = kScancodeA;
    g_bindings.p1.l = kScancodeA + ('C' - 'A');
    g_bindings.p1.r = kScancodeA + ('V' - 'A');
    g_bindings.p1.start = kScancodeReturn;
    g_bindings.p1.select = kScancodeRightShift;
    g_bindings.p1.up = kScancodeUp;
    g_bindings.p1.down = kScancodeDown;
    g_bindings.p1.left = kScancodeLeft;
    g_bindings.p1.right = kScancodeRight;
}

static void trim(char *text) {
    char *first = text;
    size_t length;
    while (*first != '\0' && isspace((unsigned char)*first)) {
        ++first;
    }
    if (first != text) {
        memmove(text, first, strlen(first) + 1u);
    }
    length = strlen(text);
    while (length != 0u && isspace((unsigned char)text[length - 1u])) {
        text[--length] = '\0';
    }
}

static void normalize_name(const char *input, char output[64]) {
    size_t count = 0u;
    while (*input != '\0' && count + 1u < 64u) {
        unsigned char value = (unsigned char)*input++;
        if (value != ' ' && value != '_' && value != '-') {
            output[count++] = (char)tolower(value);
        }
    }
    output[count] = '\0';
}

typedef struct NamedScancode {
    const char *name;
    int value;
} NamedScancode;

static int parse_scancode(const char *name) {
    char normalized[64];
    static const NamedScancode named[] = {
        {"none", kScancodeUnknown},
        {"unknown", kScancodeUnknown},
        {"return", kScancodeReturn},
        {"enter", kScancodeReturn},
        {"escape", kScancodeEscape},
        {"esc", kScancodeEscape},
        {"backspace", kScancodeBackspace},
        {"tab", kScancodeTab},
        {"space", kScancodeSpace},
        {"minus", kScancodeMinus},
        {"equals", kScancodeEquals},
        {"leftbracket", kScancodeLeftBracket},
        {"rightbracket", kScancodeRightBracket},
        {"backslash", kScancodeBackslash},
        {"semicolon", kScancodeSemicolon},
        {"apostrophe", kScancodeApostrophe},
        {"grave", kScancodeGrave},
        {"comma", kScancodeComma},
        {"period", kScancodePeriod},
        {"slash", kScancodeSlash},
        {"right", kScancodeRight},
        {"left", kScancodeLeft},
        {"down", kScancodeDown},
        {"up", kScancodeUp},
        {"leftctrl", kScancodeLeftCtrl},
        {"lctrl", kScancodeLeftCtrl},
        {"leftshift", kScancodeLeftShift},
        {"lshift", kScancodeLeftShift},
        {"leftalt", kScancodeLeftAlt},
        {"lalt", kScancodeLeftAlt},
        {"leftgui", kScancodeLeftGui},
        {"rightctrl", kScancodeRightCtrl},
        {"rctrl", kScancodeRightCtrl},
        {"rightshift", kScancodeRightShift},
        {"rshift", kScancodeRightShift},
        {"rightalt", kScancodeRightAlt},
        {"ralt", kScancodeRightAlt},
        {"rightgui", kScancodeRightGui},
    };
    size_t index;
    normalize_name(name, normalized);
    if (normalized[0] != '\0' && normalized[1] == '\0') {
        if (normalized[0] >= 'a' && normalized[0] <= 'z') {
            return kScancodeA + normalized[0] - 'a';
        }
        if (normalized[0] >= '1' && normalized[0] <= '9') {
            return kScancode1 + normalized[0] - '1';
        }
        if (normalized[0] == '0') {
            return kScancode1 + 9;
        }
    }
    if (normalized[0] == 'f' && normalized[1] >= '1' &&
        normalized[1] <= '9') {
        int number = normalized[1] - '0';
        if (normalized[2] != '\0') {
            if (normalized[3] != '\0' || normalized[2] < '0' ||
                normalized[2] > '9') {
                return kScancodeUnknown;
            }
            number = number * 10 + normalized[2] - '0';
        }
        if (number >= 1 && number <= 12) {
            return kScancodeF1 + number - 1;
        }
    }
    for (index = 0u; index < sizeof(named) / sizeof(named[0]); ++index) {
        if (strcmp(normalized, named[index].name) == 0) {
            return named[index].value;
        }
    }
    return kScancodeUnknown;
}

static const char *format_scancode(int scancode, char buffer[16]) {
    static const char *const names[] = {
        "None", "Return", "Escape", "Backspace", "Tab", "Space", "Minus",
        "Equals", "Left Bracket", "Right Bracket", "Backslash", "Semicolon",
        "Apostrophe", "Grave", "Comma", "Period", "Slash", "Right", "Left",
        "Down", "Up", "Left Ctrl", "Left Shift", "Left Alt", "Left GUI",
        "Right Ctrl", "Right Shift", "Right Alt", "Right GUI",
    };
    static const int values[] = {
        kScancodeUnknown, kScancodeReturn, kScancodeEscape, kScancodeBackspace,
        kScancodeTab, kScancodeSpace, kScancodeMinus, kScancodeEquals,
        kScancodeLeftBracket, kScancodeRightBracket, kScancodeBackslash,
        kScancodeSemicolon, kScancodeApostrophe, kScancodeGrave, kScancodeComma,
        kScancodePeriod, kScancodeSlash, kScancodeRight, kScancodeLeft,
        kScancodeDown, kScancodeUp, kScancodeLeftCtrl, kScancodeLeftShift,
        kScancodeLeftAlt, kScancodeLeftGui, kScancodeRightCtrl,
        kScancodeRightShift, kScancodeRightAlt, kScancodeRightGui,
    };
    size_t index;
    if (scancode >= kScancodeA && scancode < kScancodeA + 26) {
        buffer[0] = (char)('A' + scancode - kScancodeA);
        buffer[1] = '\0';
        return buffer;
    }
    if (scancode >= kScancode1 && scancode <= kScancode1 + 9) {
        buffer[0] = scancode == kScancode1 + 9 ? '0'
                                                : (char)('1' + scancode - kScancode1);
        buffer[1] = '\0';
        return buffer;
    }
    if (scancode >= kScancodeF1 && scancode < kScancodeF1 + 12) {
        snprintf(buffer, 16u, "F%d", scancode - kScancodeF1 + 1);
        return buffer;
    }
    for (index = 0u; index < sizeof(values) / sizeof(values[0]); ++index) {
        if (scancode == values[index]) {
            return names[index];
        }
    }
    return "None";
}

static void derive_config_path(const char *executable_path) {
    const char *separator = NULL;
    const char *cursor;
    size_t directory_length = 0u;
    static const char filename[] = "keybinds.ini";
    if (executable_path != NULL) {
        for (cursor = executable_path; *cursor != '\0'; ++cursor) {
            if (*cursor == '/' || *cursor == '\\') {
                separator = cursor;
            }
        }
    }
    if (separator != NULL) {
        directory_length = (size_t)(separator - executable_path) + 1u;
    }
    if (directory_length + sizeof(filename) > sizeof(g_config_path)) {
        directory_length = 0u;
    }
    if (directory_length != 0u) {
        memcpy(g_config_path, executable_path, directory_length);
    }
    memcpy(g_config_path + directory_length, filename, sizeof(filename));
}

static void write_player(FILE *file, const char *section,
                         const PlayerBinds *bindings) {
    size_t index;
    fprintf(file, "[%s]\n", section);
    for (index = 0u; index < sizeof(kButtons) / sizeof(kButtons[0]); ++index) {
        const int scancode = *(const int *)((const char *)bindings +
                                            kButtons[index].offset);
        char buffer[16];
        fprintf(file, "%-7s = %s\n", kButtons[index].name,
                format_scancode(scancode, buffer));
    }
    fputc('\n', file);
}

static void write_defaults(void) {
    FILE *file = fopen(g_config_path, "wb");
    if (file == NULL) {
        return;
    }
    fputs("# SNES controller bindings. Use None to leave a button unbound.\n\n",
          file);
    write_player(file, "player1", &g_bindings.p1);
    write_player(file, "player2", &g_bindings.p2);
    fclose(file);
}

static void load_bindings(FILE *file) {
    char line[256];
    PlayerBinds *section = NULL;
    while (fgets(line, sizeof(line), file) != NULL) {
        char *equals;
        char *key;
        char *value;
        size_t index;
        trim(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (line[0] == '[') {
            char *end = strchr(line + 1, ']');
            section = NULL;
            if (end != NULL) {
                *end = '\0';
                if (strcmp(line + 1, "player1") == 0) {
                    section = &g_bindings.p1;
                } else if (strcmp(line + 1, "player2") == 0) {
                    section = &g_bindings.p2;
                }
            }
            continue;
        }
        if (section == NULL || (equals = strchr(line, '=')) == NULL) {
            continue;
        }
        *equals = '\0';
        key = line;
        value = equals + 1;
        trim(key);
        trim(value);
        for (index = 0u; index < strlen(key); ++index) {
            key[index] = (char)tolower((unsigned char)key[index]);
        }
        for (index = 0u; index < sizeof(kButtons) / sizeof(kButtons[0]); ++index) {
            if (strcmp(key, kButtons[index].name) == 0) {
                *(int *)((char *)section + kButtons[index].offset) =
                    parse_scancode(value);
                break;
            }
        }
    }
}

void keybinds_init(const char *executable_path) {
    FILE *file;
    reset_defaults();
    derive_config_path(executable_path);
    file = fopen(g_config_path, "rb");
    if (file == NULL) {
        write_defaults();
        return;
    }
    load_bindings(file);
    fclose(file);
}

const KeyBinds *keybinds_get(void) {
    return &g_bindings;
}

uint16_t keybinds_read_player(const uint8_t *keyboard_state, int player) {
    const PlayerBinds *bindings;
    uint16_t result = 0u;
    static const struct {
        size_t offset;
        uint16_t bit;
    } map[] = {
        {offsetof(PlayerBinds, r), 1u << 0},
        {offsetof(PlayerBinds, l), 1u << 1},
        {offsetof(PlayerBinds, x), 1u << 2},
        {offsetof(PlayerBinds, a), 1u << 3},
        {offsetof(PlayerBinds, right), 1u << 4},
        {offsetof(PlayerBinds, left), 1u << 5},
        {offsetof(PlayerBinds, down), 1u << 6},
        {offsetof(PlayerBinds, up), 1u << 7},
        {offsetof(PlayerBinds, start), 1u << 8},
        {offsetof(PlayerBinds, select), 1u << 9},
        {offsetof(PlayerBinds, y), 1u << 10},
        {offsetof(PlayerBinds, b), 1u << 11},
    };
    size_t index;
    if (keyboard_state == NULL || (player != 1 && player != 2)) {
        return 0u;
    }
    bindings = player == 1 ? &g_bindings.p1 : &g_bindings.p2;
    for (index = 0u; index < sizeof(map) / sizeof(map[0]); ++index) {
        int scancode = *(const int *)((const char *)bindings + map[index].offset);
        if (scancode > kScancodeUnknown && keyboard_state[scancode] != 0u) {
            result |= map[index].bit;
        }
    }
    return result;
}
