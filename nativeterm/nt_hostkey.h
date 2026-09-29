/*
 * NativeTerm: a changed host key, decided in NativeTerm's window (see
 * nt_hostkey.c).
 */
#ifndef NT_HOSTKEY_H
#define NT_HOSTKEY_H

struct sshkey;
struct hostkey_entry;

/*
 * The host key `key` of `host` (`ip`) differs from the one known in
 * `found`: whether the person, asked in NativeTerm's window, replaced it
 * (the shim took the old one out); 0 where NativeTerm doesn't ask.
 */
int nt_hostkey_changed(const char *host, const char *ip, const char *type,
    const struct sshkey *key, const struct hostkey_entry *found,
    int fingerprint_hash);

#endif /* NT_HOSTKEY_H */
