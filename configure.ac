AC_INIT([streamproxy],[1.0],[enigma2-devel@lists.elitedvb.net])
AM_INIT_AUTOMAKE([dist-bzip2 foreign no-define tar-pax -Wno-portability])

# Silent rules are available since 1.11, but older versions
# are still in use. So don't use them unconditionally.
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC

AC_OUTPUT([
Makefile
src/Makefile
])
