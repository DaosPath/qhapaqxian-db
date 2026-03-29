# contrib/qhapaqxian_output/Makefile

MODULES = qhapaqxian_output
PGFILEDESC = "qhapaqxian_output - QhapaqXian semantic logical output plugin"

REGRESS = semantic_messages
REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/qhapaqxian_output/logical.conf

# Requires wal_level=logical in the temporary cluster.
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/qhapaqxian_output
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
