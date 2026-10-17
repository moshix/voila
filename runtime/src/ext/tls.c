/* ext/tls.c — std/tls's natives: TLS 1.2 over a std/net socket, on the
 * vendored BearSSL 0.6 (runtime/src/vendor/bearssl, MIT).
 *
 * This file is a runtime EXTENSION (voila_std.h, vl_ext_register): it and
 * BearSSL are compiled into their own archive only for a program whose
 * natives reach std/tls — std/http does, for https:// — and BearSSL needs
 * nothing but libc, so such a program still links libc alone.
 *
 * HOW A CONNECTION BECOMES TLS. The handshake runs here, on the socket's
 * descriptor; then the session is installed as the socket's FILTER
 * (vl_socket_set_filter, VlSocket.flt): an O_HANDLE whose read and write
 * ops replace recv and send on every byte path the runtime has — net._send,
 * net._recv and an io.reader/io.writer over the socket. So the Voilà side
 * keeps an ordinary net.Conn, and std/http, which reads and writes nothing
 * but net.Conn, speaks HTTPS without a line of TLS in it.
 *
 * THE I/O LOOP is our own, not BearSSL's br_sslio, for two reasons. A read
 * that times out (the socket's SO_RCVTIMEO, set by Conn.set_timeout) must
 * come back as std/net's "timed out" and leave the session usable — sslio
 * would fail the engine for good. And a peer may send application data
 * while we are writing: sslio calls that an error; here it is kept in
 * `pend` and handed to the next read.
 *
 * VERIFICATION. A client verifies the server's chain against trust anchors
 * (the system's CA bundle, or Config.ca_file), its validity dates, and that
 * it names the host (SNI carries the same name, unless it is an IP
 * address). The name itself, and the rest of the Web PKI's rules BearSSL's
 * minimal engine does not apply, are checked by Policy (below).
 * `insecure` swaps in an engine that checks nothing but still extracts the
 * server's key (the handshake needs it): for tests, never for production.
 *
 * THREADS. A session is one state machine; `mu` serializes every operation
 * on it. A task blocked reading therefore holds it, and a writer on another
 * task waits for that read — TLS connections are half-duplex across tasks,
 * which is how std/http uses them. Loaded CA bundles and server keys are
 * cached process-wide, under `g_mu`, keyed by path and invalidated when the
 * file's size or mtime changes; an entry is never freed while the process
 * runs (a connection may still be using it), only superseded.
 */
#include "voila_int.h"

#include "bearssl.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

/* The most application data kept for a reader while a writer works. */
#define TLS_MAX_PEND (16 * 1024 * 1024)

#ifdef MSG_NOSIGNAL
#define TLS_SEND_FLAGS MSG_NOSIGNAL /* Linux: no SIGPIPE on a dead peer */
#else
#define TLS_SEND_FLAGS 0 /* macOS: the socket has SO_NOSIGPIPE (std.c) */
#endif

/* ------------------------------------------------------------ errors */

static Value tls_err(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Value m = vl_str(buf);
  Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
  vl_release(m);
  return e;
}

/* engine_why names a BearSSL error code the way a user can act on it. */
static Value engine_err(int e, const char *name) {
  switch (e) {
  case BR_ERR_X509_NOT_TRUSTED:
    return tls_err("tls: certificate is not trusted (unknown certificate "
                   "authority)");
  case BR_ERR_X509_BAD_SERVER_NAME:
    return tls_err("tls: certificate does not match host name \"%s\"", name);
  case BR_ERR_X509_EXPIRED:
    return tls_err("tls: certificate has expired or is not yet valid");
  case BR_ERR_X509_NOT_CA:
    return tls_err("tls: certificate chain uses a non-CA certificate as an "
                   "issuer");
  case BR_ERR_X509_WEAK_PUBLIC_KEY:
    return tls_err("tls: certificate key is too weak");
  case BR_ERR_X509_EMPTY_CHAIN:
    return tls_err("tls: the peer sent no certificate");
  case BR_ERR_UNSUPPORTED_VERSION:
  case BR_ERR_BAD_VERSION:
    return tls_err("tls: the peer does not speak TLS 1.2");
  case BR_ERR_WRONG_KEY_USAGE:
  case BR_ERR_X509_FORBIDDEN_KEY_USAGE:
    return tls_err("tls: the certificate's key usage does not allow it to "
                   "be used for this connection");
  case BR_ERR_BAD_CIPHER_SUITE:
    return tls_err("tls: no cipher suite in common with the peer");
  case BR_ERR_IO:
    return tls_err("tls: connection failed during the handshake");
  default:
    break;
  }
  if (e >= BR_ERR_SEND_FATAL_ALERT)
    return tls_err("tls: handshake refused (sent alert %d)",
                   e - BR_ERR_SEND_FATAL_ALERT);
  if (e >= BR_ERR_RECV_FATAL_ALERT) {
    int a = e - BR_ERR_RECV_FATAL_ALERT;
    /* 42..48: the peer rejected OUR certificate or its issuer. */
    if (a == 48) return tls_err("tls: the peer does not trust our certificate");
    return tls_err("tls: the peer refused the handshake (alert %d)", a);
  }
  if (e >= BR_ERR_X509_OK)
    return tls_err("tls: invalid certificate (X.509 error %d)", e);
  return tls_err("tls: protocol error (BearSSL error %d)", e);
}

/* ------------------------------------------------------------ files */

/* read_file returns a malloc'd copy of the file, or NULL with errno set. */
static unsigned char *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 65536, n = 0;
  unsigned char *b = (unsigned char *)malloc(cap);
  if (!b) {
    fclose(f);
    return NULL;
  }
  for (;;) {
    if (n == cap) {
      cap *= 2;
      unsigned char *nb = (unsigned char *)realloc(b, cap);
      if (!nb) {
        free(b);
        fclose(f);
        return NULL;
      }
      b = nb;
    }
    size_t k = fread(b + n, 1, cap - n, f);
    n += k;
    if (k == 0) break;
  }
  int bad = ferror(f);
  fclose(f);
  if (bad) {
    free(b);
    errno = EIO;
    return NULL;
  }
  *len = n;
  return b;
}

/* A growable byte vector, for the decoders' callbacks. */
typedef struct {
  unsigned char *b;
  size_t n, cap;
} Vec;

static void vec_put(void *ctx, const void *p, size_t n) {
  Vec *v = (Vec *)ctx;
  if (v->n + n > v->cap) {
    size_t c = v->cap ? v->cap : 256;
    while (c < v->n + n) c *= 2;
    unsigned char *nb = (unsigned char *)realloc(v->b, c);
    if (!nb) vl_abort("out of memory");
    v->b = nb;
    v->cap = c;
  }
  memcpy(v->b + v->n, p, n);
  v->n += n;
}

/* One object of a PEM file: its label ("CERTIFICATE", "PRIVATE KEY") and
 * its DER bytes. */
typedef struct {
  char *name;
  unsigned char *der;
  size_t len;
} PemObj;

