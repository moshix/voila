/* voila_int.h — runtime-internal object layouts. Not part of the ABI that
 * generated code sees. */
#ifndef VOILA_INT_H
#define VOILA_INT_H

#include "voila.h"
#include <pthread.h>

/* ---------------------------------------------------------------- objects */

/* The code generator also emits LARGE string constants as static objects
 * of this exact shape (voilac/cg/cg.voi, emit_consts): keep the two in step,
 * member for member. */
typedef struct {
  VlObj hdr;
  int64_t len;
  char b[1]; /* UTF-8, NUL-terminated */
} VlStr;

/* Arbitrary-precision decimal: value = (-1)^neg * coef * 10^exp, where the
 * coefficient is held as decimal digits, most significant first. Decimal
 * limbs keep the arithmetic exactly what the specification describes and
 * make rounding (half-even, on a digit boundary) trivial to get right. */
typedef struct {
  VlObj hdr;
  int8_t neg;
  int32_t exp;
  int32_t nd;
  char d[1]; /* '0'..'9' */
} VlDec;

typedef struct {
  VlObj hdr;
  Value *e;
  int64_t len, cap;
} VlSlice;

typedef struct VlEntry {
  Value k, v;
  uint64_t hash;
  bool used;
} VlEntry;

/* Insertion-ordered map: an entry vector in insertion order plus an open
 * addressed index. Iteration order is deterministic — the golden tests and
 * the self-hosted compiler depend on it. */
typedef struct {
  VlObj hdr;
  VlEntry *ents;
  int64_t nents, capents; /* insertion order, may contain tombstones */
  int64_t *idx;           /* open addressing into ents, -1 empty */
  int64_t nidx;
  int64_t count;
  bool is_set;
} VlMap;

/* VlSeq walks a SEQUENCE argument (native kind 'q'): a slice's elements in
 * order, or a set's members in insertion order (§3.3: a set preserves it),
 * skipping a set's tombstones. Nothing is allocated, and every element it
 * yields is BORROWED from the collection. The collection's storage is read
 * afresh at every step, never cached: the loop body may run user code (an
 * operator on a newtype, say) that grows the collection and moves it. */
typedef struct {
  Value v;
  int64_t i;
} VlSeq;

static inline VlSeq vl_seq(Value v) {
  VlSeq q = {v, 0};
  return q;
}

static inline bool vl_seq_next(VlSeq *q, Value *out) {
  Value v = q->v;
  if (v.t != VL_OBJ) return false;
  if (v.u.o->kind == O_SLICE) {
    VlSlice *s = (VlSlice *)v.u.o;
    if (q->i >= s->len) return false;
    *out = s->e[q->i++];
    return true;
  }
  if (v.u.o->kind == O_SET || v.u.o->kind == O_MAP) {
    VlMap *m = (VlMap *)v.u.o;
    while (q->i < m->nents) {
      const VlEntry *en = &m->ents[q->i++];
      if (en->used) {
        *out = en->k;
        return true;
      }
    }
  }
  return false;
}

/* The number of elements a VlSeq over `v` yields. */
static inline int64_t vl_seq_len(Value v) {
  if (v.t != VL_OBJ) return 0;
  if (v.u.o->kind == O_SLICE) return ((VlSlice *)v.u.o)->len;
  if (v.u.o->kind == O_SET || v.u.o->kind == O_MAP)
    return ((VlMap *)v.u.o)->count;
  return 0;
}

typedef struct {
  VlObj hdr;
  Value *e;
  int n;
} VlTuple;

typedef struct {
  VlObj hdr;
  const VlType *type;
  Value *f; /* one per field, in declaration order */
} VlStruct;

typedef struct {
  VlObj hdr;
  const VlVariant *var;
  Value *f;
  int n;
} VlEnum;

typedef struct {
  VlObj hdr;
  Value type_name; /* str, "" for a plain err() */
  Value msg;       /* str */
  Value fields;    /* struct value, or nil */
  Value cause;     /* err or nil */
  Value suppressed;/* slice of err */
  Value trace;     /* slice of str */
} VlErr;

typedef struct {
  VlObj hdr;
  VlFn fn;
  VlFrame *up;    /* captured frame (retained), NULL for natives */
  const char *name;
  const char *kinds; /* a native's argument kinds (vl_native_kinds), or NULL */
} VlClosure;

