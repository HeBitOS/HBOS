/* vi: set sw=4 ts=4: */
/*
 * Utility routines.
 *
 * Copyright (C) 2006 Gabriel Somlo <somlo at cmu.edu>
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */
#include "libbb.h"

/* check if path points to an executable file;
 * return 1 if found;
 * return 0 otherwise;
 *
 * HBOS: real ramfs files are checked first (unchanged upstream behavior),
 * but HBOS's actual "programs" are .hax apps looked up by bare name in an
 * embedded kernel registry (see unistd.h's hax_app_exists()), not files
 * that stat()/access() can ever see. So also check the registry, using
 * the last path component as the app name -- this is what makes `which`
 * (and anything else built on find_executable()) able to find real HBOS
 * apps like `cat`, `tcc`, etc. that aren't ramfs files at all. */
int FAST_FUNC file_is_executable(const char *name)
{
	struct stat s;
	if (!access(name, X_OK) && !stat(name, &s) && S_ISREG(s.st_mode))
		return 1;

	const char *base = name;
	for (const char *p = name; *p; p++)
		if (*p == '/') base = p + 1;
	return hax_app_exists(base);
}

/* search (*PATHp) for an executable file;
 * return allocated string containing full path if found;
 *  PATHp points to the component after the one where it was found
 *  (or NULL),
 *  you may call find_executable again with this PATHp to continue
 *  (if it's not NULL).
 * return NULL otherwise; (PATHp is undefined)
 * in all cases (*PATHp) contents are temporarily modified
 * but are restored on return (s/:/NUL/ and back).
 */
char* FAST_FUNC find_executable(const char *filename, char **PATHp)
{
	/* About empty components in $PATH:
	 * http://pubs.opengroup.org/onlinepubs/009695399/basedefs/xbd_chap08.html
	 * 8.3 Other Environment Variables - PATH
	 * A zero-length prefix is a legacy feature that indicates the current
	 * working directory. It appears as two adjacent colons ( "::" ), as an
	 * initial colon preceding the rest of the list, or as a trailing colon
	 * following the rest of the list.
	 */
	char *p, *n;

	p = *PATHp;
	while (p) {
		int ex;

		n = strchr(p, ':');
		if (n) *n = '\0';
		p = concat_path_file(
			p[0] ? p : ".", /* handle "::" case */
			filename
		);
		ex = file_is_executable(p);
		if (n) *n++ = ':';
		if (ex) {
			*PATHp = n;
			return p;
		}
		free(p);
		p = n;
	} /* on loop exit p == NULL */
	return p;
}

/* search $PATH for an executable file;
 * return 1 if found;
 * return 0 otherwise;
 */
int FAST_FUNC executable_exists(const char *filename)
{
	char *path = getenv("PATH");
	char *ret = find_executable(filename, &path);
	free(ret);
	return ret != NULL;
}

/* HBOS: BB_EXECVP/BB_EXECVP_or_die removed -- upstream's multi-call-binary
 * "try to launch an applet named 'file' first" optimization doesn't apply
 * (this port has one .hax per applet, no applet table at all -- see
 * third_party/busybox/README.md), and no applet vendored so far calls
 * either function anyway. */