static void pem_free(PemObj *objs, size_t n) {
  for (size_t i = 0; i < n; i++) {
    free(objs[i].name);
    free(objs[i].der);
  }
  free(objs);
}

/* pem_decode splits a PEM file into its objects. Text outside the
 * -----BEGIN/END----- lines (a bundle's comments) is skipped, as BearSSL's
 * decoder does. Returns false on malformed PEM. */
static bool pem_decode(const unsigned char *buf, size_t len, PemObj **out,
                       size_t *nout) {
  br_pem_decoder_context pc;
  br_pem_decoder_init(&pc);
  PemObj *objs = NULL;
  size_t n = 0, cap = 0;
  Vec cur = {NULL, 0, 0};
  char *name = NULL;
  bool inobj = false, extra_nl = true;
  while (len > 0 || extra_nl) {
    if (len == 0) { /* a final newline flushes an unterminated last line */
      buf = (const unsigned char *)"\n";
      len = 1;
      extra_nl = false;
    }
    size_t k = br_pem_decoder_push(&pc, buf, len);
    buf += k;
    len -= k;
    switch (br_pem_decoder_event(&pc)) {
    case BR_PEM_BEGIN_OBJ:
      free(name);
      name = strdup(br_pem_decoder_name(&pc));
      cur.n = 0;
      br_pem_decoder_setdest(&pc, vec_put, &cur);
      inobj = true;
      break;
    case BR_PEM_END_OBJ:
      if (inobj) {
        if (n == cap) {
          cap = cap ? cap * 2 : 16;
          PemObj *no = (PemObj *)realloc(objs, cap * sizeof(PemObj));
          if (!no) vl_abort("out of memory");
          objs = no;
        }
        objs[n].name = name;
        objs[n].der = (unsigned char *)malloc(cur.n ? cur.n : 1);
        if (!objs[n].der) vl_abort("out of memory");
        memcpy(objs[n].der, cur.b, cur.n);
        objs[n].len = cur.n;
        n++;
        name = NULL;
        inobj = false;
      }
      break;
    case BR_PEM_ERROR:
      free(name);
      free(cur.b);
      pem_free(objs, n);
      return false;
    default:
      break;
    }
  }
  free(name);
  free(cur.b);
  if (inobj) { /* BEGIN without END */
    pem_free(objs, n);
    return false;
  }
  *out = objs;
  *nout = n;
  return true;
}

/* ------------------------------------------------------------ trust */

typedef struct {
  br_x509_trust_anchor *ta;
  size_t n;
} Anchors;

/* anchor_of turns one CA certificate into a trust anchor: its subject DN
 * and public key, copied (the decoder's buffers do not outlive it). A
 * certificate BearSSL cannot read — a key type it lacks — is skipped by
 * returning false; a system bundle may hold a few. */
static bool anchor_of(const unsigned char *der, size_t len,
                      br_x509_trust_anchor *ta) {
  Vec dn = {NULL, 0, 0};
  br_x509_decoder_context dc;
  br_x509_decoder_init(&dc, vec_put, &dn);
  br_x509_decoder_push(&dc, der, len);
  const br_x509_pkey *pk = br_x509_decoder_get_pkey(&dc);
  if (!pk || br_x509_decoder_last_error(&dc) != 0) {
    free(dn.b);
    return false;
  }
  memset(ta, 0, sizeof *ta);
  ta->dn.data = dn.b;
  ta->dn.len = dn.n;
  ta->flags = br_x509_decoder_isCA(&dc) ? BR_X509_TA_CA : 0;
  ta->pkey.key_type = pk->key_type;
  if (pk->key_type == BR_KEYTYPE_RSA) {
    unsigned char *nn = (unsigned char *)malloc(pk->key.rsa.nlen);
    unsigned char *e = (unsigned char *)malloc(pk->key.rsa.elen);
    if (!nn || !e) vl_abort("out of memory");
    memcpy(nn, pk->key.rsa.n, pk->key.rsa.nlen);
    memcpy(e, pk->key.rsa.e, pk->key.rsa.elen);
    ta->pkey.key.rsa.n = nn;
    ta->pkey.key.rsa.nlen = pk->key.rsa.nlen;
    ta->pkey.key.rsa.e = e;
    ta->pkey.key.rsa.elen = pk->key.rsa.elen;
  } else if (pk->key_type == BR_KEYTYPE_EC) {
    unsigned char *q = (unsigned char *)malloc(pk->key.ec.qlen);
    if (!q) vl_abort("out of memory");
    memcpy(q, pk->key.ec.q, pk->key.ec.qlen);
    ta->pkey.key.ec.curve = pk->key.ec.curve;
    ta->pkey.key.ec.q = q;
    ta->pkey.key.ec.qlen = pk->key.ec.qlen;
  } else {
    free(dn.b);
    return false;
  }
  return true;
}

/* The process-wide cache of loaded files (see THREADS in the header). */
typedef struct CacheEnt {
  struct CacheEnt *next;
  char *key;      /* the path(s) */
  off_t size;     /* the file's size and mtime when it was loaded */
  time_t mtime;
  off_t size2;    /* the key file's, for a server pair */
  time_t mtime2;
  void *val;      /* an Anchors or a ServerMat */
} CacheEnt;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static CacheEnt *g_cache;

static bool stat_of(const char *path, off_t *size, time_t *mtime) {
  struct stat st;
  if (stat(path, &st) != 0) return false;
  *size = st.st_size;
  *mtime = st.st_mtime;
  return true;
}

/* cache_find returns the live entry for key whose files are unchanged, or
 * NULL. Call with g_mu held. */
static void *cache_find(const char *key, off_t s1, time_t m1, off_t s2,
                        time_t m2) {
  for (CacheEnt *e = g_cache; e; e = e->next)
    if (strcmp(e->key, key) == 0 && e->size == s1 && e->mtime == m1 &&
        e->size2 == s2 && e->mtime2 == m2)
      return e->val;
  return NULL;
}

static void cache_add(const char *key, off_t s1, time_t m1, off_t s2,
                      time_t m2, void *val) {
  CacheEnt *e = (CacheEnt *)vl_alloc(sizeof(CacheEnt));
  e->key = strdup(key);
  e->size = s1;
  e->mtime = m1;
  e->size2 = s2;
  e->mtime2 = m2;
  e->val = val;
  e->next = g_cache;
  g_cache = e;
}

