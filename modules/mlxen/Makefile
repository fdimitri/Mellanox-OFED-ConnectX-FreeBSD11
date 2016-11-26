# $FreeBSD$
.PATH:  ${.CURDIR}/../../ofed/drivers/net/mlx4

.include <bsd.own.mk>


.PATH:  ${.CURDIR}/../../ofed/include/linux

KMOD    = mlxen
SRCS    = device_if.h bus_if.h pci_if.h vnode_if.h opt_inet.h opt_inet6.h
SRCS+= utils.c en_main.c en_netdev.c en_rx.c en_cq.c en_tx.c en_port.c en_resources.c

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
