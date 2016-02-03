AM_CFLAGS = \
	@LIBTBM_SHM_CFLAGS@ \
	-I$(top_srcdir) \
	-I$(top_srcdir)/src

libtbm_shm_la_LTLIBRARIES = libtbm_shm.la
libtbm_shm_ladir = /${bufmgr_dir}
libtbm_shm_la_LIBADD = @LIBTBM_SHM_LIBS@

libtbm_shm_la_SOURCES = \
	tbm_bufmgr_shm.c
