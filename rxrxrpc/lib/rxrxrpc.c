/*
 * Copyright 2000, 2012, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */



#define _XOPEN_SOURCE 500
#define _GNU_SOURCE /* epoll */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <assert.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <afs/stds.h>
#include <afs/errors.h>
#include "rxrxrpc.h"
#include <keyutils.h>

#ifndef MIN
#define MIN(a,b)  ((a)<(b)?(a):(b))
#endif
/* Totally not thread safe */
/* May need a listener, or may need 1 socket *per call* so rx_Read can just
   recvmsg */


#ifndef AF_RXRPC
#define AF_RXRPC                33
#endif
#ifndef PF_RXRPC
#define PF_RXRPC                AF_RXRPC
#endif
#define SOL_RXRPC               272
#define RXRPC_USER_CALL_ID      1       /* User call ID specifier */
#define RXRPC_ABORT             2       /* Abort request / notification */
#define RXRPC_ACK               3       /* [Server] RPC op final ACK received */
#define RXRPC_RESPONSE          4       /* [Server] security response received */
#define RXRPC_NET_ERROR         5       /* network error received */
#define RXRPC_BUSY              6       /* server busy received */
#define RXRPC_LOCAL_ERROR       7       /* local error generated */
#define RXRPC_PREPARE_CALL_SLOT 8       /* Propose user call ID specifier for next call */
#define RXRPC_SECURITY_KEY              1       /* [clnt] set client security key */
#define RXRPC_SECURITY_KEYRING          2       /* [srvr] set ring of server security keys *//
#define RXRPC_EXCLUSIVE_CONNECTION      3       /* [clnt] use exclusive RxRPC connection */
#define RXRPC_MIN_SECURITY_LEVEL        4       /* minimum security level */

#define RXRPC_ADD_CALLID(control, ctrllen, id)                          \
do {                                                                    \
        struct cmsghdr *__cmsg;                                         \
        __cmsg = (void *)(control) + (ctrllen);                         \
        __cmsg->cmsg_len        = CMSG_LEN(sizeof(unsigned long));      \
        __cmsg->cmsg_level      = SOL_RXRPC;                            \
        __cmsg->cmsg_type       = RXRPC_USER_CALL_ID;                   \
        *(unsigned long *)CMSG_DATA(__cmsg) = (id);                     \
        (ctrllen) += __cmsg->cmsg_len;                                  \
                                                                        \
} while (0)

#define RXRPC_ADD_ABORT(control, ctrllen, abort_code)                   \
do {                                                                    \
        struct cmsghdr *__cmsg;                                         \
        __cmsg = (void *)(control) + (ctrllen);                         \
        __cmsg->cmsg_len        = CMSG_LEN(sizeof(unsigned long));      \
        __cmsg->cmsg_level      = SOL_RXRPC;                            \
        __cmsg->cmsg_type       = RXRPC_ABORT;                          \
        *(unsigned long *)CMSG_DATA(__cmsg) = (abort_code);             \
        (ctrllen) += __cmsg->cmsg_len;                                  \
                                                                        \
} while (0)
static int rxi_SendCallAbort(struct rx_call *call);
void rxi_FlushWrite(struct rx_call *call);
static int rx_Inited;
static int rx_port=-1;
static struct rx_fd *rx_socket_obj;

static int epollfd;

#define HASH_TABLE_SIZE 257

static pthread_mutexattr_t rx_mutexattr;
static pthread_mutex_t rx_InitLock=PTHREAD_MUTEX_INITIALIZER;
static pthread_t rx_Listener_pid;
struct rx_queue *rx_callHashTable[HASH_TABLE_SIZE];
static pthread_mutex_t rx_callHashTable_lock=PTHREAD_MUTEX_INITIALIZER;


