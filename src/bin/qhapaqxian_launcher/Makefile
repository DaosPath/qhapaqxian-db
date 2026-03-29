#-------------------------------------------------------------------------
#
# Makefile for src/bin/qhapaqxian_launcher
#
# Copyright (c) 2026, QhapaqXian DB contributors
#
#-------------------------------------------------------------------------

PGFILEDESC = "QhapaqXian DB Windows launcher"
PGAPPICON = win32

subdir = src/bin/qhapaqxian_launcher
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = \
	$(WIN32RES) \
	qhapaqxian_db_launcher.o

all: qhapaqxian-db

qhapaqxian-db: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_SCRIPT) qhapaqxian-db$(X) '$(DESTDIR)$(bindir)/qhapaqxian-db$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/qhapaqxian-db$(X)'

clean distclean:
	rm -f qhapaqxian-db$(X) $(OBJS)
