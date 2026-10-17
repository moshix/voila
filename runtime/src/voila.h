/* voila.h — the Voilà native runtime (libvoila).
 *
 * The C backend compiles a Voilà program to C that calls into this API.
 * Memory is reference-counted, never garbage-collected: the Acyclicity Rule
 * (spec §6.3) guarantees the owning type graph is a DAG, so refcounts always
 * reach zero.
 *
 * Ownership convention, obeyed by every function here and by generated code:
 *
 *     - helpers BORROW their arguments and return an OWNED value
 *     - frame slots OWN their contents
 *     - vl_set(&slot, owned) releases the old value and takes the new one
 *
 * Generated code therefore reads:  vl_set(&F->r[3], vl_add(F->r[1], F->r[2]));
 */
#ifndef VOILA_H
#define VOILA_H

/* On glibc, -std=c11 hides POSIX/BSD declarations — clock_gettime, nanosleep,
 * readlink, random/srandom — unless a feature-test macro is defined before the
 * first system header. voila.h is the first header every translation unit
 * includes (directly, or via voila_int.h), so this is the one place to set it.
 * It is scoped to Linux: macOS exposes those APIs already, and defining
 * _POSIX_C_SOURCE there would instead HIDE the BSD ones. */
#if defined(__linux__)
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#endif

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---------------------------------------------------------------- values */

typedef enum {
  VL_NIL,
  VL_UNIT,
  VL_BOOL,
  VL_INT,
  VL_FLOAT,
  VL_RUNE,
  VL_DUR,     /* duration, nanoseconds */
  VL_INSTANT, /* instant, nanoseconds since the epoch */
  VL_OBJ
} VlTag;

typedef struct VlObj VlObj;

typedef struct {
  VlTag t;
  union {
    int64_t i;
    double f;
    uint32_t r;
    bool b;
    VlObj *o;
  } u;
} Value;

typedef enum {
  O_STR,
  O_DEC,
  O_SLICE,
  O_MAP,
  O_SET,
  O_TUPLE,
  O_STRUCT,
  O_ENUM,
  O_ERR,
  O_CLOSURE,
  O_NATIVE,
  O_CHAN,
  O_TASK,
  O_SHARED,
  O_CELL,
  O_WEAK,
  O_RANGE,
  O_FILE,
  O_BUILDER,
  O_TYPE,
  O_FRAME,
  O_ITER,
  O_SOCKET,     /* a network socket (std/net); owns an OS file descriptor */
  O_READER,     /* std/io: a buffered byte source (os.stdin, io.reader) */
  O_WRITER,     /* std/io: a byte sink (os.stdout, io.writer) */
  O_MUTEX,      /* std/sync: a pthread mutex (sync.Mutex) */
  O_RWLOCK,     /* std/sync: a pthread rwlock (sync.RWLock) */
  O_ATOMIC,     /* std/sync: one atomically-updated int64 (sync.Atomic) */
  O_REGEX,      /* std/regex: a compiled pattern (its NFA program) */
  O_BOX,        /* Box[T]: an owned, unique heap indirection (§6.3) */
  O_PROC,       /* std/os: a started child process (os.start, proc.c) */
  O_HANDLE      /* a resource a C extension owns (VlHandle, voila_int.h) */
} VlKind;

struct VlObj {
  int32_t rc;
  uint8_t kind;
};

/* ---------------------------------------------------------------- frames */

typedef struct VlFrame VlFrame;

/* A re-entrant lock owned by a task context (value.c, vl_lock_acquire). */
typedef struct {
  void *owner;
  int depth;
} VlLock;

/* VlFnInfo names a compiled function for stack traces (§8.6, manual §9.5):
 * its Voilà name as a user reads it and the source file it came from. One
 * static instance per function, emitted by the code generator. */