typedef struct {
  VlObj hdr;
  Value *buf;
  int64_t cap, head, tail, count;
  bool closed;
  pthread_mutex_t mu;
  pthread_cond_t cv;
} VlChan;

typedef struct VlGroup VlGroup;

typedef struct {
  VlObj hdr;
  pthread_t th;
  bool joined, done;
  Value result;
  Value exc;
  bool consumed;
  VlGroup *group;
  pthread_mutex_t mu;
  pthread_cond_t cv;
  /* work */
  VlFn fn;
  Value closure;
  Value *argv;
  int argc;
} VlTask;

/* VlWeakCtl is the control block a `shared` value and its `weak` handles
 * share (§6.3): the handles hold the BLOCK, not the value, so a handle
 * outliving the value finds `target` NULL instead of freed memory. `rc`
 * counts the value (while it lives) plus every handle; the block is freed
 * when it reaches zero. All of it is guarded by one runtime mutex
 * (value.c): weak handles are created and upgraded rarely. */
typedef struct {
  int64_t rc;
  VlObj *target; /* the shared value, or NULL once it has been freed */
} VlWeakCtl;

typedef struct {
  VlObj hdr;
  Value v; /* deeply immutable */
  VlWeakCtl *wctl; /* NULL until the first weak handle is made */
} VlShared;

/* Box[T] (§6.3): one owned value behind a unique heap indirection. It is
 * what makes a recursive enum or struct legal under the Acyclicity Rule —
 * `Node(v T, l Box[Tree[T]], r Box[Tree[T]])` — and it is TRANSPARENT to
 * everything that reads a value: printing, equality, ordering, fields,
 * methods and pattern matching all see the value inside. `get()` reads it
 * and `set(v)` replaces it in place. */
typedef struct {
  VlObj hdr;
  Value v;
} VlBox;

typedef struct {
  VlObj hdr;
  Value v;
  pthread_mutex_t mu;
} VlCell;

typedef struct {
  VlObj hdr;
  VlWeakCtl *ctl; /* NULL for the zero weak handle */
} VlWeak;

Value vl_weak_upgrade(Value w);

typedef struct {
  VlObj hdr;
  int64_t lo, hi, by;
  bool inclusive;
} VlRange;

typedef struct {
  VlObj hdr;
  FILE *f;
  Value name;
  bool closed, is_std;
} VlFile;

/* A network socket owns an OS file descriptor and closes it when the last
 * reference is released (obj_free), mirroring VlFile. std/net wraps it in a
 * Voilà Listener/Conn/Udp struct; this struct is deliberately opaque. */
typedef struct {
  VlObj hdr;
  int fd;
  bool closed;
  int timeout_ms;   /* 0 = no deadline; used to bound accept() portably */
  /* A FILTER the socket's bytes pass through, or nil: an O_HANDLE whose
   * read/write ops replace recv/send (std/tls installs its session here
   * after the handshake). Every byte path — net._send, net._recv and an
   * io.reader/io.writer over the socket — consults it, so a TLS connection
   * is still a plain net.Conn to every Voilà layer above. Set once, by
   * vl_socket_set_filter; released in obj_free BEFORE the fd is closed. */
  Value flt;
  /* net._close is ending the filter's protocol outside g_sockmu (std.c):
   * the descriptor is still this socket's, and that close will release it. */
  bool closing;
} VlSocket;

/* ------------------------------------------------------- external handles
 *
 * VlHandle is how a C EXTENSION (runtime/src/ext: a TLS session, a
 * compression stream, an SQLite connection or statement) hands a resource it
 * owns to Voilà code. libvoila knows nothing of what is behind `ctx`; the
 * extension's ops table says how to free it and, for a byte stream, how to
 * read and write it. Because the core reaches the resource only through
 * these pointers, it links none of the vendored libraries.
 *
 * read  returns the bytes stored (>0), 0 at end of stream, or -1 with *err
 *       set to an owned error value.
 * write returns the bytes consumed (>0), or -1 with *err set.
 * flush/close are optional (NULL: nothing to do); close must be idempotent.
 * free  runs once, from obj_free, when the last reference is dropped. It
 *       must not do I/O that can fail visibly: a stream that needs to write
 *       a trailer does so in close, which the program calls.
 *
 * A handle used as an io.reader or io.writer's source (io.c) keeps every
 * guarantee the reader/writer gives a file: the reader retains the handle,
 * and its mutex serializes the calls into ops. */
