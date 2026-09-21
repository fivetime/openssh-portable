/*
 * NativeTerm: files dropped into the tab.
 *
 * Windows Terminal answers a drop by pasting the file names as text, which
 * arrives here as keyboard input. When every word of it is a path that
 * exists on this machine, the text is held back and the names are handed to
 * NativeTerm (`<helper> --drop <path>...`), which asks what to do with
 * them: upload them over SFTP, or send the text to the session after all
 * (through the tab's shim, as "send text" does).
 *
 * Text the terminal brackets (ESC [ 200 ~ ... ESC [ 201 ~, which a shell
 * that asked for bracketed paste gets) is collected first; anything else is
 * judged as it comes. Whatever does not parse as existing paths is passed
 * on untouched, so typing is never taken for a drop.
 *
 * Nothing here runs without $NATIVETERM_ZMODEM, which names the helper.
 */

#include "includes.h"

#include <sys/types.h>

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
#include "nativeterm/nt_drop.h"

#ifdef WINDOWS
#include <windows.h>
#else
extern char **environ;
#endif

/* A bracketed paste longer than this, or still unfinished after this long,
 * is not a drop: it goes to the session as it came. */
#define NT_DROP_MAX	(64 * 1024)
#define NT_DROP_WAIT	1.0
/* At most this many names go on the helper's command line. */
#define NT_DROP_NAMES	64

/*
 * What NativeTerm puts before text it sends back to the session after a
 * drop ("type the names into the terminal"): without it the names would be
 * read as a drop again, for ever. It is stripped here and never reaches
 * the server. An APC string, which nothing types.
 */
static const char PASS_MARKER[] = "\033_nt\033\\";
#define MARKER_LEN	(sizeof(PASS_MARKER) - 1)

static const char BRACKET_START[] = "\033[200~";
static const char BRACKET_END[] = "\033[201~";
#define START_LEN	(sizeof(BRACKET_START) - 1)
#define END_LEN		(sizeof(BRACKET_END) - 1)

/* A bracketed paste being collected. */
static struct {
	struct sshbuf *held;
	double since;
	/* how much of PASS_MARKER has come so far (it can be split) */
	size_t matched;
	double matched_since;
} drop;

/* Where `needle` starts in `hay`, or NULL. */
static const char *
find(const char *hay, size_t hay_len, const char *needle, size_t needle_len)
{
	size_t i;

	if (needle_len > hay_len)
		return NULL;
	for (i = 0; i + needle_len <= hay_len; i++) {
		if (memcmp(hay + i, needle, needle_len) == 0)
			return hay + i;
	}
	return NULL;
}

#ifdef WINDOWS
/* Whether `path` names a file or folder of this machine. */
static int
path_exists(const char *path)
{
	wchar_t *wide;
	int n, ok = 0;

	n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	if (n <= 0)
		return 0;
	wide = xcalloc((size_t)n, sizeof(*wide));
	if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, n) > 0)
		ok = GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES;
	free(wide);
	return ok;
}
#else
static int
path_exists(const char *path)
{
	(void)path;
	return 0;
}
#endif

/* An absolute Windows path: "C:\..." or "\\server\share". */
static int
looks_absolute(const char *path)
{
	if (path[0] != '\0' && path[1] == ':' &&
	    (path[2] == '\\' || path[2] == '/'))
		return 1;
	return path[0] == '\\' && path[1] == '\\';
}

/*
 * The paths in `text`, as Terminal writes them: separated by spaces, a name
 * with a space in quotes. Returns how many were stored in `out`, or -1 when
 * the text holds anything else (so it is not a drop).
 */
static int
parse_paths(const char *text, size_t len, char **out, int max)
{
	size_t i = 0;
	int n = 0;

	while (i < len) {
		size_t start, end;
		int quoted = 0;
		char *path;

		while (i < len && (text[i] == ' ' || text[i] == '\t' ||
		    text[i] == '\r' || text[i] == '\n'))
			i++;
		if (i >= len)
			break;
		if (text[i] == '"') {
			quoted = 1;
			i++;
		}
		start = i;
		while (i < len && (quoted ? text[i] != '"' :
		    text[i] != ' ' && text[i] != '\t' &&
		    text[i] != '\r' && text[i] != '\n'))
			i++;
		end = i;
		if (quoted) {
			if (i >= len)	/* an unclosed quote: not a drop */
				goto no;
			i++;
		}
		if (end == start || n >= max)
			goto no;
		path = xmalloc(end - start + 1);
		memcpy(path, text + start, end - start);
		path[end - start] = '\0';
		if (!looks_absolute(path) || !path_exists(path)) {
			free(path);
			goto no;
		}
		out[n++] = path;
	}
	if (n == 0)
		goto no;
	return n;
no:
	while (n > 0)
		free(out[--n]);
	return -1;
}

