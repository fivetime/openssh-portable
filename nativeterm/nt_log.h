/*
 * NativeTerm: the session log's feed (see nt_log.c).
 */
#ifndef NT_LOG_H
#define NT_LOG_H

#include <sys/types.h>

struct Channel;

/*
 * Takes $NATIVETERM_LOG (the write end of the shim's pipe) out of the
 * environment. Called before closefrom(): on Unix the feed is moved to
 * STDERR_FILENO + 1, which the caller then keeps open (see nt_log_keep).
 */
void nt_log_init(void);

/* The lowest descriptor closefrom() may close: past the feed's, if any. */
int nt_log_keep(void);

/* The session channel, whose output is logged. */
void nt_log_channel(int id);

/* What the channel `c` wrote to the terminal: `len` bytes of `buf`. */
void nt_log_written(struct Channel *c, const u_char *buf, size_t len);

/*
 * ssh's own messages go to the feed as trace too, where
 * $NATIVETERM_LOG_TRACE asks for it (the shim raised LogLevel then);
 * the ones the console would show anyway still go there.
 */
void nt_log_trace_start(void);

#endif /* NT_LOG_H */