typedef struct {
  const char *name;
  const char *file;
  /* upmask[i] = 1: register i is read by a closure of this function
   * through an upvalue (vl_frame_clear must not drop it at exit). NULL:
   * none is. */
  const unsigned char *upmask;
  int nupmask;
} VlFnInfo;

/* A compiled function's registers live in a heap frame so closures can
 * capture them by reference (the interpreter's environments, made explicit).
 * `up` is the lexically enclosing frame — an upvalue is (depth, slot). */
struct VlFrame {
  VlObj hdr;
  VlFrame *up;
  int nregs;
  /* 1: the storage is the caller's C stack (frame elision, -O2). Never
   * free()d; still registered on the ctx frame stack so the exception
   * unwinder releases its registers exactly as for a heap frame. Occupies
   * what was struct padding: sizeof(VlFrame) is unchanged. */
  uint8_t stackalloc;
  /* 1: some closure captured this frame (or a descendant), so it can cross
   * threads and its refcount needs atomic updates. A closure is the ONLY
   * way a frame escapes its thread, and vl_closure sets the flag on the
   * whole up-chain BEFORE the closure value escapes — so on every
   * SYNCHRONIZED publication route (channel send, spawn, await, group,
   * select, cell: each has a mutex/pthread_create happens-before edge) a
   * plain read of this flag is race-free, and the overwhelmingly common
   * unshared frame keeps the plain non-atomic ++/-- it always had.
   * Publishing a closure through a MODULE GLOBAL has no such edge: plain
   * 16-byte Value stores already tear there (a pre-existing data race on
   * any cross-task global), and this flag adds one more reason that
   * pattern is undefined — pass closures through channels, not globals. */
  uint8_t shared;
  /* 1 once the function has returned (vl_frame_clear): only closures read
   * the frame from then on. Also former padding. */
  uint8_t done;
  /* 1 once a closure over this frame — or over a frame below it — MAY
   * ESCAPE its task: one the lowerer could not prove is only called in
   * place or handed straight to a builtin collection method that runs it
   * at once (low/expr.voi, closure_escapes). Set when such a closure is
   * made (vl_closure_escapes), on the frame's own task, before the closure
   * value is anywhere else; set on every frame of the chain, so a frame
   * that escapes has ancestors that do. Only then can another task reach
   * the frame's variables: reads of them take the frame's lock
   * (vl_varread) and `mut` arguments in them are copied in (SHAREDTHR).
   * Former padding. */
  uint8_t escapes;
  Value *defers;
  void *reaper; /* the task clearing a garbage frame (self_cycle_only) */ /* closures, run LIFO on exit and on unwind */
  /* The frame's reference count (hdr.rc is unused for frames): real
   * references, calls in progress and judgement holds, packed (value.c,
   * FRAME_REFS). Then the judgement's bookkeeping: a request to judge
   * again, and whether this frame counts as a call in progress of its
   * `up` (frame_enter). */
  uint64_t frc;
  uint8_t pending;
  uint8_t inact;
  VlLock vlock; /* its captured variables' writes (vl_varlock) */
  int ndefers, capdefers;
  /* Where this activation is, for an abort's or an uncaught exception's
   * stack trace: the function (set once, after the frame is made) and the
   * source line of the last statement that can fail (one store per source
   * line, emitted by the code generator only where a trap is possible). */
  const VlFnInfo *info;
  int line;
  Value r[1]; /* flexible */
};

VlFrame *vl_frame_new(int nregs, VlFrame *up);
VlFrame *vl_frame_stack_init(VlFrame *f, int nregs, VlFrame *up);

/* The -O2 prologue: registers live in the caller's C stack frame. VL_NIL is
 * all-zero bits, so the zeroed buffer needs no nil-init loop. */
