/* call.c — calls, method dispatch, conversions (§3.2). */
#include "voila_int.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

Value vl_call(Value callee, Value *argv, int argc) {
  /* A Box or a newtype over a function (`type Handler fn(Req) Resp`) is
   * called through. */
  callee = vl_deref(callee);
  if (callee.t != VL_OBJ) vl_fault("%s is not callable", vl_kind_name(callee));
  if (callee.u.o->kind == O_CLOSURE || callee.u.o->kind == O_NATIVE) {
    VlClosure *c = (VlClosure *)callee.u.o;
    /* A native called as a value: the compiler could not check this call,
     * so its kinds row is checked here (the vector may be a spread's). */
    if (callee.u.o->kind == O_NATIVE && c->kinds && c->kinds[0]) {
      /* The vector may be a spread's (a slice's own storage), so it is never
       * written. A Box or newtype argument is unwrapped exactly as a direct
       * call unwraps it (vl_argk with `fix`), in a COPY on the stack — a
       * native may throw, and a longjmp reclaims the stack by itself. A
       * call wider than the copy keeps the unfixed check; no native taking
       * a kind-checked argument is that wide. */
      enum { ARGK_INLINE = 16 };
      if (argc <= ARGK_INLINE) {
        Value fixed[ARGK_INLINE];
        for (int i = 0; i < argc; i++) fixed[i] = argv[i];
        vl_argk(c->name, c->kinds, fixed, argc, true);
        return c->fn(argc ? fixed : argv, argc, c->up);
      }
      vl_argk(c->name, c->kinds, argv, argc, false);
    }
    return c->fn(argv, argc, c->up);
  }
  vl_fault("%s is not callable", vl_kind_name(callee));
  return vl_nil();
}

Value vl_call_named(Value callee, Value *argv, int argc, const char **names,
                    int nnamed) {
  (void)names;
  (void)nnamed;
  /* The code generator resolves named arguments to positions before the call
   * whenever the callee is statically known; anything else is positional. */
  return vl_call(callee, argv, argc);
}

/* ---------------------------------------------------------------- sorting */

typedef struct {
  Value cmp; /* closure or nil */
} SortCtx;

static bool sort_less(SortCtx *sc, Value a, Value b) {
  if (sc->cmp.t == VL_OBJ) {
    Value args[2] = {a, b};
    Value r = vl_call(sc->cmp, args, 2);
    bool less = r.t == VL_BOOL && r.u.b;
    vl_release(r);
    return less;
  }
  Value r = vl_cmplt(a, b);
  bool less = r.t == VL_BOOL && r.u.b;
  vl_release(r);
  return less;
}

/* stable merge sort */
static void msort(SortCtx *sc, Value *a, Value *tmp, int64_t lo, int64_t hi) {
  if (hi - lo < 2) return;
  int64_t mid = lo + (hi - lo) / 2;
  msort(sc, a, tmp, lo, mid);
  msort(sc, a, tmp, mid, hi);
  int64_t i = lo, j = mid, k = lo;
  while (i < mid && j < hi)
    tmp[k++] = sort_less(sc, a[j], a[i]) ? a[j++] : a[i++];
  while (i < mid) tmp[k++] = a[i++];
  while (j < hi) tmp[k++] = a[j++];
  for (int64_t x = lo; x < hi; x++) a[x] = tmp[x];
}

/* vl_sort_slice sorts a slice in place — but OUT of place first: the
 * comparator (a closure, or a type's own `cmp` reached through vl_cmplt)
 * is Voilà code, and no lock is held across Voilà code, while another
 * task may be copying this very slice under its lock (vl_cow_unique) or a
 * builtin writer appending to it. So the elements are sorted
 * in a private array, each with a reference of its own, and only the
 * finished order is stored back, under the slice's address lock — the lock
 * every builtin writer of the slice holds — in one step, and only if the
 * slice still has the length it had (a comparator that grew or shrank it
 * leaves it as it was). */
void vl_sort_slice(Value sv, Value cmp) {
  VlSlice *s = (VlSlice *)sv.u.o;
  SortCtx sc;
  sc.cmp = cmp;
  /* The length and the elements are read together, under the lock. */
  VlLock *lk = vl_addr_lock(sv.u.o);
  vl_lock_acquire(lk);
  int64_t n = s->len;
  if (n < 2) {
    vl_lock_release(lk);
    return;
  }
  Value *work = (Value *)vl_alloc(sizeof(Value) * (size_t)n);
  for (int64_t i = 0; i < n; i++) work[i] = vl_retain(s->e[i]);
  vl_lock_release(lk);
  Value *tmp = (Value *)vl_alloc(sizeof(Value) * (size_t)n);
  /* The comparator may throw: the private arrays — and the references
   * `work` holds — are released by the unwind. */
  int mark = vl_cleanup_mark();
  vl_cleanup_mem((void **)&tmp);
  vl_cleanup_values(work, n);
  msort(&sc, work, tmp, 0, n);
  vl_cleanup_to(mark);
  free(tmp);
  vl_lock_acquire(lk);
  bool same = s->len == n;
  if (same) {
    /* The same elements in the new order: swap the arrays' contents, so
     * `work` ends up holding the references the slice held. */
    for (int64_t i = 0; i < n; i++) {
      Value t = s->e[i];
      s->e[i] = work[i];
      work[i] = t;
    }
  }
  vl_lock_release(lk);
  for (int64_t i = 0; i < n; i++) vl_release(work[i]);
  free(work);
}

