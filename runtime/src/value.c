/* value.c — values, reference counting, frames, equality, printing. */
#include "voila_int.h"

static void frame_mark_shared(VlFrame *f);
static void frame_retain(VlFrame *f);

#include <assert.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>   /* close() for O_SOCKET in obj_free */

void *vl_alloc(size_t n) {
  void *p = calloc(1, n);
  if (!p) {
    fprintf(stderr, "voila: out of memory\n");
    exit(1);
  }
  return p;
}

VlObj *vl_obj_new(VlKind kind, size_t size) {
  VlObj *o = (VlObj *)vl_alloc(size);
  o->rc = 1;
  o->kind = (uint8_t)kind;
  return o;
}

char *vl_strdup_n(const char *s, int64_t n) {
  char *p = (char *)vl_alloc((size_t)n + 1);
  memcpy(p, s, (size_t)n);
  p[n] = 0;
  return p;
}

/* ---------------------------------------------------------------- ctors */

Value vl_nil(void) { Value v; v.u.i = 0; v.t = VL_NIL; v.u.i = 0; return v; }
Value vl_unit(void) { Value v; v.u.i = 0; v.t = VL_UNIT; v.u.i = 0; return v; }
Value vl_bool(bool b) { Value v; v.u.i = 0; v.t = VL_BOOL; v.u.b = b; return v; }
Value vl_int(int64_t n) { Value v; v.u.i = 0; v.t = VL_INT; v.u.i = n; return v; }
Value vl_float(double f) { Value v; v.u.i = 0; v.t = VL_FLOAT; v.u.f = f; return v; }
Value vl_rune(uint32_t r) { Value v; v.u.i = 0; v.t = VL_RUNE; v.u.r = r; return v; }
Value vl_dur(int64_t ns) { Value v; v.u.i = 0; v.t = VL_DUR; v.u.i = ns; return v; }
Value vl_instant(int64_t ns) { Value v; v.u.i = 0; v.t = VL_INSTANT; v.u.i = ns; return v; }

static Value obj(VlObj *o) {
  Value v;
  v.u.i = 0;
  v.t = VL_OBJ;
  v.u.o = o;
  return v;
}

Value vl_str_n(const char *s, int64_t n) {
  VlStr *o = (VlStr *)vl_obj_new(O_STR, sizeof(VlStr) + (size_t)n);
  o->len = n;
  if (n > 0) memcpy(o->b, s, (size_t)n);
  o->b[n] = 0;
  return obj(&o->hdr);
}

Value vl_str(const char *s) { return vl_str_n(s, (int64_t)strlen(s)); }

Value vl_str_take(char *s, int64_t n) {
  Value v = vl_str_n(s, n);
  free(s);
  return v;
}

const char *vl_cstr(Value v) {
  if (v.t != VL_OBJ || v.u.o->kind != O_STR) vl_abort("expected str, got %s", vl_kind_name(v));
  return ((VlStr *)v.u.o)->b;
}

int64_t vl_strlen(Value v) {
  if (v.t != VL_OBJ || v.u.o->kind != O_STR) vl_abort("expected str");
  return ((VlStr *)v.u.o)->len;
}

Value vl_slice_new(int64_t n) {
  VlSlice *s = (VlSlice *)vl_obj_new(O_SLICE, sizeof(VlSlice));
  s->len = n;
  s->cap = n > 0 ? n : 4;
  s->e = (Value *)vl_alloc(sizeof(Value) * (size_t)s->cap);
  for (int64_t i = 0; i < n; i++) s->e[i] = vl_nil();
  return obj(&s->hdr);
}

Value vl_slice_of(Value *elems, int n) {
  Value v = vl_slice_new(0);
  for (int i = 0; i < n; i++) vl_slice_append(v, vl_retain(elems[i]));
  return v;
}

void vl_slice_append(Value sv, Value owned) {
  VlSlice *s = (VlSlice *)sv.u.o;
  if (s->len == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 4;
    s->e = (Value *)realloc(s->e, sizeof(Value) * (size_t)s->cap);
    if (!s->e) vl_abort("out of memory");
  }
  s->e[s->len++] = owned;
}

Value vl_tuple_new(Value *elems, int n) {
  VlTuple *t = (VlTuple *)vl_obj_new(O_TUPLE, sizeof(VlTuple));
  t->n = n;
  t->e = (Value *)vl_alloc(sizeof(Value) * (size_t)(n ? n : 1));
  for (int i = 0; i < n; i++) t->e[i] = vl_retain(elems[i]);
  return obj(&t->hdr);
}

Value vl_range_new(int64_t lo, int64_t hi, int64_t by, bool inclusive) {
  VlRange *r = (VlRange *)vl_obj_new(O_RANGE, sizeof(VlRange));
  r->lo = lo; r->hi = hi; r->by = by; r->inclusive = inclusive;
  return obj(&r->hdr);
}