#define VL_FRAME_ON_STACK(Fv, N, UP)                                        \
  struct {                                                                  \
    VlFrame f;                                                              \
    Value pad_[(N) > 1 ? (N) - 1 : 1];                                      \
  } Fv##_buf;                                                               \
  VlFrame *Fv = vl_frame_stack_init(&Fv##_buf.f, (N), (UP))
void vl_frame_release(VlFrame *f);
void vl_frame_clear(VlFrame *f); /* breaks frame<->closure reference cycles */
void vl_frame_push(VlFrame *f);           /* onto the task's frame stack */
void vl_frame_pop_run_defers(VlFrame *f); /* normal exit: defers, then pop */
void vl_defer(VlFrame *f, Value closure);

Value vl_up(VlFrame *f, int depth, int slot);            /* borrowed */
void vl_upset(VlFrame *f, int depth, int slot, Value v); /* takes ownership */

/* ---------------------------------------------------------------- memory */

Value vl_retain(Value v);
void vl_release(Value v);
void vl_set(Value *slot, Value owned);

/* ---------------------------------------------------------------- ctors */

Value vl_nil(void);
Value vl_unit(void);
Value vl_bool(bool b);
Value vl_int(int64_t n);
Value vl_float(double f);
Value vl_rune(uint32_t r);
Value vl_dur(int64_t ns);
Value vl_instant(int64_t ns);

Value vl_str(const char *s);              /* NUL-terminated */
Value vl_str_n(const char *s, int64_t n); /* explicit length */
const char *vl_cstr(Value v);             /* borrowed bytes, NUL-terminated */
int64_t vl_strlen(Value v);

Value vl_dec_parse(const char *s); /* throws ConvError on garbage */
Value vl_dec_from_int(int64_t n);

Value vl_slice_new(int64_t n); /* n nil elements */
Value vl_map_new(void);
Value vl_set_new(void);
Value vl_tuple_new(Value *elems, int n);
Value vl_range_new(int64_t lo, int64_t hi, int64_t by, bool inclusive);
Value vl_builder_new(void);

/* ---------------------------------------------------------------- types */

/* Type descriptors are emitted by the code generator. */
typedef struct {
  const char *name;
  int nfields;
  const char *const *fields;
  const char *const *ftypes; /* declared type of each field, for zero values */
  /* 1: a NEWTYPE (`type Cel int`, §3.8) — a distinct named type over one
   * underlying value, held in the single field f[0]. The runtime treats it
   * as transparent wherever the underlying value is what is wanted
   * (printing, calls, indexing, iteration, comparison) and as nominal
   * wherever the type is (method dispatch, equality across types). Trailing
   * member: initializers that omit it leave it 0. */
  int newtype;
  /* Each field's DECLARED DEFAULT (`limit int = 10`), as a pointer into the
   * program's constant pool, or NULL where the field has none; the member
   * itself is NULL for a type that declares no default. The generator
   * applies defaults at a literal itself — this is for code that builds a
   * struct from data, json.decode[T] (json.c), so that a field with a
   * default may be absent. Trailing, like `newtype`. */
  const Value *const *defaults;
} VlType;

typedef struct {
  const char *type;    /* owning enum */
  const char *variant; /* variant name */
  int nfields;
  const char *const *fields;
  /* The declared type of each payload field (json.decode[T] reads them);
   * trailing, so an initializer that omits it leaves it NULL. */
  const char *const *ftypes;
} VlVariant;

/* The generated program registers its metadata before main runs. */
typedef Value (*VlFn)(Value *argv, int argc, VlFrame *up);

typedef struct {
  const char *type;
  const char *method;
  VlFn fn;
  bool has_self;
} VlMethod;

void vl_register_types(const VlType *types, int ntypes, const VlVariant *vars,
                       int nvars, const VlMethod *methods, int nmethods);

Value vl_struct_new(const char *type, Value *positional, int npos,
                    const char **names, Value *named, int nnamed);
Value vl_enum_new(const char *type, const char *variant, Value *argv, int argc);
Value vl_exc_new(const char *type, Value *positional, int npos,
                 const char **names, Value *named, int nnamed);
Value vl_zero(const char *type_text); /* zero value of a declared type */

/* ---------------------------------------------------------------- calls */

Value vl_closure(VlFn fn, VlFrame *up);
Value vl_native(const char *name, VlFn fn);
/* The argument-kind check of a native call (std.c, low.NativeKinds). */
void vl_argk(const char *name, const char *kinds, Value *argv, int argc,
             bool fix);
const char *vl_native_kinds(const char *name);
Value vl_call(Value callee, Value *argv, int argc); /* borrowed argv */
Value vl_call_named(Value callee, Value *argv, int argc, const char **names,
                    int nnamed);
Value vl_callm(Value recv, const char *method, Value *argv, int argc);
/* Static trait methods through a type parameter (voilac/low/traitdesc.voi):
 * a type descriptor is the type's flat name as a str. */
Value vl_typedesc(Value v);
/* A Box's copy-on-write (types.c). */
Value vl_box_unique(Value v, int64_t n);
Value vl_cow_unique(Value v, int64_t n);
Value *vl_up_slot(VlFrame *f, int depth, int slot);
/* A write through a variable that closures capture (low/boxcow.voi,
 * var_root): its frame's lock is held from the start of the write's path
 * to its store (or, for a `mut` argument or a `mut self` receiver, to the
 * call's return), so writes from several tasks into one captured variable
 * go one at a time and none is lost to a copy-on-write judged by another
 * writer's temporaries (§10). Released on unwind too. */
void vl_varlock(VlFrame *f);
void vl_varunlock(VlFrame *f);
VlFrame *vl_frame_up(VlFrame *f, int depth);
/* A read of a variable that closures capture: an OWNED reference, taken
 * under the frame's lock once a closure over the frame may escape its task
 * (value.c). */
Value vl_varread(VlFrame *f, int slot);
/* The closure just made may escape its task: mark its frame chain. */
void vl_closure_escapes(Value c);
Value vl_callm_as(Value recv, const char *type, const char *method,
                  Value *argv, int argc);
Value vl_self_desc(Value self);
/* The object kinds with value semantics that UNIQ copies on write: a Box, a
 * struct, a slice, a map and a set (bit i = VlKind i). */
#define VL_COW_KINDS ((1u << O_BOX) | (1u << O_STRUCT) | (1u << O_SLICE) | \
                      (1u << O_MAP) | (1u << O_SET))