/* ---------------------------------------------------------------- methods */

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

Value vl_str_method(Value self, const char *name, Value *argv, int argc,
                    bool *ok);          /* str.c */
Value vl_file_method(Value self, const char *name, Value *argv, int argc,
                     bool *ok);         /* std.c */
Value vl_regex_stub(void);

static Value deep_clone(Value v) {
  if (v.t != VL_OBJ) return vl_retain(v);
  switch ((VlKind)v.u.o->kind) {
  case O_SLICE: {
    VlSlice *s = (VlSlice *)v.u.o;
    Value out = vl_slice_new(0);
    for (int64_t i = 0; i < s->len; i++) vl_slice_append(out, deep_clone(s->e[i]));
    return out;
  }
  case O_MAP: {
    VlMap *m = (VlMap *)v.u.o;
    Value out = vl_map_new();
    for (int64_t i = 0; i < m->nents; i++)
      if (m->ents[i].used) vl_map_set(out, m->ents[i].k, deep_clone(m->ents[i].v));
    return out;
  }
  case O_SET: {
    VlMap *m = (VlMap *)v.u.o;
    Value out = vl_set_new();
    for (int64_t i = 0; i < m->nents; i++)
      if (m->ents[i].used) vl_map_set(out, m->ents[i].k, vl_nil());
    return out;
  }
  case O_STRUCT: {
    VlStruct *s = (VlStruct *)v.u.o;
    VlStruct *c = (VlStruct *)vl_obj_new(O_STRUCT, sizeof(VlStruct));
    c->type = s->type;
    c->f = (Value *)vl_alloc(sizeof(Value) * (size_t)(s->type->nfields ? s->type->nfields : 1));
    for (int i = 0; i < s->type->nfields; i++) c->f[i] = deep_clone(s->f[i]);
    Value out;
    out.t = VL_OBJ;
    out.u.o = &c->hdr;
    return out;
  }
  case O_TUPLE: {
    VlTuple *t = (VlTuple *)v.u.o;
    Value out = vl_tuple_new(t->e, t->n);
    return out;
  }
  case O_ENUM: {
    VlEnum *e = (VlEnum *)v.u.o;
    Value out = vl_enum_new(e->var->type, e->var->variant, e->f, e->n);
    return out;
  }
  case O_BOX:
    /* A Box is a UNIQUE owner: its clone owns a clone of the value. */
    return vl_box_new(deep_clone(((VlBox *)v.u.o)->v));
  default:
    return vl_retain(v);
  }
}