Value vl_builder_new(void) {
  VlBuilder *b = (VlBuilder *)vl_obj_new(O_BUILDER, sizeof(VlBuilder));
  b->cap = 64;
  b->b = (char *)vl_alloc((size_t)b->cap);
  return obj(&b->hdr);
}

Value vl_shared(Value v) {
  VlShared *s = (VlShared *)vl_obj_new(O_SHARED, sizeof(VlShared));
  s->v = vl_retain(v);
  return obj(&s->hdr);
}

Value vl_cell(Value v) {
  VlCell *c = (VlCell *)vl_obj_new(O_CELL, sizeof(VlCell));
  c->v = vl_retain(v);
  pthread_mutex_init(&c->mu, NULL);
  return obj(&c->hdr);
}

Value vl_weak(Value sh) {
  VlWeak *w = (VlWeak *)vl_obj_new(O_WEAK, sizeof(VlWeak));
  if (sh.t == VL_OBJ && sh.u.o->kind == O_SHARED) w->target = sh.u.o;
  return obj(&w->hdr);
}

Value vl_closure(VlFn fn, VlFrame *up) {
  VlClosure *c = (VlClosure *)vl_obj_new(O_CLOSURE, sizeof(VlClosure));
  c->fn = fn;
  c->up = up;
  if (up) {
    /* The optimizer's elision predicate FORBIDS closures over stack
     * frames (any MKCLOS disqualifies the function). This assert is the
     * tripwire for a predicate bug: better an immediate abort in a debug
     * build than a dangling frame pointer in production. */
    assert(!up->stackalloc && "closure over a stack-allocated frame");
    /* From here on this frame (and every ancestor it can reach) may cross
     * threads inside the closure. Mark BEFORE the closure value escapes:
     * publication (channel send, spawn) synchronizes, so consumers see the
     * flag. The retain itself may already race with an earlier closure over
     * the same frame running elsewhere, hence frame_retain's atomic path. */
    frame_mark_shared(up);
    frame_retain(up);
  }
  return obj(&c->hdr);
}

Value vl_native(const char *name, VlFn fn) {
  VlClosure *c = (VlClosure *)vl_obj_new(O_NATIVE, sizeof(VlClosure));
  c->fn = fn;
  c->name = name;
  return obj(&c->hdr);
}

/* ---------------------------------------------------------------- memory */

static void obj_free(VlObj *o);

Value vl_retain(Value v) {
  if (v.t == VL_OBJ && v.u.o)
    __atomic_add_fetch(&v.u.o->rc, 1, __ATOMIC_RELAXED);
  return v;
}

void vl_release(Value v) {
  if (v.t != VL_OBJ || !v.u.o) return;
  VlObj *o = v.u.o;
  if (__atomic_sub_fetch(&o->rc, 1, __ATOMIC_ACQ_REL) > 0) return;
  obj_free(o);
}

void vl_set(Value *slot, Value owned) {
  Value old = *slot;
  *slot = owned;
  vl_release(old);
}

static void obj_free(VlObj *o) {
  switch ((VlKind)o->kind) {
  case O_STR:
  case O_DEC:
  case O_RANGE:
    break;
  case O_SLICE: {
    VlSlice *s = (VlSlice *)o;
    for (int64_t i = 0; i < s->len; i++) vl_release(s->e[i]);
    free(s->e);
    break;
  }
  case O_MAP:
  case O_SET: {
    VlMap *m = (VlMap *)o;
    for (int64_t i = 0; i < m->nents; i++) {
      if (!m->ents[i].used) continue;
      vl_release(m->ents[i].k);
      if (!m->is_set) vl_release(m->ents[i].v);
    }
    free(m->ents);
    free(m->idx);
    break;
  }
  case O_TUPLE: {
    VlTuple *t = (VlTuple *)o;
    for (int i = 0; i < t->n; i++) vl_release(t->e[i]);
    free(t->e);
    break;
  }
  case O_STRUCT: {
    VlStruct *s = (VlStruct *)o;
    for (int i = 0; i < s->type->nfields; i++) vl_release(s->f[i]);
    free(s->f);
    break;
  }
  case O_ENUM: {
    VlEnum *e = (VlEnum *)o;
    for (int i = 0; i < e->n; i++) vl_release(e->f[i]);
    free(e->f);
    break;
  }
  case O_ERR: {
    VlErr *e = (VlErr *)o;
    vl_release(e->type_name);
    vl_release(e->msg);
    vl_release(e->fields);
    vl_release(e->cause);
    vl_release(e->suppressed);
    vl_release(e->trace);
    break;
  }
  case O_CLOSURE: {
    VlClosure *c = (VlClosure *)o;
    if (c->up) vl_frame_release(c->up);
    break;
  }
  case O_NATIVE:
    break;
  case O_CHAN: {
    VlChan *c = (VlChan *)o;
    for (int64_t i = 0; i < c->count; i++)
      vl_release(c->buf[(c->head + i) % c->cap]);
    free(c->buf);
    pthread_mutex_destroy(&c->mu);
    pthread_cond_destroy(&c->cv);
    break;
  }
  case O_TASK: {
    VlTask *t = (VlTask *)o;
    vl_release(t->result);
    vl_release(t->exc);
    vl_release(t->closure);
    for (int i = 0; i < t->argc; i++) vl_release(t->argv[i]);
    free(t->argv);
    pthread_mutex_destroy(&t->mu);
    pthread_cond_destroy(&t->cv);
    break;
  }
  case O_SHARED:
    vl_release(((VlShared *)o)->v);
    break;
  case O_CELL: {
    VlCell *c = (VlCell *)o;
    vl_release(c->v);
    pthread_mutex_destroy(&c->mu);
    break;
  }
  case O_WEAK:
    break;
  case O_FILE: {
    VlFile *f = (VlFile *)o;
    if (f->f && !f->closed && !f->is_std) fclose(f->f);
    vl_release(f->name);
    break;
  }
  case O_SOCKET: {
    /* Close the fd when the last reference drops. This arm is mandatory:
     * the build has no -Wswitch, so its absence would silently leak the
     * descriptor on every dropped socket. */
    VlSocket *s = (VlSocket *)o;
    if (!s->closed && s->fd >= 0) close(s->fd);
    break;
  }
  case O_BUILDER:
    free(((VlBuilder *)o)->b);
    break;
  case O_TYPE:
    break;
  case O_FRAME:
    /* frames are released through vl_frame_release */
    break;
  case O_ITER: {
    VlIter *it = (VlIter *)o;
    vl_release(it->src);
    vl_release(it->ch);
    break;
  }
  }
  free(o);
}

