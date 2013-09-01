all: librxrxrpc.a


clean::
	rm -f *.o openafs/*.o *~ librxrxrpc.a
DEBUG=
AFSDIR=$(if $(wildcard /usr/local/include/afs/*.h),/usr/local,$(if $(wildcard /usr/afsws/include/afs/*.h),/usr/afsws,$(if $(wildcard /usr/include/afs/*.h),/usr,)))
ifeq ($(AFSDIR),)
$(error AFSDIR not set and could not be derived)
endif


OPENAFS_OBJS=openafs/rx_misc.o openafs/rx_null.o openafs/xdr_afsuuid.o \
 openafs/xdr_array.o openafs/xdr_arrayn.o openafs/xdr.o \
 openafs/xdr_int32.o openafs/xdr_int64.o openafs/xdr_len.o openafs/xdr_mem.o \
 openafs/xdr_refernce.o openafs/xdr_rx.o openafs/xdr_update.o

OBJS=rxrxrpc.o

ifeq ($(origin CC),default)
ifneq ($(findstring Linux, $(UN)),)
CC=gcc
endif
endif

CFLAGS=-g -I../include
LDFLAGS=-L${AFSDIR}/lib/afs
ifneq ($(DEBUG),)
CFLAGS+=-DRXRXRPC_DEBUG
endif


ifneq ($(AFSDIR),/usr)
ifneq ($(findstring gcc, $(CC)),)
ifneq ($(AFSDIR),/usr/local)
CFLAGS+= -I${AFSDIR}/include 
LDFLAGS+= -L$(AFSDIR)/lib 
endif
else
CFLAGS+= -I${AFSDIR}/include 
LDFLAGS+= -L$(AFSDIR)/lib 
endif
endif

ifeq ($(findstring gcc, $(CC)),)
CFLAGS+= -I/usr/local/include
LDFLAGS+= -L/usr/local/lib
else
CFLAGS+=-Wall -Wno-format -Wno-unused
endif

#CC=purify gcc

librxrxrpc.a: $(OBJS) $(OPENAFS_OBJS)
	ar cru $@ $^

$(OBJS): ../include/rxrxrpc.h

