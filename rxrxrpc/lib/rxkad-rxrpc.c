/*
 * Copyright 2000, 2012, International Business Machines Corporation and others.
 * All Rights Reserved.
 *
 * This software has been released under the terms of the IBM Public
 * License.  For details, see the LICENSE file in the top-level source
 * directory or online at http://www.openafs.org/dl/license10.html
 */


#define _RX_
#define RX_GLOBALS_H
#define _XOPEN_SOURCE 600
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <afs/stds.h>
#include <afs/errors.h>
#include "rxrxrpc.h"
typedef void *rx_securityConfigVariables;
#include <rx/rxkad.h>
#include <keyutils.h>
#define SOL_RXRPC               272
#define RXRPC_SECURITY_KEY              1       /* [clnt] set client security key */
#define RXRPC_MIN_SECURITY_LEVEL        4       /* minimum security level */

struct rxrpc_key_sec2_v1 {
        uint32_t        kver;                   /* key payload interface version */
        uint16_t        security_index;         /* RxRPC header security index */
        uint16_t        ticket_length;          /* length of ticket[] */
        uint32_t        expiry;                 /* time at which expires */
        uint32_t        kvno;                   /* key version number */
        uint8_t         session_key[8];         /* DES session key */
        uint8_t         ticket[0];              /* the encrypted ticket */
};

struct rxkad_cprivate {
  int socket;
};
int rxkad_Close(struct rx_securityClass *aobj) {
  struct rxkad_cprivate *tcp = (struct rxkad_cprivate *)aobj->privateData;
  close(tcp->socket);
  free(tcp);
  return 0;
}

int rxkad_NewConnection(struct rx_securityClass *aobj,
			struct rx_connection *aconn) {
  struct rxkad_cprivate *tcp = (struct rxkad_cprivate *)aobj->privateData;
  aconn->socket=tcp->socket;
  return 0;
}
int rxkad_DestroyConnection(struct rx_securityClass *aobj,
			    struct rx_connection *aconn) {
  return 0;
}

static struct rx_securityOps rxkad_client_ops = {
    rxkad_Close,
    rxkad_NewConnection,        /* every new connection */
    rxkad_DestroyConnection,
};
static struct rx_securityClass *
rxkad_AllocClientSecurityObject(void)
{
  struct rx_securityClass *aobj;
  struct rxkad_cprivate *tcp;
  aobj=calloc(1, sizeof(struct rx_securityClass));
  if (!aobj) 
    return NULL;
  tcp=calloc(1, sizeof(struct rxkad_cprivate));
  if (!tcp) {
    free(aobj);
    return NULL;
  }
  aobj->ops=&rxkad_client_ops;
  aobj->privateData=tcp;
  return aobj;
}
static struct rx_securityClass *
rxkad_InitClientSecurityObject(struct rx_securityClass *aobj,
			       rxkad_level level, char *keydesc)
{
  struct rxkad_cprivate *tcp = (struct rxkad_cprivate *)aobj->privateData;
  int ilevel=level;
  struct sockaddr_rxrpc srx;
  
  tcp->socket=socket(AF_RXRPC, SOCK_DGRAM, PF_INET);
  if (tcp->socket < 0) {
   free(tcp);
   free(aobj);
   return NULL;
  }
  srx.srx_family = AF_RXRPC;
  srx.srx_service = 0; /* it's a client */
  srx.transport_type = SOCK_DGRAM;
  srx.transport_len = sizeof(srx.transport.sin);
  srx.transport.sin.sin_family = AF_INET;
  srx.transport.sin.sin_addr.s_addr=htonl(INADDR_ANY);
  srx.transport.sin.sin_port = htons(0); /* maybe export rx_port? */
  
  if (setsockopt(tcp->socket, SOL_RXRPC, RXRPC_SECURITY_KEY, keydesc,
		 strlen(keydesc)) < 0) {
    close(tcp->socket);
    free(tcp);
    free(aobj);
    return NULL;
  }
  if (setsockopt(tcp->socket, SOL_RXRPC, RXRPC_MIN_SECURITY_LEVEL, &ilevel,
		 sizeof(ilevel)) < 0) {
    close(tcp->socket);
    free(tcp);
    free(aobj);
    return NULL;
  } 
  if (bind(tcp->socket, (struct sockaddr *) &srx, sizeof(srx)) < 0) {
    close(tcp->socket);
    free(tcp);
    free(aobj);
    return NULL;
  }
  return aobj;
}
struct rx_securityClass *
rxkad_NewClientSecurityObject(rxkad_level level,
			      struct    ktc_encryptionKey  *sessionkey,
			      afs_int32 kvno,      
			      int ticketLen,      char *ticket) {

  struct rx_securityClass *ret;
  size_t plen;
  struct rxrpc_key_sec2_v1 *payload;
  char keydesc[128];
  key_serial_t k;

  ret=rxkad_AllocClientSecurityObject();
  if (!ret)
    return NULL;
  plen = sizeof(*payload) + ticketLen;
  payload = calloc(1, plen + 4);
  if (!payload) {
    free(ret->privateData);
    free(ret);
    return NULL;
  }

  payload->kver           = 1;
  payload->security_index = 2;
  payload->ticket_length  = ticketLen;
  payload->expiry         = 0xffffffff;
  payload->kvno           = kvno;
  memcpy(payload->session_key, sessionkey->data, 8);
  memcpy(payload->ticket, ticket, ticketLen);
  snprintf(keydesc, 128, "rxkad@%p", ret);
  k=add_key("rxrpc", keydesc, payload, plen, KEY_SPEC_PROCESS_KEYRING);

  if (k == -1) {
    free(ret->privateData);
    free(ret);
    free(payload);
    return NULL;
  }
  free(payload);
  ret=rxkad_InitClientSecurityObject(ret, level, keydesc);
  if (ret == NULL) 
    keyctl_unlink(k, KEY_SPEC_PROCESS_KEYRING);
  return ret;
}


struct rx_securityClass *
rxkad_NewClientSecurityObject2(rxkad_level level, char *keydesc) {
  struct rx_securityClass *ret;
  ret=rxkad_AllocClientSecurityObject();
  if (!ret)
    return NULL;
  return rxkad_InitClientSecurityObject(ret, level, keydesc);
}

