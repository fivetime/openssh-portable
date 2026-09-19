/*
 * NativeTerm: rz / sz in an interactive session.
 *
 * When $NATIVETERM_ZMODEM names a helper program, the session channel
 * watches what the server sends for the start of a ZMODEM transfer: a hex
 * header ZDLE 'B' "00" (ZRQINIT, the server's `sz` offers files) or "01"
 * (ZRINIT, its `rz` waits for files). There the helper is started as
 * `<helper> --zmodem download|upload`, and the channel's local ends are
 * switched to it: what the server sends goes to the helper's stdin, what the
 * helper writes goes to the server; the keyboard isn't read meanwhile. The
 * helper keeps the console (stderr) for progress. It writes NT_ZMODEM_END
 * last; then the terminal gets the session back. Text before the header
 * still goes to the terminal ("rz waiting to receive.").
 *
 * The write end of the helper's stdout pipe stays open here, so a helper
 * that ends without the marker never looks like the end of the keyboard
 * (which would close the session's input): the next data from the server
 * finds it gone and gives the terminal back then.
 *
 * Nothing here runs without $NATIVETERM_ZMODEM.
 */

#include "includes.h"

#include <sys/types.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "openbsd-compat/sys-queue.h"
#include "xmalloc.h"
#include "log.h"
#include "misc.h"
#include "sshbuf.h"
#include "channels.h"
#include "nativeterm/nt_zmodem.h"

#ifdef WINDOWS
#include <windows.h>
#else
extern char **environ;
#endif

/* After the terminal gets the session back: keyboard data read meanwhile
 * is dropped for this long (keys meant for the transfer, like Esc), and
 * the sender's closing "OO" is hidden for this long. */
#define NT_DROP_TYPED	0.3
#define NT_HIDE_OO	1.0

#define ZDLE	0x18

struct nt_zmodem {
	char *helper;
	nt_escape_filter_fn *escape;
	void *escape_ctx;
	nt_escape_cleanup_fn *escape_cleanup;
	int active;
	int broken;		/* the helper couldn't start: stay out of the way */
	pid_t pid;
	int to_helper;		/* its stdin */
	int from_helper;	/* its stdout */
	int from_helper_w;	/* our copy of its stdout's write end */
	int saved_rfd, saved_wfd, saved_isatty, saved_wfd_isatty;
	int download;		/* the server ran sz */
	double drop_typed_until;
	double hide_oo_until;
	int oo_left;
};

const char *
nt_zmodem_helper(void)
{
	const char *h = getenv("NATIVETERM_ZMODEM");

	return (h != NULL && *h != '\0') ? h : NULL;
}

void *
nt_zmodem_new_ctx(const char *helper, nt_escape_filter_fn *escape,
    void *escape_ctx, nt_escape_cleanup_fn *escape_cleanup)
{
	struct nt_zmodem *z = xcalloc(1, sizeof(*z));

	z->helper = xstrdup(helper);
	z->escape = escape;
	z->escape_ctx = escape_ctx;
	z->escape_cleanup = escape_cleanup;
	z->to_helper = z->from_helper = z->from_helper_w = -1;
	return z;
}

/* Where a ZMODEM hex header starts in buf: its ZDLE, with the mode in *mode
 * ('0' = the server sends, '1' = it receives). -1 if there is none. *partial
 * is where a header may start that the buffer cuts off, or -1. */
static long
find_header(const u_char *buf, size_t len, char *mode, long *partial)
{
	size_t i;

	*partial = -1;
	for (i = 0; i < len; i++) {
		if (buf[i] != ZDLE)
			continue;
		if (i + 3 < len) {
			if (buf[i + 1] == 'B' && buf[i + 2] == '0' &&
			    (buf[i + 3] == '0' || buf[i + 3] == '1')) {
				*mode = buf[i + 3];
				return (long)i;
			}
		} else if ((i + 1 == len || buf[i + 1] == 'B') &&
		    (i + 2 >= len || buf[i + 2] == '0')) {
			*partial = (long)i;
			return -1;
		}
	}
	return -1;
}

/* The '*' padding right before the header, if it is in this buffer. */
static long
padding_start(const u_char *buf, long at)
{
	long start = at;

	while (start > 0 && at - start < 2 && buf[start - 1] == '*')
		start--;
	return start;
}

