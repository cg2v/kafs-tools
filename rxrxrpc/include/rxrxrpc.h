/*
 * Copyright 2000,2012, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */

#define rx_ConnectionOf(call)           ((call)->conn)
#define rx_PeerOf(conn)                 ((conn)->peer)
#define rx_HostOf(peer)                 ((peer)->host)
#define rx_PortOf(peer)                 ((peer)->port)
#define rx_SetLocalStatus(call, status) ((call)->localStatus = (status))
#define rx_GetLocalStatus(call, status) ((call)->localStatus)
#define rx_GetRemoteStatus(call)        ((call)->remoteStatus)
#define rx_Error(call)                  ((call)->error)
#define rx_ConnError(conn)              ((conn)->error)
#define rx_IsServerConn(conn)           ((conn)->type == RX_SERVER_CONNECTION)
#define rx_IsClientConn(conn)           ((conn)->type == RX_CLIENT_CONNECTION)
#define rx_ServiceIdOf(conn)            ((conn)->serviceId)
#define rx_SecurityClassOf(conn)        ((conn)->securityIndex)
#define rx_SecurityObjectOf(conn)       ((conn)->securityObject)
#define    RX_MAXCALLS 4

static_inline int
rx_IsLoopbackAddr(afs_uint32 addr)
{
    return ((addr & 0xffff0000) == 0x7f000000);
}
struct sockaddr_rxrpc {
        sa_family_t     srx_family;     /* address family */
        unsigned short  srx_service;    /* service desired */
        unsigned short  transport_type; /* type of transport socket (SOCK_DGRAM) */
        unsigned short  transport_len;  /* length of transport address */
        union {
                sa_family_t family;             /* transport address family */
                struct sockaddr_in sin;         /* IPv4 transport address */
                struct sockaddr_in6 sin6;       /* IPv6 transport address */
        } transport;
};

#define rx_Read(call, buf, nbytes)   rx_ReadProc(call, buf, nbytes)
#define rx_Read32(call, value)   rx_ReadProc32(call, value)
#define rx_Write(call, buf, nbytes) rx_WriteProc(call, buf, nbytes)
#define rx_Write32(call, value) rx_WriteProc32(call, value)
#define rx_PutConnection(conn) rx_DestroyConnection(conn)

struct rx_connection {
  //    struct rx_connection *next; /*  on hash chain _or_ free list */
  //struct rx_peer *peer;
  int socket;
  struct sockaddr_rxrpc target;
    afs_int32 error;            /* If this connection is in error, this is it */
    struct rx_call *call[RX_MAXCALLS];
    afs_uint32 callNumber[RX_MAXCALLS]; /* Current call numbers */
    afs_uint32 lastBusy[RX_MAXCALLS]; /* timestamp of the last time we got an
                                       * RX_PACKET_TYPE_BUSY packet for this
                                       * call slot, or 0 if the slot is not busy */
    int abortCount;             /* count of abort messages sent */
    u_int16_t serviceId;          /* To stamp on requests (clients only) */
    afs_uint32 refCount;        /* Reference count (rx_refcnt_mutex) */
    u_int8_t flags;               /* Defined below - (conn_data_lock) */
    u_int8_t type;                /* Type of connection, defined below */
    u_int8_t securityIndex;       /* corresponds to the security class of the */
    /* securityObject for this conn */
    struct rx_securityClass *securityObject;    /* Security object for this connection */
    void *securityData;         /* Private data for this conn's security class */
    u_int16_t securityHeaderSize; /* Length of security module's packet header data */
    u_int16_t securityMaxTrailerSize;     /* Length of security module's packet trailer data */
  int makeCallWaiters;

    int timeout;                /* Overall timeout per call (seconds) for this conn */
    int lastSendTime;           /* Last send time for this connection */
    int nSpecific;              /* number entries in specific data */
    void **specific;            /* pointer to connection specific data */
};


#define RX_CONN_MAKECALL_WAITING    1   /* rx_NewCall is waiting for a channel */
#define RX_CONN_DESTROY_ME          2   /* Destroy *client* connection after last call */
#define RX_CONN_USING_PACKET_CKSUM  4   /* non-zero header.spare field seen */
#define RX_CONN_KNOW_WINDOW         8   /* window size negotiation works */
#define RX_CONN_RESET              16   /* connection is reset, remove */
#define RX_CONN_BUSY               32   /* connection is busy; don't delete */
#define RX_CONN_ATTACHWAIT         64   /* attach waiting for peer->lastReach */
#define RX_CONN_MAKECALL_ACTIVE   128   /* a thread is actively in rx_NewCall */
#define RX_CONN_NAT_PING          256   /* nat ping requested */

/* Type of connection, client or server */
#define RX_CLIENT_CONNECTION    0
#define RX_SERVER_CONNECTION    1

