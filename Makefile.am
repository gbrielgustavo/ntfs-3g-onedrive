ACLOCAL_AMFLAGS = -I m4

EXTRA_DIST = README COPYING

plugindir = $(libdir)/ntfs-3g

plugin_LTLIBRARIES = ntfs-plugin-9000001a.la

ntfs_plugin_9000001a_la_SOURCES =	\
	src/onedrive.c

ntfs_plugin_9000001a_la_LDFLAGS  = -module -shared -avoid-version
ntfs_plugin_9000001a_la_CPPFLAGS = -D_FILE_OFFSET_BITS=64
ntfs_plugin_9000001a_la_CFLAGS   = $(LIBNTFS_3G_CFLAGS)
ntfs_plugin_9000001a_la_LIBADD   = $(LIBNTFS_3G_LIBS)
