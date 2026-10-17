/* value.c — values, reference counting, frames, equality, printing. */
#include "voila_int.h"

static void frame_mark_shared(VlFrame *f);
static void frame_retain(VlFrame *f);
static void frame_enter(VlFrame *f, VlFrame *up);
static void frame_free(VlFrame *f);

/* A frame's count `frc` packs three counts, so that one atomic operation
 * reads or changes them together (see self_cycle_only):
 *   bits  0-31  real references (closures over it, frames under it, its
 *               own activation);
 *   bits 32-51  ACTIVE: the frames under it that entered through
 *               frame_enter — calls of closures over it in progress (and
 *               activations they left captured); a subset of the above;
 *   bits 52-61  HOLDs: judgements of it in progress, or deferred
 *               (defer_judgement);
 *   bit  62     a judgement is reading its registers (FRAME_JUDGING).
 * The frame is freed when the whole count reaches 0. */
#define FRAME_REFS 0xffffffffULL
#define FRAME_ACT (1ULL << 32)
#define FRAME_ACTS (0xfffffULL << 32)
#define FRAME_HOLD (1ULL << 52)
#define FRAME_HOLDS (0x3ffULL << 52)
/* A judgement is reading the registers (self_cycle_only): a call that
 * enters meanwhile waits for it to finish before it can write them. */
#define FRAME_JUDGING (1ULL << 62)


#include <assert.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
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
  if (v.t != VL_OBJ || v.u.o->kind != O_STR) vl_fault("expected str, got %s", vl_kind_name(v));
  return ((VlStr *)v.u.o)->b;
}

