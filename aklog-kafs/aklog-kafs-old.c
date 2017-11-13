/* aklog.c: description
 *
 * Copyright (C) 2008,2012 Chaskiel Grundman. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * Based on code:
 * Copyright (C) 2007 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 */

#define _XOPEN_SOURCE 500
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <keyutils.h>
#include <krb5/krb5.h>

struct rxrpc_key_sec2_v1 {
        uint32_t        kver;                   /* key payload interface version */
        uint16_t        security_index;         /* RxRPC header security index */
        uint16_t        ticket_length;          /* length of ticket[] */
        uint32_t        expiry;                 /* time at which expires */
        uint32_t        kvno;                   /* key version number */
        uint8_t         session_key[8];         /* DES session key */
        uint8_t         ticket[0];              /* the encrypted ticket */
};

#define RXKAD_TKT_TYPE_KERBEROS_V5              256
#define OSERROR(X, Y) do { if ((long)(X) == -1) { perror(Y); exit(1); } } while(0)
#define KRBERROR(X, Y) do { if ((X) != 0) { const char *msg = krb5_get_error_message(k5_ctx, (X)); fprintf(stderr, "%s: %s\n", (Y), msg); krb5_free_error_message(k5_ctx, msg); exit(1); } } while(0)

int main(int argc, char **argv) {
  char *cell, *realm, *p;
  int ret, mode;
  size_t plen;
  struct rxrpc_key_sec2_v1 *payload;
  char description[256];
  key_serial_t dest_keyring, sessring, usessring;
  krb5_error_code kresult;
  krb5_context k5_ctx;
  krb5_ccache cc;
  krb5_creds search_cred, *creds;

  if (argc < 3) {
    fprintf(stderr, "Usage: aklog cell realm\n");
    exit(1);
  }
  cell=argv[1];
  realm=argv[2];

  kresult=krb5_init_context(&k5_ctx);
  if (kresult) { fprintf(stderr, "krb5_init_context failed\n"); exit(1); }
  kresult=krb5_allow_weak_crypto(k5_ctx, 1);
  KRBERROR(kresult, "Enabling weak crypto (DES) use");
  kresult = krb5_cc_default(k5_ctx, &cc);
  KRBERROR(kresult, "Getting credential cache");

  memset(&search_cred, 0, sizeof(krb5_creds));

  kresult = krb5_cc_get_principal(k5_ctx, cc, &search_cred.client);
  KRBERROR(kresult, "Getting client principal");

  search_cred.keyblock.enctype=ENCTYPE_DES_CBC_CRC;

#if 1
  for (mode=0;mode < 2;mode++) {
    kresult = krb5_build_principal(k5_ctx, &search_cred.server,
				   strlen(realm), realm, "afs",
				   mode ? NULL : cell, NULL);
    KRBERROR(kresult, "Building server principal name");
    kresult = krb5_get_credentials(k5_ctx, 0, cc, &search_cred, &creds);
    if (kresult == 0)
      break;
    krb5_free_principal(k5_ctx, search_cred.server);
    search_cred.server=NULL;
  }
#else
  kresult = krb5_parse_name(k5_ctx, cell, &search_cred.server);
  KRBERROR(kresult, "Parsing server principal name");
  kresult = krb5_get_credentials(k5_ctx, 0, cc, &search_cred, &creds);
#endif  
  KRBERROR(kresult, "Getting tickets");

  plen = sizeof(*payload) + creds->ticket.length;
  payload = calloc(1, plen + 4);
  if (!payload) {
    perror("calloc");
    exit(1);
  }
  
  /* use version 1 of the key data interface */
  payload->kver           = 1;
  payload->security_index = 2;
  payload->ticket_length  = creds->ticket.length;
  payload->expiry         = creds->times.endtime;
  payload->kvno           = RXKAD_TKT_TYPE_KERBEROS_V5;
  memcpy(payload->session_key, creds->keyblock.contents, 8);
  memcpy(payload->ticket, creds->ticket.data, creds->ticket.length);

  /* if the session keyring is not set (i.e. using the uid session keyring),
     then the kernel will instantiate a new session keyring if any keys are
     added to KEY_SPEC_SESSION_KEYRING! Since we exit immediately, that
     keyring will be orphaned. So, add the key to KEY_SPEC_USER_SESSION_KEYRING
     in that case */
  dest_keyring=KEY_SPEC_SESSION_KEYRING;
  sessring=keyctl_get_keyring_ID(KEY_SPEC_SESSION_KEYRING, 0);
  usessring=keyctl_get_keyring_ID(KEY_SPEC_USER_SESSION_KEYRING, 0);
  if (sessring == usessring)
     dest_keyring=KEY_SPEC_USER_SESSION_KEYRING;
  snprintf(description, 255, "afs@%s", cell);
  p=&description[4];
  while(*p) {
     if (isalpha(*p) && islower(*p)) *p=toupper(*p);
     p++;
  }

  ret = add_key("rxrpc", description, payload, plen, dest_keyring);
  OSERROR(ret, "add_key");

  krb5_free_creds(k5_ctx, creds);
  krb5_free_cred_contents(k5_ctx, &search_cred);
  krb5_cc_close(k5_ctx, cc);
  krb5_free_context(k5_ctx);
  exit(0);
}
