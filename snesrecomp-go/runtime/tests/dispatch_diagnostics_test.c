#include "snesrecomp/game/cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

/* Portable capture without replacing the process's stderr or environment.
 * Compile the actual formatting helper with its output call substituted.
 * The guard emitter test requires the presence check in the generated caller. */
static char *setting;
static char output[1024];
static unsigned queries, prints;
static int bad_query;
static char *test_getenv(const char *name) {
    ++queries;
    if (strcmp(name, "AR_RTSDISP_MISS")) bad_query = 1;
    return setting;
}
static int test_fprintf(FILE *stream, const char *format, ...) {
    int result;
    va_list ap;
    if (stream != stderr) bad_query = 1;
    ++prints;
    va_start(ap, format);
    result = vsnprintf(output, sizeof(output), format, ap);
    va_end(ap);
    return result;
}
#define fprintf test_fprintf
#include "../src/core/dispatch_diagnostics.c"
#undef fprintf

int main(void) {
    CpuState cpu, before;
    char *settings[] = {NULL, "", "0", "1", NULL};
    unsigned i;
    const char *width = "[rts_dispatch_width] site=$128765 popped target=$ABCD: runtime m=1 x=0 has no decoded body -> generic return (refused wrong-width goto)\n";
    const char *miss = "[rts_dispatch_miss] site=$128765 popped target=$ABCD (UNREGISTERED -> generic return; add to rts_dispatch) S=$1FEC m=1 x=0\n";
    memset(&cpu, 0, sizeof(cpu)); cpu.S = 0x1fec; cpu.m_flag = 1; before = cpu;
    for (i = 0; i < countof(settings); ++i) {
        setting = settings[i]; prints = queries = 0; output[0] = 0;
        if (test_getenv("AR_RTSDISP_MISS")) sr_rts_dispatch_width(&cpu, 0x128765, 0xabcd);
        if (queries != 1 || prints != (unsigned)(setting != NULL) ||
            strcmp(output, setting ? width : "")) return 1;
        if (test_getenv("AR_RTSDISP_MISS")) sr_rts_dispatch_miss(&cpu, 0x128765, 0xabcd);
        if (queries != 2 || prints != 2u * (unsigned)(setting != NULL) ||
            strcmp(output, setting ? miss : "") || memcmp(&before, &cpu, sizeof(cpu))) return 1;
    }
    return bad_query ? 1 : 0;
}