/* load_anchors reads a PEM bundle of CA certificates, cached. */
static Anchors *load_anchors(const char *path, Value *err) {
  off_t sz;
  time_t mt;
  if (!stat_of(path, &sz, &mt)) {
    *err = tls_err("tls: cannot read CA file %s: %s", path, strerror(errno));
    return NULL;
  }
  pthread_mutex_lock(&g_mu);
  Anchors *a = (Anchors *)cache_find(path, sz, mt, 0, 0);
  if (a) {
    pthread_mutex_unlock(&g_mu);
    return a;
  }
  size_t len = 0;
  unsigned char *buf = read_file(path, &len);
  if (!buf) {
    pthread_mutex_unlock(&g_mu);
    *err = tls_err("tls: cannot read CA file %s: %s", path, strerror(errno));
    return NULL;
  }
  PemObj *objs = NULL;
  size_t n = 0;
  bool ok = pem_decode(buf, len, &objs, &n);
  free(buf);
  if (!ok) {
    pthread_mutex_unlock(&g_mu);
    *err = tls_err("tls: CA file %s is not valid PEM", path);
    return NULL;
  }
  a = (Anchors *)vl_alloc(sizeof(Anchors));
  a->ta = (br_x509_trust_anchor *)vl_alloc((n ? n : 1) *
                                           sizeof(br_x509_trust_anchor));
  for (size_t i = 0; i < n; i++) {
    if (strcmp(objs[i].name, "CERTIFICATE") != 0 &&
        strcmp(objs[i].name, "X509 CERTIFICATE") != 0)
      continue;
    if (anchor_of(objs[i].der, objs[i].len, &a->ta[a->n])) a->n++;
  }
  pem_free(objs, n);
  if (a->n == 0) {
    pthread_mutex_unlock(&g_mu);
    free(a->ta);
    free(a);
    *err = tls_err("tls: CA file %s holds no usable certificate", path);
    return NULL;
  }
  cache_add(path, sz, mt, 0, 0, a);
  pthread_mutex_unlock(&g_mu);
  return a;
}

/* system_ca is the platform's CA bundle: $SSL_CERT_FILE when set, else the
 * first of the usual locations that exists — macOS and the BSDs, Debian
 * and Ubuntu, Fedora and RHEL, openSUSE, Alpine. NULL when none does. */
static const char *system_ca(void) {
  const char *env = getenv("SSL_CERT_FILE");
  if (env && env[0]) return env;
  static const char *const paths[] = {
      "/etc/ssl/cert.pem",
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",
      "/etc/ssl/ca-bundle.pem",
      "/usr/local/etc/openssl/cert.pem",
      "/usr/local/share/certs/ca-root-nss.crt",
      NULL,
  };
  for (int i = 0; paths[i]; i++) {
    struct stat st;
    if (stat(paths[i], &st) == 0 && S_ISREG(st.st_mode)) return paths[i];
  }
  return NULL;
}

/* ------------------------------------------------------------ server keys */

typedef struct {
  br_x509_certificate *chain;
  size_t n;
  br_skey_decoder_context skd; /* holds the key; rsa/ec point into it */
  int key_type;
  unsigned issuer_type; /* the key type that signed the leaf (EC suites) */
} ServerMat;

static ServerMat *load_server(const char *cert, const char *key, Value *err) {
  off_t s1, s2;
  time_t m1, m2;
  if (!stat_of(cert, &s1, &m1)) {
    *err = tls_err("tls: cannot read certificate file %s: %s", cert,
                   strerror(errno));
    return NULL;
  }
  if (!stat_of(key, &s2, &m2)) {
    *err = tls_err("tls: cannot read key file %s: %s", key, strerror(errno));
    return NULL;
  }
  size_t kl = strlen(cert) + strlen(key) + 2;
  char *ck = (char *)vl_alloc(kl);
  snprintf(ck, kl, "%s\n%s", cert, key);
  pthread_mutex_lock(&g_mu);
  ServerMat *m = (ServerMat *)cache_find(ck, s1, m1, s2, m2);
  if (m) {
    pthread_mutex_unlock(&g_mu);
    free(ck);
    return m;
  }
  size_t len = 0;
  PemObj *objs = NULL;
  size_t n = 0;
  unsigned char *buf = read_file(cert, &len);
  if (!buf || !pem_decode(buf, len, &objs, &n)) {
    free(buf);
    pthread_mutex_unlock(&g_mu);
    free(ck);
    *err = tls_err("tls: certificate file %s is not readable PEM", cert);
    return NULL;
  }
  free(buf);
  m = (ServerMat *)vl_alloc(sizeof(ServerMat));
  m->chain = (br_x509_certificate *)vl_alloc((n ? n : 1) *
                                             sizeof(br_x509_certificate));
  for (size_t i = 0; i < n; i++) {
    if (strcmp(objs[i].name, "CERTIFICATE") != 0) continue;
    m->chain[m->n].data = objs[i].der;
    m->chain[m->n].data_len = objs[i].len;
    objs[i].der = NULL; /* now the chain's */
    m->n++;
  }
  pem_free(objs, n);
  if (m->n == 0) {
    pthread_mutex_unlock(&g_mu);
    free(m->chain);
    free(m);
    free(ck);
    *err = tls_err("tls: certificate file %s holds no certificate", cert);
    return NULL;
  }
  /* The leaf's signer key type, for the static-ECDH suites of an EC key. */
  br_x509_decoder_context dc;
  br_x509_decoder_init(&dc, NULL, NULL);
  br_x509_decoder_push(&dc, m->chain[0].data, m->chain[0].data_len);
  m->issuer_type = br_x509_decoder_get_signer_key_type(&dc);
  if (m->issuer_type == 0) m->issuer_type = BR_KEYTYPE_EC;

  buf = read_file(key, &len);
  objs = NULL;
  n = 0;
  bool found = false;
  if (buf && pem_decode(buf, len, &objs, &n)) {
    for (size_t i = 0; i < n && !found; i++) {
      if (strcmp(objs[i].name, "RSA PRIVATE KEY") != 0 &&
          strcmp(objs[i].name, "EC PRIVATE KEY") != 0 &&
          strcmp(objs[i].name, "PRIVATE KEY") != 0)
        continue;
      br_skey_decoder_init(&m->skd);
      br_skey_decoder_push(&m->skd, objs[i].der, objs[i].len);
      if (br_skey_decoder_last_error(&m->skd) == 0) {
        m->key_type = br_skey_decoder_key_type(&m->skd);
        found = m->key_type == BR_KEYTYPE_RSA || m->key_type == BR_KEYTYPE_EC;
      }
    }
    pem_free(objs, n);
  }
  free(buf);
  if (!found) {
    pthread_mutex_unlock(&g_mu);
    for (size_t i = 0; i < m->n; i++) free(m->chain[i].data);
    free(m->chain);
    free(m);
    free(ck);
    *err = tls_err("tls: key file %s holds no RSA or EC private key BearSSL "
                   "can read (an encrypted key is not supported)", key);
    return NULL;
  }
  cache_add(ck, s1, m1, s2, m2, m);
  pthread_mutex_unlock(&g_mu);
  free(ck);
  return m;
}

/* ------------------------------------------------------------ insecure */

/* NoVerify is an X.509 engine that accepts any chain. It still decodes the
 * end-entity certificate, because the key exchange needs the server's
 * public key. */
typedef struct {
  const br_x509_class *vtable;
  br_x509_decoder_context dc;
  int depth;
} NoVerify;

static void nv_start_chain(const br_x509_class **ctx, const char *name) {
  (void)name;
  ((NoVerify *)(void *)ctx)->depth = 0;
}

