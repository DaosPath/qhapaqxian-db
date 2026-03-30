#-------------------------------------------------------------------------
#
# Makefile for src/bin/qhapaqxian_tool_runner
#
#-------------------------------------------------------------------------

subdir = src/bin/qhapaqxian_tool_runner
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

LDFLAGS_INTERNAL += -L$(top_builddir)/src/common -lpgcommon -L$(top_builddir)/src/port -lpgport

OBJS = qhapaqxian_tool_runner.o

all: qhapaqxian-tool-runner

qhapaqxian-tool-runner: $(OBJS) | submake-libpgport
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) qhapaqxian-tool-runner$(X) '$(DESTDIR)$(bindir)/qhapaqxian-tool-runner$(X)'
	$(INSTALL_DATA) qx_remote_ed25519_private.pem '$(DESTDIR)$(bindir)/qx_remote_ed25519_private.pem'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/qhapaqxian-tool-runner$(X)'
	rm -f '$(DESTDIR)$(bindir)/qx_remote_ed25519_private.pem'

clean distclean:
	rm -f qhapaqxian-tool-runner$(X) $(OBJS)