static Value slice_method(Value self, const char *name, Value *argv, int argc,
                          bool *ok) {
  VlSlice *s = (VlSlice *)self.u.o;
  *ok = true;
  if (eq(name, "append")) {
    for (int i = 0; i < argc; i++) vl_slice_append(self, vl_retain(argv[i]));
    return vl_unit();
  }
  if (eq(name, "pop")) {
    if (s->len == 0) return vl_nil();
    return s->e[--s->len]; /* ownership transfers out */
  }
  /* xs.insert(i, v) puts v at position i, 0 <= i <= len, shifting the tail
   * up by one; xs.remove(i) takes the element at i out, shifting the tail
   * down, and returns it. Both are in place, like append and pop, and an
   * index out of range is the RangeError slice indexing throws. */
  if (eq(name, "insert")) {
    if (argc != 2 || argv[0].t != VL_INT)
      vl_throwf("ValueError", "insert(i, v) takes an int position and a value");
    int64_t i = argv[0].u.i;
    if (i < 0 || i > s->len)
      vl_throwf("RangeError", "insert: index %lld out of range 0..%lld",
                (long long)i, (long long)s->len);
    vl_slice_append(self, vl_nil()); /* may move s->e: re-read below */
    memmove(&s->e[i + 1], &s->e[i], sizeof(Value) * (size_t)(s->len - 1 - i));
    s->e[i] = vl_retain(argv[1]);
    return vl_unit();
  }
  if (eq(name, "remove")) {
    if (argc != 1 || argv[0].t != VL_INT)
      vl_throwf("ValueError", "remove(i) takes an int position");
    int64_t i = argv[0].u.i;
    if (i < 0 || i >= s->len)
      vl_throwf("RangeError", "remove: index %lld out of range for length %lld",
                (long long)i, (long long)s->len);
    Value out = s->e[i]; /* ownership transfers out, as for pop */
    memmove(&s->e[i], &s->e[i + 1], sizeof(Value) * (size_t)(s->len - 1 - i));
    s->len--;
    return out;
  }
  if (eq(name, "first")) return s->len ? vl_retain(s->e[0]) : vl_nil();
  if (eq(name, "last")) return s->len ? vl_retain(s->e[s->len - 1]) : vl_nil();
  if (eq(name, "len")) return vl_int(s->len);
  /* map, filter and reduce hold an owned result across calls back into
   * Voilà; each is registered as an unwind cleanup (err.c), so a callback
   * that throws — or faults under a recovery point — releases it instead of
   * leaking it. */
  if (eq(name, "map")) {
    Value out = vl_slice_new(0);
    int mark = vl_cleanup_mark();
    vl_cleanup_slot(&out);
    for (int64_t i = 0; i < s->len; i++) {
      Value a = s->e[i];
      vl_slice_append(out, vl_call(argv[0], &a, 1));
    }
    vl_cleanup_to(mark);
    return out;
  }
  if (eq(name, "filter")) {
    Value out = vl_slice_new(0);
    int mark = vl_cleanup_mark();
    vl_cleanup_slot(&out);
    for (int64_t i = 0; i < s->len; i++) {
      Value a = s->e[i];
      Value r = vl_call(argv[0], &a, 1);
      if (r.t == VL_BOOL && r.u.b) vl_slice_append(out, vl_retain(a));
      vl_release(r);
    }
    vl_cleanup_to(mark);
    return out;
  }
  if (eq(name, "reduce")) {
    Value acc = vl_retain(argv[1]);
    int mark = vl_cleanup_mark();
    vl_cleanup_slot(&acc);
    for (int64_t i = 0; i < s->len; i++) {
      Value args[2] = {acc, s->e[i]};
      Value next = vl_call(argv[0], args, 2);
      vl_release(acc);
      acc = next;
    }
    vl_cleanup_to(mark);
    return acc;
  }
  if (eq(name, "each")) {
    for (int64_t i = 0; i < s->len; i++) {
      Value a = s->e[i];
      vl_release(vl_call(argv[0], &a, 1));
    }
    return vl_unit();
  }
  if (eq(name, "any") || eq(name, "all")) {
    bool all = eq(name, "all");
    for (int64_t i = 0; i < s->len; i++) {
      Value a = s->e[i];
      Value r = vl_call(argv[0], &a, 1);
      bool t = r.t == VL_BOOL && r.u.b;
      vl_release(r);
      if (all && !t) return vl_bool(false);
      if (!all && t) return vl_bool(true);
    }
    return vl_bool(all);
  }
  if (eq(name, "sort")) {
    vl_sort_slice(self, vl_nil());
    return vl_unit();
  }
  if (eq(name, "sort_by")) {
    vl_sort_slice(self, argv[0]);
    return vl_unit();
  }
  if (eq(name, "sorted")) {
    Value out = vl_slice_new(0);
    for (int64_t i = 0; i < s->len; i++) vl_slice_append(out, vl_retain(s->e[i]));
    int mark = vl_cleanup_mark(); /* a user `cmp` may throw */
    vl_cleanup_slot(&out);
    vl_sort_slice(out, vl_nil());
    vl_cleanup_to(mark);
    return out;
  }
  if (eq(name, "contains")) {
    for (int64_t i = 0; i < s->len; i++)
      if (vl_equal(s->e[i], argv[0])) return vl_bool(true);
    return vl_bool(false);
  }
  if (eq(name, "index_of")) {
    for (int64_t i = 0; i < s->len; i++)
      if (vl_equal(s->e[i], argv[0])) return vl_int(i);
    return vl_int(-1);
  }
  if (eq(name, "reverse")) {
    for (int64_t i = 0, j = s->len - 1; i < j; i++, j--) {
      Value t = s->e[i];
      s->e[i] = s->e[j];
      s->e[j] = t;
    }
    return vl_unit();
  }
  if (eq(name, "join")) {
    Value sep = argc > 0 ? vl_tostr(argv[0]) : vl_str("");
    Value b = vl_builder_new();
    int mark = vl_cleanup_mark(); /* an element's `show` may throw */
    vl_cleanup_slot(&b);
    vl_cleanup_slot(&sep);
    for (int64_t i = 0; i < s->len; i++) {
      if (i) vl_builder_write(b, sep);
      vl_builder_write(b, s->e[i]);
    }
    vl_cleanup_to(mark);
    Value out = vl_builder_done(b);
    vl_release(b);
    vl_release(sep);
    return out;
  }
  *ok = false;
  return vl_nil();
}

