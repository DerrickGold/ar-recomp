#ifndef AR_PERFORMANCE_REPORT_H
#define AR_PERFORMANCE_REPORT_H

#include <stddef.h>
#include <stdio.h>

typedef struct PerformanceReport {
  char text[16384];
  size_t used;
  FILE *output;
} PerformanceReport;

/* Set the output and start with used = 0. Flush after appending all fields. */
void PerformanceReport_Append(PerformanceReport *report, const char *format, ...)
#ifdef __GNUC__
    __attribute__((format(printf, 2, 3)))
#endif
    ;
void PerformanceReport_Flush(PerformanceReport *report);

#endif