static struct rx_call *rxi_FindCall(void *callid, struct sockaddr *sa) {
    struct sockaddr_rxrpc *srx;
    struct sockaddr_in *sin;
    int h;
    struct rx_call *c, *nc;
    char netbuf[INET_ADDRSTRLEN];
    const char *m;
    if (sa->sa_family == AF_INET) {
	srx=(struct sockaddr_rxrpc *)sa;
	sa=(struct sockaddr *)&srx->transport;
    }
    /*    if (sa->sa_family == AF_INET) {
	sin=(struct sockaddr_in *)sa;
    } else {
	return NULL;
	}*/
#ifdef DEBUG_LISTENER
    m=inet_ntop(sa->sa_family, sa->sa_data, netbuf, INET_ADDRSTRLEN);
    if (m)
      printf("Call from %s\n", m);
#endif
    h=((intptr_t)callid) % HASH_TABLE_SIZE;
#ifdef DEBUG_LISTENER
    printf("%p => %d\n", callid, h);
#endif
    assert(0==pthread_mutex_lock(&rx_callHashTable_lock));
    for (queue_Scan(&rx_callHashTable[h], c, nc, rx_call)) {
#ifdef DEBUG_LISTENER
      printf("Trying %p %p\n", c, c->kernel_id);
#endif
	if (c->kernel_id==callid) {
	    assert(0==pthread_mutex_lock(&c->lock));
	    assert(0==pthread_mutex_unlock(&rx_callHashTable_lock));
	    return c;
	}
    }
    assert(0==pthread_mutex_unlock(&rx_callHashTable_lock));
    return NULL;
}

static void *rx_Listener(void *a) {
    struct rx_pkt *pktbuf=NULL;
    
    for (;;) {
	struct sockaddr_storage ss;
	char controlbuf[256];
	struct epoll_event ev[10];
	int i, count;
	struct msghdr mh;
	struct iovec iov[1];
	int code;
	int ctrls;
	struct cmsghdr *cmsg;
	struct rx_call *call;
	ssize_t datalen;
	
	count=epoll_wait(epollfd, ev, 10, -1);
	if (count < 0) {
	    usleep(1000);
	    continue;
	}
#ifdef DEBUG_LISTENER
	printf("Got a wakeup\n");
#endif
	for (i=0; i<count;i++) {
	    while (!pktbuf) {
		pktbuf=calloc(1, sizeof(struct rx_pkt));
		if (!pktbuf)
		    usleep(1000);
	    }
	    iov[0].iov_base=&pktbuf->data;
	    iov[0].iov_len=sizeof(pktbuf->data);
	    mh.msg_name    = &ss;
	    mh.msg_namelen = sizeof(ss);
	    mh.msg_iov     = iov;
	    mh.msg_iovlen  = 1;
	    mh.msg_control = controlbuf;
	    mh.msg_controllen = sizeof(controlbuf);
	    mh.msg_flags   = 0;
	    
	    datalen=recvmsg(ev[i].data.fd, &mh, 0);
	    if (datalen == -1) {
		if (errno == EAGAIN)
		    continue;
		usleep(1000);
		continue;
	    }
#ifdef DEBUG_LISTENER
	    printf("Got a message\n");
#endif
	    if (mh.msg_flags & (MSG_TRUNC|MSG_CTRUNC|MSG_OOB|MSG_ERRQUEUE))
		abort();
	    call=NULL;
	    ctrls=0;
	    for (cmsg = CMSG_FIRSTHDR(&mh); cmsg; cmsg = CMSG_NXTHDR(&mh, cmsg)) {
		int n = cmsg->cmsg_len - CMSG_ALIGN(sizeof(*cmsg));
		void *p = CMSG_DATA(cmsg);
		if (cmsg->cmsg_level == SOL_RXRPC) {
		    unsigned long callid;
		    switch(cmsg->cmsg_type) {
		    case RXRPC_USER_CALL_ID:
			if (ctrls & 1) /* multiple CALL_ID */
			    goto next_pkt;
			if (n != sizeof(uintptr_t))
			    goto next_pkt;
			memcpy(&callid, p, n);
#ifdef DEBUG_LISTENER
			printf("Got callid 0x%lx\n", callid);
#endif
			call=rxi_FindCall((void *)callid, (struct sockaddr *)&ss);
			if (!call)
			    goto next_pkt;
#ifdef DEBUG_LISTENER
			printf("Got call %p\n", call);
#endif
			ctrls|=1;
			break;
		    case RXRPC_ABORT:
		    case RXRPC_NET_ERROR:
		    case RXRPC_LOCAL_ERROR:
			if (n != sizeof(afs_int32))
			    goto next_pkt;
			if (!call)
			    goto next_pkt;
			memcpy(&call->error, p, sizeof(afs_int32));
			call->mode=RX_MODE_ERROR;
			if (call->flags & RX_CALL_READER_WAIT)
			    assert(0==pthread_cond_signal(&call->cv_rq));
			printf("Got an error\n");
			goto next_pkt;
		    case RXRPC_BUSY:
			if (!call)
			    goto next_pkt;
			call->error=RX_CALL_BUSY;
			call->mode=RX_MODE_ERROR;
			if (call->flags & RX_CALL_READER_WAIT)
			    assert(0==pthread_cond_signal(&call->cv_rq));
			printf("Got a busy\n");
			goto next_pkt;
		    case RXRPC_ACK:
			/*case RXRPC_NEW_CALL:*/
		    case RXRPC_RESPONSE:
			/* servers not supported here */
			abort();
		    }
		}
	    }
	    if (call) {
		pktbuf->datalen=datalen;
		queue_Append(&call->rq, pktbuf);
		pktbuf=NULL;
#if 0 /* too cute */
		if (!call->pktinuse && !call->nLeft && call->currentPacket) {
		    pktbuf=call->currentPacket;
		    call->currentPacket=NULL;
		}
#endif
		if (call->flags & RX_CALL_READER_WAIT)
		    assert(0==pthread_cond_signal(&call->cv_rq));
		if (mh.msg_flags & MSG_EOR) 
		    call->flags |= RX_CALL_RECEIVE_DONE;
	    }
	next_pkt:
	    if (call)
	      assert(0==pthread_mutex_unlock(&call->lock));
	}
	
    }
}


