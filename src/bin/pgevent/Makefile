#-------------------------------------------------------------------------
#
# Makefile for src/bin/pgevent
#
# Copyright (c) 1996-2005, PostgreSQL Global Development Group
#
#-------------------------------------------------------------------------

PGFILEDESC = "Eventlog message formatter"
PGFILESHLIB = 1
subdir = src/bin/pgevent
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS=pgevent.o pgmsgevent.o
NAME=pgevent.dll

all: $(NAME)

install: all install-lib

pgevent.dll: $(OBJS) pgevent.def
	dllwrap --def pgevent.def -o $(NAME) $(OBJS)
	
pgmsgevent.o: pgmsgevent.rc win32ver.rc
	windres pgmsgevent.rc -o pgmsgevent.o --include-dir=$(top_builddir)/src/include

all-lib: $(NAME)

install-lib: $(NAME)
	$(INSTALL_STLIB) $< $(DESTDIR)$(libdir)/$<

uninstall-lib:
	rm -f $(DESTDIR)$(libdir)/$(NAME)

clean distclean:
	rm -f $(OBJS) $(NAME) win32ver.rc

clean-lib:
	rm -f $(NAME)