static void nv_start_cert(const br_x509_class **ctx, uint32_t length) {
  (void)length;
  NoVerify *nv = (NoVerify *)(void *)ctx;
  if (nv->depth == 0) br_x509_decoder_init(&nv->dc, NULL, NULL);
}

static void nv_append(const br_x509_class **ctx, const unsigned char *buf,
                      size_t len) {
  NoVerify *nv = (NoVerify *)(void *)ctx;
  if (nv->depth == 0) br_x509_decoder_push(&nv->dc, buf, len);
}

static void nv_end_cert(const br_x509_class **ctx) {
  ((NoVerify *)(void *)ctx)->depth++;
}

static unsigned nv_end_chain(const br_x509_class **ctx) {
  NoVerify *nv = (NoVerify *)(void *)ctx;
  if (nv->depth == 0) return BR_ERR_X509_EMPTY_CHAIN;
  if (!br_x509_decoder_get_pkey(&nv->dc)) return BR_ERR_X509_UNSUPPORTED;
  return 0;
}

static const br_x509_pkey *nv_get_pkey(const br_x509_class *const *ctx,
                                       unsigned *usages) {
  NoVerify *nv = (NoVerify *)(void *)ctx;
  if (usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;
  return br_x509_decoder_get_pkey(&nv->dc);
}

static const br_x509_class nv_vtable = {
    sizeof(NoVerify), nv_start_chain, nv_start_cert, nv_append,
    nv_end_cert,      nv_end_chain,   nv_get_pkey};

/* ------------------------------------------------------------ policy
 *
 * BearSSL's minimal X.509 engine checks the chain's signatures, its dates,
 * basic constraints and key usage. It does NOT check what the rest of the
 * Web PKI expects of a server certificate, so Policy wraps it and adds the
 * checks Go's crypto/x509 makes:
 *
 *   - the leaf's extendedKeyUsage, when it has one, must allow serverAuth
 *     (or anyExtendedKeyUsage): a client-only certificate is no server's;
 *   - the leaf must carry a subjectAltName — the Common Name is NOT a
 *     fallback (Go >= 1.15);
 *   - the name is matched here, not by BearSSL: a DNS name against the
 *     dNSName entries, where a wildcard is only a whole left-most label
 *     with at least two labels after it (`*.example.com`, never `*.com` or
 *     `*.localhost`) and covers exactly one label; an IP address (a host
 *     that parses as one) against the iPAddress entries, byte for byte;
 *   - no certificate the PEER sent may be signed with SHA-1 (Go >= 1.18),
 *     except a self-signed one — a root, whose signature nobody verifies.
 *
 * The wrapped engine is started with NO server name, which switches its own
 * (CN-falling-back, `*.com`-accepting) name check off; SNI is unaffected,
 * because the engine sends the name given to br_ssl_client_reset. The certs
 * are recorded as they stream past, then parsed with the small DER reader
 * below once the wrapped engine has accepted the chain. */

#define POL_MAXCERTS 10

typedef struct {
  const unsigned char *p;
  size_t n;
} Der;

/* der_next reads one TLV off the front of d. Definite lengths only, up to
 * four length bytes — DER allows nothing else. */
static bool der_next(Der *d, unsigned *tag, Der *val) {
  if (d->n < 2) return false;
  unsigned t = d->p[0];
  size_t i = 1, len = d->p[i++];
  if (len & 0x80) {
    size_t k = len & 0x7f;
    if (k == 0 || k > 4 || d->n < 2 + k) return false;
    len = 0;
    for (size_t j = 0; j < k; j++) len = (len << 8) | d->p[i++];
  }
  if (len > d->n - i) return false;
  *tag = t;
  val->p = d->p + i;
  val->n = len;
  d->p += i + len;
  d->n -= i + len;
  return true;
}

static bool der_is(Der v, const unsigned char *oid, size_t n) {
  return v.n == n && memcmp(v.p, oid, n) == 0;
}

static const unsigned char OID_SAN[] = {0x55, 0x1d, 0x11};
static const unsigned char OID_EKU[] = {0x55, 0x1d, 0x25};
static const unsigned char OID_ANY_EKU[] = {0x55, 0x1d, 0x25, 0x00};
static const unsigned char OID_SERVER_AUTH[] = {0x2b, 0x06, 0x01, 0x05,
                                                0x05, 0x07, 0x03, 0x01};
static const unsigned char OID_SHA1_RSA[] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                             0x0d, 0x01, 0x01, 0x05};
static const unsigned char OID_SHA1_RSA_OLD[] = {0x2b, 0x0e, 0x03, 0x02,
                                                 0x1d};
static const unsigned char OID_SHA1_ECDSA[] = {0x2a, 0x86, 0x48, 0xce, 0x3d,
                                               0x04, 0x01};
static const unsigned char OID_SHA1_DSA[] = {0x2a, 0x86, 0x48, 0xce, 0x38,
                                             0x04, 0x03};

/* What one certificate says, as far as the policy cares. */
typedef struct {
  Der issuer, subject, sigalg; /* sigalg: the outer algorithm's OID */
  bool has_san, has_eku, eku_server;
  Der san;                     /* the GeneralNames SEQUENCE's contents */
} CertInfo;

static bool cert_parse(const unsigned char *p, size_t n, CertInfo *ci) {
  memset(ci, 0, sizeof *ci);
  Der d = {p, n}, cert, tbs, alg, v;
  unsigned tag;
  if (!der_next(&d, &tag, &cert) || tag != 0x30) return false;
  if (!der_next(&cert, &tag, &tbs) || tag != 0x30) return false;
  if (!der_next(&cert, &tag, &alg) || tag != 0x30) return false;
  if (!der_next(&alg, &tag, &ci->sigalg) || tag != 0x06) return false;
  /* tbsCertificate: [0] version?, serial, signature, issuer, validity,
   * subject, subjectPublicKeyInfo, [1]?, [2]?, [3] extensions? */
  Der t = tbs;
  if (!der_next(&t, &tag, &v)) return false;
  if (tag == 0xa0 && !der_next(&t, &tag, &v)) return false; /* serial */
  if (!der_next(&t, &tag, &v)) return false;                /* signature */
  if (!der_next(&t, &tag, &ci->issuer) || tag != 0x30) return false;
  if (!der_next(&t, &tag, &v)) return false;                /* validity */
  if (!der_next(&t, &tag, &ci->subject) || tag != 0x30) return false;
  if (!der_next(&t, &tag, &v)) return false;                /* spki */
  while (t.n > 0) {
    if (!der_next(&t, &tag, &v)) return false;
    if (tag != 0xa3) continue;
    Der exts;
    if (!der_next(&v, &tag, &exts) || tag != 0x30) return false;
    while (exts.n > 0) {
      Der ext, oid, val;
      if (!der_next(&exts, &tag, &ext) || tag != 0x30) return false;
      if (!der_next(&ext, &tag, &oid) || tag != 0x06) return false;
      if (!der_next(&ext, &tag, &val)) return false;
      if (tag == 0x01 && !der_next(&ext, &tag, &val)) return false; /* crit */
      if (tag != 0x04) return false;
      Der inner;
      if (der_is(oid, OID_SAN, sizeof OID_SAN)) {
        if (!der_next(&val, &tag, &inner) || tag != 0x30) return false;
        ci->has_san = true;
        ci->san = inner;
      } else if (der_is(oid, OID_EKU, sizeof OID_EKU)) {
        if (!der_next(&val, &tag, &inner) || tag != 0x30) return false;
        ci->has_eku = true;
        while (inner.n > 0) {
          Der u;
          if (!der_next(&inner, &tag, &u) || tag != 0x06) return false;
          if (der_is(u, OID_SERVER_AUTH, sizeof OID_SERVER_AUTH) ||
              der_is(u, OID_ANY_EKU, sizeof OID_ANY_EKU))
            ci->eku_server = true;
        }
      }
    }
  }
  return true;
}