int rx_Init(unsigned int port) {
    int i;
    if (pthread_mutex_lock(&rx_InitLock))
	return -1;
    if (rx_Inited) {
	pthread_mutex_unlock(&rx_InitLock);
	return rx_port == port ? 0 : -1;
    }
    assert(0==pthread_mutexattr_init(&rx_mutexattr));

    rx_port=port;
    epollfd=epoll_create(10);
    if (epollfd == -1) {
      rx_port=-1;
      pthread_mutex_unlock(&rx_InitLock);
      return -1;
    }

    rx_socket_obj = rxi_AllocClientSocket();
    if (!rx_socket_obj) {
	close(epollfd);
	rx_port=-1;
	pthread_mutex_unlock(&rx_InitLock);
	return -1;
    }
    rx_Inited=1;
    for (i=0; i< HASH_TABLE_SIZE; i++) 
      queue_Init(&rx_callHashTable[i]);
    assert(0==pthread_create(&rx_Listener_pid, NULL, &rx_Listener, NULL));
    pthread_mutex_unlock(&rx_InitLock);
    return 0;
}



struct rx_connection *
rx_NewConnection(afs_uint32 shost, u_int16_t sport, u_int16_t sservice,
                 struct rx_securityClass *securityObject,
                 int serviceSecurityIndex)
{
  struct rx_connection *ret=calloc(sizeof(struct rx_connection), 1);
  if (!ret)
    return NULL;
  ret->type = RX_CLIENT_CONNECTION;
  ret->serviceId = sservice;
  ret->securityObject = securityObject;
  ret->securityData = (void *) 0;
  ret->securityIndex = serviceSecurityIndex;
  ret->error = 0;
  ret->socket=rxi_GetSocket(rx_socket_obj);
  ret->target.srx_family = AF_RXRPC;
  ret->target.srx_service = sservice;
  ret->target.transport_type = SOCK_DGRAM;
  ret->target.transport_len = sizeof(ret->target.transport.sin);
  ret->target.transport.sin.sin_family = AF_INET;
  ret->target.transport.sin.sin_addr.s_addr=htonl(shost);
  ret->target.transport.sin.sin_port = htons(sport);
  pthread_mutex_init(&ret->conn_call_lock, &rx_mutexattr);
  pthread_cond_init(&ret->conn_call_cv, NULL);
  pthread_mutex_init(&ret->conn_data_lock, &rx_mutexattr);

  RXS_NewConnection(securityObject, ret);
  return ret;
}


