all: ptstest2


clean::
	rm -f *.o *~ 
DEBUG=
AFSDIR=$(if $(wildcard /usr/local/include/afs/*.h),/usr/local,$(if $(wildcard /usr/afsws/include/afs/*.h),/usr/afsws,$(if $(wildcard /usr/include/afs/*.h),/usr,)))
ifeq ($(AFSDIR),)
$(error AFSDIR not set and could not be derived)
endif


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
CFLAGS+=-pthread -Wall -Wno-format -Wno-unused
endif

#CC=purify gcc

ptstest2: ptstest2.o 
	$(CC) -o $@ ptstest2.o ../lib/librxrxrpc.a -L$(AFSDIR)/lib/afs -lprot -lubik ../lib/librxrxrpc.a
ptstest2.o: ../include/rxrxrpc.h