struct rx_call {
  void *kernel_id;
    u_int16_t nLeft;              /* Number bytes left in first receive packet */
    u_int16_t nFree;              /* Number bytes free in last send packet */
  int pktinuse;
    char currentPacket[2048];    /* Current packet being assembled or being read */
    char *curpos;               /* current position in curvec */
    u_int8_t channel;             /* Index of call, within connection */
    u_int8_t state;               /* Current call state as defined below */
    u_int8_t mode;                /* Current mode of a call in ACTIVE state */
    struct rx_connection *conn; /* Parent connection for this call */
    afs_uint32 *callNumber;     /* Pointer to call number field within connection */
    afs_uint32 flags;           /* Some random flags */
    u_int8_t localStatus;         /* Local user status sent out of band */
    u_int8_t remoteStatus;        /* Remote user status received out of band */
    afs_int32 error;            /* Error condition for this call */
    afs_uint32 timeout;         /* High level timeout for this call */
    afs_uint32 rnext;           /* Next sequence number expected to be read by rx_ReadData */
    int abortCode;              /* error code from last RPC */
    int abortCount;             /* number of times last error was sent */
};

/* Call modes:  the modes of a call in RX_STATE_ACTIVE state (process attached) */
#define RX_MODE_SENDING   1     /* Sending or ready to send */
#define RX_MODE_RECEIVING 2     /* Receiving or ready to receive */
#define RX_MODE_ERROR     3     /* Something in error for current conversation */
#define RX_MODE_EOF       4     /* Server has flushed (or client has read) last reply packet */

/* Flags */
#define RX_CALL_READER_WAIT        1    /* Reader is waiting for next packet */
#define RX_CALL_WAIT_WINDOW_ALLOC  2    /* Sender is waiting for window to allocate buffers */
#define RX_CALL_WAIT_WINDOW_SEND   4    /* Sender is waiting for window to send buffers */
#define RX_CALL_WAIT_PACKETS       8    /* Sender is waiting for packet buffers */
#define RX_CALL_WAIT_PROC         16    /* Waiting for a process to be assigned */
#define RX_CALL_RECEIVE_DONE      32    /* All packets received on this call */
#define RX_CALL_CLEARED           64    /* Receive queue cleared in precall state */
#define RX_CALL_TQ_BUSY          128    /* Call's Xmit Queue is busy; don't modify */
#define RX_CALL_TQ_CLEARME       256    /* Need to clear this call's TQ later */
#define RX_CALL_TQ_SOME_ACKED    512    /* rxi_Start needs to discard ack'd packets. */
#define RX_CALL_TQ_WAIT         1024    /* Reader is waiting for TQ_BUSY to be reset */
#define RX_CALL_FAST_RECOVER    2048    /* call is doing congestion recovery */
/* 4096 was RX_CALL_FAST_RECOVER_WAIT */
#define RX_CALL_SLOW_START_OK   8192    /* receiver acks every other packet */
#define RX_CALL_IOVEC_WAIT      16384   /* waiting thread is using an iovec */
#define RX_CALL_HAVE_LAST       32768   /* Last packet has been received */
#define RX_CALL_NEED_START      0x10000 /* tells rxi_Start to start again */
#define RX_CALL_PEER_BUSY       0x20000 /* the last packet we received on this call was a
                                         * BUSY packet; i.e. the channel for this call is busy */
#define RX_CALL_ACKALL_SENT     0x40000 /* ACKALL has been sent on the call */


struct rx_securityClass {
    struct rx_securityOps {
      int (*op_Close) (struct rx_securityClass * aobj);
      int (*op_NewConnection) (struct rx_securityClass * aobj,
			       struct rx_connection * aconn);
      int (*op_DestroyConnection) (struct rx_securityClass * aobj,
				   struct rx_connection * aconn);
    } *ops;
  void *privateData;
  int refCount;
};

#define RXS_OP(obj,op,args) ((obj && (obj->ops->op_ ## op)) ? (*(obj)->ops->op_ ## op)args : 0)

#define RXS_Close(obj) RXS_OP(obj,Close,(obj))
#define RXS_NewConnection(obj,conn) RXS_OP(obj,NewConnection,(obj,conn))
#define RXS_DestroyConnection(obj,conn) RXS_OP(obj,DestroyConnection,(obj,conn))

extern int rx_Init(unsigned int port);
extern int rx_InitHost(unsigned int host, unsigned int port);

extern struct rx_connection *rx_NewConnection(afs_uint32 shost,
                                              u_int16_t sport, 
					      u_int16_t sservice,
                                              struct rx_securityClass
                                              *securityObject,
                                              int serviceSecurityIndex);
extern void rxi_DestroyConnection(struct rx_connection *conn);
extern void rx_DestroyConnection(struct rx_connection *conn);
extern struct rx_call *rx_NewCall(struct rx_connection *conn);
extern afs_int32 rx_EndCall(struct rx_call *call, afs_int32 rc);
extern int rxs_Release(struct rx_securityClass *aobj);
