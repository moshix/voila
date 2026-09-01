/* voila_int.h — runtime-internal object layouts. Not part of the ABI that
 * generated code sees. */
#ifndef VOILA_INT_H
#define VOILA_INT_H

#include "voila.h"
#include <pthread.h>

/* ---------------------------------------------------------------- objects */

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

typedef struct {
  VlObj hdr;
  Value v; /* deeply immutable */
} VlShared;

typedef struct {
  VlObj hdr;
  Value v;
  pthread_mutex_t mu;
} VlCell;

typedef struct {
  VlObj hdr;
  VlObj *target; /* NOT retained */
} VlWeak;

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
} VlSocket;

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
  Value owner; /* retained O_FILE / O_SOCKET, or nil */
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
} VlCtx;

VlCtx *vl_ctx(void);
void vl_ctx_teardown(void); /* free a task thread's context at exit */

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
void vl_throw_conv(const char *from, const char *to, Value value);
void vl_set_abort_handler(Value h);

/* type registry */
const VlType *vl_find_type(const char *name);
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
Value vl_regex_method(Value self, const char *name, Value *argv, int argc,
                      bool *ok);

#endif /* VOILA_INT_H */