static bool sigalg_is_sha1(Der oid) {
  return der_is(oid, OID_SHA1_RSA, sizeof OID_SHA1_RSA) ||
         der_is(oid, OID_SHA1_RSA_OLD, sizeof OID_SHA1_RSA_OLD) ||
         der_is(oid, OID_SHA1_ECDSA, sizeof OID_SHA1_ECDSA) ||
         der_is(oid, OID_SHA1_DSA, sizeof OID_SHA1_DSA);
}

/* label_eq compares two DNS name pieces, ASCII case-insensitively. */
static bool label_eq(const char *a, size_t an, const char *b, size_t bn) {
  if (an != bn) return false;
  for (size_t i = 0; i < an; i++) {
    unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
    if (x >= 'A' && x <= 'Z') x = (unsigned char)(x + 32);
    if (y >= 'A' && y <= 'Z') y = (unsigned char)(y + 32);
    if (x != y) return false;
  }
  return true;
}

/* dns_match matches a host against one dNSName pattern (see the policy
 * above for the wildcard rule). A trailing dot on either is ignored. */
static bool dns_match(const char *pat, size_t pn, const char *host,
                      size_t hn) {
  if (pn && pat[pn - 1] == '.') pn--;
  if (hn && host[hn - 1] == '.') hn--;
  if (pn == 0 || hn == 0) return false;
  /* An empty label — "a..example.com", or a leading dot — names nothing. */
  for (size_t i = 0; i + 1 < pn; i++)
    if (pat[i] == '.' && pat[i + 1] == '.') return false;
  if (pat[0] == '.') return false;
  if (pat[0] != '*') {
    if (memchr(pat, '*', pn)) return false; /* a '*' anywhere else: never */
    return label_eq(pat, pn, host, hn);
  }
  /* "*." then at least two labels: the rest must hold a dot of its own. */
  if (pn < 2 || pat[1] != '.') return false;
  const char *rest = pat + 2;
  size_t rn = pn - 2;
  if (rn == 0 || memchr(rest, '*', rn) || !memchr(rest, '.', rn))
    return false;
  if (rest[0] == '.' || rest[rn - 1] == '.') return false;
  /* the wildcard covers exactly the host's first label, never empty */
  const char *dot = memchr(host, '.', hn);
  if (!dot || dot == host) return false;
  size_t off = (size_t)(dot - host) + 1;
  return label_eq(rest, rn, host + off, hn - off);
}

typedef struct {
  const br_x509_class *vtable;
  br_x509_minimal_context *inner;
  char host[256];          /* the name (or address) to verify */
  unsigned char ip[16];    /* the host as an address, when it is one */
  size_t iplen;            /* 4, 16, or 0 for a DNS name */
  unsigned char *der[POL_MAXCERTS];
  size_t len[POL_MAXCERTS], cap[POL_MAXCERTS];
  int ncerts;
  char why[320];           /* the policy's refusal, or "" */
} Policy;

static void pol_start_chain(const br_x509_class **ctx, const char *name) {
  (void)name; /* ours is p->host: see above */
  Policy *p = (Policy *)(void *)ctx;
  p->ncerts = 0;
  p->why[0] = 0;
  p->inner->vtable->start_chain(&p->inner->vtable, NULL);
}

static void pol_start_cert(const br_x509_class **ctx, uint32_t length) {
  Policy *p = (Policy *)(void *)ctx;
  if (p->ncerts < POL_MAXCERTS) p->len[p->ncerts] = 0;
  p->inner->vtable->start_cert(&p->inner->vtable, length);
}

static void pol_append(const br_x509_class **ctx, const unsigned char *buf,
                       size_t len) {
  Policy *p = (Policy *)(void *)ctx;
  int i = p->ncerts;
  if (i < POL_MAXCERTS) {
    if (p->len[i] + len > p->cap[i]) {
      size_t c = p->cap[i] ? p->cap[i] : 2048;
      while (c < p->len[i] + len) c *= 2;
      unsigned char *nb = (unsigned char *)realloc(p->der[i], c);
      if (!nb) vl_abort("out of memory");
      p->der[i] = nb;
      p->cap[i] = c;
    }
    memcpy(p->der[i] + p->len[i], buf, len);
    p->len[i] += len;
  }
  p->inner->vtable->append(&p->inner->vtable, buf, len);
}

static void pol_end_cert(const br_x509_class **ctx) {
  Policy *p = (Policy *)(void *)ctx;
  p->ncerts++;
  p->inner->vtable->end_cert(&p->inner->vtable);
}

/* pol_check runs the policy over the recorded chain; the refusal, if any,
 * goes into p->why. */
static void pol_check(Policy *p) {
  int n = p->ncerts < POL_MAXCERTS ? p->ncerts : POL_MAXCERTS;
  CertInfo leaf;
  memset(&leaf, 0, sizeof leaf);
  for (int i = 0; i < n; i++) {
    CertInfo ci;
    if (!cert_parse(p->der[i], p->len[i], &ci)) {
      snprintf(p->why, sizeof p->why, "certificate %d of the chain is "
               "malformed", i + 1);
      return;
    }
    if (i == 0) leaf = ci;
    bool self = ci.issuer.n == ci.subject.n &&
                memcmp(ci.issuer.p, ci.subject.p, ci.issuer.n) == 0;
    if (!self && sigalg_is_sha1(ci.sigalg)) {
      snprintf(p->why, sizeof p->why, "certificate %d of the chain is "
               "signed with SHA-1, which is no longer accepted", i + 1);
      return;
    }
  }
  if (n == 0) return; /* the wrapped engine refuses an empty chain */
  if (leaf.has_eku && !leaf.eku_server) {
    snprintf(p->why, sizeof p->why, "certificate is not for a server (its "
             "extended key usage does not include serverAuth)");
    return;
  }
  if (!leaf.has_san) {
    snprintf(p->why, sizeof p->why, "certificate has no subjectAltName "
             "(a Common Name alone is not accepted)");
    return;
  }
  /* Walk the GeneralNames: [2] dNSName, [7] iPAddress. */
  Der g = leaf.san, v;
  unsigned tag;
  while (g.n > 0) {
    if (!der_next(&g, &tag, &v)) break;
    if (p->iplen == 0 && tag == 0x82 &&
        dns_match((const char *)v.p, v.n, p->host, strlen(p->host)))
      return;
    if (p->iplen != 0 && tag == 0x87 && v.n == p->iplen &&
        memcmp(v.p, p->ip, v.n) == 0)
      return;
  }
  snprintf(p->why, sizeof p->why, "certificate does not match host name "
           "\"%s\"", p->host);
}