static void rxi_CleanupConnection(struct rx_connection *conn)
{
    RXS_DestroyConnection(conn->securityObject, conn);
    if (conn->specific) {
        int i;
        for (i = 0; i < conn->nSpecific; i++) {
#if 0
            if (conn->specific[i] && rxi_keyCreate_destructor[i])
                (*rxi_keyCreate_destructor[i]) (conn->specific[i]);
#endif
            conn->specific[i] = NULL;
        }
        free(conn->specific);
    }
    conn->specific = NULL;
    conn->nSpecific = 0;
    pthread_mutex_destroy(&conn->conn_data_lock);
    pthread_cond_destroy(&conn->conn_call_cv);
    pthread_mutex_destroy(&conn->conn_call_lock);
    free(conn);
}


/* Destroy the specified connection */
void
rx_DestroyConnection(struct rx_connection *conn)
{
  int i;
  pthread_mutex_lock(&conn->conn_call_lock);
  if ((conn->type == RX_CLIENT_CONNECTION)
      && (conn->flags & 
	  (RX_CONN_MAKECALL_WAITING|RX_CONN_MAKECALL_ACTIVE))) {
    conn->flags |= RX_CONN_DESTROY_ME;
    pthread_mutex_unlock(&conn->conn_call_lock);
    return;
  }
  for (i = 0; i < RX_MAXCALLS; i++) {
    struct rx_call *call = conn->call[i];
    if (call) {
      pthread_mutex_unlock(&conn->conn_call_lock);
      conn->flags |= RX_CONN_DESTROY_ME;
      return;
    }
  }
  pthread_mutex_unlock(&conn->conn_call_lock);
  rxi_CleanupConnection(conn);
}


struct rx_call *
rx_NewCall(struct rx_connection *conn)
{
    struct rx_call *call;
    int h;
    pthread_mutex_lock(&conn->conn_call_lock);
    while (conn->flags & RX_CONN_MAKECALL_ACTIVE) {
	conn->flags |= RX_CONN_MAKECALL_WAITING;
	conn->makeCallWaiters++;
	pthread_cond_wait(&conn->conn_call_cv, &conn->conn_call_lock);
	conn->makeCallWaiters--;
	if (conn->makeCallWaiters == 0)
	    conn->flags &= ~RX_CONN_MAKECALL_WAITING;
    }
    conn->flags |= RX_CONN_MAKECALL_ACTIVE;
    for (;;) {
	int i;
	for (i = 0; i < RX_MAXCALLS; i++) {
	    call = conn->call[i];
	    if (!call) {
		call = calloc(sizeof(struct rx_call), 1);
		call->kernel_id=call;
		call->conn=conn;
		queue_Init(&call->rq);
		assert(0==pthread_mutex_init(&call->lock, &rx_mutexattr));
		assert(0==pthread_cond_init(&call->cv_rq, NULL));
		break;
	    }
	}
	if (i < RX_MAXCALLS) {
	    break;
	}
	conn->flags |= RX_CONN_MAKECALL_WAITING;
	conn->makeCallWaiters++;
	pthread_cond_wait(&conn->conn_call_cv, &conn->conn_call_lock);
	conn->makeCallWaiters--;
	if (conn->makeCallWaiters == 0)
	    conn->flags &= ~RX_CONN_MAKECALL_WAITING;
    }
    call->error = conn->error;
    if (call->error)
	call->mode = RX_MODE_ERROR;
    else
	call->mode = RX_MODE_SENDING;
    conn->flags &= ~RX_CONN_MAKECALL_ACTIVE;
    call->owner=pthread_self();
    pthread_cond_signal(&conn->conn_call_cv);
    pthread_mutex_unlock(&conn->conn_call_lock);
    assert(0==pthread_mutex_lock(&rx_callHashTable_lock));
    h=((uintptr_t)call->kernel_id) % HASH_TABLE_SIZE;
    queue_Prepend(&rx_callHashTable[h], call);
    assert(0==pthread_mutex_unlock(&rx_callHashTable_lock));
    return call;
}