Value vl_callt(Value desc, const char *method, Value *argv, int argc);
Value vl_spawn_fn(VlFn fn, Value *argv, int argc);
Value vl_spawn_closure(Value closure);
Value vl_await(Value task);

/* ---------------------------------------------------------------- ops */

Value vl_add(Value a, Value b);
Value vl_sub(Value a, Value b);
Value vl_mul(Value a, Value b);
Value vl_div(Value a, Value b);  /* exact division → float (§5.1) */
Value vl_idiv(Value a, Value b); /* floor division */
Value vl_mod(Value a, Value b);  /* floor remainder */
Value vl_pow(Value a, Value b);
Value vl_cat(Value a, Value b); /* || string concatenation */
Value vl_shl(Value a, Value b);
Value vl_shr(Value a, Value b);
Value vl_band(Value a, Value b);
Value vl_bor(Value a, Value b);
Value vl_bxor(Value a, Value b);
Value vl_neg(Value a);
Value vl_not(Value a);
Value vl_bnot(Value a);

bool vl_truthy(Value v); /* bool only; anything else aborts */
bool vl_equal(Value a, Value b);
int vl_compare(Value a, Value b); /* -1, 0, 1; throws on incomparable */
Value vl_cmpeq(Value a, Value b);
Value vl_cmpne(Value a, Value b);
Value vl_cmplt(Value a, Value b);
Value vl_cmple(Value a, Value b);
Value vl_cmpgt(Value a, Value b);
Value vl_cmpge(Value a, Value b);
Value vl_cmpin(Value a, Value b); /* x in coll */

