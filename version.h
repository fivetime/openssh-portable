/* $OpenBSD: version.h,v 1.107 2025/10/08 00:32:52 djm Exp $ */

#define SSH_WINDOWS_VERSION "OpenSSH_for_Windows_10.2"
#define SSH_WINDOWS_BANNER " Win32-OpenSSH-GitHub"
/* NativeTerm: the plain OpenSSH name on every platform it is built for
 * (`ssh -V` and what servers are told), no platform in it; the Windows
 * names above stay for Microsoft's tools (Sync-VersionResource.ps1) */
#define SSH_VERSION	"OpenSSH_10.2"

#define SSH_PORTABLE	"p1"
#define SSH_RELEASE	SSH_VERSION SSH_PORTABLE
