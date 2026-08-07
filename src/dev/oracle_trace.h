#ifndef ORACLE_TRACE_H
#define ORACLE_TRACE_H

/* Install the optional frame-end WRAM/oracle callback when any supported
 * AR_* trace or snapshot environment control is enabled. */
void OracleTrace_Init(void);

/* Flush and close trace streams, then detach the frame callback. */
void OracleTrace_Shutdown(void);

#endif /* ORACLE_TRACE_H */