Value vl_index(Value coll, Value idx);            /* out of bounds ABORTS */
Value vl_slice_range(Value coll, Value range);    /* out of range THROWS */
void vl_setidx(Value coll, Value idx, Value v);   /* takes ownership of v */
Value vl_field(Value obj, const char *name);      /* borrowed → owned */
void vl_setfld(Value obj, const char *name, Value v);
Value vl_conv(const char *type, Value v, Value *base); /* checked (§3.2) */
Value vl_interp(Value *parts, int n);                  /* string interpolation */

/* Iteration protocol: ranges, slices, maps, sets, strings, channels. */
Value vl_iter(Value src);
bool vl_iter_next(Value it, Value *key, Value *val); /* owned outputs */

/* Helpers emitted by the C backend. */
int64_t vl_int_of(Value v);
Value *vl_slice_data(Value s);
void vl_spread(Value dst, Value src);
void vl_slice_append(Value s, Value owned);
Value vl_map_new(void);
Value vl_set_new(void);
int64_t vl_len(Value v);

/* ---------------------------------------------------------------- errors */

typedef struct VlHandler {
  jmp_buf buf;
  struct VlHandler *prev;
  int frame_depth;
  int cleanup_depth; /* the unwind-cleanup stack's height (err.c) */
} VlHandler;

void vl_eh_prepare(VlHandler *h);
void vl_eh_pop(void);
Value vl_eh_current(void); /* owned copy of the in-flight exception */
/* The in-flight exception INCLUDING a fault (an abort under a recovery
 * point): what a try with a `finally` needs, to run the finally on a
 * fault's way past (err.c). */
Value vl_eh_pending(void);
bool vl_istype(Value exc, const char *types); /* comma-separated names */

#define VL_TRY(h) (vl_eh_prepare(&(h)), setjmp((h).buf))

Value vl_err(Value msg);       /* error VALUE (§8.1) */
Value vl_errf(Value *argv, int argc);
bool vl_isfail(Value v);       /* err or nil — the `else` operator's test */
Value vl_tryp(Value v);        /* `try expr`: propagate failure (see below) */
Value vl_must(Value v);        /* error value → exception */
void vl_throw(Value exc);      /* raise; unwinds, running defers */
void vl_rethrow(Value exc);
void vl_throw_from(Value exc, Value cause);
void vl_throwf(const char *type, const char *fmt, ...);
void vl_abort(const char *fmt, ...); /* §8.6: uncatchable, no defers */

/* ------------------------------------------------- native arithmetic (-O3)
 * The unboxing pass renders int/float/bool registers as C locals and their
 * arithmetic as the inline `_ck` helpers below. TRAP IDENTITY is the
 * contract: every trap message exists ONCE, in the cold vl_trap_* functions
 * (ops.c), which are the bodies of BOTH the boxed path and these inlines —
 * so optimized and unoptimized programs cannot disagree on a message. */

void vl_trap_iovf(const char *op, int64_t a, int64_t b) __attribute__((noreturn));
void vl_trap_div0(void) __attribute__((noreturn));
void vl_trap_itof(int64_t n) __attribute__((noreturn));
void vl_trap_shift(int64_t n) __attribute__((noreturn));
void vl_trap_inegovf(void) __attribute__((noreturn));

/* Out-of-line numeric cores shared by both paths (rare enough not to
 * warrant inlining; each carries its trap checks inside). */
int64_t vl_ifloordiv(int64_t a, int64_t b);   /* ~/ : div0, INT64_MIN~/-1 */
int64_t vl_ifloormod(int64_t a, int64_t b);   /* %  : div0                */
int64_t vl_ipow_i(int64_t a, int64_t b);      /* ** : overflow, neg exp   */
double  vl_ffloormod(double x, double y);     /* %  : div0, sign fix      */
int64_t vl_ifloordiv_f(double x, double y);   /* ~/ : div0, floor, trunc  */
double  vl_fpow(double x, double y);          /* ** : no traps            */