typedef struct VlHandleOps {
  const char *name; /* printed as <name>, and the kind name in diagnostics */
  void (*free)(void *ctx);
  int64_t (*read)(void *ctx, char *buf, int64_t n, Value *err);
  int64_t (*write)(void *ctx, const char *buf, int64_t n, Value *err);
  bool (*flush)(void *ctx, Value *err);
  bool (*close)(void *ctx, Value *err);
} VlHandleOps;

typedef struct {
  VlObj hdr;
  const VlHandleOps *ops;
  void *ctx;
} VlHandle;

/* vl_handle_new wraps ctx; it is freed by ops->free with the last
 * reference. vl_handle_ctx is the checked unwrap: the ctx when v is a handle
 * made with exactly these ops, else NULL — so an extension can never be
 * handed another extension's handle and cast it. */
Value vl_handle_new(const VlHandleOps *ops, void *ctx);
void *vl_handle_ctx(Value v, const VlHandleOps *ops);

/* The socket half of std/net, for an extension that layers a protocol on a
 * connected socket (std/tls). vl_socket_fd is the descriptor, or -1 when v
 * is not an open socket. vl_socket_set_filter installs an O_HANDLE (see
 * VlSocket.flt) and fails — false — if one is already installed. */
int vl_socket_fd(Value v);
bool vl_socket_set_filter(Value sock, Value filter);
/* The installed filter, BORROWED; nil when there is none (or v is no
 * socket). */
Value vl_socket_filter(Value v);

/* std/io — a buffered reader over one byte source, and a writer over one byte
 * sink. They are runtime KINDS rather than Voilà structs for two reasons:
 * `os.stdin` must exist with no `use` at all, and vl_iter_next must be able to
 * stream lines from one (`each line in os.stdin`).
 *
 * The source is named INDIRECTLY whenever a Voilà value owns it: `owner` is a
 * retained O_FILE or O_SOCKET and the live handle is read back out of it on
 * every operation (rd_src in io.c). That costs one branch and buys two things
 * a cached FILE* or fd cannot:
 *   - the owner cannot be freed under us — without the retain,
 *     `io.reader(must os.open(p))` drops the file, obj_free fclose()s it, and
 *     the reader is left holding a dangling stream;
 *   - closing the owner (`f.close()`, `conn.close()`) is SEEN here, so the
 *     reader reports "closed" instead of reading whatever the OS has since
 *     handed that descriptor number to.
 * `owner` is nil only for a source no Voilà value owns: the process's own
 * stdin/stdout/stderr (is_std), and the two ends of io.pipe() (own_src, which
 * says closing the descriptor is ours to do).
 *
 * b[r..w) is what has been read from the source and not yet handed out. Tasks
 * are OS threads and the standard handles are process-wide singletons, so
 * every method holds `mu` for the whole of its work. */
typedef struct {
  VlObj hdr;
  int fd;      /* -1 when FILE*-backed */
  FILE *f;     /* NULL when fd-backed */
  Value owner; /* retained O_FILE / O_SOCKET / O_HANDLE, or nil */
  Value name;  /* str, for name() and for printing */
  char *b;
  int64_t cap, r, w;
  bool eof, closed, own_src, is_std;
  pthread_mutex_t mu;
} VlReader;

/* A writer is deliberately UNBUFFERED: stdio already buffers a FILE*, and a
 * second buffer in front of os.stdout would reorder output against `say`,
 * which writes to stdout directly. flush() therefore only has work to do on
 * the FILE*-backed ones. */
typedef struct {
  VlObj hdr;
  int fd;
  FILE *f;
  Value owner;
  Value name;
  bool closed, own_src, is_std, discard;
  pthread_mutex_t mu;
} VlWriter;

/* std/sync — the three primitives that need a machine-level object. They are
 * runtime KINDS and not Voilà structs because a pthread_mutex_t has to sit at
 * a fixed address for its whole life: a Voilà struct is a heap object whose
 * fields are Values, with nowhere to keep one.
 *
 * std/sync wraps each of these in a Voilà struct (`Mutex{h: …}`), exactly as
 * std/net wraps VlSocket in `Conn{h: …}`, so the closure-shaped API can be
 * written in Voilà with `defer` — see sync.c's header for why that matters. */
typedef struct {
  VlObj hdr;
  pthread_mutex_t mu; /* PTHREAD_MUTEX_ERRORCHECK; see sync.c */
} VlMutex;

