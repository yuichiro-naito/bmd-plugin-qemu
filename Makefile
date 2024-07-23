MK_DEBUG_FILES=	no
SHLIB=		bmdplugin_qemu
SHLIB_MAJOR=	1
CFLAGS+=	-I${LOCALBASE}/include -DLOCALBASE=\"${LOCALBASE}\"
LIBDIR=		${LOCALBASE}/libexec/bmd
SRCS=		qemu.c
MAN=		bmd-plugin-qemu.8
MANDIR=		$(LOCALBASE)/share/man/man

WARNS?=		6

.include "Makefile.inc"
.include <bsd.lib.mk>
