#-------------------------------------------------------------------------
#
# Makefile for src/bin/qhapaqxian_tool_runner
#
#-------------------------------------------------------------------------

subdir = src/bin/qhapaqxian_tool_runner
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

OBJS = qhapaqxian_tool_runner.o

all: qhapaqxian-tool-runner

qhapaqxian-tool-runner: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

install: all installdirs
	$(INSTALL_PROGRAM) qhapaqxian-tool-runner$(X) '$(DESTDIR)$(bindir)/qhapaqxian-tool-runner$(X)'

installdirs:
	$(MKDIR_P) '$(DESTDIR)$(bindir)'

uninstall:
	rm -f '$(DESTDIR)$(bindir)/qhapaqxian-tool-runner$(X)'

clean distclean:
	rm -f qhapaqxian-tool-runner$(X) $(OBJS)
