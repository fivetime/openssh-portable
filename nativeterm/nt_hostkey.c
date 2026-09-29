/*
 * NativeTerm: a changed host key, decided in NativeTerm's window.
 *
 * OpenSSH refuses a host whose key differs from the known one, and says
 * what to edit by hand. When the shim that started this ssh asks for it
 * ($NATIVETERM_HOSTKEY, set together with its forced askpass helper), the
 * question goes to NativeTerm instead, through the askpass helper, as a
 * prompt of its own: a marker line, then "key value" lines (host, ip,
 * type, the new and the old fingerprint, the file and line of the old
 * key). NativeTerm shows the warning with both fingerprints; the person
 * may keep the connection refused, or have the old key taken out (the
 * shim does that, with ssh-keygen -R) and the new one saved, as for a new
 * host. The answer is "yes" or "no".
 *
 * Nothing changes without $NATIVETERM_HOSTKEY, or in batch mode.
 */

#include "includes.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "xmalloc.h"
#include "ssh.h"
#include "log.h"
#include "misc.h"
#include "sshkey.h"
#include "hostfile.h"
#include "readconf.h"
#include "nativeterm/nt_hostkey.h"

extern Options options;

/* What the helper looks for; see NativeTerm's shim (hostkey.rs). */
#define NT_HOSTKEY_MARKER "NATIVETERM-HOSTKEY-CHANGED"

int
nt_hostkey_changed(const char *host, const char *ip, const char *type,
    const struct sshkey *key, const struct hostkey_entry *found,
    int fingerprint_hash)
{
	const char *on = getenv("NATIVETERM_HOSTKEY");
	char *fp = NULL, *old = NULL, *prompt = NULL, *answer, *p;
	int yes = 0;

	if (on == NULL || *on == '\0' || options.batch_mode || found == NULL)
		return 0;
	fp = sshkey_fingerprint(key, fingerprint_hash, SSH_FP_DEFAULT);
	old = sshkey_fingerprint(found->key, fingerprint_hash, SSH_FP_DEFAULT);
	if (fp == NULL || old == NULL)
		goto out;
	xasprintf(&prompt, "%s\nhost %s\nip %s\ntype %s\nnew %s\nold %s %s\n"
	    "file %s\nline %lu\n", NT_HOSTKEY_MARKER, host,
	    ip == NULL ? "" : ip, type, fp, sshkey_type(found->key), old,
	    found->file, found->line);
	answer = read_passphrase(prompt, RP_ECHO);
	if (answer != NULL) {
		p = answer + strspn(answer, " \t");
		p[strcspn(p, " \t\r\n")] = '\0';
		yes = strcasecmp(p, "yes") == 0;
		free(answer);
	}
	if (yes)
		logit("The host key of %.200s was replaced in NativeTerm.",
		    host);
 out:
	free(prompt);
	free(fp);
	free(old);
	return yes;
}
