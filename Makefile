LOCALBASE?= /usr/local
MK_DEBUG_FILES=no
LIB=bmdplugin_qemu
SHLIB_MAJOR=1
CFLAGS+=-I${LOCALBASE}/include -DLOCALBASE=${LOCALBASE}
LIBDIR=${LOCALBASE}/libexec/bmd
SRCS =  qemu.c

.include <bsd.lib.mk>