static int
start_helper(struct nt_zmodem *z, Channel *c, char mode, int padded)
{
	int in[2], out[2];
	char *argv[4];
	posix_spawn_file_actions_t actions;
	pid_t pid = -1;

	if (pipe(in) == -1)
		return -1;
	if (pipe(out) == -1) {
		close(in[0]);
		close(in[1]);
		return -1;
	}
	/* the helper's ends only */
	fcntl(in[1], F_SETFD, FD_CLOEXEC);
	fcntl(out[0], F_SETFD, FD_CLOEXEC);
	argv[0] = z->helper;
	argv[1] = "--zmodem";
	argv[2] = mode == '0' ? "download" : "upload";
	argv[3] = NULL;
	if (posix_spawn_file_actions_init(&actions) != 0 ||
	    posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO) != 0 ||
	    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO) != 0 ||
#ifdef WINDOWS
	    posix_spawnp(&pid, z->helper, &actions, NULL, argv, NULL) != 0) {
#else
	    posix_spawnp(&pid, z->helper, &actions, NULL, argv, environ) != 0) {
#endif
		error("NativeTerm: could not start %s for rz/sz: %s",
		    z->helper, strerror(errno));
		posix_spawn_file_actions_destroy(&actions);
		close(in[0]); close(in[1]); close(out[0]); close(out[1]);
		return -1;
	}
	posix_spawn_file_actions_destroy(&actions);
	close(in[0]);
	set_nonblock(in[1]);
	set_nonblock(out[0]);
	z->pid = pid;
	z->to_helper = in[1];
	z->from_helper = out[0];
	z->from_helper_w = out[1];
	/* the header's padding went to the terminal in an earlier write */
	if (!padded)
		(void)write(z->to_helper, "**", 2);

	z->saved_rfd = c->rfd;
	z->saved_wfd = c->wfd;
	z->saved_isatty = c->isatty;
#ifdef _AIX
	z->saved_wfd_isatty = c->wfd_isatty;
#endif
	c->rfd = z->from_helper;
	c->wfd = z->to_helper;
	c->isatty = 0;
#ifdef _AIX
	c->wfd_isatty = 0;
#endif
	z->active = 1;
	z->download = mode == '0';
#ifdef WINDOWS
	/*
	 * ssh already waits for a key in its console reader thread: give it
	 * one, so the next keys (Esc for cancel) reach the helper. It is
	 * dropped when the terminal gets the session back.
	 */
	{
		INPUT_RECORD r;
		DWORD n = 0;

		memset(&r, 0, sizeof(r));
		r.EventType = KEY_EVENT;
		r.Event.KeyEvent.bKeyDown = TRUE;
		r.Event.KeyEvent.wRepeatCount = 1;
		r.Event.KeyEvent.uChar.UnicodeChar = L'x';
		(void)WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &r, 1, &n);
	}
#endif
	debug_f("rz/sz: %s started (pid %ld)", argv[2], (long)pid);
	return 0;
}

/* The terminal gets the session back. */
static void
give_back(struct nt_zmodem *z, Channel *c)
{
	int status;

	if (!z->active)
		return;
	c->rfd = z->saved_rfd;
	c->wfd = z->saved_wfd;
	c->isatty = z->saved_isatty;
#ifdef _AIX
	c->wfd_isatty = z->saved_wfd_isatty;
#endif
	close(z->to_helper);
	close(z->from_helper);
	close(z->from_helper_w);
	z->to_helper = z->from_helper = z->from_helper_w = -1;
	(void)waitpid(z->pid, &status, WNOHANG);
	z->active = 0;
	z->drop_typed_until = monotime_double() + NT_DROP_TYPED;
	if (z->download) {
		z->hide_oo_until = monotime_double() + NT_HIDE_OO;
		z->oo_left = 2;
	}
	debug_f("rz/sz: done");
}

static int
helper_gone(struct nt_zmodem *z)
{
	int status;

	return waitpid(z->pid, &status, WNOHANG) == z->pid;
}

