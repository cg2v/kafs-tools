/*
 * Copyright 2000, 2012, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */



#define _XOPEN_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
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
static int rx_socket=-1;
static int rx_port=-1;

static int epollfd;

/* write here to wakeup listener to detect changes in the listenfds array */
static int wakeup_listener_fd;

static pthread_mutext_t rx_InitLock=PTHREAD_MUTEX_INITIALIZER;


static void *rx_Listener(void *a) {
    struct rx_pkt *pktbuf=NULL;
    
    for (;;) {
	struct sockaddr_storage ss;
	char controlbuf[256];
	struct epoll_events ev[10];
	int i;
	struct msghdr mh;
	struct iovec iov[1];
	int code;
	int ctrls;
	struct cmsghdr *cmsg;
	struct rx_call *call;
	ssize_t datalen;
	
	count=epoll_wait(epollfd, &ev, 10, -1);
	if (count < 0) {
	    usleep(1000);
	    continue;
	}
	for (i=0; i<count;i++) {
	    if (!pktbuf)
		pktbuf=calloc(1, sizeof(struct rx_pkt));
	    if (!pktbuf) {
		usleep(1000);
		continue;
	    }
	    iov[0].iov_base=&pktbuf->data;
	    iov[0].iov_len=16384;
	    mh.msg_name    = &ss;
	    mh.msg_namelen = sizeof(ss);
	    mh.msg_iov     = iov;
	    mh.msg_iovlen  = 1;
	    mh.msg_control = controlbuf;
	    mh.msg_controllen = sizeof(controlbuf);
	    mh.msg_flags   = 0;
	    
	    datalen=recvmsg(ev[i].data.fd, &mh, 0);
	    if (datalen == -1) {
		usleep(1000);
		continue;
	    }
	    if (mh.msg_flags & (MSG_TRUNC|MSG_CTRUNC|MSG_OOB|MSG_ERRQUEUE))
		abort();
	    call=NULL;
	    ctrls=0;
	    for (cmsg = CMSG_FIRSTHDR(&mh); cmsg; cmsg = CMSG_NXTHDR(&mh, cmsg)) {
		int n = cmsg->cmsg_len - CMSG_ALIGN(sizeof(*cmsg));
		void *p = CMSG_DATA(cmsg);
		if (cmsg->cmsg_level == SOL_RXRPC) {
		    switch(cmsg->cmsg_type) {
		    case RXRPC_USER_CALL_ID:
			unsigned long call_id;
			if (ctrls & 1) /* multiple CALL_ID */
			    goto next_pkt;
			if (n != sizeof(long))
			    goto next_pkg;
			memcpy(&callid, p, n);
			call=rxi_FindCall(callid, (struct sockaddr *)&ss);
			if (!call)
			    goto next_pkt;
			ctrls|=1;
			break;
		    case RXRPC_ABORT:
		    case RXRPC_NET_ERROR:
		    case RXRPC_LOCAL_ERROR:
			if (n != sizeof(afs_int32))
			    goto next_pkt;
			if (!call)
			    goto next_pkt;
			assert(0==pthread_mutex_lock(&call->lock));
			memcpy(&call->error, p, sizeof(afs_int32));
			call->mode=RX_MODE_ERROR;
			if (call->flags & RX_CALL_READER_WAIT)
			    assert(0==pthread_cond_signal(&call->cv_rq));
			assert(0==pthread_mutex_unlock(&call->lock));
			goto next_pkt;
		    case RXRPC_BUSY:
			if (!call)
			    goto next_pkt;
			assert(0==pthread_mutex_lock(&call->lock));
			call->error=RX_CALL_BUSY;
			call->mode=RX_MODE_ERROR;
			if (call->flags & RX_CALL_READER_WAIT)
			    assert(0==pthread_cond_signal(&call->cv_rq));
			assert(0==pthread_mutex_unlock(&call->lock));
			goto next_pkt;
		    case RXRPC_ACK:
		    case RXRPC_NEW_CALL:
			/* servers not supported here */
			abort();
		    }
		}
	    }
	    if (call) {
		assert(0==pthread_mutex_lock(&call->lock));
		pktbuf->datalen=datalen;
		queue_Append(&call->rq, pktbuf);
		pktbuf=NULL;
		if (call->flags & RX_CALL_READER_WAIT)
		    assert(0==pthread_cond_signal(&call->cv_rq));
		if (mh.msg_flags & MSG_EOR) 
		    call->flags |= RX_CALL_RECEIVE_DONE;
		assert(0==pthread_mutex_unlock(&call->lock));
	    }
	next_pkt:
	}
	
    }
    
}


