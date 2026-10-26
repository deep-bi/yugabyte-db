# This library replaces PAM only in test processes. Never ship or install it
NO_INSTALL = 1

subdir = src/test/modules/yb_test_pam_provider
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
ifeq ($(PORTNAME),linux)
ifneq (,$(filter -lpam,$(LIBS)))
MODULE_big = yb_test_pam_provider
OBJS = yb_test_pam_provider.o
endif
endif
include $(top_srcdir)/contrib/contrib-global.mk