static int
rxi_GetNextPacket(struct rx_call *call) {
    int code;
    
    if (call->error || call->mode != RX_MODE_RECEIVING) {
	return -1;
    }
    if (call->flags & RX_CALL_RECEIVE_DONE)
	return 0;
    if (call->currentPacket) {
	free(call->currentPacket);
	call->currentPacket=NULL;
    }
    while (queue_IsEmpty(&call->rq)) {
	call->flags |= RX_CALL_READER_WAIT;
	assert(0==pthread_cond_wait(&call->cv_rq, &call->lock));
	call->flags &= ~RX_CALL_READER_WAIT;
	if (call->mode == RX_MODE_ERROR) {
	    return -1;
	}
    }
    call->currentPacket=queue_First(&call->rq, rx_pkt);
    queue_Remove(call->currentPacket);
    call->curpos=call->currentPacket->data;
    call->nLeft = call->currentPacket->datalen;
    return 0;
}

static int
rxi_SendData(struct rx_call *call, int last) {
  char controlbuf[256];
  size_t controllen=0;
  struct msghdr mh;
  struct iovec iov[1];
  int code;
  iov[0].iov_base=call->currentPacket->data;
  iov[0].iov_len=call->currentPacket->datalen-call->nFree;

  RXRPC_ADD_CALLID(controlbuf, controllen, ((unsigned long)call->kernel_id));

  mh.msg_name    = &call->conn->target;
  mh.msg_namelen = sizeof(call->conn->target);
  mh.msg_iov     = iov;
  mh.msg_iovlen  = 1;
  mh.msg_control = controlbuf;
  mh.msg_controllen = controllen;
  mh.msg_flags   = 0;

  code=sendmsg(call->conn->socket->fd, &mh, last?0:MSG_MORE);
  if (code < 0)
    call->error=errno;
  return code > 0 ? 0 : code;
}

static int
rxi_SendCallAbort(struct rx_call *call) {
  char controlbuf[256];
  size_t controllen=0;
  struct msghdr mh;
  int abortcode, code;

  abortcode=call->error;
  if (abortcode == RX_CALL_IDLE || abortcode == RX_CALL_BUSY)
      abortcode = RX_CALL_TIMEOUT;

  RXRPC_ADD_CALLID(controlbuf, controllen, ((unsigned long)call->kernel_id));
  RXRPC_ADD_ABORT(controlbuf, controllen, abortcode);

  mh.msg_name    = NULL;
  mh.msg_namelen = 0;
  mh.msg_iov     = NULL;
  mh.msg_iovlen  = 0;
  mh.msg_control = controlbuf;
  mh.msg_controllen = controllen;
  mh.msg_flags   = 0;

  code=sendmsg(call->conn->socket->fd, &mh, 0);
  return code;
}
  
int
rxi_ReadProc(struct rx_call *call, char *buf,
             int nbytes)
{
    int code;
    int requestCount=nbytes;
    do {
	if (call->nLeft == 0) {
	    /* Get next packet */
	    for (;;) {
		if (call->mode == RX_MODE_SENDING) {
		    rxi_FlushWrite(call);
		    continue;
		}
		if (call->error || (call->mode != RX_MODE_RECEIVING)) {
		    call->mode = RX_MODE_ERROR;
		    if (!call->error)
			call->error=RX_PROTOCOL_ERROR;
		    return 0;
		}
		code = rxi_GetNextPacket(call);
		if (code)
		    return 0;
		if (call->nLeft)
		    break;
		if (call->flags & RX_CALL_RECEIVE_DONE)
		    return requestCount - nbytes;
		abort();
	    }
	}
	while (nbytes && call->nLeft) {
	    unsigned int t = MIN((int)call->nLeft, nbytes);
	    memcpy(buf, call->curpos, t);
	    buf += t;
	    nbytes -= t;
	    call->curpos += t;
	    call->nLeft -= t;
	}
    } while (nbytes);

    return requestCount;
}

int
rx_ReadProc(struct rx_call *call, char *buf, int nbytes) {
    int ret;
    assert(0==pthread_mutex_lock(&call->lock));
    ret= rxi_ReadProc(call, buf, nbytes);
    assert(0==pthread_mutex_unlock(&call->lock));
    return ret;
}