static unsigned pol_end_chain(const br_x509_class **ctx) {
  Policy *p = (Policy *)(void *)ctx;
  unsigned r = p->inner->vtable->end_chain(&p->inner->vtable);
  if (r != 0) return r;
  pol_check(p);
  /* Any refusal is reported to the engine as "not trusted"; the session
   * reads the real reason back out of p->why (run_until). */
  return p->why[0] ? BR_ERR_X509_NOT_TRUSTED : 0;
}

static const br_x509_pkey *pol_get_pkey(const br_x509_class *const *ctx,
                                        unsigned *usages) {
  const Policy *p = (const Policy *)(const void *)ctx;
  return p->inner->vtable->get_pkey(
      (const br_x509_class *const *)&p->inner->vtable, usages);
}

static const br_x509_class pol_vtable = {
    sizeof(Policy), pol_start_chain, pol_start_cert, pol_append,
    pol_end_cert,   pol_end_chain,   pol_get_pkey};

static void pol_init(Policy *p, br_x509_minimal_context *inner,
                     const char *host) {
  p->vtable = &pol_vtable;
  p->inner = inner;
  snprintf(p->host, sizeof p->host, "%s", host);
  p->iplen = 0;
  if (inet_pton(AF_INET, host, p->ip) == 1) p->iplen = 4;
  else if (inet_pton(AF_INET6, host, p->ip) == 1) p->iplen = 16;
}

static void pol_free(Policy *p) {
  for (int i = 0; i < POL_MAXCERTS; i++) free(p->der[i]);
}

/* ------------------------------------------------------------ sessions */

typedef struct {
  union {
    br_ssl_client_context c;
    br_ssl_server_context s;
  } u;
  br_x509_minimal_context xc;
  NoVerify nv;
  Policy pol;
  br_ssl_engine_context *eng;
  unsigned char iobuf[BR_SSL_BUFSIZE_BIDI];
  int fd;
  bool handshaken, closed;
  /* The transport under the engine is gone: `broken` after an I/O error,
   * a truncation or a write that failed half-way (every later call fails).
   * BearSSL has no public way to fail its engine from outside, so the
   * session remembers it here. */
  bool broken;
  /* Set — atomically, and WITHOUT `mu` — by close, so a task blocked in
   * run_until on another thread stops touching the descriptor, which close
   * is about to release (and the OS to reuse), before its next syscall. */
  int closing;
  unsigned char *pend; /* application data that arrived during a write */
  size_t pend_len, pend_pos;
  char name[256];      /* the server name, for diagnostics */
  pthread_mutex_t mu;
} Tls;

static Value io_err_of(int en, const char *op) {
  if (en == EAGAIN || en == EWOULDBLOCK) return tls_err("timed out");
  return tls_err("%s: %s", op, strerror(en));
}

/* run_until drives the engine until one of `target`'s states is reached,
 * sending and receiving records as it asks. Returns 0 there, 1 when the
 * connection has ended cleanly (close_notify, or a peer that hung up after
 * the handshake), -1 with *err on failure. A timeout leaves the engine
 * intact: the caller may simply try again. */
static int run_until(Tls *t, unsigned target, Value *err) {
  if (t->broken) {
    *err = tls_err("tls: the connection failed earlier");
    return -1;
  }
  for (;;) {
    if (__atomic_load_n(&t->closing, __ATOMIC_ACQUIRE)) {
      *err = tls_err("tls: the connection is closed");
      return -1;
    }
    unsigned st = br_ssl_engine_current_state(t->eng);
    if (st & BR_SSL_CLOSED) {
      int e = br_ssl_engine_last_error(t->eng);
      if (e == BR_ERR_OK) return 1;
      if (t->pol.why[0]) *err = tls_err("tls: %s", t->pol.why);
      else *err = engine_err(e, t->name);
      return -1;
    }
    if (st & BR_SSL_SENDREC) {
      size_t len;
      unsigned char *buf = br_ssl_engine_sendrec_buf(t->eng, &len);
      ssize_t w = send(t->fd, buf, len, TLS_SEND_FLAGS);
      if (w < 0) {
        if (errno == EINTR) continue;
        int en = errno;
        if (en != EAGAIN && en != EWOULDBLOCK) t->broken = true;
        *err = io_err_of(en, "send");
        return -1;
      }
      br_ssl_engine_sendrec_ack(t->eng, (size_t)w);
      continue;
    }
    if (st & target) return 0;
    if ((st & BR_SSL_RECVAPP) && !(target & BR_SSL_RECVAPP)) {
      /* The peer sent data while we want to write: keep it for read. */
      size_t len;
      unsigned char *buf = br_ssl_engine_recvapp_buf(t->eng, &len);
      if (t->pend_pos > 0 && t->pend_pos == t->pend_len) {
        t->pend_pos = t->pend_len = 0;
      }
      if (t->pend_len + len > TLS_MAX_PEND) {
        /* A peer that streams while we write must not grow this without
         * bound: nobody is reading it. */
        t->broken = true;
        *err = tls_err("tls: the peer sent more than %d bytes nobody read",
                       TLS_MAX_PEND);
        return -1;
      }
      unsigned char *nb = (unsigned char *)realloc(t->pend, t->pend_len + len);
      if (!nb) vl_abort("out of memory");
      t->pend = nb;
      memcpy(t->pend + t->pend_len, buf, len);
      t->pend_len += len;
      br_ssl_engine_recvapp_ack(t->eng, len);
      continue;
    }
    if (st & BR_SSL_RECVREC) {
      size_t len;
      unsigned char *buf = br_ssl_engine_recvrec_buf(t->eng, &len);
      ssize_t r = recv(t->fd, buf, len, 0);
      if (r < 0) {
        if (errno == EINTR) continue;
        int en = errno;
        if (en != EAGAIN && en != EWOULDBLOCK) t->broken = true;
        *err = io_err_of(en, "recv");
        return -1;
      }
      if (r == 0) {
        /* The peer hung up WITHOUT close_notify. That is not an end of
         * stream: anyone on the path can cut a TCP connection, and a body
         * read to its end (an HTTP response with no length) would be
         * silently truncated. So it is an error, as a clean TLS close —
         * close_notify, the BR_SSL_CLOSED/BR_ERR_OK case above — is not. */
        t->broken = true;
        if (t->handshaken)
          *err = tls_err("tls: the peer closed the connection without "
                         "close_notify (the data may be truncated)");
        else
          *err = tls_err("tls: the peer closed the connection during the "
                         "handshake");
        return -1;
      }
      br_ssl_engine_recvrec_ack(t->eng, (size_t)r);
      continue;
    }
    /* Nothing to send or receive and not there yet: the engine is holding
     * a partial record; flushing makes it sendable. If that changes
     * nothing, nothing ever will — fail rather than spin. */
    br_ssl_engine_flush(t->eng, 0);
    if (br_ssl_engine_current_state(t->eng) == st) {
      t->broken = true;
      *err = tls_err("tls: protocol stalled (engine state %u)", st);
      return -1;
    }
  }
}

