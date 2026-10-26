subdir = src/test/modules/yb_test_pam
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
ifneq (,$(filter -lpam,$(LIBS)))
MODULE_big = yb_test_pam
OBJS = yb_test_pam.o
PGFILEDESC = "yb_test_pam - PAM authentication cleanup tests"
REGRESS = yb.orig.pam_cleanup
endif
include $(top_srcdir)/contrib/contrib-global.mk

yb_test_pam.o: $(top_srcdir)/src/backend/libpq/auth.c