/* Data read from the channel's local end: the keyboard, or the helper. */
int
nt_zmodem_infilter(struct ssh *ssh, Channel *c, char *buf, int len)
{
	struct nt_zmodem *z = c->filter_ctx;
	void *ours;
	int r;

	if (z->active) {
		/* everything before the marker goes to the server */
		char *end = memchr(buf, NT_ZMODEM_END[0], len);
		int n = end == NULL ? len : (int)(end - buf);

		if (n > 0 && (r = sshbuf_put(c->input, buf, n)) != 0)
			fatal_fr(r, "rz/sz: put");
		if (end != NULL)
			give_back(z, c);
		return 0;
	}
	/* keys pressed during the transfer, read by ssh meanwhile */
	if (z->drop_typed_until > 0 && monotime_double() < z->drop_typed_until) {
		debug_f("rz/sz: dropped %d typed bytes", len);
		return 0;
	}
	z->drop_typed_until = 0;
	if (z->escape == NULL) {
		if ((r = sshbuf_put(c->input, buf, len)) != 0)
			fatal_fr(r, "put");
		return 0;
	}
	/* the ~ escape filter, with its own context */
	ours = c->filter_ctx;
	c->filter_ctx = z->escape_ctx;
	r = z->escape(ssh, c, buf, len);
	c->filter_ctx = ours;
	return r;
}

/* Data from the server, before it is written to the channel's local end. */
u_char *
nt_zmodem_outfilter(struct ssh *ssh, Channel *c, u_char **data, size_t *dlen)
{
	struct nt_zmodem *z = c->filter_ctx;
	u_char *buf = sshbuf_mutable_ptr(c->output);
	size_t len = sshbuf_len(c->output);
	long at, partial, start;
	char mode = 0;

	*data = NULL;
	*dlen = len;
	if (z->active) {
		if (!helper_gone(z))
			return buf;	/* written to the helper */
		/* it ended without its marker */
		give_back(z, c);
	}
	if (z->oo_left > 0) {
		size_t i = 0;

		if (monotime_double() > z->hide_oo_until)
			z->oo_left = 0;
		/* blanked (NUL is ignored by terminals): the length stays */
		while (z->oo_left > 0 && i < len && buf[i] == 'O') {
			buf[i++] = '\0';
			z->oo_left--;
		}
		if (i < len)
			z->oo_left = 0;
	}
	if (z->broken || len == 0)
		return buf;
	at = find_header(buf, len, &mode, &partial);
	if (at < 0) {
		/* hold back a header cut off at the end, until the rest comes */
		if (partial > 0)
			*dlen = (size_t)partial;
		/*
		 * sz's "rz\r" often comes on its own, just before the header:
		 * held back at the end of a write, blanked when it is all there
		 * is (a bare "rz\r" with no line feed is hardly anything else)
		 */
		else if (len >= 3 && memcmp(buf + len - 3, "rz\r", 3) == 0) {
			if (len > 3)
				*dlen = len - 3;
			else
				memset(buf, 0, 3);
		}
		return buf;
	}
	start = padding_start(buf, at);
	/* sz says "rz\r" first (for receivers that start by themselves):
	 * not shown */
	if (start >= 3 && memcmp(buf + start - 3, "rz\r", 3) == 0) {
		if (start > 3) {
			*dlen = (size_t)(start - 3);
			return buf;
		}
		if (sshbuf_consume(c->output, 3) != 0)
			return buf;
		buf = sshbuf_mutable_ptr(c->output);
		len = sshbuf_len(c->output);
		*dlen = len;
		at -= 3;
		start = 0;
	}
	if (start > 0) {
		/* the text before it goes to the terminal first */
		*dlen = (size_t)start;
		return buf;
	}
	if (start_helper(z, c, mode, at > 0) != 0)
		z->broken = 1;
	/* from the header on, to the helper (or the terminal, if it failed) */
	return buf;
}

void
nt_zmodem_cleanup(struct ssh *ssh, int cid, void *ctx)
{
	struct nt_zmodem *z = ctx;

	if (z->active) {
		close(z->to_helper);
		close(z->from_helper);
		close(z->from_helper_w);
	}
	if (z->escape_cleanup != NULL && z->escape_ctx != NULL)
		z->escape_cleanup(ssh, cid, z->escape_ctx);
	free(z->helper);
	free(z);
}