static int64_t tls_read(void *ctx, char *buf, int64_t n, Value *err) {
  Tls *t = (Tls *)ctx;
  pthread_mutex_lock(&t->mu);
  int64_t out;
  if (t->closed) {
    *err = tls_err("tls: the connection is closed");
    out = -1;
  } else if (t->pend_pos < t->pend_len) {
    size_t k = t->pend_len - t->pend_pos;
    if ((int64_t)k > n) k = (size_t)n;
    memcpy(buf, t->pend + t->pend_pos, k);
    t->pend_pos += k;
    out = (int64_t)k;
  } else {
    int r = run_until(t, BR_SSL_RECVAPP, err);
    if (r == 1) {
      out = 0;
    } else if (r < 0) {
      out = -1;
    } else {
      size_t len;
      unsigned char *p = br_ssl_engine_recvapp_buf(t->eng, &len);
      if ((int64_t)len > n) len = (size_t)n;
      memcpy(buf, p, len);
      br_ssl_engine_recvapp_ack(t->eng, len);
      out = (int64_t)len;
    }
  }
  pthread_mutex_unlock(&t->mu);
  return out;
}

static int64_t tls_write(void *ctx, const char *buf, int64_t n, Value *err) {
  Tls *t = (Tls *)ctx;
  pthread_mutex_lock(&t->mu);
  int64_t done = 0;
  if (t->closed) {
    *err = tls_err("tls: the connection is closed");
    done = -1;
  }
  while (done >= 0 && done < n) {
    int r = run_until(t, BR_SSL_SENDAPP, err);
    if (r != 0) {
      if (r == 1) *err = tls_err("tls: the peer closed the connection");
      /* Bytes already handed to the engine may or may not be on the wire:
       * a retry could send them twice, so the session is done for. */
      if (done > 0) t->broken = true;
      done = -1;
      break;
    }
    size_t len;
    unsigned char *p = br_ssl_engine_sendapp_buf(t->eng, &len);
    if ((int64_t)len > n - done) len = (size_t)(n - done);
    memcpy(p, buf + done, len);
    br_ssl_engine_sendapp_ack(t->eng, len);
    done += (int64_t)len;
  }
  if (done == n) {
    /* Every write is pushed onto the wire before it returns: a request
     * sitting in the engine's buffer would leave both ends waiting. */
    br_ssl_engine_flush(t->eng, 0);
    int r = run_until(t, BR_SSL_SENDAPP | BR_SSL_RECVAPP, err);
    if (r < 0) {
      t->broken = true; /* accepted, perhaps not sent: see above */
      done = -1;
    }
  }
  pthread_mutex_unlock(&t->mu);
  return done;
}

/* close sends close_notify and pushes it out, without waiting for the
 * peer's reply: the descriptor is about to be closed. A session busy on
 * another task (a blocked read holds `mu`) is not waited for. */
static bool tls_close(void *ctx, Value *err) {
  (void)err;
  Tls *t = (Tls *)ctx;
  /* First, unconditionally: a task blocked in run_until must stop using
   * the descriptor (see `closing`). */
  __atomic_store_n(&t->closing, 1, __ATOMIC_RELEASE);
  if (pthread_mutex_trylock(&t->mu) != 0) return true;
  if (!t->closed) {
    t->closed = true;
    br_ssl_engine_close(t->eng);
    /* close_notify is a courtesy, and the socket is being closed anyway:
     * a peer that has stopped reading must not hold this task for the
     * send timeout. The descriptor goes non-blocking for good (a send flag
     * alone is not enough — macOS still blocks a TCP send on a full buffer
     * with MSG_DONTWAIT set). */
    int fl = fcntl(t->fd, F_GETFL, 0);
    if (fl >= 0) fcntl(t->fd, F_SETFL, fl | O_NONBLOCK);
    for (int i = 0; i < 64; i++) {
      unsigned st = br_ssl_engine_current_state(t->eng);
      if (!(st & BR_SSL_SENDREC)) break;
      size_t len;
      unsigned char *buf = br_ssl_engine_sendrec_buf(t->eng, &len);
      ssize_t w = send(t->fd, buf, len, TLS_SEND_FLAGS | MSG_DONTWAIT);
      if (w <= 0) break;
      br_ssl_engine_sendrec_ack(t->eng, (size_t)w);
    }
  }
  pthread_mutex_unlock(&t->mu);
  return true;
}

static void tls_free(void *ctx) {
  Tls *t = (Tls *)ctx;
  pol_free(&t->pol);
  free(t->pend);
  pthread_mutex_destroy(&t->mu);
  free(t);
}

static const VlHandleOps tls_ops = {"tls.Session", tls_free, tls_read,
                                    tls_write, NULL, tls_close};

/* handshake runs the handshake on a fresh session and, on success,
 * installs it as the socket's filter. */
static Value handshake(Tls *t, Value sock) {
  Value err = vl_nil();
  int r = run_until(t, BR_SSL_SENDAPP, &err);
  if (r != 0) {
    if (r == 1) err = tls_err("tls: the peer closed the connection during "
                              "the handshake");
    tls_free(t);
    return err;
  }
  t->handshaken = true;
  Value h = vl_handle_new(&tls_ops, t);
  bool ok = vl_socket_set_filter(sock, h);
  vl_release(h); /* the socket holds it now — or, refused, it is freed */
  if (!ok) return tls_err("tls: the connection already speaks TLS");
  return vl_unit();
}

static Tls *tls_new(int fd, const char *name) {
  Tls *t = (Tls *)vl_alloc(sizeof(Tls));
  t->fd = fd;
  snprintf(t->name, sizeof t->name, "%s", name);
  pthread_mutex_init(&t->mu, NULL);
  return t;
}

static int want_fd(Value sock, Value *err) {
  int fd = vl_socket_fd(sock);
  if (fd < 0) *err = tls_err("tls: the connection is closed");
  else if (vl_socket_filter(sock).t != VL_NIL)
    *err = tls_err("tls: the connection already speaks TLS");
  else return fd;
  return -1;
}

static const char *want_str(Value v, const char *who) {
  if (v.t != VL_OBJ || v.u.o->kind != O_STR)
    vl_abort("%s: expected a str, got %s", who, vl_kind_name(v));
  return vl_cstr(v);
}