/* ---------------------------------------------------------------- frames */

/* frame_mark_shared flags a frame and its whole up-chain as thread-crossing.
 * Runs on the frame's owning thread, before the capturing closure escapes;
 * an already-shared ancestor means the rest of the chain is already marked. */
static void frame_mark_shared(VlFrame *f) {
  for (; f && !f->shared; f = f->up) f->shared = 1;
}

/* frame_retain bumps a frame's refcount: plain for thread-local frames,
 * atomic once a closure made the frame shareable. */
static void frame_retain(VlFrame *f) {
  if (f->shared) __atomic_add_fetch(&f->hdr.rc, 1, __ATOMIC_RELAXED);
  else f->hdr.rc++;
}

VlFrame *vl_frame_new(int nregs, VlFrame *up) {
  size_t size = sizeof(VlFrame) + sizeof(Value) * (size_t)(nregs > 0 ? nregs - 1 : 0);
  VlFrame *f = (VlFrame *)vl_alloc(size);
  f->hdr.rc = 1;
  f->hdr.kind = O_FRAME;
  f->nregs = nregs;
  f->stackalloc = 0;
  f->shared = 0;
  f->up = up;
  if (up) {
    assert(!up->stackalloc && "a stack frame must never be captured as up");
    frame_retain(up);
  }
  for (int i = 0; i < nregs; i++) f->r[i] = vl_nil();
  return f;
}

/* vl_frame_stack_init readies caller-provided (stack) storage as a frame.
 * The buffer arrives zeroed by the C compiler? NO — it arrives as
 * uninitialized automatic storage; memset here is the nil-init (VL_NIL is
 * all-zero bits), and it is the only per-call cost that remains. */
VlFrame *vl_frame_stack_init(VlFrame *f, int nregs, VlFrame *up) {
  memset(f, 0, sizeof(VlFrame) + sizeof(Value) * (size_t)(nregs > 0 ? nregs - 1 : 0));
  f->hdr.rc = 1;
  f->hdr.kind = O_FRAME;
  f->nregs = nregs;
  f->stackalloc = 1;
  f->up = up;
  if (up) {
    assert(!up->stackalloc && "a stack frame must never be captured as up");
    frame_retain(up);
  }
  return f;
}

/* vl_frame_clear breaks the one reference cycle refcounting cannot: a closure
 * that captured THIS frame and is also held IN it (`let f = fn() {...}`, a
 * temporary passed to `map`, a `defer` body). Dropping the frame's reference
 * to such a closure lets it die, which in turn releases the frame.
 *
 * Registers holding anything else are left alone: a closure that ESCAPED is
 * retained by its new owner and must keep reading this frame's registers. */
void vl_frame_clear(VlFrame *f) {
  if (!f) return;
  for (int i = 0; i < f->nregs; i++) {
    Value v = f->r[i];
    if (v.t != VL_OBJ || v.u.o->kind != O_CLOSURE) continue;
    if (((VlClosure *)v.u.o)->up != f) continue; /* not a cycle through us */
    f->r[i] = vl_nil();
    vl_release(v);
  }
}

