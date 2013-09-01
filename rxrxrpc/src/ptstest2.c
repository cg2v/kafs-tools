
/* mask rx headers */
#define _RX_
#define AFS_RX_GLOBALS_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <netdb.h>

#include <afs/stds.h>
#include <rxrxrpc.h>
#include <afs/ptserver.h>
#include <afs/cellconfig.h>
#include <afs/ptint.h>
#include <rx/xdr.h>

int PR_NameToID(register struct rx_connection *z_conn,namelist * nlist,idlist * ilist)
{
        struct rx_call *z_call = rx_NewCall(z_conn);
        static int z_op = 504;
        int z_result;
        XDR z_xdrs;
        xdrrx_create(&z_xdrs, z_call, XDR_ENCODE);

        /* Marshal the arguments */
        if ((!xdr_int(&z_xdrs, &z_op))
             || (!xdr_namelist(&z_xdrs, nlist))) {
                z_result = RXGEN_CC_MARSHAL;
                goto fail;
        }

        /* Un-marshal the reply arguments */
        z_xdrs.x_op = XDR_DECODE;
        if ((!xdr_idlist(&z_xdrs, ilist))) {
                z_result = RXGEN_CC_UNMARSHAL;
                goto fail;
        }

        z_result = RXGEN_SUCCESS;
fail:
        z_result = rx_EndCall(z_call, z_result);

        return z_result;
}

int PR_IDToName(register struct rx_connection *z_conn,idlist * ilist,namelist * nlist)
{
        struct rx_call *z_call = rx_NewCall(z_conn);
        static int z_op = 505;
        int z_result;
        XDR z_xdrs;

        xdrrx_create(&z_xdrs, z_call, XDR_ENCODE);

        /* Marshal the arguments */
        if ((!xdr_int(&z_xdrs, &z_op))
             || (!xdr_idlist(&z_xdrs, ilist))) {
                z_result = RXGEN_CC_MARSHAL;
                goto fail;
        }

        /* Un-marshal the reply arguments */
        z_xdrs.x_op = XDR_DECODE;
        if ((!xdr_namelist(&z_xdrs, nlist))) {
                z_result = RXGEN_CC_UNMARSHAL;
                goto fail;
        }

        z_result = RXGEN_SUCCESS;
fail:
        z_result = rx_EndCall(z_call, z_result);


        return z_result;
}

int PR_ListEntry(register struct rx_connection *z_conn,afs_int32 id,struct prcheckentry * entry)
{
        struct rx_call *z_call = rx_NewCall(z_conn);
        static int z_op = 512;
        int z_result;
        XDR z_xdrs;
        xdrrx_create(&z_xdrs, z_call, XDR_ENCODE);

        /* Marshal the arguments */
        if ((!xdr_int(&z_xdrs, &z_op))
             || (!xdr_afs_int32(&z_xdrs, &id))) {
                z_result = RXGEN_CC_MARSHAL;
                goto fail;
        }

        /* Un-marshal the reply arguments */
        z_xdrs.x_op = XDR_DECODE;
        if ((!xdr_prcheckentry(&z_xdrs, entry))) {
                z_result = RXGEN_CC_UNMARSHAL;
                goto fail;
        }

        z_result = RXGEN_SUCCESS;
fail:
        z_result = rx_EndCall(z_call, z_result);
        return z_result;
}

int PR_ListElements(register struct rx_connection *z_conn,afs_int32 id,prlist * elist,afs_int32 * over)
{
        struct rx_call *z_call = rx_NewCall(z_conn);
        static int z_op = 514;
        int z_result;
        XDR z_xdrs;
        xdrrx_create(&z_xdrs, z_call, XDR_ENCODE);

        /* Marshal the arguments */
        if ((!xdr_int(&z_xdrs, &z_op))
             || (!xdr_afs_int32(&z_xdrs, &id))) {
                z_result = RXGEN_CC_MARSHAL;
                goto fail;
        }

        /* Un-marshal the reply arguments */
        z_xdrs.x_op = XDR_DECODE;
        if ((!xdr_prlist(&z_xdrs, elist))
             || (!xdr_afs_int32(&z_xdrs, over))) {
                z_result = RXGEN_CC_UNMARSHAL;
                goto fail;
        }

        z_result = RXGEN_SUCCESS;
fail:
        z_result = rx_EndCall(z_call, z_result);

        return z_result;
}

