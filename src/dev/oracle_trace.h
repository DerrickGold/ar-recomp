#ifndef ORACLE_TRACE_H
#define ORACLE_TRACE_H

#include "snesrecomp/runner.h"

/* Configure optional frame-end WRAM/oracle diagnostics. */
void OracleTrace_Init(SrRunnerHandle *runner);

/* Service one completed RtlRunFrame. The application owns this schedule so
 * turbo and catch-up ticks remain distinct without a mutable runner hook. */
void OracleTrace_CompleteTick(void);

/* Flush and close trace streams. */
void OracleTrace_Shutdown(void);

#endif /* ORACLE_TRACE_H */