static inline int64_t vl_iadd_ck(int64_t a, int64_t b) {
  int64_t c;
  if (__builtin_add_overflow(a, b, &c)) vl_trap_iovf("+", a, b);
  return c;
}
static inline int64_t vl_isub_ck(int64_t a, int64_t b) {
  int64_t c;
  if (__builtin_sub_overflow(a, b, &c)) vl_trap_iovf("-", a, b);
  return c;
}
static inline int64_t vl_imul_ck(int64_t a, int64_t b) {
  int64_t c;
  if (__builtin_mul_overflow(a, b, &c)) vl_trap_iovf("*", a, b);
  return c;
}
/* int → float only when exactly representable (§3.2) — same rule and same
 * message as the boxed widening. */
static inline double vl_itof_ck(int64_t n) {
  if (n > (1LL << 53) || n < -(1LL << 53)) vl_trap_itof(n);
  return (double)n;
}
/* `/` on two ints yields float (§5.1): div0 is checked BEFORE the operands
 * widen, matching the boxed order of traps. */
static inline double vl_idivf_ck(int64_t a, int64_t b) {
  if (b == 0) vl_trap_div0();
  /* Sequenced: C leaves the division's operand order unspecified, and the
   * ConvError must name the LEFT operand first on every toolchain. */
  double x = vl_itof_ck(a);
  return x / vl_itof_ck(b);
}
static inline double vl_fdiv_ck(double x, double y) {
  if (y == 0) vl_trap_div0();
  return x / y;
}
static inline int64_t vl_ishl_ck(int64_t a, int64_t n) {
  if (n < 0 || n > 63) vl_trap_shift(n);
  /* Via uint64_t: shifting a negative (or into the sign bit) is UB on the
   * signed type; two's-complement wrap is the documented behavior. */
  return (int64_t)((uint64_t)a << n);
}
static inline int64_t vl_ishr_ck(int64_t a, int64_t n) {
  if (n < 0 || n > 63) vl_trap_shift(n);
  return a >> n;
}
static inline int64_t vl_ineg_ck(int64_t a) {
  if (a == INT64_MIN) vl_trap_inegovf();
  return -a;
}

/* ------------------------------------------------- sized integers (§3.2)
 * Every integer is an int64 at run time; a SIZED type (i8 … u64) is a range
 * the compiler knows statically. Arithmetic whose static type is sized, a
 * compound assignment to a sized location, and a store of a value of
 * another type into one are checked against that range — the type's code
 * travels as a small integer, and the message names the type. As with the
 * int64 traps above, every message lives once, in the cold vl_trap_sz*
 * functions (ops.c), behind both the boxed and the -O3 native path. u64 is
 * 0..INT64_MAX: the representation is an int64. */
enum { VL_SZ_I8 = 1, VL_SZ_I16, VL_SZ_I32, VL_SZ_U8, VL_SZ_U16, VL_SZ_U32,
       VL_SZ_U64 };
static inline int64_t vl_sz_lo(int t) {
  switch (t) {
  case VL_SZ_I8: return -128;
  case VL_SZ_I16: return -32768;
  case VL_SZ_I32: return -2147483648LL;
  default: return 0;
  }
}
static inline int64_t vl_sz_hi(int t) {
  switch (t) {
  case VL_SZ_I8: return 127;
  case VL_SZ_I16: return 32767;
  case VL_SZ_I32: return 2147483647LL;
  case VL_SZ_U8: return 255;
  case VL_SZ_U16: return 65535;
  case VL_SZ_U32: return 4294967295LL;
  default: return INT64_MAX;
  }
}
void vl_trap_szovf(const char *op, int64_t a, int64_t b, int t)
    __attribute__((noreturn));