/* name_why says what is wrong with a server name, or NULL when it is a
 * host name or an IP address. It is checked before the name is used for
 * anything — SNI or verification — because both take it as a C string:
 * "a.example.com\0.evil" would be verified as its first half. */
static const char *name_why(const char *name, int64_t n) {
  if ((int64_t)strlen(name) != n) return "contains a NUL byte";
  if (n == 0) return NULL;
  if (n > 253) return "is longer than 253 bytes";
  /* An IPv6 address is the only name with a ':' — and then all of it. */
  unsigned char a6[16];
  if (strchr(name, ':'))
    return inet_pton(AF_INET6, name, a6) == 1
               ? NULL
               : "contains ':' but is not an IPv6 address";
  size_t label = 0;
  for (int64_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c == '.') {
      /* a leading dot, "..", or a second trailing dot: an empty label */
      if (label == 0) return "has an empty label";
      if (name[i - 1] == '-') return "has a label that ends with '-'";
      label = 0;
      continue;
    }
    if (c == '*') return "contains '*' (a wildcard is a certificate's, not "
                         "a host's)";
    if (c >= 0x80) return "is not ASCII (use its IDNA xn-- form)";
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_';
    if (!ok) return "contains a character no host name has";
    if (label == 0 && c == '-') return "has a label that starts with '-'";
    if (++label > 63) return "has a label longer than 63 bytes";
  }
  if (name[n - 1] == '-') return "has a label that ends with '-'";
  return NULL;
}

/* tls._client(sock, server_name, ca_file, insecure) -> unit!. */
static Value tls_client(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  const char *name = want_str(argv[1], "tls.client");
  const char *ca = want_str(argv[2], "tls.client");
  bool insecure = argv[3].t == VL_BOOL && argv[3].u.b;
  Value err = vl_nil();
  int fd = want_fd(argv[0], &err);
  if (fd < 0) return err;
  const char *why = name_why(name, vl_strlen(argv[1]));
  if (why) return tls_err("tls: server name %s", why);
  Anchors *a = NULL;
  if (!insecure) {
    if (!name[0])
      return tls_err("tls: verifying a certificate needs a server name");
    const char *path = ca[0] ? ca : system_ca();
    if (!path)
      return tls_err("tls: no system CA bundle found — set Config.ca_file "
                     "(or $SSL_CERT_FILE)");
    a = load_anchors(path, &err);
    if (!a) return err;
  }
  Tls *t = tls_new(fd, name);
  br_ssl_client_init_full(&t->u.c, &t->xc, a ? a->ta : NULL, a ? a->n : 0);
  t->eng = &t->u.c.eng;
  br_ssl_engine_set_versions(t->eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_set_buffer(t->eng, t->iobuf, sizeof t->iobuf, 1);
  /* A client never renegotiates: nothing here needs it, and it has been
   * the root of more TLS attacks than features. */
  br_ssl_engine_add_flags(t->eng, BR_OPT_NO_RENEGOTIATION);
  if (insecure) {
    t->nv.vtable = &nv_vtable;
    br_ssl_engine_set_x509(t->eng, &t->nv.vtable);
  } else {
    pol_init(&t->pol, &t->xc, name);
    br_ssl_engine_set_x509(t->eng, &t->pol.vtable);
  }
  /* SNI carries a host NAME only (RFC 6066): an IP address is verified
   * against the certificate's IP SANs (Policy), but never sent as SNI. */
  bool is_ip = t->pol.iplen != 0 || strchr(name, ':') != NULL;
  if (!br_ssl_client_reset(&t->u.c, name[0] && !is_ip ? name : NULL, 0)) {
    Value e = engine_err(br_ssl_engine_last_error(t->eng), name);
    tls_free(t);
    return e;
  }
  return handshake(t, argv[0]);
}

/* tls._server(sock, cert_file, key_file) -> unit!. */
static Value tls_server(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  const char *cert = want_str(argv[1], "tls.server");
  const char *key = want_str(argv[2], "tls.server");
  Value err = vl_nil();
  int fd = want_fd(argv[0], &err);
  if (fd < 0) return err;
  ServerMat *m = load_server(cert, key, &err);
  if (!m) return err;
  Tls *t = tls_new(fd, "");
  if (m->key_type == BR_KEYTYPE_RSA)
    br_ssl_server_init_full_rsa(&t->u.s, m->chain, m->n,
                                br_skey_decoder_get_rsa(&m->skd));
  else
    br_ssl_server_init_full_ec(&t->u.s, m->chain, m->n, m->issuer_type,
                               br_skey_decoder_get_ec(&m->skd));
  t->eng = &t->u.s.eng;
  br_ssl_engine_set_versions(t->eng, BR_TLS12, BR_TLS12);
  br_ssl_engine_set_buffer(t->eng, t->iobuf, sizeof t->iobuf, 1);
  /* No client-initiated renegotiation: a cheap way for one client to make
   * the server do a handshake's worth of work again and again. */
  br_ssl_engine_add_flags(t->eng, BR_OPT_NO_RENEGOTIATION);
  if (!br_ssl_server_reset(&t->u.s)) {
    Value e = engine_err(br_ssl_engine_last_error(t->eng), "");
    tls_free(t);
    return e;
  }
  return handshake(t, argv[0]);
}

/* tls._check_server(cert_file, key_file) -> unit!: load the pair now, so a
 * listener reports a bad file when it starts rather than on the first
 * connection. */
static Value tls_check_server(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  Value err = vl_nil();
  if (!load_server(want_str(argv[0], "tls.listen"),
                   want_str(argv[1], "tls.listen"), &err))
    return err;
  return vl_unit();
}

/* tls._system_ca() -> str: the CA bundle a client trusts by default, or "". */
static Value tls_system_ca(Value *argv, int argc, VlFrame *up) {
  (void)argv, (void)argc, (void)up;
  const char *p = system_ca();
  return vl_str(p ? p : "");
}

/* tls._version(sock) -> str: "TLS1.2", or "" for a plain socket. */
static Value tls_version(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  Tls *t = (Tls *)vl_handle_ctx(vl_socket_filter(argv[0]), &tls_ops);
  if (!t) return vl_str("");
  pthread_mutex_lock(&t->mu);
  unsigned v = br_ssl_engine_get_version(t->eng);
  pthread_mutex_unlock(&t->mu);
  switch (v) {
  case BR_TLS12: return vl_str("TLS1.2");
  case BR_TLS11: return vl_str("TLS1.1");
  case BR_TLS10: return vl_str("TLS1.0");
  default: return vl_str("");
  }
}

/* The natives table (voila_std.h): registered by the program's main() when
 * it uses std/tls. Every row is a name in low.StdSrcNatives. */
const VlExtEntry vl_ext_tls[] = {
    {"tls._client", tls_client},
    {"tls._server", tls_server},
    {"tls._check_server", tls_check_server},
    {"tls._system_ca", tls_system_ca},
    {"tls._version", tls_version},
    {NULL, NULL},
};
