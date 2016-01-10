AM_CFLAGS = \
	@LIBTBM_DUMB_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/src \
	-I/usr/include/drm

libtbm_dumb_la_LTLIBRARIES = libtbm_dumb.la
libtbm_dumb_ladir = /${bufmgr_dir}
libtbm_dumb_la_LIBADD = @LIBTBM_DUMB_LIBS@

libtbm_dumb_la_SOURCES = \
	tbm_bufmgr_dumb.c \
	tbm_wayland.c
