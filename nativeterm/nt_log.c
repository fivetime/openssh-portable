/*
 * NativeTerm: the session log's feed.
 *
 * NativeTerm writes the session log itself (its shim, which started this
 * ssh): what the terminal shows, and with a trace level ssh's own
 * messages. When $NATIVETERM_LOG names the write end of the shim's pipe
 * (a HANDLE on Windows, a descriptor elsewhere, inherited), everything the
 * session channel writes to the terminal is copied there, and nothing
 * else is changed: ZMODEM data going to the rz / sz helper is not copied,
 * and neither is the keyboard. The shim decides whether and where to log
 * (it may start and stop while this ssh runs), so the copy is made
 * whenever the feed is there.
 *
 * A record is one byte of kind ('O' output, 'T' a message of ssh's), its
 * length as four bytes little-endian, and the bytes. A write that fails
 * (the shim gone) ends the feed; the session goes on.
 *
 * Nothing here runs without $NATIVETERM_LOG.
 */

#include "includes.h"

#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "openbsd-compat/sys-queue.h"
#include "xmalloc.h"
#include "log.h"
#include "misc.h"
#include "channels.h"
#include "nativeterm/nt_log.h"
#include "nativeterm/nt_zmodem.h"

#ifdef WINDOWS
#include <windows.h>
static HANDLE feed = INVALID_HANDLE_VALUE;
#define FEED_OPEN (feed != INVALID_HANDLE_VALUE)
#else
static int feed = -1;
#define FEED_OPEN (feed != -1)
#endif

static int session_channel = -1;

/* channels.c's hook */
extern void (*nt_channel_written)(Channel *, const u_char *, size_t);
/* the level up to which the console showed ssh's messages before the
 * shim raised it for the trace */
static LogLevel console_level = SYSLOG_LEVEL_INFO;
/* ssh's messages go to the feed (log_init() drops the handler: it is
 * set again after each) */
static int tracing;

static void
feed_close(void)
{
#ifdef WINDOWS
	if (FEED_OPEN)
		CloseHandle(feed);
	feed = INVALID_HANDLE_VALUE;
#else
	if (FEED_OPEN)
		close(feed);
	feed = -1;
#endif
}

static int
feed_write_all(const u_char *p, size_t len)
{
	while (len > 0) {
#ifdef WINDOWS
		DWORD n = 0;
		DWORD chunk = len > 0x10000 ? 0x10000 : (DWORD)len;

		if (!WriteFile(feed, p, chunk, &n, NULL) || n == 0)
			return -1;
#else
		ssize_t n = write(feed, p, len);

		if (n == -1 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n <= 0)
			return -1;
#endif
		p += n;
		len -= (size_t)n;
	}
	return 0;
}

static void
feed_record(char kind, const u_char *buf, size_t len)
{
	u_char head[5];

	if (!FEED_OPEN || len == 0 || len > 0x7fffffff)
		return;
	head[0] = (u_char)kind;
	head[1] = len & 0xff;
	head[2] = (len >> 8) & 0xff;
	head[3] = (len >> 16) & 0xff;
	head[4] = (len >> 24) & 0xff;
	if (feed_write_all(head, sizeof(head)) != 0 ||
	    feed_write_all(buf, len) != 0)
		feed_close();
}

void
nt_log_init(void)
{
	const char *v = getenv("NATIVETERM_LOG");
	char *end = NULL;
	unsigned long long n;

	if (v == NULL || *v == '\0')
		return;
	n = strtoull(v, &end, 10);
	/* not for any program this ssh starts */
	unsetenv("NATIVETERM_LOG");
	if (end == NULL || *end != '\0')
		return;
#ifdef WINDOWS
	feed = (HANDLE)(uintptr_t)n;
	if (feed == NULL || GetFileType(feed) != FILE_TYPE_PIPE) {
		feed = INVALID_HANDLE_VALUE;
		return;
	}
	/* the helpers this ssh starts don't get it */
	SetHandleInformation(feed, HANDLE_FLAG_INHERIT, 0);
#else
	if (n <= STDERR_FILENO || n > 0x7fffffff ||
	    fcntl((int)n, F_GETFD) == -1)
		return;
	feed = (int)n;
	/* where closefrom() leaves it */
	if (feed != STDERR_FILENO + 1) {
		if (dup2(feed, STDERR_FILENO + 1) == -1) {
			feed_close();
			return;
		}
		close(feed);
		feed = STDERR_FILENO + 1;
	}
	(void)fcntl(feed, F_SETFD, FD_CLOEXEC);
#endif
}

int
nt_log_keep(void)
{
#ifdef WINDOWS
	return STDERR_FILENO + 1;
#else
	return FEED_OPEN ? STDERR_FILENO + 2 : STDERR_FILENO + 1;
#endif
}

void
nt_log_channel(int id)
{
	session_channel = id;
	if (FEED_OPEN)
		nt_channel_written = nt_log_written;
}

void
nt_log_written(Channel *c, const u_char *buf, size_t len)
{
	if (!FEED_OPEN || c == NULL || c->self != session_channel)
		return;
	/* the rz / sz helper's data, not the terminal's */
	if (nt_zmodem_busy(c))
		return;
	feed_record('O', buf, len);
}

static const char *
level_prefix(LogLevel level)
{
	switch (level) {
	case SYSLOG_LEVEL_DEBUG1:
		return "debug1: ";
	case SYSLOG_LEVEL_DEBUG2:
		return "debug2: ";
	case SYSLOG_LEVEL_DEBUG3:
		return "debug3: ";
	default:
		return "";
	}
}

static void
trace_handler(LogLevel level, int force, const char *msg, void *ctx)
{
	char line[1024];
	int n;

	n = snprintf(line, sizeof(line), "%s%s", level_prefix(level), msg);
	if (n > 0)
		feed_record('T', (const u_char *)line,
		    MINIMUM((size_t)n, sizeof(line) - 1));
	/* what the console showed before still shows */
	if (force || level <= console_level) {
		n = snprintf(line, sizeof(line), "%s\r\n", msg);
		if (n > 0)
			(void)write(STDERR_FILENO, line,
			    MINIMUM((size_t)n, sizeof(line) - 1));
	}
}

void
nt_log_trace_start(void)
{
	const char *v = getenv("NATIVETERM_LOG_TRACE");
	LogLevel level;

	if (v != NULL && *v != '\0') {
		level = log_level_number((char *)v);
		unsetenv("NATIVETERM_LOG_TRACE");
		console_level = level == SYSLOG_LEVEL_NOT_SET ?
		    SYSLOG_LEVEL_INFO : level;
		tracing = FEED_OPEN;
	}
	if (tracing && FEED_OPEN)
		set_log_handler(trace_handler, NULL);
}