int64_t vl_strlen(Value v) {
  if (v.t != VL_OBJ || v.u.o->kind != O_STR) vl_fault("expected str");
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

static pthread_mutex_t g_weak_mu = PTHREAD_MUTEX_INITIALIZER;

/* ctl_drop releases one reference to a weak control block (lock held). */
static void ctl_drop(VlWeakCtl *c) {
  if (--c->rc == 0) free(c);
}

Value vl_weak(Value sh) {
  VlWeak *w = (VlWeak *)vl_obj_new(O_WEAK, sizeof(VlWeak));
  if (sh.t == VL_OBJ && sh.u.o->kind == O_SHARED) {
    VlShared *s = (VlShared *)sh.u.o;
    pthread_mutex_lock(&g_weak_mu);
    if (!s->wctl) {
      s->wctl = (VlWeakCtl *)vl_alloc(sizeof(VlWeakCtl));
      s->wctl->rc = 1;               /* the value's own reference */
      s->wctl->target = sh.u.o;
    }
    s->wctl->rc++;
    w->ctl = s->wctl;
    pthread_mutex_unlock(&g_weak_mu);
  }
  return obj(&w->hdr);
}

/* vl_weak_upgrade answers the shared value while it lives, else nil. The
 * count is raised only from a live (non-zero) one, under the same lock the
 * freeing side clears `target` under — so a value whose last strong
 * reference is dropping is never resurrected, and never read once freed. */
Value vl_weak_upgrade(Value wv) {
  VlWeak *w = (VlWeak *)wv.u.o;
  Value out = vl_nil();
  if (!w->ctl) return out;
  pthread_mutex_lock(&g_weak_mu);
  VlObj *t = w->ctl->target;
  if (t) {
    int32_t rc = __atomic_load_n(&t->rc, __ATOMIC_ACQUIRE);
    while (rc > 0) {
      if (__atomic_compare_exchange_n(&t->rc, &rc, rc + 1, false,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        out.t = VL_OBJ;
        out.u.o = t;
        break;
      }
    }
  }
  pthread_mutex_unlock(&g_weak_mu);
  return out;
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

Value vl_handle_new(const VlHandleOps *ops, void *ctx) {
  VlHandle *h = (VlHandle *)vl_obj_new(O_HANDLE, sizeof(VlHandle));
  h->ops = ops;
  h->ctx = ctx;
  return obj(&h->hdr);
}

void *vl_handle_ctx(Value v, const VlHandleOps *ops) {
  if (v.t != VL_OBJ || !v.u.o || v.u.o->kind != O_HANDLE) return NULL;
  VlHandle *h = (VlHandle *)v.u.o;
  return h->ops == ops ? h->ctx : NULL;
}

Value vl_native(const char *name, VlFn fn) {
  VlClosure *c = (VlClosure *)vl_obj_new(O_NATIVE, sizeof(VlClosure));
  c->fn = fn;
  c->name = name;
  c->kinds = vl_native_kinds(name);
  return obj(&c->hdr);
}

/* ---------------------------------------------------------------- memory */

static void obj_free(VlObj *o);

Value vl_retain(Value v) {
  if (v.t == VL_OBJ && v.u.o)
    __atomic_add_fetch(&v.u.o->rc, 1, __ATOMIC_RELAXED);
  return v;
}

/* Destruction is ITERATIVE. obj_free releases an object's children, and a
 * child whose count reaches zero would recurse straight back into obj_free:
 * a million-node list built of Box or enum cells then needs a million C
 * frames to die, and overflows the stack. Instead, a release that happens
 * while this thread is already inside obj_free only PARKS the dead object
 * on a per-thread worklist; the outermost release drains the list before it
 * returns. Every object is still freed before that outermost vl_release
 * returns — the order among children changes, the timing a program can
 * observe (a file closed by its last reference) does not. */
/* The worklist starts in a small per-thread array and moves to the heap
 * only for a structure wider than that; the heap block is returned as soon
 * as the drain ends, so a task thread never exits holding one (`leaks`
 * would report it). */
enum { DEAD_INLINE = 512 };
static __thread VlObj *g_dead_inline[DEAD_INLINE];
static __thread VlObj **g_dead;      /* NULL: use g_dead_inline */
static __thread int64_t g_ndead, g_capdead;
static __thread bool g_freeing;

static void park_dead(VlObj *o) {
  if (!g_dead) {
    if (g_ndead < DEAD_INLINE) {
      g_dead_inline[g_ndead++] = o;
      return;
    }
    g_capdead = DEAD_INLINE * 4;
    g_dead = (VlObj **)malloc(sizeof(VlObj *) * (size_t)g_capdead);
    if (!g_dead) vl_abort("out of memory");
    memcpy(g_dead, g_dead_inline, sizeof(VlObj *) * DEAD_INLINE);
  } else if (g_ndead == g_capdead) {
    g_capdead *= 2;
    g_dead = (VlObj **)realloc(g_dead, sizeof(VlObj *) * (size_t)g_capdead);
    if (!g_dead) vl_abort("out of memory");
  }
  g_dead[g_ndead++] = o;
}

static bool self_cycle_only(VlFrame *f, int deep);
static void frame_release_judged(VlFrame *f, int deep);
static void frame_unhold(VlFrame *f);

void vl_release(Value v) {
  if (v.t != VL_OBJ || !v.u.o) return;
  VlObj *o = v.u.o;
  /* A closure down to ONE holder may be held only by a register of the
   * finished frame it closes over — `fib` in `var fib = fn(n) { … fib(n -
   * 1) … }; return fib`, once the caller lets go: frame and closure then
   * keep each other alive, and self_cycle_only frees both. The frame is
   * read and HELD before the count drops: after it, this thread owns no
   * reference to the closure, and another may free it (and the frame). */
  VlFrame *cf = NULL;
  if (o->kind == O_CLOSURE) {
    VlFrame *f = ((VlClosure *)o)->up;
    if (f && __atomic_load_n(&f->done, __ATOMIC_ACQUIRE)) {
      /* A judgement hold (see self_cycle_only): keeps f allocated, and
       * no judge counts it as a reference. */
      __atomic_add_fetch(&f->frc, FRAME_HOLD, __ATOMIC_SEQ_CST);
      cf = f;
    }
  }
  int32_t left = __atomic_sub_fetch(&o->rc, 1, __ATOMIC_ACQ_REL);
  if (cf) {
    if (left != 1 || !self_cycle_only(cf, 1)) frame_unhold(cf);
  }
  if (left > 0) return;
  if (g_freeing) {
    park_dead(o);
    return;
  }
  g_freeing = true;
  obj_free(o);
  while (g_ndead > 0) {
    VlObj *next = g_dead ? g_dead[--g_ndead] : g_dead_inline[--g_ndead];
    obj_free(next);
  }
  if (g_dead) {
    free(g_dead);
    g_dead = NULL;
    g_capdead = 0;
  }
  g_freeing = false;
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
  case O_SHARED: {
    VlShared *s = (VlShared *)o;
    if (s->wctl) {
      /* Every weak handle now finds nothing (vl_weak_upgrade). */
      pthread_mutex_lock(&g_weak_mu);
      s->wctl->target = NULL;
      ctl_drop(s->wctl);
      pthread_mutex_unlock(&g_weak_mu);
    }
    vl_release(s->v);
    break;
  }
  case O_BOX:
    vl_release(((VlBox *)o)->v);
    break;
  case O_CELL: {
    VlCell *c = (VlCell *)o;
    vl_release(c->v);
    pthread_mutex_destroy(&c->mu);
    break;
  }
  case O_WEAK:
    if (((VlWeak *)o)->ctl) {
      pthread_mutex_lock(&g_weak_mu);
      ctl_drop(((VlWeak *)o)->ctl);
      pthread_mutex_unlock(&g_weak_mu);
    }
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
    /* The filter first: it may hold state about the stream (a TLS session)
     * but never the descriptor, which stays the socket's to close. */
    vl_release(s->flt);
    if (!s->closed && s->fd >= 0) close(s->fd);
    break;
  }
  case O_READER:
  case O_WRITER:
    /* Same mandate as O_SOCKET above, and one more besides: a reader RETAINS
     * the file or socket it reads from, so without this arm the owner is
     * never released and the file it kept alive is never closed. The body
     * lives in io.c so the POSIX close(2) stays in one file. */
    if (o->kind == O_READER) vl_reader_free(o);
    else vl_writer_free(o);
    break;
  case O_MUTEX:
  case O_RWLOCK:
    /* Mandatory, like O_SOCKET above: a pthread lock owns kernel or libc
     * state that pthread_*_destroy is what releases. The bodies live in
     * sync.c so the pthread calls stay in the one file that owns them. */
    if (o->kind == O_MUTEX) vl_sync_mutex_free(o);
    else vl_sync_rwlock_free(o);
    break;
  case O_REGEX:
    /* Mandatory, like the arms above: a compiled pattern owns its program,
     * its class bitmaps and the pattern text. This switch has no `default:`
     * precisely so a new kind cannot leak by being forgotten here. */
    vl_regex_free(o);
    break;
  case O_PROC:
    /* Mandatory, like O_SOCKET: the handle owns its pipes' reader and
     * writer, and may have a child to reap. The body lives in proc.c. */
    vl_proc_free(o);
    break;
  case O_HANDLE: {
    /* Mandatory, like O_SOCKET: the extension's resource is released by
     * the extension's own free, the only code that knows what it is. */
    VlHandle *h = (VlHandle *)o;
    if (h->ops && h->ops->free) h->ops->free(h->ctx);
    break;
  }
  case O_ATOMIC:
    /* One int64 inline in the object; nothing to release. The arm is here
     * anyway — an omitted case is exactly the silent hole this switch has no
     * `default:` in order to expose. */
    break;
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

/* ---------------------------------------------------------- address locks */

/* A re-entrant lock: owned by one task context at a time (its vl_ctx),
 * which may take it again — a write's path may call a function that writes
 * the same variable, a sort_by key may read the slice it sorts.
 * Uncontended, taking one is a single CAS. */
void vl_lock_acquire(VlLock *l) {
  void *me = vl_ctx();
  if (__atomic_load_n(&l->owner, __ATOMIC_ACQUIRE) == me) {
    l->depth++;
    return;
  }
  for (int spins = 0;; spins++) {
    void *none = NULL;
    if (__atomic_compare_exchange_n(&l->owner, &none, me, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      break;
    if (spins > 64) sched_yield();
  }
  l->depth = 1;
}

void vl_lock_release(VlLock *l) {
  if (--l->depth == 0) __atomic_store_n(&l->owner, NULL, __ATOMIC_RELEASE);
}

/* The collections' locks (call.c, coll.c): a striped table picked by the
 * collection's address. Two collections may share an entry; that only
 * makes them wait on each other — they are held around runtime work only
 * (and a sort_by key). */
#define ADDR_LOCKS 256
typedef struct {
  VlLock l;
  char pad_[64 - sizeof(VlLock)];
} AddrLock;
static AddrLock g_addr_locks[ADDR_LOCKS];

VlLock *vl_addr_lock(const void *addr) {
  uintptr_t a = (uintptr_t)addr;
  a ^= a >> 17;
  a *= 0x9E3779B97F4A7C15ULL;
  return &g_addr_locks[(a >> 56) & (ADDR_LOCKS - 1)].l;
}

/* A captured variable's lock is its FRAME's (VlFrame.vlock): one lock per
 * frame, never shared with another frame's — a write that holds it while it
 * calls a function whose tasks write that function's own variables cannot
 * wait on itself. */
void vl_varlock(VlFrame *f) {
  vl_lock_acquire(&f->vlock);
  vl_cleanup_lock(&f->vlock);
}

/* The write is done: its cleanup entry is the newest (anything a native
 * registered in between was dropped by that native), so it is popped. */
void vl_varunlock(VlFrame *f) {
  VlCtx *c = vl_ctx();
  if (c->ncleanups > 0) vl_cleanup_to(c->ncleanups - 1);
  vl_lock_release(&f->vlock);
}

/* vl_closure_escapes marks the frame chain of a closure that may escape
 * its task (VlFrame.escapes). The lowerer emits it right after the closure
 * is made (ESCAPE, low/expr.voi), on the task that made it, before the
 * value is stored, sent, returned or handed anywhere: no other task can
 * hold the closure yet. The walk stops at the first frame already marked:
 * every frame above a marked one is marked (each mark walks to the root). */
void vl_closure_escapes(Value c) {
  if (c.t != VL_OBJ || c.u.o->kind != O_CLOSURE) return;
  for (VlFrame *f = ((VlClosure *)c.u.o)->up; f && !f->escapes; f = f->up)
    f->escapes = 1;
}

/* vl_varread reads register `slot` of frame f — a variable closures
 * capture — as an OWNED reference: a snapshot of its value.
 *
 * Once a closure over the frame may escape its task (`escapes`), the
 * frame's variables can be reached from any task the closure reaches, by
 * any route — a spawn, a channel, a struct field, a global. So from then
 * on every read takes its reference under the frame's lock, the lock every
 * write through a captured variable holds (vl_varlock), and no write can
 * replace the value — and release it — between the load and the reference.
 * `escapes` is set on the frame's own task when such a closure is made,
 * before it can be anywhere else (vl_closure_escapes), so there is no
 * window in which another task reads or writes while the frame's own task
 * reads unlocked. Before that — and in a frame whose closures are only
 * called in place or run at once by a builtin collection method — the
 * frame's task is the only one that can reach it, and the read takes a
 * reference only. (A binding never written after it is made needs no lock
 * either: the lowerer reads it with a plain retain, VARREAD's `fixed`.)
 *
 * Why the reference is enough. Every write through a captured variable
 * holds the lock from the start of its path to its store and copies on
 * write whatever it finds held by more than the location itself (UNIQ):
 * an element or field stored, a builtin collection method that grows or
 * shrinks it, Box `set`, the copy-in of a `mut` argument or a `mut self`
 * receiver (boxcow.voi, expr.voi). Sorting works on a copy of the
 * elements and commits under the collection's lock (vl_sort_slice). A
 * reader holding its own reference therefore never has the value
 * reallocated, an element released or a field replaced under it: it
 * indexes, measures, iterates and hands its snapshot to natives —
 * callbacks included — with no lock held. */
Value vl_varread(VlFrame *f, int slot) {
  if (!f->escapes) return vl_retain(f->r[slot]);
  vl_lock_acquire(&f->vlock);
  Value v = vl_retain(f->r[slot]);
  vl_lock_release(&f->vlock);
  return v;
}

/* vl_frame_up is the frame `depth` levels above f (vl_up's walk). */
VlFrame *vl_frame_up(VlFrame *f, int depth) {
  for (int i = 0; i < depth; i++) {
    if (!f) vl_abort("upvalue depth exceeded");
    f = f->up;
  }
  if (!f) vl_abort("upvalue depth exceeded");
  return f;
}

/* ---------------------------------------------------------------- frames */

/* frame_mark_shared flags a frame and its whole up-chain as thread-crossing.
 * Runs on the frame's owning thread, before the capturing closure escapes;
 * an already-shared ancestor means the rest of the chain is already marked. */
static void frame_mark_shared(VlFrame *f) {
  for (; f && !f->shared; f = f->up) {
    f->shared = 1;
  }
}

/* frame_retain bumps a frame's refcount: plain for thread-local frames,
 * atomic once a closure made the frame shareable. */
static void frame_retain(VlFrame *f) {
  if (f->shared) __atomic_add_fetch(&f->frc, 1, __ATOMIC_RELAXED);
  else f->frc++;
}

VlFrame *vl_frame_new(int nregs, VlFrame *up) {
  size_t size = sizeof(VlFrame) + sizeof(Value) * (size_t)(nregs > 0 ? nregs - 1 : 0);
  VlFrame *f = (VlFrame *)vl_alloc(size);
  f->hdr.rc = 1;
  f->frc = 1;
  f->hdr.kind = O_FRAME;
  f->nregs = nregs;
  f->stackalloc = 0;
  f->shared = 0;
  f->info = NULL;
  f->line = 0;
  f->up = up;
  if (up) {
    assert(!up->stackalloc && "a stack frame must never be captured as up");
    frame_enter(f, up);
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
  f->frc = 1;
  f->hdr.kind = O_FRAME;
  f->nregs = nregs;
  f->stackalloc = 1;
  f->up = up;
  if (up) {
    assert(!up->stackalloc && "a stack frame must never be captured as up");
    frame_enter(f, up);
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
/*
 * A closure held in a register that another closure READS through an
 * upvalue (the function's VlFnInfo.upmask) is left alone: `let h = fn…;
 * return fn() { h() }` must find `h` when the escaped closure runs. That
 * leaves a frame⇄closure cycle alive for as long as an outside holder
 * keeps the frame — and vl_frame_release breaks it when the last outside
 * holder lets go (self_cycle_only). */
/*
 * A frame no closure captured cannot be in a cycle and is left alone. A
 * captured one is the dangerous case, and not only for a closure held in a
 * register: a STRUCT (or any value) held there may hold a closure over this
 * frame — `return H{less: fn(a, b) …}` keeps the returned H in the RET
 * register, H keeps the closure, the closure keeps the frame. The function
 * has returned, so every register nothing reads through an upvalue is dead:
 * all of those are dropped. (Code compiled without the mask — a NULL info —
 * keeps the narrower rule: closures over this frame only.) */
void vl_frame_clear(VlFrame *f) {
  if (!f) return;
  if (!f->shared) return;          /* never captured: no cycle through it */
  /* The function has returned: from here on only closures read the frame
   * (through upvalues), which is what lets self_cycle_only scan it. */
  __atomic_store_n(&f->done, 1, __ATOMIC_RELEASE);
  const unsigned char *um = f->info ? f->info->upmask : NULL;
  int nu = f->info ? f->info->nupmask : 0;
  bool all = f->info != NULL;
  for (int i = 0; i < f->nregs; i++) {
    Value v = f->r[i];
    if (v.t != VL_OBJ) continue;
    if (um && i < nu && um[i]) continue;        /* an upvalue still reads it */
    if (!all) {
      if (v.u.o->kind != O_CLOSURE) continue;
      if (((VlClosure *)v.u.o)->up != f) continue;
    }
    f->r[i] = vl_nil();
    vl_release(v);
  }
}

/* The most values owned_refs looks at for one frame. The walk runs each
 * time a holder of a finished, captured frame lets go, so it must stay
 * cheap: a frame whose cycle hides deeper than this (a closure over the
 * frame stored inside a large container the frame owns) is not reaped. */
#define CYCLE_WALK_BUDGET 256

/* owned_refs counts the references to frame f held by closures over f
 * that are reachable from v through objects NOTHING ELSE holds: v itself
 * and every container on the way must have a refcount of 1. Those are
 * references f keeps alive only because f keeps them alive — the cycle
 * frame -> register -> struct/map/slice/... -> closure -> frame.
 *
 * A closure over f with rc 1 is one such reference (its `up`). A struct,
 * enum, tuple, Box, slice or map with rc 1 is owned by whoever reached
 * it, so its contents are walked too. Anything shared (rc > 1) may be
 * held from outside f and ends the path. *budget bounds the walk; when it
 * runs out the result is -1 ("unknown"), which never matches a count. */
static int64_t owned_refs(Value v, VlFrame *f, int *budget) {
  if (v.t != VL_OBJ || !v.u.o) return 0;
  VlObj *o = v.u.o;
  if (--*budget < 0) return -1;
  if (__atomic_load_n(&o->rc, __ATOMIC_RELAXED) != 1) return 0;
  Value *e = NULL;
  int64_t n = 0;
  switch (o->kind) {
  case O_CLOSURE:
    return ((VlClosure *)o)->up == f ? 1 : 0;
  case O_STRUCT:
    e = ((VlStruct *)o)->f;
    n = ((VlStruct *)o)->type->nfields;
    break;
  case O_ENUM:
    e = ((VlEnum *)o)->f;
    n = ((VlEnum *)o)->n;
    break;
  case O_TUPLE:
    e = ((VlTuple *)o)->e;
    n = ((VlTuple *)o)->n;
    break;
  case O_SLICE:
    e = ((VlSlice *)o)->e;
    n = ((VlSlice *)o)->len;
    break;
  case O_BOX:
    return owned_refs(((VlBox *)o)->v, f, budget);
  case O_MAP: {
    /* Keys are hashable values and cannot hold a closure: values only. */
    VlMap *m = (VlMap *)o;
    if (m->is_set) return 0;
    int64_t sum = 0;
    for (int64_t i = 0; i < m->nents; i++) {
      if (!m->ents[i].used) continue;
      int64_t c = owned_refs(m->ents[i].v, f, budget);
      if (c < 0) return -1;
      sum += c;
    }
    return sum;
  }
  default:
    return 0;
  }
  int64_t sum = 0;
  for (int64_t i = 0; i < n; i++) {
    int64_t c = owned_refs(e[i], f, budget);
    if (c < 0) return -1;
    sum += c;
  }
  return sum;
}

/* self_refs sums owned_refs over f's registers: how many of f's
 * references come from values only f holds. -1 when the walk gave up.
 * Shallow (deep == 0): closures held by a register directly, nothing
 * inside a container — the cheap judgement made at every call's exit. */
static int64_t self_refs(VlFrame *f, int deep) {
  int budget = CYCLE_WALK_BUDGET;
  int64_t n = 0;
  for (int i = 0; i < f->nregs; i++) {
    Value v = f->r[i];
    if (!deep && (v.t != VL_OBJ || v.u.o->kind != O_CLOSURE)) continue;
    int64_t c = owned_refs(v, f, &budget);
    if (c < 0) return -1;
    n += c;
  }
  return n;
}

/* How the frame-cycle judgement stays safe with closures over f running
 * on other tasks (they write f's registers through upvalues). Everything
 * it needs lives in f's one count word, `frc`, so each step of it is ONE
 * atomic operation on that word — a call's entry and exit included, which
 * are the frequent case and cost what a plain retain and release would:
 *
 *  - ACTIVE counts the calls of closures over f in progress. A judge reads
 *    f's registers only after setting FRAME_JUDGING with ACTIVE at 0, in
 *    one compare-and-swap; a call that enters while the flag is up sees it
 *    in the very increment that counts it, and waits for the judge to drop
 *    it before touching a register (frame_enter). The judge then clears
 *    the flag with a compare-and-swap expecting the word it set: a word
 *    that changed meanwhile (a call came, a reference came or went) voids
 *    the verdict, and the judge looks again.
 *  - A judgement that finds calls in progress is DEFERRED: the frame, with
 *    the judge's hold, goes on a list judged again when a task ends or a
 *    group is joined (vl_judge_deferred). Handing the judgement to the
 *    last call out instead would need that exit to inspect the frame after
 *    letting go of it — a second exit racing it could free the frame in
 *    between — or to keep a hold across its exit, two atomic steps on every
 *    call where one now suffices.
 *  - A thread in the middle of a judgement keeps f allocated with a HOLD
 *    (a thread dropping its reference turns it into a hold until it has
 *    judged). The judge counts only real references — read in the same
 *    load as the holds, so no hold is ever half-counted: two threads
 *    letting go of f's last outside references at once must not each see
 *    the other's hold and both decide f is still referenced.
 *  - One judge at a time, WITHOUT waiting: a judge that finds the flag up
 *    leaves `pending` set, and the judge, having let go, judges again if
 *    it was set — no judge ever spins on another's judgement, so two
 *    judges each freeing a closure over the other's frame cannot deadlock.
 *    `reaper` marks the thread clearing the registers of a frame found to
 *    be garbage, so that the releases it makes do not judge it again. */

/* frame_enter is the call side of the protocol above: frame `f`, whose
 * `up` is shared, takes its reference to up and counts itself ACTIVE in
 * one step; a judgement reading up's registers at that moment is waited
 * for (rare, and brief). An unshared up is never judged; `inact` records
 * which way f entered, for frame_exit (up may become shared while f
 * runs). */
static void frame_enter(VlFrame *f, VlFrame *up) {
  if (!up->shared) {
    up->frc++;
    return;
  }
  f->inact = 1;
  /* The increment returns the word it changed: a judge that set the flag
   * before it is seen there, and one that tries after it fails its
   * compare-and-swap, which expected the word without this call. RELAXED:
   * an acquiring add costs a third more when many tasks call over one
   * frame (8 tasks x 1M calls), and nothing needs it — when the flag is
   * up, the wait below acquires from the judge's release; when it is not,
   * no judge is reading, and this call's writes, all control-dependent on
   * the result, cannot be seen before the increment is. */
  uint64_t prev = __atomic_fetch_add(&up->frc, 1 + FRAME_ACT, __ATOMIC_RELAXED);
  if ((prev & FRAME_JUDGING) && up->reaper != vl_ctx()) {
    while (__atomic_load_n(&up->frc, __ATOMIC_ACQUIRE) & FRAME_JUDGING)
      sched_yield();
  }
}

/* self_cycle_only judges a finished, captured frame f that the caller
 * keeps alive with a hold: when every remaining real reference to f comes
 * from closures f itself owns (held in its registers, directly or — deep
 * — inside containers nothing else holds, see owned_refs), f is garbage,
 * and dropping those registers frees it. The caller's hold keeps f
 * allocated across the clearing. */
/* The frames whose judgement was deferred (defer_judgement), each with
 * the hold its judge took. */
static pthread_mutex_t g_defer_mu = PTHREAD_MUTEX_INITIALIZER;
static VlFrame **g_deferred;
static int g_ndeferred, g_capdeferred;

/* defer_judgement: calls over f are in progress, so its registers are
 * being written and must not be read — and f, referenced by those calls,
 * is no garbage now. Its judgement waits on the deferred list, with the
 * caller's hold: true then, and the caller must not drop the hold. False
 * when no call is in progress: the caller judges now. */
static bool defer_judgement(VlFrame *f) {
  if (!(__atomic_load_n(&f->frc, __ATOMIC_ACQUIRE) & FRAME_ACTS)) return false;
  pthread_mutex_lock(&g_defer_mu);
  if (g_ndeferred == g_capdeferred) {
    g_capdeferred = g_capdeferred ? g_capdeferred * 2 : 16;
    g_deferred = (VlFrame **)realloc(g_deferred, sizeof(VlFrame *) *
                                                     (size_t)g_capdeferred);
    if (!g_deferred) vl_abort("out of memory");
  }
  g_deferred[g_ndeferred++] = f;
  pthread_mutex_unlock(&g_defer_mu);
  return true;
}

/* vl_judge_deferred judges the frames whose judgement was deferred: those
 * no call is in progress over any more are judged now, the others stay on
 * the list. Run when a task ends and when a group is joined (task.c), so a
 * frame freed by a deferred judgement waits at most until then. */
void vl_judge_deferred(void) {
  pthread_mutex_lock(&g_defer_mu);
  int n = g_ndeferred;
  VlFrame **list = g_deferred;
  g_deferred = NULL;
  g_ndeferred = g_capdeferred = 0;
  pthread_mutex_unlock(&g_defer_mu);
  for (int i = 0; i < n; i++) {
    /* A judgement that defers again re-lists the frame, with this hold. */
    if (!self_cycle_only(list[i], 1)) frame_unhold(list[i]);
  }
  free(list);
}

static bool self_cycle_only(VlFrame *f, int deep) {
  /* Only a frame whose function has returned (vl_frame_clear): a running
   * one is still writing its registers. */
  if (!__atomic_load_n(&f->done, __ATOMIC_ACQUIRE)) return false;
  void *me = vl_ctx();
  if (f->reaper == me) return false;    /* re-entered from our clearing */
  for (;;) {
    if (defer_judgement(f)) return true;
    uint64_t c = __atomic_load_n(&f->frc, __ATOMIC_ACQUIRE);
    if (c & FRAME_ACTS) continue;                 /* a call came: defer */
    if (c & FRAME_JUDGING) {
      /* Another judge is reading: it looks again when done. */
      __atomic_store_n(&f->pending, 1, __ATOMIC_SEQ_CST);
      if (__atomic_load_n(&f->frc, __ATOMIC_SEQ_CST) & FRAME_JUDGING)
        return false;
      continue;                                   /* it finished: ours */
    }
    if (!__atomic_compare_exchange_n(&f->frc, &c, c | FRAME_JUDGING, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
      continue;
    __atomic_store_n(&f->pending, 0, __ATOMIC_SEQ_CST);
    int64_t n = self_refs(f, deep);
    bool garbage = n > 0 && n == (int64_t)(c & FRAME_REFS);
    /* The verdict stands only if the word is still as we set it: if it
     * changed, the registers read may predate a reference that came or a
     * call that waited on us, and the verdict is void. A garbage verdict
     * keeps the flag up while the registers are cleared, so that no other
     * judge reads them meanwhile; any other drops it in the same step. */
    uint64_t want = c | FRAME_JUDGING;
    if (!__atomic_compare_exchange_n(&f->frc, &want,
                                     garbage ? want : c, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      __atomic_fetch_and(&f->frc, ~FRAME_JUDGING, __ATOMIC_ACQ_REL);
      continue;
    }
    if (garbage) {
      /* Every real reference to f is accounted for by values f alone
       * owns, so nothing outside can reach f any more — no call can enter
       * now. Dropping the registers that lead to those closures frees
       * them, and with them their references to f. */
      f->reaper = me;
      for (int i = 0; i < f->nregs; i++) {
        Value v = f->r[i];
        if (!deep && (v.t != VL_OBJ || v.u.o->kind != O_CLOSURE)) continue;
        int budget = CYCLE_WALK_BUDGET;
        if (owned_refs(v, f, &budget) <= 0) continue;
        f->r[i] = vl_nil();
        vl_release(v);
      }
      f->reaper = NULL;
      __atomic_fetch_and(&f->frc, ~FRAME_JUDGING, __ATOMIC_ACQ_REL);
      return false;
    }
    /* Someone let go while we judged: judge again. */
    if (!__atomic_exchange_n(&f->pending, 0, __ATOMIC_SEQ_CST)) return false;
  }
}

/* frame_exit drops the reference (and the ACTIVE mark) a frame that
 * entered through frame_enter holds on its up — every call's exit, so the
 * frequent case, and the cheap one: ONE atomic step, as a plain release
 * would be. A judgement that found this call in progress was deferred
 * (defer_judgement), so there is nothing to judge here. */
static void frame_exit(VlFrame *up) {
  uint64_t prev = __atomic_fetch_sub(&up->frc, 1 + FRAME_ACT, __ATOMIC_ACQ_REL);
  if (prev == 1 + FRAME_ACT) frame_free(up);
}

/* frame_free releases what a frame holds and its storage, once its count
 * is 0. */
static void frame_free(VlFrame *f) {
  for (int i = 0; i < f->nregs; i++) vl_release(f->r[i]);
  for (int i = 0; i < f->ndefers; i++) vl_release(f->defers[i]);
  free(f->defers);
  if (f->up) {
    if (f->inact) frame_exit(f->up);
    else frame_release_judged(f->up, 0);
  }
  if (!f->stackalloc) free(f);
}

/* frame_unhold drops a judgement hold (frame_release_judged, frame_exit,
 * vl_release): it may be the last thing keeping f allocated. */
static void frame_unhold(VlFrame *f) {
  if (__atomic_sub_fetch(&f->frc, FRAME_HOLD, __ATOMIC_ACQ_REL) == 0)
    frame_free(f);
}

/* frame_release_judged drops one reference to f; a captured f that is
 * still referenced afterwards is judged (self_cycle_only), deep or
 * shallow. The reference becomes a hold for the judgement — f stays
 * allocated while this thread reads it — and is dropped after. */
static void frame_release_judged(VlFrame *f, int deep) {
  if (!f) return;
  if (!f->shared) {
    /* Never captured by a closure => never left this thread. */
    if (--f->frc > 0) return;
    frame_free(f);
    return;
  }
  /* One step decides, on the count it changes: while other references
   * remain, ours becomes a hold and f is judged; the last one goes. The
   * judgement is made even if f's function is still running (it then
   * returns at once): reading `done` first would let f return between
   * the read and the decrement, its own final judgement still counting
   * our reference — and nobody would judge again. */
  uint64_t c = __atomic_load_n(&f->frc, __ATOMIC_RELAXED);
  uint64_t next;
  bool judge;
  do {
    judge = (c & FRAME_REFS) > 1;
    next = judge ? c - 1 + FRAME_HOLD : c - 1;
  } while (!__atomic_compare_exchange_n(&f->frc, &c, next, true,
                                        __ATOMIC_SEQ_CST, __ATOMIC_RELAXED));
  if (judge) {
    if (!self_cycle_only(f, deep)) frame_unhold(f);
    return;
  }
  /* SEQ_CST (on success): the zero-observing thread sees every register
   * write the other threads made before their own releases. */
  if (next == 0) frame_free(f);
}

void vl_frame_release(VlFrame *f) { frame_release_judged(f, 1); }

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

/* run_defers runs a frame's defers, newest first, taking each one OFF the
 * frame before it runs. A defer that throws (or faults) is therefore not run
 * again when the unwinder comes back to this frame — which it does, because
 * the frame stays on the frame stack until its defers are done (see
 * vl_unwind_to) — and the defers still pending are. The closure being run is
 * registered as an unwind cleanup (err.c), so a throw out of it releases it
 * once the unwinding is over instead of leaking it. */
static void run_defers(VlFrame *f) {
  while (f->ndefers > 0) {
    Value d = f->defers[--f->ndefers];
    int mark = vl_cleanup_mark();
    vl_cleanup_slot(&d);
    vl_release(vl_call(d, NULL, 0));
    vl_cleanup_to(mark);
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
    /* The frame is popped only AFTER its defers have run: if one of them
     * throws, the nested unwinding finds this frame still on the stack and
     * finishes it — its remaining defers, its release — exactly once. */
    VlFrame *f = c->frames[c->nframes - 1];
    run_defers(f);
    c->nframes--;
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
  case O_READER: return "reader";
  case O_WRITER: return "writer";
  case O_MUTEX: return "mutex";
  case O_RWLOCK: return "rwlock";
  case O_ATOMIC: return "atomic";
  case O_REGEX: return "regex";
  case O_BUILDER: return "builder";
  case O_TYPE: return "type";
  case O_FRAME: return "frame";
  case O_ITER: return "iter";
  case O_BOX: return "Box";
  case O_PROC: return "proc";
  case O_HANDLE: return ((VlHandle *)v.u.o)->ops->name;
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
  /* A Box is transparent to equality: it compares what it holds. */
  if (vl_is_box(a) || vl_is_box(b)) return vl_equal(vl_unbox(a), vl_unbox(b));
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
  /* The float's bits are hashed after the switch: they must live at
   * function scope. (They used to be a block-local `d` the pointer outlived,
   * which hashed whatever the stack held next — so `m[1.5]` never matched
   * the key it had stored.) */
  double fbits = 0;
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
    /* Only a float inside int64's range can be integral-as-an-int: the cast
     * of anything else (±inf, NaN, |d| ≥ 2^63) is undefined. */
    if (d > -9223372036854775808.0 && d < 9223372036854775808.0 &&
        d == (double)(int64_t)d) {
      int64_t i64 = (int64_t)d;
      p = (const unsigned char *)&i64;
      n = sizeof(int64_t);
      for (int64_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
      return h;
    }
    fbits = d;
    p = (const unsigned char *)&fbits;
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
    /* Tuples, structs and slices compare STRUCTURALLY in vl_equal, so they
     * must hash structurally too: hashing the pointer put `(r, c)` and an
     * equal tuple built later in different buckets, and `m[(r, c)]` missed
     * a key `say m` plainly showed. */
    if (v.u.o->kind == O_BOX) return vl_hash(vl_unbox(v));
    if (v.u.o->kind == O_TUPLE) {
      VlTuple *t = (VlTuple *)v.u.o;
      uint64_t hh = 0x9e3779b97f4a7c15ULL ^ (uint64_t)t->n;
      for (int i = 0; i < t->n; i++) hh = hh * 31 + vl_hash(t->e[i]);
      return hh;
    }
    if (v.u.o->kind == O_STRUCT) {
      VlStruct *s = (VlStruct *)v.u.o;
      uint64_t hh = (uint64_t)(uintptr_t)s->type;
      for (int i = 0; i < s->type->nfields; i++) hh = hh * 31 + vl_hash(s->f[i]);
      return hh;
    }
    if (v.u.o->kind == O_SLICE) {
      VlSlice *s = (VlSlice *)v.u.o;
      uint64_t hh = 0x51ce5eedULL ^ (uint64_t)s->len;
      for (int64_t i = 0; i < s->len; i++) hh = hh * 31 + vl_hash(s->e[i]);
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
  if (vl_is_box(v)) {
    quote_into(b, vl_unbox(v));
    return;
  }
  if (vl_is_newtype(v)) {
    /* A newtype with no `show` prints as its underlying value, quoted the
     * same way inside a collection. */
    bool ok = false;
    Value shown = vl_user_show(v, &ok);
    if (ok) {
      bwrite(b, vl_cstr(shown), vl_strlen(shown));
      vl_release(shown);
      return;
    }
    quote_into(b, ((VlStruct *)v.u.o)->f[0]);
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

/* str_into is recursive in the value's depth, and a value can be deep: a
 * list of 20,000 nested Boxes overflowed the C stack. Past STR_MAX_DEPTH the
 * rest is printed as `…` — a printout is for reading, and nobody reads past
 * that. The counter is per thread; a throw out of a Show method mid-print
 * leaves it raised, which can only shorten later printouts, never crash. */
#define STR_MAX_DEPTH 2048
static __thread int g_str_depth;
static void str_into_body(Value b, Value v);
static void str_into(Value b, Value v) {
  if (g_str_depth >= STR_MAX_DEPTH) {
    bputs(b, "…");
    return;
  }
  g_str_depth++;
  str_into_body(b, v);
  g_str_depth--;
}

static void str_into_body(Value b, Value v) {
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
    if (s->type->newtype) {
      str_into(b, s->f[0]);
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
  case O_READER: {
    bputs(b, "<reader ");
    Value n = ((VlReader *)v.u.o)->name;
    bwrite(b, vl_cstr(n), vl_strlen(n));
    bputs(b, ((VlReader *)v.u.o)->closed ? " closed>" : ">");
    return;
  }
  case O_WRITER: {
    bputs(b, "<writer ");
    Value n = ((VlWriter *)v.u.o)->name;
    bwrite(b, vl_cstr(n), vl_strlen(n));
    bputs(b, ((VlWriter *)v.u.o)->closed ? " closed>" : ">");
    return;
  }
  /* A lock has no state that can be read without taking it, and taking one
   * to print it would be a lock held across `say`. So they print as their
   * kind. The atomic is different twice over: it HAS a readable state and a
   * seq-cst load is exactly what str_into may do, and it is a box around a
   * value rather than an opaque handle — so it prints like its two nearest
   * relatives, O_SHARED and O_CELL just above, which print what is inside
   * them rather than <shared>/<cell>. */
  case O_REGEX: {
    bputs(b, "/");
    bputs(b, vl_regex_src(v));
    bputs(b, "/");
    return;
  }
  case O_MUTEX: bputs(b, "<mutex>"); return;
  case O_RWLOCK: bputs(b, "<rwlock>"); return;
  case O_ATOMIC: {
    snprintf(tmp, sizeof tmp, "%lld", (long long)vl_sync_atomic_peek(v.u.o));
    bputs(b, tmp);
    return;
  }
  case O_BUILDER: bputs(b, "<builder>"); return;
  case O_TYPE: bputs(b, "<type>"); return;
  case O_FRAME: bputs(b, "<frame>"); return;
  case O_ITER: bputs(b, "<iter>"); return;
  case O_BOX: str_into(b, ((VlBox *)v.u.o)->v); return;
  case O_PROC: vl_proc_str(b, v); return;
  case O_HANDLE:
    bputs(b, "<");
    bputs(b, ((VlHandle *)v.u.o)->ops->name);
    bputs(b, ">");
    return;
  }
}

/* may_show reports whether printing v can run a program's own `show`
 * method — a user type, or a container that may hold one. Only then is the
 * builder registered as an unwind cleanup (err.c): a `show` that throws
 * would otherwise leak it, and leave the depth counter raised. Strings and
 * scalars, the common case, skip the bookkeeping. */
static bool may_show(Value v) {
  if (v.t != VL_OBJ) return false;
  switch ((VlKind)v.u.o->kind) {
  case O_STR: case O_DEC: case O_RANGE: case O_BUILDER: return false;
  default: return true;
  }
}

/* text_guard registers the builder and the depth counter for unwinding. */
static int text_guard(Value *b) {
  int mark = vl_cleanup_mark();
  vl_cleanup_slot(b);
  vl_cleanup_int(&g_str_depth, g_str_depth);
  return mark;
}

Value vl_tostr(Value v) {
  if (is_str(v)) return vl_retain(v);
  Value b = vl_builder_new();
  int mark = may_show(v) ? text_guard(&b) : -1;
  str_into(b, v);
  if (mark >= 0) vl_cleanup_to(mark);
  VlBuilder *bb = (VlBuilder *)b.u.o;
  Value s = vl_str_n(bb->b, bb->len);
  vl_release(b);
  return s;
}

Value vl_interp(Value *parts, int n) {
  Value b = vl_builder_new();
  int mark = -1;
  for (int i = 0; i < n; i++) {
    if (mark < 0 && may_show(parts[i])) mark = text_guard(&b);
    str_into(b, parts[i]);
  }
  if (mark >= 0) vl_cleanup_to(mark);
  VlBuilder *bb = (VlBuilder *)b.u.o;
  Value s = vl_str_n(bb->b, bb->len);
  vl_release(b);
  return s;
}

void vl_say(Value *argv, int argc) {
  Value b = vl_builder_new();
  int mark = -1;
  for (int i = 0; i < argc; i++) {
    if (i) bputs(b, " ");
    if (mark < 0 && may_show(argv[i])) mark = text_guard(&b);
    str_into(b, argv[i]);
  }
  if (mark >= 0) vl_cleanup_to(mark);
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
