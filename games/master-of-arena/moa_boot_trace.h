/* moa_boot_trace.h — Rogue Arena boot trace (diagnostic). See moa_boot_trace.c. */
#ifndef MOA_BOOT_TRACE_H
#define MOA_BOOT_TRACE_H

/* Registers the trace's function hooks. Call before any other hook on the
 * same addresses is registered, so a filter that skips a function (the
 * title-open filter) can't hide the call from the trace. */
void moa_btrace_register(void);

/* The last menu routines called (all session), oldest first, as
 * "frame:name frame:name ..." (the run's hang watch logs it). */
void moa_btrace_recent(char *out, unsigned size);

#endif