int rx_Init(unsigned int port) {
  struct sockaddr_rxrpc srx;
  if (pthread_mutex_lock(&rx_InitLock))
    return -1;
  if (rx_Inited) {
    pthread_mutex_unlock(&rx_InitLock);
    return -1;
  }
  pthread_mutex_unlock(&rx_InitLock);
  rx_port=port;
  rx_socket=socket(AF_RXRPC, SOCK_DGRAM, PF_INET);
  if (rx_socket <0)
    return -1;
  /* bind an address to the local endpoint */
  srx.srx_family = AF_RXRPC;
  srx.srx_service = 0; /* it's a client */
  srx.transport_type = SOCK_DGRAM;
  srx.transport_len = sizeof(srx.transport.sin);
  srx.transport.sin.sin_family = AF_INET;
  srx.transport.sin.sin_addr.s_addr=htonl(INADDR_ANY);
  srx.transport.sin.sin_port = htons(rx_port);
  
  if (bind(rx_socket, (struct sockaddr *) &srx, sizeof(srx)) < 0) {
    return -1;
  }
#if 0 /* does not work */
  if (rx_port == 0) {
    socklen_t srxs=sizeof(srx);
    if (0 == getsockname(rx_socket, (struct sockaddr *) &srx, &srxs))
      rx_port=ntohs(srx.transport.sin.sin_port);
  }
#endif
  rx_Inited=1;
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
  ret->socket=rx_socket;
  ret->target.srx_family = AF_RXRPC;
  ret->target.srx_service = sservice;
  ret->target.transport_type = SOCK_DGRAM;
  ret->target.transport_len = sizeof(ret->target.transport.sin);
  ret->target.transport.sin.sin_family = AF_INET;
  ret->target.transport.sin.sin_addr.s_addr=htonl(shost);
  ret->target.transport.sin.sin_port = htons(sport);
  pthread_mutex_init(&ret->conn_call_lock, NULL);
  pthread_cond_init(&ret->conn_call_cv, NULL);
  pthread_mutex_init(&ret->conn_data_lock, NULL);

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
  int wait;
  struct rx_call *call;
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
    wait = 1;
    int i;
    for (i = 0; i < RX_MAXCALLS; i++) {
      call = conn->call[i];
      if (!call) {
	call = calloc(sizeof(struct rx_call), 1);
	call->kernel_id=&call;
	call->conn=conn;
	call->channel=i;
	call->callNumber = &conn->callNumber[i];
	conn->call[i] = call;
	/* if the channel's never been used (== 0), we should start at 1, otherwise
	 * the call number is valid from the last time this channel was used */
	if (*call->callNumber == 0)
	  *call->callNumber = 1;
	break;
      }
    }
    if (i < RX_MAXCALLS) {
      break;
    }
    if (!wait)
      continue;
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
  pthread_cond_signal(&conn->conn_call_cv);
  pthread_mutex_unlock(&conn->conn_call_lock);
  return call;
}


static int
rxi_GetNextPacket(struct rx_call *call) {
    int code;
    
    assert(0==pthread_mutex_lock(&call->lock));
    if (call->error || call->mode != RX_MODE_RECEIVING) {
	assert(0==pthread_mutex_unlock(&call->lock));
	return -1;
    }
    
    if (call->currentPacket) {
	free(call->currentPacket);
	call->currentPacket=NULL;
    }
    while (queue_IsEmpty(&call->rq)) {
	call->flags |= RX_CALL_READER_WAIT;
	assert(0==pthread_cond_wait(&call->cv_rq, &call->lock));
	call->flags &= ~RX_CALL_READER_WAIT;
	if (call->mode == RX_MODE_ERROR) {
	    assert(0==pthread_mutex_unlock(&call->lock));
	    return -1;
	}
    }
    call->currentPacket=queue_First(&call_rq, struct rx_pkt);
    queue_Remove(call->currentPacket);
    call->curpos=call->currentPacket.data;
    call->nLeft = call->currentPacket.datalen;
    assert(0==pthread_mutex_unlock(&call->lock));
    return 0;
}

