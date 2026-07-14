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

#endif /* VOILA_INT_H */