void vl_frame_release(VlFrame *f) {
  if (!f) return;
  if (f->shared) {
    /* ACQ_REL: the zero-observing thread must see every register write the
     * other thread made before its own release. */
    if (__atomic_sub_fetch(&f->hdr.rc, 1, __ATOMIC_ACQ_REL) > 0) return;
  } else {
    /* Never captured by a closure => never left this thread. */
    if (--f->hdr.rc > 0) return;
  }
  for (int i = 0; i < f->nregs; i++) vl_release(f->r[i]);
  for (int i = 0; i < f->ndefers; i++) vl_release(f->defers[i]);
  free(f->defers);
  if (f->up) vl_frame_release(f->up);
  if (!f->stackalloc) free(f);
}

void vl_frame_push(VlFrame *f) {
  VlCtx *c = vl_ctx();
  if (c->nframes == c->capframes) {
    c->capframes = c->capframes ? c->capframes * 2 : 32;
    c->frames = (VlFrame **)realloc(c->frames, sizeof(VlFrame *) * (size_t)c->capframes);
    if (!c->frames) vl_abort("out of memory");
  }
  c->frames[c->nframes++] = f;
}

void vl_defer(VlFrame *f, Value closure) {
  if (f->ndefers == f->capdefers) {
    f->capdefers = f->capdefers ? f->capdefers * 2 : 4;
    f->defers = (Value *)realloc(f->defers, sizeof(Value) * (size_t)f->capdefers);
    if (!f->defers) vl_abort("out of memory");
  }
  f->defers[f->ndefers++] = vl_retain(closure);
}

/* run_defers executes a frame's defers LIFO, exactly once. */
static void run_defers(VlFrame *f) {
  int n = f->ndefers;
  f->ndefers = 0;
  for (int i = n - 1; i >= 0; i--) {
    Value d = f->defers[i];
    vl_release(vl_call(d, NULL, 0));
    vl_release(d);
  }
}

void vl_frame_pop_run_defers(VlFrame *f) {
  run_defers(f);
  VlCtx *c = vl_ctx();
  if (c->nframes > 0 && c->frames[c->nframes - 1] == f) c->nframes--;
}

/* vl_unwind_to pops frames above depth, running their defers. Used by throw. */
void vl_unwind_to(int depth) {
  VlCtx *c = vl_ctx();
  while (c->nframes > depth) {
    VlFrame *f = c->frames[--c->nframes];
    run_defers(f);
    vl_frame_clear(f); /* same cycle break as the normal exit path */
    vl_frame_release(f);
  }
}

Value vl_up(VlFrame *f, int depth, int slot) {
  for (int i = 0; i < depth; i++) {
    if (!f) vl_abort("upvalue depth exceeded");
    f = f->up;
  }
  if (!f || slot >= f->nregs) vl_abort("bad upvalue slot");
  return f->r[slot];
}

void vl_upset(VlFrame *f, int depth, int slot, Value v) {
  for (int i = 0; i < depth; i++) {
    if (!f) vl_abort("upvalue depth exceeded");
    f = f->up;
  }
  if (!f || slot >= f->nregs) vl_abort("bad upvalue slot");
  vl_set(&f->r[slot], v);
}

/* ---------------------------------------------------------------- equality */

const char *vl_kind_name(Value v) {
  switch (v.t) {
  case VL_NIL: return "nil";
  case VL_UNIT: return "unit";
  case VL_BOOL: return "bool";
  case VL_INT: return "int";
  case VL_FLOAT: return "float";
  case VL_RUNE: return "rune";
  case VL_DUR: return "duration";
  case VL_INSTANT: return "instant";
  case VL_OBJ: break;
  }
  switch ((VlKind)v.u.o->kind) {
  case O_STR: return "str";
  case O_DEC: return "dec";
  case O_SLICE: return "slice";
  case O_MAP: return "map";
  case O_SET: return "set";
  case O_TUPLE: return "tuple";
  case O_STRUCT: return ((VlStruct *)v.u.o)->type->name;
  case O_ENUM: return ((VlEnum *)v.u.o)->var->type;
  case O_ERR: {
    VlErr *e = (VlErr *)v.u.o;
    const char *tn = vl_cstr(e->type_name);
    return tn[0] ? tn : "error";
  }
  case O_CLOSURE:
  case O_NATIVE: return "func";
  case O_CHAN: return "chan";
  case O_TASK: return "task";
  case O_SHARED: return "shared";
  case O_CELL: return "cell";
  case O_WEAK: return "weak";
  case O_RANGE: return "range";
  case O_FILE: return "file";
  case O_SOCKET: return "socket";
  case O_BUILDER: return "builder";
  case O_TYPE: return "type";
  case O_FRAME: return "frame";
  case O_ITER: return "iter";
  }
  return "?";
}