int
rx_ReadProc32(struct rx_call *call, afs_int32 * value)
{
    int ret;
    /*
     * Most common case, all of the data is in the current iovec.
     * We are relying on nLeft being zero unless the call is in receive mode.
     */
    assert(pthread_equal(pthread_self(), call->owner));
    if (!call->error && call->nLeft >= sizeof(afs_int32)) {
	memcpy((char *)value, call->curpos, sizeof(afs_int32));
	call->curpos += sizeof(afs_int32);
	call->nLeft  -= sizeof(afs_int32);
	return sizeof(afs_int32);
    }
    assert(0==pthread_mutex_lock(&call->lock));
    ret = rxi_ReadProc(call, (char *)value, sizeof(afs_int32));
    assert(0==pthread_mutex_unlock(&call->lock));
    return ret;
}


int
rxi_WriteProc(struct rx_call *call, char *buf,
              int nbytes)
{
    struct rx_connection *conn = call->conn;
    int requestCount = nbytes;
    int code;

    if (call->mode != RX_MODE_SENDING) {
      if ((conn->type == RX_SERVER_CONNECTION)
	  && (call->mode == RX_MODE_RECEIVING)) {
	call->mode = RX_MODE_SENDING;
	call->nFree=0;
	call->pktinuse=0;
      } else {
	  return 0;
      }
    }

    do {
      if (call->nFree == 0) {
	if (call->error) {
	  call->mode = RX_MODE_ERROR;
	  return 0;
	}
	code=0;
	if (call->pktinuse) 
	  code=rxi_SendData(call, 0);
	if (code || call->error) {
	  call->mode = RX_MODE_ERROR;
	  return 0;
	}
	if (!call->currentPacket) {
	    call->currentPacket=calloc(1, sizeof(struct rx_pkt));
	    if (!call->currentPacket) {
		call->mode = RX_MODE_ERROR;
		return 0;
	    }	
	    call->currentPacket->datalen=sizeof(call->currentPacket->data);
	}
	call->nFree=call->currentPacket->datalen;
	call->curpos=call->currentPacket->data;
      }
      
      while (nbytes && call->nFree) {

	unsigned int t = MIN((int)call->nFree, nbytes);
	call->pktinuse=1;
	memcpy(call->curpos, buf, t);
	buf += t;
	nbytes -= t;
	call->curpos += t;
	call->nFree -= t;
      }
    } while (nbytes);
    
    return requestCount - nbytes;
}


int
rx_WriteProc(struct rx_call *call, char *buf, int nbytes) {
    int ret;
    assert(0==pthread_mutex_lock(&call->lock));
    ret=rxi_WriteProc(call, buf, nbytes);
    assert(0==pthread_mutex_lock(&call->lock));
    return ret;
}

/* Optimization for marshalling 32 bit arguments */
int
rx_WriteProc32(struct rx_call *call, afs_int32 * value)
{
    int ret;
    assert(pthread_equal(pthread_self(), call->owner));
    if (!call->error && call->nFree >= sizeof(afs_int32)) {
      memcpy(call->curpos, (char *)value, sizeof(afs_int32));
      call->curpos += sizeof(afs_int32);
      call->nFree  -= sizeof(afs_int32);
      return sizeof(afs_int32);
    }
    assert(0==pthread_mutex_lock(&call->lock));
    ret=rxi_WriteProc(call, (char *)value, sizeof(afs_int32));
    assert(0==pthread_mutex_unlock(&call->lock));
    return ret;
}

void
rxi_FlushWrite(struct rx_call *call)
{
    if (call->mode == RX_MODE_SENDING) {

      call->mode =
	(call->conn->type ==
	 RX_CLIENT_CONNECTION ? RX_MODE_RECEIVING : RX_MODE_EOF);
      if (call->error)
	call->mode = RX_MODE_ERROR;
      rxi_SendData(call, 1);
    }
}

void
rx_FlushWrite(struct rx_call *call)
{
    assert(0==pthread_mutex_lock(&call->lock));
    rxi_FlushWrite(call);
    assert(0==pthread_mutex_unlock(&call->lock));
}