int main(int argc, char **argv) {
  char *user;
  char *server;
  int i;
  struct addrinfo *he,*hep,hi;;
  prname pn;
  afs_int32 id;
  namelist nl;
  idlist il;
  prcheckentry pr;
  prlist pl;
  afs_int32 over;
  int code;
  struct rx_connection *c;
  afs_uint32 host;
  if (argc < 3) {
    fprintf(stderr, "Usage: ptstest server nameorid\n");
  }
  server=argv[1];
  user=argv[2];

  memset(&hi, 0, sizeof(hi));
  hi.ai_family=AF_INET;
  hi.ai_socktype=0;
  hi.ai_protocol=0;
  hi.ai_flags=AI_ADDRCONFIG;
  if (isdigit(server[0]))
    hi.ai_flags|=AI_NUMERICHOST;
  code=getaddrinfo(server, NULL, &hi, &he);
  if (code) {
    fprintf(stderr, "cannot look up server: %s", gai_strerror(code));
    exit(1);
  }
  host=0;
  for (hep=he;hep;hep=hep->ai_next) {
    if (hep->ai_family==AF_INET) {
      struct sockaddr_in *sin=(struct sockaddr_in *)hep->ai_addr;
      host=ntohl(sin->sin_addr.s_addr);
      if (host)
	break;
    }
  }
  freeaddrinfo(he);
  

  code=rx_Init(0);
  if (code)
    abort();
  
  c=rx_NewConnection(host, AFSCONF_PROTPORT, PRSRV, NULL, 0);

  if (isdigit(user[0]) || user[0] == '-') {
    id=strtoul(user, 0, 10);
    il.idlist_len=1;
    il.idlist_val=&id;
    memset(&nl, 0, sizeof(nl));
    code=PR_IDToName(c, &il, &nl);
    if (code) {
      fprintf(stderr, "IDToName failed: %d\n", code);
      exit(1);
    }
    user=strdup(nl.namelist_val[0]);
    xdr_free((xdrproc_t) xdr_namelist, &nl);
    
  } else {
    memset(pn, 0, sizeof(pn));
    strncpy(pn, user, sizeof(pn));
    nl.namelist_len=1;
    nl.namelist_val=&pn;
    memset(&il, 0, sizeof(il));
    code=PR_NameToID(c, &nl, &il);
    if (code) {
      fprintf(stderr, "NameToID failed: %d\n", code);
      exit(1);
    }
    id=il.idlist_val[0];
    xdr_free((xdrproc_t) xdr_idlist, &il);
    
  }
  printf("%s %d\n", user, id);
  memset(&pr, 0, sizeof(pr));
  code=PR_ListEntry(c, id, &pr);
  if (code) {
    fprintf(stderr, "ListEntry failed: %d\n", code);
    exit(1);
  }
  printf("Meta: %d %d %d %d %d %d %d %s\n", pr.id, pr.flags, pr.owner, pr.creator, pr.ngroups, pr.nusers, pr.count, pr.name);

  memset(&pl, 0, sizeof(pl));
  over=0;
  code=PR_ListElements(c, id, &pl, &over);
  if (code) {
    fprintf(stderr, "ListElements failed: %d\n", code);
    exit(1);
  }
  il.idlist_len=pl.prlist_len;
  il.idlist_val=pl.prlist_val;
  memset(&nl, 0, sizeof(nl));
  code=PR_IDToName(c, &il, &nl);
  if (code) {
    fprintf(stderr, "IDToName(2) failed: %d\n", code);
    exit(1);
  }
  printf("Members:\n");
  for (i=0;i<pl.prlist_len;i++)
    printf("%s %d\n", nl.namelist_val[i], il.idlist_val[i]);
  xdr_free((xdrproc_t) xdr_prlist, &pl);
  xdr_free((xdrproc_t) xdr_namelist, &nl);
  
  rx_DestroyConnection(c);
  exit(0);
}
