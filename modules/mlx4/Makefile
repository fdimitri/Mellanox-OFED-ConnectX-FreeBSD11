# $FreeBSD$
.PATH:  ${.CURDIR}/../../ofed/drivers/net/mlx4

.include <bsd.own.mk>

.PATH:  ${.CURDIR}/../../ofed/include/linux
KMOD    = mlx4
SRCS    = device_if.h bus_if.h pci_if.h vnode_if.h opt_inet.h opt_inet6.h
SRCS+=  alloc.c catas.c cmd.c cq.c eq.c fw.c icm.c intf.c main.c mcg.c mr.c linux_compat.c linux_radix.c linux_idr.c
SRCS+=	pd.c port.c profile.c qp.c reset.c sense.c srq.c resource_tracker.c sys_tune.c

CFLAGS+= -I${.CURDIR}/../../ofed/drivers/net/mlx4
CFLAGS+= -I${.CURDIR}/../../ofed/include

.if defined(CONFIG_RATELIMIT)
CFLAGS += -DCONFIG_RATELIMIT=1
CFLAGS += -DCONFIG_WQE_FORMAT_1=1
.endif

opt_inet.h:
	@echo "#define INET 1" > ${.TARGET}

opt_inet6.h:
	@echo "#define INET6 1" > ${.TARGET}

.include <bsd.kmod.mk>

CFLAGS+= -Wno-cast-qual -Wno-pointer-arith ${GCC_MS_EXTENSIONS}
CFLAGS+= -fms-extensions

CWARNFLAGS.mcg.c=	-Wno-unused
CWARNFLAGS+=		${CWARNFLAGS.${.IMPSRC:T}}