static Value map_method(Value self, const char *name, Value *argv, int argc,
                        bool *ok) {
  *ok = true;
  bool is_set = self.u.o->kind == O_SET;
  if (eq(name, "len")) return vl_int(vl_map_count(self));
  if (!is_set) {
    if (eq(name, "keys")) return vl_map_keys(self);
    if (eq(name, "values")) return vl_map_values(self);
    if (eq(name, "has")) return vl_bool(vl_map_has(self, argv[0]));
    if (eq(name, "get")) {
      Value out;
      if (vl_map_get(self, argv[0], &out)) return out;
      return argc > 1 ? vl_retain(argv[1]) : vl_nil();
    }
    if (eq(name, "set")) {
      vl_map_set(self, argv[0], vl_retain(argv[1]));
      return vl_unit();
    }
    if (eq(name, "delete") || eq(name, "remove")) {
      vl_map_delete(self, argv[0]);
      return vl_unit();
    }
  } else {
    if (eq(name, "add")) {
      vl_map_set(self, argv[0], vl_nil());
      return vl_unit();
    }
    if (eq(name, "remove") || eq(name, "delete")) {
      vl_map_delete(self, argv[0]);
      return vl_unit();
    }
    if (eq(name, "has") || eq(name, "contains"))
      return vl_bool(vl_map_has(self, argv[0]));
    if (eq(name, "items")) return vl_map_keys(self);
  }
  *ok = false;
  return vl_nil();
}

static Value err_method(Value self, const char *name, Value *argv, int argc,
                        bool *ok) {
  (void)argv;
  (void)argc;
  VlErr *e = (VlErr *)self.u.o;
  *ok = true;
  if (eq(name, "message")) return vl_err_message(self);
  if (eq(name, "cause")) return vl_retain(e->cause);
  if (eq(name, "trace")) return vl_retain(e->trace);
  if (eq(name, "suppressed")) return vl_retain(e->suppressed);
  *ok = false;
  return vl_nil();
}

/* vl_disp_type is the type name of the method-table row the current call
 * was dispatched through, handed from the dispatcher to the callee's
 * entry: a trait default that writes `Self.m()` reads it there
 * (vl_self_desc) as its `Self` — the implementor the call was made FOR,
 * which a value's run-time kind cannot say for a sized integer (a `u8` is
 * an `int` at run time). Set immediately before every call through the
 * table, and read before the callee runs anything else, so it is never
 * stale where it is read. One per thread: a task is a thread. */
static __thread const char *vl_disp_type;

/* vl_set_disp records the row type before a direct call through the table
 * from another runtime file (show, cmp, an iterator's next): every call of
 * a method-table function sets it, so a default never reads a stale one. */
void vl_set_disp(const char *type) { vl_disp_type = type; }

Value vl_self_desc(Value self) {
  const char *t = vl_disp_type;
  vl_disp_type = NULL;
  if (t && t[0]) return vl_str(t);
  return vl_typedesc(self);
}

/* call_user_method calls the user method `m` found for `target`. */
static Value call_user_method(const VlMethod *m, Value target, Value *argv,
                              int argc) {
  vl_disp_type = m->type;
  /* The argument vector is pure scratch: nothing here retains and nothing
   * releases, it only puts `self` in front of the caller's own arguments.
   *
   * It used to be calloc'd and freed after the call, and m->fn is USER
   * CODE THAT MAY THROW — vl_throw longjmps straight past that free, and
   * the block was then unreachable by anything. `leaks --atExit` over
   * tests/conformance/20_sync.voi found it: that file exists to prove a
   * throw out of a critical section unwinds cleanly, so it throws through
   * a method call several times over.
   *
   * The common case is now the STACK, which a longjmp reclaims by itself
   * and which is faster besides — eight slots is self plus seven
   * arguments, more than any method in the corpus takes. A wider call
   * still needs the heap, and installs a handler that frees the block and
   * then lets the exception carry on exactly as it was. */
  enum { CALLM_INLINE_ARGS = 8 };
  Value inlined[CALLM_INLINE_ARGS];
  Value *heap = NULL;
  Value *args = inlined;
  if (argc + 1 > CALLM_INLINE_ARGS) {
    heap = (Value *)vl_alloc(sizeof(Value) * (size_t)(argc + 1));
    args = heap;
  }
  int n = 0;
  if (m->has_self) args[n++] = target;
  for (int i = 0; i < argc; i++) args[n++] = argv[i];
  if (!heap) return m->fn(args, n, NULL);

  VlHandler h;
  if (VL_TRY(h)) {
    /* The throw landed here rather than at the free below. Release the
     * block, then put the exception back on its way: vl_throw retains
     * ctx->pending into ctx->pending, which leaves the refcount exactly
     * as the thrower left it. */
    free(heap);
    vl_rethrow(vl_ctx()->pending);
    /* vl_rethrow longjmps — but vl_throw HAS one path that returns, the
     * `vl_task_fail` arm taken when no handler is left. It cannot be
     * reached from here (a handler is what got us here, and vl_task_fail
     * returns false unconditionally today), and if that ever changes,
     * falling through would re-enter m->fn with `args` already freed.
     * Say so out loud instead of letting it be discovered. */
    vl_abort("unreachable: vl_rethrow returned in vl_callm");
  }
  Value r = m->fn(args, n, NULL);
  vl_eh_pop();
  free(heap);
  return r;
}

