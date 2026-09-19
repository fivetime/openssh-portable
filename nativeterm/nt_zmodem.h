/*
 * NativeTerm: rz / sz in an interactive session (see nt_zmodem.c).
 */
#ifndef NT_ZMODEM_H
#define NT_ZMODEM_H

struct ssh;
struct Channel;

typedef int nt_escape_filter_fn(struct ssh *, struct Channel *, char *, int);
typedef void nt_escape_cleanup_fn(struct ssh *, int, void *);

/* The helper program ($NATIVETERM_ZMODEM), or NULL: then nothing changes. */
const char *nt_zmodem_helper(void);

/*
 * A filter context for the session channel. `escape` (with its context and
 * cleanup) is the ~ escape filter the session would have had, or NULL.
 */
void *nt_zmodem_new_ctx(const char *helper, nt_escape_filter_fn *escape,
    void *escape_ctx, nt_escape_cleanup_fn *escape_cleanup);

int nt_zmodem_infilter(struct ssh *, struct Channel *, char *, int);
unsigned char *nt_zmodem_outfilter(struct ssh *, struct Channel *,
    unsigned char **, size_t *);
void nt_zmodem_cleanup(struct ssh *, int, void *);

/* What a helper writes last: it never appears raw in ZMODEM data (XOFF is
 * always escaped there). */
#define NT_ZMODEM_END	"\x13\x13\x13\x13"

#endif /* NT_ZMODEM_H */