void vl_trap_szneg(int64_t a, int t) __attribute__((noreturn));
void vl_trap_szval(int64_t v, int t) __attribute__((noreturn));
/* Boxed forms: the arithmetic op (0 +, 1 -, 2 *, 3 **, 4 ~/) on two
 * values, range-checked when the result is an integer; the negation; and a
 * check of a value about to be stored into a sized location. */
Value vl_arith_sz(int op, Value a, Value b, int t);
Value vl_neg_sz(Value a, int t);
void vl_cksz(Value v, int t);

static inline int64_t vl_iadd_sz(int64_t a, int64_t b, int t) {
  int64_t c;
  if (__builtin_add_overflow(a, b, &c) || c < vl_sz_lo(t) || c > vl_sz_hi(t))
    vl_trap_szovf("+", a, b, t);
  return c;
}
static inline int64_t vl_isub_sz(int64_t a, int64_t b, int t) {
  int64_t c;
  if (__builtin_sub_overflow(a, b, &c) || c < vl_sz_lo(t) || c > vl_sz_hi(t))
    vl_trap_szovf("-", a, b, t);
  return c;
}
static inline int64_t vl_imul_sz(int64_t a, int64_t b, int t) {
  int64_t c;
  if (__builtin_mul_overflow(a, b, &c) || c < vl_sz_lo(t) || c > vl_sz_hi(t))
    vl_trap_szovf("*", a, b, t);
  return c;
}
static inline int64_t vl_ineg_sz(int64_t a, int t) {
  if (a == INT64_MIN || -a < vl_sz_lo(t) || -a > vl_sz_hi(t))
    vl_trap_szneg(a, t);
  return -a;
}
/* ** and ~/ use their out-of-line int64 cores (whose own traps — a
 * negative exponent, division by zero — come first); the range check
 * follows. */
static inline int64_t vl_ipow_sz(int64_t a, int64_t b, int t) {
  int64_t c = vl_ipow_i(a, b);
  if (c < vl_sz_lo(t) || c > vl_sz_hi(t)) vl_trap_szovf("**", a, b, t);
  return c;
}
static inline int64_t vl_idiv_sz(int64_t a, int64_t b, int t) {
  int64_t c = vl_ifloordiv(a, b);
  if (c < vl_sz_lo(t) || c > vl_sz_hi(t)) vl_trap_szovf("~/", a, b, t);
  return c;
}
static inline int64_t vl_cksz_i(int64_t v, int t) {
  if (v < vl_sz_lo(t) || v > vl_sz_hi(t)) vl_trap_szval(v, t);
  return v;
}

/* `try expr` inside a function that returns T!: the generated code checks
 * vl_isfail and jumps to the function's exit with the failure as its result.
 * vl_tryp exists for the interpreter-shaped path and simply returns v. */

/* ---------------------------------------------------------------- tasks */

void vl_group_begin(Value timeout); /* nil = no timeout */
/* Plain `group` rethrows a task failure; `try group` yields it as an error
 * value (nil when the group succeeded). */
Value vl_group_end(bool as_error_value);
Value vl_time_after(int64_t ns);
void vl_sleep_ns(int64_t ns);
int64_t vl_now_ns(void);

Value vl_chan_new(int64_t cap);
void vl_chan_send(Value ch, Value v); /* takes ownership: sending moves */
Value vl_chan_recv(Value ch);
Value vl_chan_recv_ok(Value ch, Value *ok);
void vl_chan_close(Value ch);
void vl_check_cancelled(void);

/* select: the generated code describes its cases, the runtime picks one. */
typedef enum { VL_SEL_RECV, VL_SEL_SEND, VL_SEL_DEFAULT } VlSelKind;
typedef struct {
  VlSelKind kind;
  Value ch;
  Value send; /* VL_SEL_SEND */
} VlSelCase;
int vl_select(VlSelCase *cases, int n, Value *recv_val, bool *recv_ok);

Value vl_shared(Value v);
Value vl_cell(Value v);
Value vl_weak(Value shared);