/* coll_writer: the builtin methods that change a collection's length or
 * order in place (slice_method, map_method) without running Voilà code —
 * sort_by runs its key, and no lock is held across Voilà code (it could
 * wait on a task that wants the same lock). */
static bool coll_writer(int kind, const char *m) {
  /* Not sort: it runs Voilà code (the elements' `cmp`) and takes the lock
   * itself only to commit (vl_sort_slice). */
  if (kind == O_SLICE)
    return eq(m, "append") || eq(m, "pop") || eq(m, "insert") ||
           eq(m, "remove") || eq(m, "reverse");
  return eq(m, "add") || eq(m, "delete") || eq(m, "remove") || eq(m, "set");
}

Value vl_callm(Value recv, const char *method, Value *argv, int argc) {
  /* shared[T].get() reaches the handle itself, not the value inside it. */
  if (recv.t == VL_OBJ && recv.u.o->kind == O_SHARED && eq(method, "get") &&
      argc == 0)
    return vl_retain(((VlShared *)recv.u.o)->v);

  /* Box's own two methods; everything else reaches the value inside. */
  if (vl_is_box(recv)) {
    VlBox *bx = (VlBox *)recv.u.o;
    if (eq(method, "get") && argc == 0) return vl_retain(bx->v);
    if (eq(method, "set") && argc == 1) {
      vl_set(&bx->v, vl_retain(argv[0]));
      return vl_unit();
    }
    if (eq(method, "clone") && argc == 0) return deep_clone(recv);
    /* The method may write its receiver (`mut self`): the struct inside is
     * made the Box's own first, so a struct another name shares is not
     * written through this one (types.c, own_inner). */
    vl_own_inner(recv);
    return vl_callm(vl_unbox(recv), method, argv, argc);
  }

  /* 1. user impls on nominal types (structs, enums, exceptions) */
  const char *tn = NULL;
  Value target = recv;
  if (recv.t == VL_OBJ && recv.u.o->kind == O_SHARED) {
    target = ((VlShared *)recv.u.o)->v;
  }
  if (target.t == VL_OBJ) {
    switch ((VlKind)target.u.o->kind) {
    case O_STRUCT: tn = ((VlStruct *)target.u.o)->type->name; break;
    case O_ENUM: tn = ((VlEnum *)target.u.o)->var->type; break;
    case O_ERR: tn = vl_cstr(((VlErr *)target.u.o)->type_name); break;
    default: break;
    }
  }
  if (tn && tn[0]) {
    const VlMethod *m = vl_find_method(tn, method);
    if (m) return call_user_method(m, target, argv, argc);
  }

  /* 2. universal methods */
  if (eq(method, "str") && argc == 0) return vl_tostr(recv);
  if (eq(method, "clone") && argc == 0) return deep_clone(recv);

  /* A newtype that did not write the method answers with its underlying
   * value's builtin methods (`type Names []str` has `len`, `append`, …). */
  if (vl_is_newtype(target)) {
    vl_own_inner(target);             /* as for a Box, above */
    return vl_callm(((VlStruct *)target.u.o)->f[0], method, argv, argc);
  }

  /* 3. builtin methods by kind */
  bool ok = false;
  Value out;
  if (target.t == VL_OBJ) {
    switch ((VlKind)target.u.o->kind) {
    case O_STR:
      out = vl_str_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_SLICE:
    case O_MAP:
    case O_SET: {
      /* A method that grows, shrinks or reorders a collection holds the
       * collection's address lock (value.c): two tasks reaching one slice
       * or map by a path no static rule sees (a closure handed to a task, a
       * `mut` parameter) are serialised rather than reallocating it under
       * each other. Released on unwind too. */
      bool grows = coll_writer(target.u.o->kind, method);
      int mark = 0;
      VlLock *lk = NULL;
      if (grows) {
        mark = vl_cleanup_mark();
        lk = vl_addr_lock(target.u.o);
        vl_lock_acquire(lk);
        vl_cleanup_lock(lk);
      }
      if (target.u.o->kind == O_SLICE)
        out = slice_method(target, method, argv, argc, &ok);
      else
        out = map_method(target, method, argv, argc, &ok);
      if (grows) {
        vl_cleanup_to(mark);
        vl_lock_release(lk);
      }
      if (ok) return out;
      break;
    }
    case O_ERR:
      out = err_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_TASK:
      if (eq(method, "await")) return vl_await(target);
      break;
    case O_CELL: {
      VlCell *c = (VlCell *)target.u.o;
      if (eq(method, "get")) {
        pthread_mutex_lock(&c->mu);
        Value v = vl_retain(c->v);
        pthread_mutex_unlock(&c->mu);
        return v;
      }
      if (eq(method, "set")) {
        pthread_mutex_lock(&c->mu);
        vl_set(&c->v, vl_retain(argv[0]));
        pthread_mutex_unlock(&c->mu);
        return vl_unit();
      }
      if (eq(method, "update")) {
        /* The closure runs UNDER the lock (§9.4) — computing outside it would
         * lose updates. An exception escaping the closure is an abort (§8.4),
         * so the lock can never be stranded by a throw. */
        pthread_mutex_lock(&c->mu);
        VlHandler h;
        if (VL_TRY(h)) {
          pthread_mutex_unlock(&c->mu);
          Value e = vl_eh_pending();
          /* A FAULT under a recovery point (err.c) goes on to it: the lock
           * is already released and the cell keeps its old value, so there
           * is nothing left to protect by ending the process. */
          if (vl_is_fault(e)) {
            vl_release(e);
            vl_rethrow(vl_ctx()->pending);
          }
          Value m = vl_callm(e, "message", NULL, 0);
          vl_abort("exception escaped cell.update closure (must be nothrow, "
                   "§8.4): %s", vl_cstr(m));
        }
        Value cur = c->v;
        Value next = vl_call(argv[0], &cur, 1);
        vl_eh_pop();
        vl_set(&c->v, next);
        pthread_mutex_unlock(&c->mu);
        return vl_unit();
      }
      break;
    }
    case O_WEAK: {
      if (eq(method, "upgrade")) return vl_weak_upgrade(target);
      break;
    }
    case O_SHARED:
      if (eq(method, "get")) return vl_retain(((VlShared *)target.u.o)->v);
      break;
    case O_BUILDER:
      if (eq(method, "write")) {
        vl_builder_write(target, argv[0]);
        return vl_unit();
      }
      if (eq(method, "writeln")) {
        for (int i = 0; i < argc; i++) vl_builder_write(target, argv[i]);
        Value nl = vl_str("\n");
        vl_builder_write(target, nl);
        vl_release(nl);
        return vl_unit();
      }
      if (eq(method, "done")) return vl_builder_done(target);
      if (eq(method, "len")) return vl_int(vl_builder_len(target));
      break;
    case O_REGEX:
      out = vl_regex_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_FILE:
      out = vl_file_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_READER:
      out = vl_reader_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_WRITER:
      out = vl_writer_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_PROC:
      out = vl_proc_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_DEC: {
      if (eq(method, "round")) {
        if (argc && (argv[0].t != VL_INT || argv[0].u.i < 0 || argv[0].u.i > 1000))
          vl_throwf("ValueError", "round(places) needs a places count in 0..1000");
        return vl_dec_round(target, argc ? (int)argv[0].u.i : 0);
      }
      if (eq(method, "floor")) return vl_dec_floor(target);
      if (eq(method, "ceil")) return vl_dec_ceil(target);
      if (eq(method, "abs")) return vl_dec_abs(target);
      if (eq(method, "format")) {
        if (argc < 1 || argv[0].t != VL_INT || argv[0].u.i < 0 || argv[0].u.i > 1000)
          vl_throwf("ValueError", "format(places) needs a places count in 0..1000");
        char *s = vl_dec_fixed(target, (int)argv[0].u.i);
        Value v = vl_str(s);
        free(s);
        return v;
      }
      break;
    }
    default:
      break;
    }
  }
  if (target.t == VL_DUR) {
    int64_t ns = target.u.i;
    if (eq(method, "millis")) return vl_int(ns / 1000000);
    if (eq(method, "micros")) return vl_int(ns / 1000);
    if (eq(method, "nanos")) return vl_int(ns);
    if (eq(method, "seconds")) return vl_float((double)ns / 1e9);
    if (eq(method, "minutes")) return vl_float((double)ns / 6e10);
  }
  if (target.t == VL_INSTANT) {
    int64_t ns = target.u.i;
    /* FLOOR, not truncate. These used to be plain C division, which rounds
     * toward zero: an instant half a second before the epoch answered `unix`
     * with 0 rather than -1. std/time's DateTime.unix floors and says so, and
     * the exact `unix_nanos` below makes the disagreement visible — the same
     * moment cannot read as second -1 on the calendar and second 0 here. */
    if (eq(method, "unix")) return vl_int(vl_ifloordiv(ns, 1000000000));
    if (eq(method, "unix_millis")) return vl_int(vl_ifloordiv(ns, 1000000));
    /* The unrounded reading. std/time's calendar half needs it to decompose
     * an instant without losing the sub-second part, and it completes a pair
     * that was otherwise missing its finest unit. */
    if (eq(method, "unix_nanos")) return vl_int(ns);
  }

  /* 3b. a user impl on a BUILTIN type — `impl Show for int`, a trait
   * default inherited by `str` — reached in method-call form. The program's
   * method table keys those rows by the type's own name, which is what
   * step 1 never asks for a value that is not a struct, an enum or an
   * exception. Asked only after the builtin methods, so the scan costs
   * nothing on the paths that already worked. (Every integer is an `int`
   * at run time, so an impl for a sized integer type is reached only
   * through a value the runtime sees as `int` when one is written for
   * `int` too.) */
  {
    const char *pn = vl_kind_name(target);
    if (target.t == VL_DUR) pn = "dur";
    const VlMethod *m = vl_find_method(pn, method);
    if (m) return call_user_method(m, target, argv, argc);
  }

  /* 4. a struct field holding a callable */
  if (target.t == VL_OBJ && target.u.o->kind == O_STRUCT) {
    VlStruct *s = (VlStruct *)target.u.o;
    for (int i = 0; i < s->type->nfields; i++)
      if (eq(s->type->fields[i], method)) return vl_call(s->f[i], argv, argc);
  }

  vl_fault("%s has no method `%s`", vl_kind_name(recv), method);
  return vl_nil();
}