static bool is_dec(Value v) { return v.t == VL_OBJ && v.u.o->kind == O_DEC; }
static bool is_str(Value v) { return v.t == VL_OBJ && v.u.o->kind == O_STR; }

static bool num_eq(Value a, Value b) {
  /* Cross-type numeric equality, mirroring the interpreter. */
  if (a.t == VL_INT && b.t == VL_INT) return a.u.i == b.u.i;
  if (a.t == VL_FLOAT && b.t == VL_FLOAT) return a.u.f == b.u.f;
  if (a.t == VL_INT && b.t == VL_FLOAT) return (double)a.u.i == b.u.f;
  if (a.t == VL_FLOAT && b.t == VL_INT) return a.u.f == (double)b.u.i;
  return false;
}

bool vl_equal(Value a, Value b) {
  if (a.t == VL_RUNE && b.t == VL_INT) return (int64_t)a.u.r == b.u.i;
  if (a.t == VL_INT && b.t == VL_RUNE) return a.u.i == (int64_t)b.u.r;
  /* A rune equals the one-character string it denotes. */
  if ((a.t == VL_RUNE && is_str(b)) || (is_str(a) && b.t == VL_RUNE)) {
    Value ra = a.t == VL_RUNE ? a : b;
    Value sv = a.t == VL_RUNE ? b : a;
    Value rs = vl_tostr(ra);
    bool same = vl_equal(rs, sv);
    vl_release(rs);
    return same;
  }
  if (is_dec(a) || is_dec(b)) {
    if (is_dec(a) && is_dec(b)) return vl_dec_cmp(a, b) == 0;
    if (is_dec(a) && b.t == VL_INT) return vl_dec_cmp(a, vl_dec_from_int(b.u.i)) == 0;
    if (is_dec(b) && a.t == VL_INT) return vl_dec_cmp(vl_dec_from_int(a.u.i), b) == 0;
    return false;
  }
  if (a.t != b.t) return num_eq(a, b);
  switch (a.t) {
  case VL_NIL:
  case VL_UNIT: return true;
  case VL_BOOL: return a.u.b == b.u.b;
  case VL_INT:
  case VL_DUR:
  case VL_INSTANT: return a.u.i == b.u.i;
  case VL_FLOAT: return a.u.f == b.u.f;
  case VL_RUNE: return a.u.r == b.u.r;
  case VL_OBJ: break;
  }
  if (a.u.o->kind != b.u.o->kind) return false;
  switch ((VlKind)a.u.o->kind) {
  case O_STR: {
    VlStr *x = (VlStr *)a.u.o, *y = (VlStr *)b.u.o;
    return x->len == y->len && memcmp(x->b, y->b, (size_t)x->len) == 0;
  }
  case O_SLICE: {
    VlSlice *x = (VlSlice *)a.u.o, *y = (VlSlice *)b.u.o;
    if (x->len != y->len) return false;
    for (int64_t i = 0; i < x->len; i++)
      if (!vl_equal(x->e[i], y->e[i])) return false;
    return true;
  }
  case O_TUPLE: {
    VlTuple *x = (VlTuple *)a.u.o, *y = (VlTuple *)b.u.o;
    if (x->n != y->n) return false;
    for (int i = 0; i < x->n; i++)
      if (!vl_equal(x->e[i], y->e[i])) return false;
    return true;
  }
  case O_STRUCT: {
    VlStruct *x = (VlStruct *)a.u.o, *y = (VlStruct *)b.u.o;
    if (x->type != y->type) return false;
    for (int i = 0; i < x->type->nfields; i++)
      if (!vl_equal(x->f[i], y->f[i])) return false;
    return true;
  }
  case O_ENUM: {
    VlEnum *x = (VlEnum *)a.u.o, *y = (VlEnum *)b.u.o;
    if (x->var != y->var || x->n != y->n) return false;
    for (int i = 0; i < x->n; i++)
      if (!vl_equal(x->f[i], y->f[i])) return false;
    return true;
  }
  default:
    return a.u.o == b.u.o;
  }
}