typedef struct {
  VlObj hdr;
  pthread_rwlock_t rw;
} VlRWLock;

/* `n` is touched ONLY through __atomic_* (sync.c). Nothing else in the runtime
 * may read or write it directly — a plain load beside an atomic store is a
 * data race whether or not the compiler happens to emit the same instruction. */
typedef struct {
  VlObj hdr;
  int64_t n;
} VlAtomic;

typedef struct {
  VlObj hdr;
  char *b;
  int64_t len, cap;
} VlBuilder;

/* Iterator (an internal object reusing O_TUPLE-free space). */
typedef struct {
  VlObj hdr;
  Value src;
  int64_t i;
  Value ch; /* channel iteration */
} VlIter;

/* ---------------------------------------------------------------- task ctx */

struct VlGroup {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  int live;
  bool cancelled;
  Value failure;     /* first failure (err value or exception) */
  Value suppressed;  /* slice */
  bool failure_is_value;
  VlGroup *parent;
  int64_t deadline_ns; /* 0 = none */
};

typedef struct {
  VlFrame **frames;
  int nframes, capframes;
  VlHandler *handlers;
  Value pending; /* in-flight exception */
  int digits;
  int *digit_stack;
  int ndigits, capdigits;
  VlGroup *group;
  VlTask *self;
  int recover; /* open recovery points (vl_call_contained) on this thread */
  /* The unwind-cleanup stack (err.c, vl_cleanup_*): what a native that
   * calls back into Voilà must release if the callback throws through it. */
  struct VlCleanup *cleanups;
  int ncleanups, capcleanups;
  /* The group this thread is BLOCKED joining (vl_group_end), or NULL. Read
   * by other threads, atomically — std/sqlite uses it to see that a task
   * holding a connection is waiting for the very task that wants it. */
  VlGroup *joining;
} VlCtx;

/* Unwind cleanups. A native that holds an owned temporary across a call
 * back into Voilà (xs.map's result slice, sort's scratch array, a builder
 * a Show method writes into) registers it; a throw or a contained fault
 * that unwinds through the native releases it, and the native's normal
 * return drops the registration with vl_cleanup_to(mark). */
int vl_cleanup_mark(void);
void vl_cleanup_slot(Value *slot);    /* release *slot on unwind */
void vl_cleanup_mem(void **p);        /* free(*p) on unwind */
void vl_cleanup_int(int *p, int v);   /* *p = v on unwind */
void vl_cleanup_to(int mark);         /* forget entries from mark on */
void vl_cleanup_lock(VlLock *l);      /* vl_lock_release(l) on unwind */
void vl_cleanup_values(Value *a, int64_t n); /* release a[0..n), free a */

/* Re-entrant locks (value.c): a frame's, for its captured variables
 * (vl_varlock), and a striped table picked by a collection's address, for
 * a builtin method that grows, shrinks or reorders it. */
void vl_lock_acquire(VlLock *l);
void vl_lock_release(VlLock *l);
VlLock *vl_addr_lock(const void *addr);

/* Judge the frames whose cycle judgement had to wait for calls over them
 * to end (value.c): at a task's end and a group's join. */
void vl_judge_deferred(void);

VlCtx *vl_ctx(void);
void vl_ctx_teardown(void); /* free a task thread's context at exit */

/* ------------------------------------------------- newtypes and boxes */

void vl_own_inner(Value holder);
void vl_set_disp(const char *type);

static inline bool vl_is_newtype(Value v) {
  return v.t == VL_OBJ && v.u.o->kind == O_STRUCT &&
         ((VlStruct *)v.u.o)->type->newtype;
}
static inline bool vl_is_box(Value v) {
  return v.t == VL_OBJ && v.u.o->kind == O_BOX;
}
/* vl_deref strips every Box and newtype layer around a value: the value an
 * operator, an index, a call or a pattern actually works on. BORROWED. */
static inline Value vl_deref(Value v) {
  for (;;) {
    if (vl_is_box(v)) v = ((VlBox *)v.u.o)->v;
    else if (vl_is_newtype(v)) v = ((VlStruct *)v.u.o)->f[0];
    else return v;
  }
}
/* vl_unbox strips Box layers only — a newtype keeps its identity (methods,
 * equality) where a Box has none. BORROWED. */