/* ------------------------------------------------- type descriptors */

/* vl_typedesc names a value's run-time type the way the method table keys it
 * — a struct's, a newtype's, an enum's or an exception's (flat) declared
 * name, and a primitive's kind name ("int", "str") — so that it can stand for
 * a type parameter's type in a static call (vl_callt). A Box and a shared
 * handle are looked through, as vl_callm looks through them. */
Value vl_typedesc(Value v) {
  v = vl_unbox(v);
  if (v.t == VL_OBJ && v.u.o->kind == O_SHARED) v = ((VlShared *)v.u.o)->v;
  return vl_str(vl_kind_name(v));
}

/* vl_callt calls method `method` of the type a descriptor names, with the
 * caller's arguments exactly as given: a static method takes no receiver,
 * and one WITH `self` called this way (`T.show(x)`) takes the first argument
 * as its receiver. A type with no such method is a defect the checker could
 * not see — the type argument was only known at run time — so it aborts. */
Value vl_callt(Value desc, const char *method, Value *argv, int argc) {
  const char *tn = (desc.t == VL_OBJ && desc.u.o->kind == O_STR)
                       ? vl_cstr(desc)
                       : "";
  const VlMethod *m = tn[0] ? vl_find_method(tn, method) : NULL;
  if (!m) vl_fault("type `%s` has no method `%s`", tn[0] ? tn : "?", method);
  vl_disp_type = m->type;
  return m->fn(argv, argc, NULL);
}

