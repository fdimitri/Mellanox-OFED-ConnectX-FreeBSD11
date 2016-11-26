# $FreeBSD$
.PATH:  ${.CURDIR}/../../../drivers/infiniband/core
.PATH:  ${.CURDIR}/../../../include/linux

.include <bsd.own.mk>

KMOD    = ibcore
SRCS    =  cm_msgs.h mad_rmpp.h iwcm.h agent.h core_priv.h smi.h mad_priv.h sa.h uverbs.h
SRCS+= addr.c iwcm.c sa_query.c ucma.c uverbs_cmd.c 
SRCS+= agent.c multicast.c smi.c ud_header.c uverbs_main.c
SRCS+= mad.c umem.c uverbs_marshall.c peer_mem.c 
SRCS+= cache.c device.c packer.c sysfs.c user_mad.c verbs.c 
SRCS+= cm.c fmr_pool.c mad_rmpp.c ucm.c cma.c 
SRCS+= linux_compat.c linux_radix.c linux_idr.c
SRCS+= vnode_if.h device_if.h bus_if.h pci_if.h opt_inet.h opt_inet6.h

.if defined(CONFIG_RATELIMIT)
CFLAGS += -DCONFIG_RATELIMIT=1
.endif

CFLAGS+= -I${.CURDIR}/../../../drivers/infiniband/core
CFLAGS+= -I${.CURDIR}/../../../include
CFLAGS+= -DINET6 -DINET -DOFED

.include <bsd.kmod.mk>

CFLAGS+= -Wno-cast-qual -Wno-pointer-arith ${GCC_MS_EXTENSIONS} -Wno-tautological-compare

CWARNFLAGS.cm.c=	-Wno-unused-function
CWARNFLAGS+=		${CWARNFLAGS.${.IMPSRC:T}}