static inline Value vl_unbox(Value v) {
  while (vl_is_box(v)) v = ((VlBox *)v.u.o)->v;
  return v;
}
Value vl_box_new(Value owned);
/* vl_newtype_wrap wraps an OWNED underlying value as newtype `t`. */
Value vl_newtype_wrap(const VlType *t, Value owned);
const VlType *vl_user_type(const char *name);

/* ---------------------------------------------------------------- shared */

void *vl_alloc(size_t n);
VlObj *vl_obj_new(VlKind kind, size_t size);
uint64_t vl_hash(Value v);
const char *vl_kind_name(Value v);   /* type name for diagnostics */
Value vl_str_take(char *s, int64_t n); /* takes ownership of malloc'd s */

/* Decimal internals shared with fmt/json. */
char *vl_dec_str(Value d);                    /* caller frees */
char *vl_dec_fixed(Value d, int places);      /* caller frees */
Value vl_dec_add(Value a, Value b);
Value vl_dec_sub(Value a, Value b);
Value vl_dec_mul(Value a, Value b);
Value vl_dec_div(Value a, Value b);
Value vl_dec_idiv(Value a, Value b);
Value vl_dec_mod(Value a, Value b);
Value vl_dec_pow(Value a, int64_t e);
int vl_dec_cmp(Value a, Value b);
Value vl_dec_round(Value a, int places);
Value vl_dec_floor(Value a);
Value vl_dec_ceil(Value a);
Value vl_dec_trunc(Value a);
Value vl_dec_abs(Value a);
Value vl_dec_neg_v(Value a);
bool vl_dec_is_int(Value a);
bool vl_dec_try_parse(const char *s, Value *out);
void vl_throw_conv_cstr(const char *from, const char *to, const char *text)
    __attribute__((noreturn));
bool vl_dec_to_int(Value a, int64_t *out);
double vl_dec_to_float(Value a);
Value vl_dec_from_float(double f);

/* string helpers used across files */
Value vl_str_concat(Value a, Value b);
char *vl_strdup_n(const char *s, int64_t n);

/* map/slice internals */
void vl_map_set(Value m, Value k, Value v);
bool vl_map_get(Value m, Value k, Value *out); /* out is owned */
bool vl_map_has(Value m, Value k);
void vl_map_delete(Value m, Value k);
int64_t vl_map_count(Value m);
Value vl_map_keys(Value m);
Value vl_map_values(Value m);
void vl_slice_append(Value s, Value v); /* takes ownership */
Value vl_slice_of(Value *elems, int n);
int64_t vl_len(Value v);
void vl_sort_slice(Value s, Value cmp);
Value vl_str_call(const char *name, Value *argv, int argc, bool *ok);
Value vl_str_method(Value self, const char *name, Value *argv, int argc, bool *ok);
Value vl_file_method(Value self, const char *name, Value *argv, int argc, bool *ok);

/* vl_records_of is the shared body of File.records and Reader.records: the
 * fixed-width record split of §13.2. One copy, so the two cannot drift. */
Value vl_records_of(Value text, int64_t width);

/* ---------------------------------------------------------------- std/io */

/* io.c. The standard handles are process-wide singletons made once by
 * vl_io_init (called from vl_init): vl_lookup_value re-runs at EVERY LOADFN,
 * so a fresh reader per `os.stdin` would drop whatever the previous one had
 * buffered. vl_stdin/vl_stdout/vl_stderr return them BORROWED. */
void vl_io_init(void);
Value vl_stdin(void);
Value vl_stdout(void);
Value vl_stderr(void);

/* io.reader / io.writer as C: an owned reader/writer, or an IOError VALUE. */
Value vl_reader_of(Value src);
Value vl_writer_of(Value dst);

Value vl_reader_method(Value self, const char *name, Value *argv, int argc,
                       bool *ok);
Value vl_writer_method(Value self, const char *name, Value *argv, int argc,
                       bool *ok);
bool vl_io_isatty(Value h);

/* obj_free's arms, kept in io.c so the POSIX close(2) stays in one file. */
void vl_reader_free(VlObj *o);
void vl_writer_free(VlObj *o);

/* The std/io natives, registered by name in std.c's table[]. They are
 * VlFn-shaped so the table can point straight at them. */