/* vl_callm_as is vl_callm for a receiver whose STATIC type the compiler
 * knows and the run time cannot (a sized integer): that type's own row is
 * tried first, and the ordinary dispatch answers otherwise. */
Value vl_callm_as(Value recv, const char *type, const char *method,
                  Value *argv, int argc) {
  const VlMethod *m = vl_find_method(type, method);
  if (m) return call_user_method(m, recv, argv, argc);
  return vl_callm(recv, method, argv, argc);
}

/* ---------------------------------------------------------------- convert */

typedef struct {
  const char *name;
  int64_t lo, hi;
} IntRange;

static const IntRange int_ranges[] = {
    {"int", INT64_MIN, INT64_MAX}, {"i64", INT64_MIN, INT64_MAX},
    {"i8", -128, 127},             {"i16", -32768, 32767},
    {"i32", -2147483648LL, 2147483647LL},
    {"u8", 0, 255},                {"byte", 0, 255},
    {"u16", 0, 65535},             {"u32", 0, 4294967295LL},
    {"u64", 0, INT64_MAX},
};

static const IntRange *int_range(const char *t) {
  for (size_t i = 0; i < sizeof(int_ranges) / sizeof(int_ranges[0]); i++)
    if (eq(int_ranges[i].name, t)) return &int_ranges[i];
  return NULL;
}

/* parse_int_str mirrors Go's strconv.ParseInt(s, base, 64), which C's strtoll
 * does NOT: with base 0 Go understands 0x, 0o, 0b and a bare leading 0, and
 * permits `_` separators; strtoll knows only 0x and the bare leading 0, so
 * `int("0o755", 0)` threw natively while the interpreter answered 493. */
static bool parse_int_str(const char *s, int radix, int64_t *out) {
  while (*s == ' ' || *s == '\t') s++;
  bool neg = false;
  if (*s == '+' || *s == '-') {
    neg = (*s == '-');
    s++;
  }
  bool allow_underscore = (radix == 0);
  if (radix == 0) {
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
      radix = 16;
      s += 2;
    } else if (s[0] == '0' && (s[1] == 'o' || s[1] == 'O')) {
      radix = 8;
      s += 2;
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
      radix = 2;
      s += 2;
    } else if (s[0] == '0' && s[1] != 0) {
      radix = 8;
      s += 1;
    } else {
      radix = 10;
    }
  }
  uint64_t acc = 0;
  int ndigits = 0;
  for (; *s; s++) {
    if (*s == '_' && allow_underscore) continue;
    int d;
    char c = *s;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
    else return false;
    if (d >= radix) return false;
    acc = acc * (uint64_t)radix + (uint64_t)d;
    ndigits++;
  }
  if (ndigits == 0) return false;
  *out = neg ? -(int64_t)acc : (int64_t)acc;
  return true;
}

