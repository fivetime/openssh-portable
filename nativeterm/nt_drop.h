/* NativeTerm: files dropped into the tab (see nt_drop.c). */

#ifndef NT_DROP_H
#define NT_DROP_H

struct ssh;
struct Channel;

/*
 * Keyboard input, before it goes to the session. Terminal pastes the names
 * of dropped files as text: when the whole of it parses as paths that exist
 * here, they are handed to NativeTerm (`<helper> --drop <path>...`) and the
 * text is held back.
 *
 * Returns 1 when the bytes were dealt with (taken, held while the rest of a
 * bracketed paste arrives, or put in the channel's input as they were), and
 * 0 when the caller should pass them on as usual.
 */
int nt_drop_infilter(struct ssh *ssh, struct Channel *c, const char *helper,
    const char *buf, int len);

#endif /* NT_DROP_H */