Value vl_io_reader(Value *argv, int argc, VlFrame *up);
Value vl_io_writer(Value *argv, int argc, VlFrame *up);
Value vl_io_buffered(Value *argv, int argc, VlFrame *up);
Value vl_io_copy(Value *argv, int argc, VlFrame *up);
Value vl_io_pipe(Value *argv, int argc, VlFrame *up);
Value vl_io_read_all(Value *argv, int argc, VlFrame *up);
Value vl_io_discard(Value *argv, int argc, VlFrame *up);

/* -------------------------------------------------------------- std/crypto */

/* crypto.c. vl_random_bytes is the process's ONLY cryptographically secure
 * random source — getrandom(2) on Linux, getentropy(2) on macOS and the BSDs,
 * /dev/urandom where neither is reachable — and it ABORTS rather than
 * degrading to a weak one. crypto.random, crypto.random_int and uuid.new all
 * go through it.
 *
 * vl_try_random_bytes is the same source, reporting failure instead. It has
 * ONE caller by design: vl_init's seeding of std/rand, which runs in every
 * program including those that never draw a random number, and so must not
 * be able to stop one from starting. std/rand is random(3) either way — fast,
 * reproducible, and fit for neither uuid.new's job nor a key. */
void vl_random_bytes(void *out, size_t n);
bool vl_try_random_bytes(void *out, size_t n);

/* uuid.new's body, kept beside the entropy source it depends on. */
Value vl_uuid_v4(void);

/* The std/crypto natives, registered by name in std.c's table[]. SHA-1 and
 * MD5 are LEGACY — collisions are practical for both — and exist for
 * checksums and interop, never for anything that must resist an attacker. */