afs_int32
rx_EndCall(struct rx_call *call, afs_int32 rc)
{
    struct rx_connection *conn = call->conn;
    afs_int32 error;

    assert(0==pthread_mutex_lock(&rx_callHashTable_lock));
    queue_Remove(call);
    assert(0==pthread_mutex_unlock(&rx_callHashTable_lock));

    assert(0==pthread_mutex_lock(&conn->conn_call_lock));
    assert(0==pthread_mutex_lock(&call->lock));
    if (rc == 0 && call->error == 0) {
        call->abortCode = 0;
        call->abortCount = 0;
    }
    if (rc && call->error == 0) {
      call->error=rc;
      call->mode = RX_MODE_ERROR;
      rxi_SendCallAbort(call);
    }
    if (conn->type == RX_SERVER_CONNECTION) {
      /* Make sure reply or at least dummy reply is sent */
      if (call->mode == RX_MODE_RECEIVING) {
	rxi_WriteProc(call, 0, 0);
      }
      if (call->mode == RX_MODE_SENDING) {
	rxi_FlushWrite(call);
      }
    } else {
      if ((call->mode == RX_MODE_SENDING)
	  || (call->mode == RX_MODE_RECEIVING && call->rnext == 1)) {
	char dummy;
	(void)rxi_ReadProc(call, &dummy, 1);
      }
    }
    conn->call[call->channel]=NULL;
    error = call->error;
    while (!queue_IsEmpty(&call->rq)) {
	struct rx_pkt *pkt=queue_First(&call->rq, rx_pkt);
	queue_Remove(pkt);
	free(pkt);
    }
    if (call->currentPacket)
      free(call->currentPacket);
    assert(0==pthread_mutex_unlock(&call->lock));
    assert(0==pthread_mutex_destroy(&call->lock));
    assert(0==pthread_cond_destroy(&call->cv_rq));
    free(call);
    conn->flags |= RX_CONN_BUSY;
    if (conn->flags & RX_CONN_MAKECALL_WAITING) {
	pthread_cond_signal(&conn->conn_call_cv);
    }
    if (conn->type == RX_CLIENT_CONNECTION) {
      conn->flags &= ~RX_CONN_BUSY;
    }
    assert(0==pthread_mutex_unlock(&conn->conn_call_lock));
    error = ntoh_syserr_conv(error);
    return error;
}

struct rx_fd *rxi_AllocClientSocket(void) {
    struct sockaddr_rxrpc srx;
    struct rx_fd *ret=calloc(1, sizeof(struct rx_fd));
    struct epoll_event ev;
    
    if (!ret)
	return NULL;
    
    ret->fd=socket(AF_RXRPC, SOCK_DGRAM, PF_INET);
    if (ret->fd <0) {
	free(ret);
	return NULL;
    }
    /* bind an address to the local endpoint */
    memset(&srx, 0, sizeof(srx));
    srx.srx_family = AF_RXRPC;
    srx.srx_service = 0; /* it's a client */
    srx.transport_type = SOCK_DGRAM;
    srx.transport_len = sizeof(srx.transport.sin);
    srx.transport.sin.sin_family = AF_INET;
    srx.transport.sin.sin_addr.s_addr=htonl(INADDR_ANY);
    srx.transport.sin.sin_port = htons(rx_port);
    
    if (bind(ret->fd, (struct sockaddr *) &srx, sizeof(srx)) < 0) {
	close(ret->fd);
	free(ret);
	return NULL;
    }
    memset(&ev, 0, sizeof(ev));
    ev.events=EPOLLIN;
    ev.data.fd=ret->fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, ret->fd, &ev)) {
	close(ret->fd);
	free(ret);
	return NULL;
    }
    ret->refcnt=1;
    return ret;
}

struct rx_fd *rxi_GetSocket(struct rx_fd *in) {
    if (in->refcnt <1) 
	abort();
    in->refcnt++;
    return in;
}

void rxi_PutSocket(struct rx_fd *in) {
    if (in->refcnt < 1)
	abort();
    if (0 == --in->refcnt) {
	struct epoll_event ev;
	memset(&ev, 0, sizeof(ev));
	epoll_ctl(epollfd, EPOLL_CTL_DEL, in->fd, &ev);
	close(in->fd);
	free(in);
    }
}