static int64_t to_int_checked(Value v, const char *target, Value *base) {
  switch (v.t) {
  case VL_INT: return v.u.i;
  case VL_RUNE: return (int64_t)v.u.r;
  case VL_FLOAT: {
    double f = v.u.f;
    if (f != f || f >= 9.2233720368547758e18 || f <= -9.2233720368547758e18)
      vl_throw_conv("float", target, v);
    return (int64_t)trunc(f); /* documented truncation toward zero */
  }
  case VL_OBJ:
    if (v.u.o->kind == O_DEC) {
      Value t = vl_dec_trunc(v);
      int64_t out;
      bool ok = vl_dec_to_int(t, &out);
      vl_release(t);
      if (!ok) vl_throw_conv("dec", target, v);
      return out;
    }
    if (v.u.o->kind == O_STR) {
      int radix = 10;
      if (base && base->t == VL_INT) radix = (int)base->u.i;
      int64_t n;
      if (!parse_int_str(vl_cstr(v), radix, &n)) vl_throw_conv("str", target, v);
      return n;
    }
    break;
  default:
    break;
  }
  vl_throw_conv(vl_kind_name(v), target, v);
  return 0;
}

/* conv_target reports whether a type name is one vl_conv converts TO. */
static bool conv_target(const char *t) {
  return int_range(t) || eq(t, "float") || eq(t, "f32") || eq(t, "dec") ||
         eq(t, "str") || eq(t, "rune") || eq(t, "bool");
}

Value vl_conv(const char *type, Value v, Value *base) {
  /* Converting FROM a newtype or a Box starts from the value inside:
   * `int(c)` for `type Cel int`, or `float(b)` for a `Box[float]`. */
  v = vl_deref(v);
  /* Converting TO a newtype (§3.8): `Cel(5)`. The underlying value is
   * converted first when the underlying type is itself a conversion target
   * — so `Cel(5.7)` truncates as `int(5.7)` does, and `type Age u8` range
   * checks — and taken as it is otherwise (a slice, a struct, a function). */
  const VlType *nt = vl_user_type(type);
  if (nt && nt->newtype) {
    const char *under = nt->ftypes ? nt->ftypes[0] : "";
    Value inner;
    if (conv_target(under)) inner = vl_conv(under, v, base);
    else if (vl_user_type(under) && vl_user_type(under)->newtype)
      inner = vl_conv(under, v, base);
    else inner = vl_retain(v);
    return vl_newtype_wrap(nt, inner);
  }
  const IntRange *r = int_range(type);
  if (r) {
    int64_t n = to_int_checked(v, type, base);
    if (n < r->lo || n > r->hi) vl_throw_conv(vl_kind_name(v), type, v);
    return vl_int(n);
  }
  if (eq(type, "float") || eq(type, "f32")) {
    switch (v.t) {
    case VL_INT: return vl_float((double)v.u.i);
    case VL_RUNE: return vl_float((double)v.u.r);
    case VL_FLOAT: return v;
    case VL_OBJ:
      if (v.u.o->kind == O_DEC) {
        /* A decimal beyond the float range is a refusal, as Go's ParseFloat
         * reports ErrRange — never a silent infinity. */
        double f = vl_dec_to_float(v);
        if (isinf(f)) vl_throw_conv("dec", "float", v);
        return vl_float(f);
      }
      if (v.u.o->kind == O_STR) {
        const char *s = vl_cstr(v);
        char *end;
        errno = 0;
        double f = strtod(s, &end);
        if (end == s) vl_throw_conv("str", "float", v);
        /* Overflow (ERANGE with an infinite result) is an error; an
         * underflow to zero is not, and "inf" spelled out stays legal. */
        if (errno == ERANGE && isinf(f)) vl_throw_conv("str", "float", v);
        return vl_float(f);
      }
      break;
    default: break;
    }
    vl_throw_conv(vl_kind_name(v), "float", v);
  }
  if (eq(type, "dec")) {
    if (v.t == VL_OBJ && v.u.o->kind == O_DEC) return vl_retain(v);
    if (v.t == VL_INT) return vl_dec_from_int(v.u.i);
    if (v.t == VL_FLOAT) return vl_dec_from_float(v.u.f);
    if (v.t == VL_OBJ && v.u.o->kind == O_STR) {
      const char *s = vl_cstr(v);
      while (*s == ' ' || *s == '\t') s++;
      return vl_dec_parse(s);
    }
    vl_throw_conv(vl_kind_name(v), "dec", v);
  }
  if (eq(type, "str")) return vl_tostr(v);
  if (eq(type, "rune")) {
    if (v.t == VL_RUNE) return v;
    if (v.t == VL_INT) return vl_rune((uint32_t)v.u.i);
    vl_throw_conv(vl_kind_name(v), "rune", v);
  }
  if (eq(type, "bool")) {
    if (v.t == VL_BOOL) return v;
    vl_throw_conv(vl_kind_name(v), "bool", v);
  }
  vl_abort("unknown conversion %s()", type);
  return vl_nil();
}