Value vl_crypto_md5(Value *argv, int argc, VlFrame *up);
Value vl_crypto_md5_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_sha1(Value *argv, int argc, VlFrame *up);
Value vl_crypto_sha1_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_sha256(Value *argv, int argc, VlFrame *up);
Value vl_crypto_sha256_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_sha512(Value *argv, int argc, VlFrame *up);
Value vl_crypto_sha512_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_md5(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_md5_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_sha1(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_sha1_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_sha256(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_sha256_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_sha512(Value *argv, int argc, VlFrame *up);
Value vl_crypto_hmac_sha512_raw(Value *argv, int argc, VlFrame *up);
Value vl_crypto_random(Value *argv, int argc, VlFrame *up);
Value vl_crypto_random_int(Value *argv, int argc, VlFrame *up);
Value vl_crypto_equal(Value *argv, int argc, VlFrame *up);

/* ---------------------------------------------------------------- std/sync */

/* sync.c. The natives are VlFn-shaped so std.c's table[] can point straight at
 * them, and NONE of them calls back into Voilà — no lock is ever held across
 * user code by the runtime. The closure forms (`with`, `Once.do`) are Voilà
 * source over `defer`, which unlocks on the throw path too. */
Value vl_sync_mutex_new(void);
Value vl_sync_rwlock_new(void);
Value vl_sync_atomic_new(int64_t n);

/* obj_free's arms, and the one read str_into needs. */
void vl_sync_mutex_free(VlObj *o);
void vl_sync_rwlock_free(VlObj *o);
int64_t vl_sync_atomic_peek(VlObj *o);

Value vl_sync_mutex(Value *argv, int argc, VlFrame *up);
Value vl_sync_lock(Value *argv, int argc, VlFrame *up);
Value vl_sync_unlock(Value *argv, int argc, VlFrame *up);
Value vl_sync_try_lock(Value *argv, int argc, VlFrame *up);
Value vl_sync_rwlock(Value *argv, int argc, VlFrame *up);
Value vl_sync_rlock(Value *argv, int argc, VlFrame *up);
Value vl_sync_wlock(Value *argv, int argc, VlFrame *up);
Value vl_sync_rwunlock(Value *argv, int argc, VlFrame *up);
Value vl_sync_try_rlock(Value *argv, int argc, VlFrame *up);
Value vl_sync_try_wlock(Value *argv, int argc, VlFrame *up);
Value vl_sync_atomic(Value *argv, int argc, VlFrame *up);
Value vl_sync_load(Value *argv, int argc, VlFrame *up);
Value vl_sync_store(Value *argv, int argc, VlFrame *up);
Value vl_sync_add(Value *argv, int argc, VlFrame *up);
Value vl_sync_swap(Value *argv, int argc, VlFrame *up);
Value vl_sync_cas(Value *argv, int argc, VlFrame *up);

/* method dispatch shared with std.c */
const VlMethod *vl_find_method(const char *type, const char *name);
Value vl_user_show(Value v, bool *ok);

/* fmt */
Value vl_sprintf_v(Value fmtv, Value *argv, int argc);
void vl_fprint(FILE *f, Value *argv, int argc, bool newline);

/* builder */
void vl_builder_write(Value b, Value v);
Value vl_builder_done(Value b);
int64_t vl_builder_len(Value b);

/* errors / unwinding */
void vl_unwind_to(int depth);
Value vl_err_message(Value err);
Value vl_err_cstr(const char *msg); /* error value; owns its message */
void vl_throw_conv(const char *from, const char *to, Value value);
void vl_set_abort_handler(Value h);

/* type registry */
const VlType *vl_find_type(const char *name);
bool vl_enum_exists(const char *type);
/* Faults (err.c): the aborts a recovery point may contain. */
void vl_fault(const char *fmt, ...) __attribute__((noreturn));
bool vl_is_fault(Value exc);
Value vl_call_contained(Value *argv, int argc, VlFrame *up);

/* std/os's process handle (proc.c): a child started by os.start. `in` and
 * `out` are the std/io writer and reader over its stdin and stdout pipes;
 * `status` is valid once `waited`, and `mu` serialises the reaping, so two
 * tasks waiting at once both get the one status. */
typedef struct {
  VlObj hdr;
  int64_t pid;
  bool waited;
  int64_t status;
  Value in, out;
  pthread_mutex_t mu;
} VlProc;
Value vl_proc_method(Value self, const char *name, Value *argv, int argc,
                     bool *ok);
void vl_proc_free(VlObj *o);
void vl_proc_str(Value b, Value v);
bool vl_os_signal_const(const char *name, int64_t *out);
Value vl_os_signals(Value *argv, int argc, VlFrame *up);
Value vl_os_kill(Value *argv, int argc, VlFrame *up);
Value vl_os_start(Value *argv, int argc, VlFrame *up);
Value vl_os_walk(Value *argv, int argc, VlFrame *up);
Value vl_os_walk_all(Value *argv, int argc, VlFrame *up);
Value vl_os_kill_group(Value *argv, int argc, VlFrame *up);
Value vl_os_signal_stop(Value *argv, int argc, VlFrame *up);
Value vl_os_glob(Value *argv, int argc, VlFrame *up);
/* std/ffi's natives (ffi.c). */
Value vl_ffi_alloc(Value *argv, int argc, VlFrame *up);
Value vl_ffi_free(Value *argv, int argc, VlFrame *up);
Value vl_ffi_read(Value *argv, int argc, VlFrame *up);
Value vl_ffi_write(Value *argv, int argc, VlFrame *up);
Value vl_ffi_read_str(Value *argv, int argc, VlFrame *up);
Value vl_ffi_write_str(Value *argv, int argc, VlFrame *up);
/* io.c: a reader/writer that OWNS a raw descriptor (os.start's pipes). */
Value vl_io_fd_reader(int fd, const char *name);
Value vl_io_fd_writer(int fd, const char *name);
/* task.c: a send that never blocks and never throws — false (and the value
 * released) when the channel is full or closed. The signal dispatcher's. */
bool vl_chan_try_send(Value ch, Value owned);
/* std/json (json.c). decode takes the type text json.decode[T] passes, or
 * NULL for the untyped json.decode(s). */
Value vl_json_encode(Value v, bool pretty);
Value vl_json_decode(const char *type, Value src);
const VlVariant *vl_find_variant(const char *type, const char *variant);

/* files */
Value vl_file_new(FILE *f, const char *name, bool is_std);

/* iteration over a channel needs the task layer */
Value vl_chan_iter_next(Value ch, bool *ok);

/* ------------------------------------------------------------------ regex */

void vl_regex_free(VlObj *o);
const char *vl_regex_src(Value v);
Value vl_regex_compile(Value pat);
Value vl_regex_matches(Value pat, Value subject);
Value vl_regex_find(Value pat, Value subject);
Value vl_regex_find_all(Value pat, Value subject);
Value vl_regex_replace(Value pat, Value subject, Value repl);
Value vl_regex_split(Value pat, Value subject);
Value vl_regex_find_named(Value pat, Value subject);
Value vl_regex_names(Value pat);
Value vl_regex_method(Value self, const char *name, Value *argv, int argc,
                      bool *ok);

#endif /* VOILA_INT_H */