static int
rxi_SendData(struct rx_call *call, int last) {
  char controlbuf[256];
  size_t controllen=0;
  struct msghdr mh;
  struct iovec iov[1];
  int code;
  iov[0].iov_base=call->currentPacket;
  iov[0].iov_len=sizeof(call->currentPacket)-call->nFree;

  RXRPC_ADD_CALLID(controlbuf, controllen, ((unsigned long)call->kernel_id));

  mh.msg_name    = &call->conn->target;
  mh.msg_namelen = sizeof(call->conn->target);
  mh.msg_iov     = iov;
  mh.msg_iovlen  = 1;
  mh.msg_control = controlbuf;
  mh.msg_controllen = controllen;
  mh.msg_flags   = 0;

  code=sendmsg(call->conn->socket, &mh, last?0:MSG_MORE);
  if (code < 0)
    call->error=errno;
  return code > 0 ? 0 : code;
}

static int
rxi_SendCallAbort(struct rx_call *call) {
  char controlbuf[256];
  size_t controllen=0;
  struct msghdr mh;
  int code;

  RXRPC_ADD_CALLID(controlbuf, controllen, ((unsigned long)call->kernel_id));
  RXRPC_ADD_ABORT(controlbuf, controllen, call->error);

  mh.msg_name    = NULL;
  mh.msg_namelen = 0;
  mh.msg_iov     = NULL;
  mh.msg_iovlen  = 0;
  mh.msg_control = controlbuf;
  mh.msg_controllen = controllen;
  mh.msg_flags   = 0;

  code=sendmsg(call->conn->socket, &mh, 0);
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
	if (call->error || (call->mode != RX_MODE_RECEIVING)) {
	  if (call->error) {
	    call->mode = RX_MODE_ERROR;
	    return 0;
	  }
	  if (call->mode == RX_MODE_SENDING) {
	    rxi_FlushWrite(call);
	    continue;
	  }
	}
	code = rxi_GetNextPacket(call);
	if (code)
	  return 0;
	if (call->nLeft)
	  break;
	if (call->flags & RX_CALL_RECEIVE_DONE)
	  return requestCount - nbytes;
	call->flags |= RX_CALL_READER_WAIT;
	while (call->flags & RX_CALL_READER_WAIT) {
	  abort();
	  /*sleep*/
	}
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
  return rxi_ReadProc(call, buf, nbytes);
}


int
rx_ReadProc32(struct rx_call *call, afs_int32 * value)
{
    /*
     * Most common case, all of the data is in the current iovec.
     * We are relying on nLeft being zero unless the call is in receive mode.
     */
  if (!call->error && call->nLeft >= sizeof(afs_int32)) {
    memcpy((char *)value, call->curpos, sizeof(afs_int32));
    call->curpos += sizeof(afs_int32);
    call->nLeft  -= sizeof(afs_int32);
    return sizeof(afs_int32);
  }
  
  return rxi_ReadProc(call, (char *)value, sizeof(afs_int32));
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
	call->nFree=sizeof(call->currentPacket);
	call->curpos=call->currentPacket;
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
  return rxi_WriteProc(call, buf, nbytes);
}

/* Optimization for marshalling 32 bit arguments */
int
rx_WriteProc32(struct rx_call *call, afs_int32 * value)
{
    if (!call->error && call->nFree >= sizeof(afs_int32)) {
      memcpy(call->curpos, (char *)value, sizeof(afs_int32));
      call->curpos += sizeof(afs_int32);
      call->nFree  -= sizeof(afs_int32);
      return sizeof(afs_int32);
    }
    return rxi_WriteProc(call, (char *)value, sizeof(afs_int32));
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
    rxi_FlushWrite(call);
}

afs_int32
rx_EndCall(struct rx_call *call, afs_int32 rc)
{
    struct rx_connection *conn = call->conn;
    afs_int32 error;
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

    free(call);
    conn->flags |= RX_CONN_BUSY;
    if (conn->flags & RX_CONN_MAKECALL_WAITING) {
      /* wakeup */
    }
    if (conn->type == RX_CLIENT_CONNECTION) {
      conn->flags &= ~RX_CONN_BUSY;
    }
    error = ntoh_syserr_conv(error);
    return error;
}