uint64_t vl_hash(Value v) {
  uint64_t h = 1469598103934665603ULL;
  const unsigned char *p;
  int64_t n;
  char buf[32];
  switch (v.t) {
  case VL_NIL: return 2166136261u;
  case VL_UNIT: return 2166136262u;
  case VL_BOOL: return v.u.b ? 1231u : 1237u;
  case VL_RUNE: {
    int64_t r = (int64_t)v.u.r;
    p = (const unsigned char *)&r;
    n = sizeof(int64_t);
    for (int64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
  }
  case VL_INT:
  case VL_DUR:
  case VL_INSTANT:
    p = (const unsigned char *)&v.u.i;
    n = sizeof(int64_t);
    break;
  case VL_FLOAT: {
    /* An integral float must hash like the equal int (vl_equal says 1 == 1.0). */
    double d = v.u.f;
    if (d == 0) d = 0; /* collapse -0.0 */
    if (d == (double)(int64_t)d) {
      int64_t i64 = (int64_t)d;
      p = (const unsigned char *)&i64;
      n = sizeof(int64_t);
      for (int64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
      return h;
    }
    p = (const unsigned char *)&d;
    n = sizeof(double);
    break;
  }
  case VL_OBJ:
    if (v.u.o->kind == O_STR) {
      VlStr *s = (VlStr *)v.u.o;
      p = (const unsigned char *)s->b;
      n = s->len;
      break;
    }
    if (v.u.o->kind == O_DEC) {
      char *s = vl_dec_str(v);
      Value sv = vl_str(s);
      free(s);
      uint64_t hh = vl_hash(sv);
      vl_release(sv);
      return hh;
    }
    if (v.u.o->kind == O_ENUM) {
      VlEnum *e = (VlEnum *)v.u.o;
      snprintf(buf, sizeof buf, "%p", (void *)e->var);
      p = (const unsigned char *)buf;
      n = (int64_t)strlen(buf);
      uint64_t hh = 1469598103934665603ULL;
      for (int64_t i = 0; i < n; i++) { hh ^= p[i]; hh *= 1099511628211ULL; }
      for (int i = 0; i < e->n; i++) hh = hh * 31 + vl_hash(e->f[i]);
      return hh;
    }
    p = (const unsigned char *)&v.u.o;
    n = sizeof(void *);
    break;
  default:
    return 0;
  }
  for (int64_t i = 0; i < n; i++) {
    h ^= p[i];
    h *= 1099511628211ULL;
  }
  return h;
}

/* ---------------------------------------------------------------- printing */

/* vl_fmt_float reproduces Go's strconv.FormatFloat(f, 'g', -1, 64): the
 * shortest digit string that round-trips, in fixed notation unless the
 * decimal exponent is < -4 or > 20. */
char *vl_fmt_float(double f) {
  char *out = (char *)vl_alloc(48);
  if (f != f) { strcpy(out, "NaN"); return out; }
  if (f > 1.7976931348623157e308) { strcpy(out, "+Inf"); return out; }
  if (f < -1.7976931348623157e308) { strcpy(out, "-Inf"); return out; }

  char sci[64];
  int prec;
  for (prec = 1; prec <= 17; prec++) {
    snprintf(sci, sizeof sci, "%.*e", prec - 1, f);
    if (strtod(sci, NULL) == f) break;
  }
  /* sci is like "-1.2345e+07": split mantissa digits from the exponent. */
  int neg = sci[0] == '-';
  const char *m = sci + (neg ? 1 : 0);
  char digits[32];
  int nd = 0;
  const char *e = strchr(m, 'e');
  for (const char *p = m; p < e; p++)
    if (*p != '.') digits[nd++] = *p;
  digits[nd] = 0;
  int exp10 = atoi(e + 1);

  char *o = out;
  if (neg) *o++ = '-';
  if (exp10 < -4 || exp10 > 20) {
    *o++ = digits[0];
    if (nd > 1) {
      *o++ = '.';
      memcpy(o, digits + 1, (size_t)(nd - 1));
      o += nd - 1;
    }
    o += snprintf(o, 16, "e%c%02d", exp10 < 0 ? '-' : '+',
                  exp10 < 0 ? -exp10 : exp10);
    *o = 0;
    return out;
  }
  if (exp10 >= 0) {
    int intlen = exp10 + 1;
    for (int i = 0; i < intlen; i++) *o++ = i < nd ? digits[i] : '0';
    if (nd > intlen) {
      *o++ = '.';
      memcpy(o, digits + intlen, (size_t)(nd - intlen));
      o += nd - intlen;
    }
  } else {
    *o++ = '0';
    *o++ = '.';
    for (int i = 0; i < -exp10 - 1; i++) *o++ = '0';
    memcpy(o, digits, (size_t)nd);
    o += nd;
  }
  *o = 0;
  return out;
}

/* Go-style quoting for values nested inside containers. */
static void quote_into(Value b, Value v);

static void bwrite(Value bv, const char *s, int64_t n) {
  VlBuilder *b = (VlBuilder *)bv.u.o;
  if (b->len + n + 1 > b->cap) {
    while (b->len + n + 1 > b->cap) b->cap *= 2;
    b->b = (char *)realloc(b->b, (size_t)b->cap);
    if (!b->b) vl_abort("out of memory");
  }
  memcpy(b->b + b->len, s, (size_t)n);
  b->len += n;
  b->b[b->len] = 0;
}

static void bputs(Value b, const char *s) { bwrite(b, s, (int64_t)strlen(s)); }

static void str_into(Value b, Value v);

static void quote_into(Value b, Value v) {
  if (is_str(v)) {
    VlStr *s = (VlStr *)v.u.o;
    bputs(b, "\"");
    for (int64_t i = 0; i < s->len; i++) {
      unsigned char c = (unsigned char)s->b[i];
      switch (c) {
      case '"': bputs(b, "\\\""); break;
      case '\\': bputs(b, "\\\\"); break;
      case '\n': bputs(b, "\\n"); break;
      case '\t': bputs(b, "\\t"); break;
      case '\r': bputs(b, "\\r"); break;
      default:
        if (c < 0x20) {
          char tmp[8];
          snprintf(tmp, sizeof tmp, "\\x%02x", c);
          bputs(b, tmp);
        } else {
          bwrite(b, (const char *)&c, 1);
        }
      }
    }
    bputs(b, "\"");
    return;
  }
  if (v.t == VL_RUNE) {
    char tmp[8];
    int n = 0;
    uint32_t r = v.u.r;
    if (r < 0x80) tmp[n++] = (char)r;
    else if (r < 0x800) { tmp[n++] = (char)(0xC0 | (r >> 6)); tmp[n++] = (char)(0x80 | (r & 0x3F)); }
    else if (r < 0x10000) { tmp[n++] = (char)(0xE0 | (r >> 12)); tmp[n++] = (char)(0x80 | ((r >> 6) & 0x3F)); tmp[n++] = (char)(0x80 | (r & 0x3F)); }
    else { tmp[n++] = (char)(0xF0 | (r >> 18)); tmp[n++] = (char)(0x80 | ((r >> 12) & 0x3F)); tmp[n++] = (char)(0x80 | ((r >> 6) & 0x3F)); tmp[n++] = (char)(0x80 | (r & 0x3F)); }
    bputs(b, "'");
    bwrite(b, tmp, n);
    bputs(b, "'");
    return;
  }
  str_into(b, v);
}

static void str_into(Value b, Value v) {
  char tmp[64];
  switch (v.t) {
  case VL_NIL: bputs(b, "nil"); return;
  case VL_UNIT: bputs(b, "()"); return;
  case VL_BOOL: bputs(b, v.u.b ? "true" : "false"); return;
  case VL_INT: snprintf(tmp, sizeof tmp, "%lld", (long long)v.u.i); bputs(b, tmp); return;
  case VL_FLOAT: {
    char *s = vl_fmt_float(v.u.f);
    bputs(b, s);
    free(s);
    return;
  }
  case VL_RUNE: {
    Value q = vl_builder_new();
    quote_into(q, v);
    VlBuilder *qb = (VlBuilder *)q.u.o;
    bwrite(b, qb->b + 1, qb->len - 2); /* drop the quotes */
    vl_release(q);
    return;
  }
  case VL_DUR: {
    int64_t ns = v.u.i;
    if (ns % 1000000000 == 0) snprintf(tmp, sizeof tmp, "%llds", (long long)(ns / 1000000000));
    else if (ns % 1000000 == 0) snprintf(tmp, sizeof tmp, "%lldms", (long long)(ns / 1000000));
    else if (ns % 1000 == 0) snprintf(tmp, sizeof tmp, "%lldµs", (long long)(ns / 1000));
    else snprintf(tmp, sizeof tmp, "%lldns", (long long)ns);
    bputs(b, tmp);
    return;
  }
  case VL_INSTANT: snprintf(tmp, sizeof tmp, "%lld", (long long)v.u.i); bputs(b, tmp); return;
  case VL_OBJ: break;
  }
  switch ((VlKind)v.u.o->kind) {
  case O_STR: {
    VlStr *s = (VlStr *)v.u.o;
    bwrite(b, s->b, s->len);
    return;
  }
  case O_DEC: {
    char *s = vl_dec_str(v);
    bputs(b, s);
    free(s);
    return;
  }
  case O_SLICE: {
    VlSlice *s = (VlSlice *)v.u.o;
    bputs(b, "[");
    for (int64_t i = 0; i < s->len; i++) {
      if (i) bputs(b, ", ");
      quote_into(b, s->e[i]);
    }
    bputs(b, "]");
    return;
  }
  case O_MAP: {
    VlMap *m = (VlMap *)v.u.o;
    bputs(b, "{");
    bool first = true;
    for (int64_t i = 0; i < m->nents; i++) {
      if (!m->ents[i].used) continue;
      if (!first) bputs(b, ", ");
      first = false;
      quote_into(b, m->ents[i].k);
      bputs(b, ": ");
      quote_into(b, m->ents[i].v);
    }
    bputs(b, "}");
    return;
  }
  case O_SET: {
    VlMap *m = (VlMap *)v.u.o;
    bputs(b, "{");
    bool first = true;
    for (int64_t i = 0; i < m->nents; i++) {
      if (!m->ents[i].used) continue;
      if (!first) bputs(b, ", ");
      first = false;
      quote_into(b, m->ents[i].k);
    }
    bputs(b, "}");
    return;
  }
  case O_TUPLE: {
    VlTuple *t = (VlTuple *)v.u.o;
    bputs(b, "(");
    for (int i = 0; i < t->n; i++) {
      if (i) bputs(b, ", ");
      quote_into(b, t->e[i]);
    }
    bputs(b, ")");
    return;
  }
  case O_STRUCT: {
    VlStruct *s = (VlStruct *)v.u.o;
    bool ok = false;
    Value shown = vl_user_show(v, &ok);
    if (ok) {
      bwrite(b, vl_cstr(shown), vl_strlen(shown));
      vl_release(shown);
      return;
    }
    bputs(b, s->type->name);
    bputs(b, "{");
    for (int i = 0; i < s->type->nfields; i++) {
      if (i) bputs(b, ", ");
      bputs(b, s->type->fields[i]);
      bputs(b, ": ");
      quote_into(b, s->f[i]);
    }
    bputs(b, "}");
    return;
  }
  case O_ENUM: {
    VlEnum *e = (VlEnum *)v.u.o;
    bool ok = false;
    Value shown = vl_user_show(v, &ok);
    if (ok) {
      bwrite(b, vl_cstr(shown), vl_strlen(shown));
      vl_release(shown);
      return;
    }
    bputs(b, e->var->variant);
    if (e->n == 0) return;
    bputs(b, "(");
    for (int i = 0; i < e->n; i++) {
      if (i) bputs(b, ", ");
      quote_into(b, e->f[i]);
    }
    bputs(b, ")");
    return;
  }
  case O_ERR: {
    Value m = vl_callm(v, "message", NULL, 0);
    bwrite(b, vl_cstr(m), vl_strlen(m));
    vl_release(m);
    return;
  }
  case O_CLOSURE: bputs(b, "<closure>"); return;
  case O_NATIVE: {
    bputs(b, "<builtin ");
    bputs(b, ((VlClosure *)v.u.o)->name);
    bputs(b, ">");
    return;
  }
  case O_CHAN: bputs(b, "<chan>"); return;
  case O_TASK: bputs(b, "<task>"); return;
  case O_SHARED: str_into(b, ((VlShared *)v.u.o)->v); return;
  case O_CELL: str_into(b, ((VlCell *)v.u.o)->v); return;
  case O_WEAK: bputs(b, "<weak>"); return;
  case O_RANGE: {
    VlRange *r = (VlRange *)v.u.o;
    snprintf(tmp, sizeof tmp, "%lld%s%lld", (long long)r->lo,
             r->inclusive ? "..=" : "..", (long long)r->hi);
    bputs(b, tmp);
    if (r->by != 1) {
      snprintf(tmp, sizeof tmp, " by %lld", (long long)r->by);
      bputs(b, tmp);
    }
    return;
  }
  case O_FILE: {
    bputs(b, "<file ");
    Value n = ((VlFile *)v.u.o)->name;
    bwrite(b, vl_cstr(n), vl_strlen(n));
    bputs(b, ">");
    return;
  }
  case O_SOCKET: {
    VlSocket *s = (VlSocket *)v.u.o;
    snprintf(tmp, sizeof tmp, "<socket fd=%d%s>", s->fd,
             s->closed ? " closed" : "");
    bputs(b, tmp);
    return;
  }
  case O_BUILDER: bputs(b, "<builder>"); return;
  case O_TYPE: bputs(b, "<type>"); return;
  case O_FRAME: bputs(b, "<frame>"); return;
  case O_ITER: bputs(b, "<iter>"); return;
  }
}

Value vl_tostr(Value v) {
  if (is_str(v)) return vl_retain(v);
  Value b = vl_builder_new();
  str_into(b, v);
  VlBuilder *bb = (VlBuilder *)b.u.o;
  Value s = vl_str_n(bb->b, bb->len);
  vl_release(b);
  return s;
}

Value vl_interp(Value *parts, int n) {
  Value b = vl_builder_new();
  for (int i = 0; i < n; i++) str_into(b, parts[i]);
  VlBuilder *bb = (VlBuilder *)b.u.o;
  Value s = vl_str_n(bb->b, bb->len);
  vl_release(b);
  return s;
}

void vl_say(Value *argv, int argc) {
  Value b = vl_builder_new();
  for (int i = 0; i < argc; i++) {
    if (i) bputs(b, " ");
    str_into(b, argv[i]);
  }
  VlBuilder *bb = (VlBuilder *)b.u.o;
  fwrite(bb->b, 1, (size_t)bb->len, stdout);
  fputc('\n', stdout);
  vl_release(b);
}

/* Builder API used by std.c */
void vl_builder_write(Value b, Value v) { str_into(b, v); }
Value vl_builder_done(Value b) {
  VlBuilder *bb = (VlBuilder *)b.u.o;
  return vl_str_n(bb->b, bb->len);
}
int64_t vl_builder_len(Value b) { return ((VlBuilder *)b.u.o)->len; }
