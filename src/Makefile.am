### Makefile.am
include $(top_builddir)/version.mk

AM_CFLAGS = $(EXTRA_CFLAGS)
AM_CPPFLAGS = -D_POSIX_C_SOURCE=201001L -D_XOPEN_SOURCE=700 -D_BSD_SOURCE
AM_CPPFLAGS += -D_HAVE_VERSION_H
AM_LDFLAGS = $(XCCLDFLAGS)

bin_PROGRAMS =
noinst_PROGRAMS =
lib_LTLIBRARIES =
noinst_LTLIBRARIES =
pkglib_LTLIBRARIES =
noinst_HEADERS =
BUILT_SOURCES =
EXTRA_DIST = $(BUILT_SOURCES)
CLEANFILES = 
SUFFIXES =

CLEANFILES += version.c
EXTRA_DIST += version.c.in


## version rules
version.c: version.c.in $(top_builddir)/.version
	$(AM_V_GEN) PATH="$(top_builddir)/build-aux:$${PATH}" \
		yuck$(EXEEXT) scmver --ignore-noscm --force -o $@ \
			--use-reference --reference $(top_builddir)/.version $<

bin_PROGRAMS += blpcli
blpcli_SOURCES = blpcli.c blpcli.yuck
blpcli_SOURCES += nifty.h
blpcli_CPPFLAGS = $(AM_CPPFLAGS)
blpcli_CPPFLAGS += $(blpapi_CFLAGS)
blpcli_LDFLAGS = $(AM_LDFLAGS)
blpcli_LDFLAGS += -lpthread
blpcli_LDFLAGS += $(blpapi_LIBS)
BUILT_SOURCES += blpcli.yucc

bin_PROGRAMS += blp-um
blp_um_SOURCES = blp-um.c blp-um.yuck
blp_um_SOURCES += nifty.h
blp_um_CPPFLAGS = $(AM_CPPFLAGS)
blp_um_CPPFLAGS += $(blpapi_CFLAGS)
blp_um_LDFLAGS = $(AM_LDFLAGS)
blp_um_LDFLAGS += -lpthread
blp_um_LDFLAGS += $(blpapi_LIBS)
BUILT_SOURCES += blp-um.yucc


## yuck rule
SUFFIXES += .yuck
SUFFIXES += .yucc
.yuck.yucc:
	$(AM_V_GEN) PATH=$(top_builddir)/build-aux:"$${PATH}" \
		yuck$(EXEEXT) gen -o $@ $<

## Create preprocessor output (debugging purposes only)
.c.i:
	$(COMPILE) -E -o $@ $<

## Create assembler output (debugging purposes only)
.c.s:
	$(COMPILE) -S -c $(AM_CFLAGS) $<