/* Hands the names to NativeTerm, which asks what to do with them. */
static int
tell_nativeterm(const char *helper, char **paths, int count)
{
	char *argv[NT_DROP_NAMES + 3];
	posix_spawn_file_actions_t actions;
	pid_t pid = -1;
	int i, r;

	argv[0] = (char *)helper;
	argv[1] = "--drop";
	for (i = 0; i < count; i++)
		argv[i + 2] = paths[i];
	argv[count + 2] = NULL;
	/* the helper says nothing and ends at once; it keeps this console */
	if (posix_spawn_file_actions_init(&actions) != 0)
		return -1;
#ifdef WINDOWS
	r = posix_spawnp(&pid, helper, &actions, NULL, argv, NULL);
#else
	r = posix_spawnp(&pid, helper, &actions, NULL, argv, environ);
#endif
	posix_spawn_file_actions_destroy(&actions);
	if (r != 0) {
		error("NativeTerm: could not start %s for a drop: %s",
		    helper, strerror(r));
		return -1;
	}
	debug_f("drop: %d name(s) sent to NativeTerm (pid %ld)", count,
	    (long)pid);
	return 0;
}

/* `text` as a drop, if that is what it is; whether it was taken. */
static int
take(const char *helper, const char *text, size_t len)
{
	char *paths[NT_DROP_NAMES];
	int count, i, taken;

	count = parse_paths(text, len, paths, NT_DROP_NAMES);
	if (count < 0)
		return 0;
	taken = tell_nativeterm(helper, paths, count) == 0;
	for (i = 0; i < count; i++)
		free(paths[i]);
	return taken;
}

/* What was collected goes to the session after all. */
static void
flush_held(Channel *c)
{
	int r;

	if (drop.held == NULL || sshbuf_len(drop.held) == 0)
		return;
	if ((r = sshbuf_putb(c->input, drop.held)) != 0)
		fatal_fr(r, "drop: flush");
	sshbuf_reset(drop.held);
}

/*
 * Text NativeTerm sends back after a drop, which starts with PASS_MARKER:
 * the marker is dropped and the rest goes to the session without being
 * looked at again. Returns 1 when the bytes were dealt with here.
 */
static int
passed_back(Channel *c, const char *buf, int len)
{
	size_t held_before, i = 0;
	int r;

	/* a marker cut off long ago was not one */
	if (drop.matched > 0 && monotime_double() - drop.matched_since > 0.5)
		drop.matched = 0;
	if (drop.matched == 0 && buf[0] != PASS_MARKER[0])
		return 0;
	/* what earlier reads held back; this read's bytes the caller still has */
	held_before = drop.matched;

	while (i < (size_t)len && drop.matched < MARKER_LEN) {
		if (buf[i] != PASS_MARKER[drop.matched]) {
			/* not the marker: what was held goes on as it came,
			 * and the caller passes on this read as usual */
			if (held_before > 0 && (r = sshbuf_put(c->input,
			    PASS_MARKER, held_before)) != 0)
				fatal_fr(r, "drop: marker");
			drop.matched = 0;
			return 0;
		}
		drop.matched++;
		i++;
	}
	if (drop.matched < MARKER_LEN) {
		drop.matched_since = monotime_double();
		return 1;	/* the rest of the marker is still to come */
	}
	drop.matched = 0;
	if (i < (size_t)len &&
	    (r = sshbuf_put(c->input, buf + i, (size_t)len - i)) != 0)
		fatal_fr(r, "drop: passed back");
	return 1;
}

int
nt_drop_infilter(struct ssh *ssh, Channel *c, const char *helper,
    const char *buf, int len)
{
	const char *held, *end;
	size_t held_len;
	int collecting, r;

	(void)ssh;
	if (helper == NULL || len <= 0)
		return 0;

	if (passed_back(c, buf, len))
		return 1;

	collecting = drop.held != NULL && sshbuf_len(drop.held) > 0;
	if (!collecting) {
		if ((size_t)len < START_LEN ||
		    memcmp(buf, BRACKET_START, START_LEN) != 0) {
			/* no brackets: the text as it came */
			return take(helper, buf, (size_t)len);
		}
		if (drop.held == NULL && (drop.held = sshbuf_new()) == NULL)
			return 0;
		drop.since = monotime_double();
	}

	if ((r = sshbuf_put(drop.held, buf, (size_t)len)) != 0)
		fatal_fr(r, "drop: collect");
	held = (const char *)sshbuf_ptr(drop.held);
	held_len = sshbuf_len(drop.held);
	end = find(held + START_LEN, held_len - START_LEN, BRACKET_END,
	    END_LEN);
	if (end == NULL) {
		/* wait for the rest, but not for ever */
		if (held_len > NT_DROP_MAX ||
		    monotime_double() - drop.since > NT_DROP_WAIT)
			flush_held(c);
		return 1;
	}
	if (take(helper, held + START_LEN, (size_t)(end - held) - START_LEN)) {
		sshbuf_reset(drop.held);
		return 1;
	}
	flush_held(c);
	return 1;
}
