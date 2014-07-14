#-------------------------------------------------------------------------
#
# Makefile--
#    Makefile for access/bitmap
#
# IDENTIFICATION
#    $PostgreSQL$
#
#-------------------------------------------------------------------------

subdir = src/backend/access/bitmap
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

OBJS = bitmapcompare.o bitmaputil.o bitmapattutil.o \
bitmappages.o bitmapinsert.o bitmapsearch.o bitmap.o bitmapxlog.o

all: SUBSYS.o

SUBSYS.o: $(OBJS)
	$(LD) $(LDREL) $(LDOUT) SUBSYS.o $(OBJS)

depend dep:
	$(CC) -MM $(CFLAGS) *.c >depend

clean: 
	rm -f SUBSYS.o $(OBJS)

ifeq (depend,$(wildcard depend))
include depend
endif