/* ---------------------------------------------------------------- numeric */

void vl_numeric_digits(int n);
int vl_get_digits(void);
void vl_digits_save(void);
void vl_digits_restore(void);

/* ---------------------------------------------------------------- text */

Value vl_tostr(Value v); /* the form `say` prints (Show honored) */
char *vl_fmt_float(double f); /* Go-compatible shortest form; caller frees */

/* ---------------------------------------------------------------- parse */

typedef enum { VL_T_VAR, VL_T_DISCARD, VL_T_LIT, VL_T_COL } VlTermKind;
typedef struct {
  VlTermKind kind;
  int slot;        /* VL_T_VAR: frame register */
  const char *lit; /* VL_T_LIT */
  int col;         /* VL_T_COL */
} VlTerm;
void vl_parse(VlFrame *f, Value src, const char *fold, const VlTerm *terms,
              int nterms);

/* ---------------------------------------------------------------- match */

/* Patterns are emitted as a flat postfix program; see cgen. */
typedef enum {
  VL_P_WILD,
  VL_P_NIL,
  VL_P_LIT,
  VL_P_BIND,
  VL_P_VARIANT,
  VL_P_OR,     /* elems = the alternatives; the first that matches wins */
  VL_P_RANGE,  /* elems = lo, hi (VL_P_LIT); name "=" when inclusive */
  VL_P_STRUCT, /* name = struct type; elems = VL_P_FIELD */
  VL_P_FIELD,  /* name = field; elems[0] = the field's sub-pattern */
  VL_P_TUPLE   /* elems = one sub-pattern per position */
} VlPatKind;
typedef struct VlPat {
  VlPatKind kind;
  const char *name; /* variant, struct type or field name; range "=" */
  int slot;         /* VL_P_BIND: frame register */
  int lit;          /* VL_P_LIT: index into the program's constant pool */
  int nelems;
  const struct VlPat *elems;
} VlPat;
bool vl_match(VlFrame *f, Value subject, const VlPat *p, const Value *consts);

/* ---------------------------------------------------------------- entry */

void vl_init(int argc, char **argv);
int vl_finish(void); /* joins the root group; returns the exit code */
void vl_say(Value *argv, int argc);

/* ---------------------------------------------------------------- ffi */

/* The conversions around a call into C from an `ffi "C" { … }` block (§12,
 * ffi.c): the generator unboxes each argument with one of these, calls the
 * C function through a prototype bound to its symbol, and boxes the result.
 * `argi` and `fn` name the argument in an abort. */
int64_t vl_ffi_int(Value v, int argi, const char *fn);
double vl_ffi_float(Value v, int argi, const char *fn);
bool vl_ffi_bool(Value v, int argi, const char *fn);
const char *vl_ffi_cstr(Value v, int argi, const char *fn);
void *vl_ffi_ptr(Value v, int argi, const char *fn);
Value vl_ffi_ret_u64(unsigned long long r);
Value vl_ffi_ret_ptr(void *p);
Value vl_ffi_ret_cstr(const char *s);
int vl_ffi_vclass(Value v);      /* 1: a variadic argument travels as double */
double vl_ffi_vdouble(Value v);
long long vl_ffi_vword(Value v);
/* VL_FFI_SYM spells a C symbol for an asm label: the platform's user-label
 * prefix (`_` on Darwin, nothing on Linux) glued to the name. Binding the
 * prototype to the symbol this way gives it a C name of the generator's own,
 * so it can never clash with a declaration the headers already made. */
#define VL_FFI_STR2(x) #x
#define VL_FFI_STR(x) VL_FFI_STR2(x)
#define VL_FFI_SYM(name) VL_FFI_STR(__USER_LABEL_PREFIX__) name

/* Builtin (prelude) and package functions used by generated code are
 * declared in voila_std.h, which the generator includes. */
#include "voila_std.h"

#endif /* VOILA_H */
