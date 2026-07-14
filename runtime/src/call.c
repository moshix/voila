/* call.c — calls, method dispatch, conversions (§3.2). */
#include "voila_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

Value vl_call(Value callee, Value *argv, int argc) {
  if (callee.t != VL_OBJ) vl_abort("%s is not callable", vl_kind_name(callee));
  if (callee.u.o->kind == O_CLOSURE || callee.u.o->kind == O_NATIVE) {
    VlClosure *c = (VlClosure *)callee.u.o;
    return c->fn(argv, argc, c->up);
  }
  vl_abort("%s is not callable", vl_kind_name(callee));
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

void vl_sort_slice(Value sv, Value cmp) {
  VlSlice *s = (VlSlice *)sv.u.o;
  if (s->len < 2) return;
  SortCtx sc;
  sc.cmp = cmp;
  Value *tmp = (Value *)vl_alloc(sizeof(Value) * (size_t)s->len);
  msort(&sc, s->e, tmp, 0, s->len);
  free(tmp);
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
  if (eq(name, "first")) return s->len ? vl_retain(s->e[0]) : vl_nil();
  if (eq(name, "last")) return s->len ? vl_retain(s->e[s->len - 1]) : vl_nil();
  if (eq(name, "len")) return vl_int(s->len);
  if (eq(name, "map")) {
    Value out = vl_slice_new(0);
    for (int64_t i = 0; i < s->len; i++) {
      Value a = s->e[i];
      vl_slice_append(out, vl_call(argv[0], &a, 1));
    }
    return out;
  }
  if (eq(name, "filter")) {
    Value out = vl_slice_new(0);
    for (int64_t i = 0; i < s->len; i++) {
      Value a = s->e[i];
      Value r = vl_call(argv[0], &a, 1);
      if (r.t == VL_BOOL && r.u.b) vl_slice_append(out, vl_retain(a));
      vl_release(r);
    }
    return out;
  }
  if (eq(name, "reduce")) {
    Value acc = vl_retain(argv[1]);
    for (int64_t i = 0; i < s->len; i++) {
      Value args[2] = {acc, s->e[i]};
      Value next = vl_call(argv[0], args, 2);
      vl_release(acc);
      acc = next;
    }
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
    vl_sort_slice(out, vl_nil());
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
    for (int64_t i = 0; i < s->len; i++) {
      if (i) vl_builder_write(b, sep);
      vl_builder_write(b, s->e[i]);
    }
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

Value vl_callm(Value recv, const char *method, Value *argv, int argc) {
  /* shared[T].get() reaches the handle itself, not the value inside it. */
  if (recv.t == VL_OBJ && recv.u.o->kind == O_SHARED && eq(method, "get") &&
      argc == 0)
    return vl_retain(((VlShared *)recv.u.o)->v);

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
    if (m) {
      Value *args = (Value *)vl_alloc(sizeof(Value) * (size_t)(argc + 1));
      int n = 0;
      if (m->has_self) args[n++] = target;
      for (int i = 0; i < argc; i++) args[n++] = argv[i];
      Value r = m->fn(args, n, NULL);
      free(args);
      return r;
    }
  }

  /* 2. universal methods */
  if (eq(method, "str") && argc == 0) return vl_tostr(recv);
  if (eq(method, "clone") && argc == 0) return deep_clone(recv);

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
      out = slice_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
    case O_MAP:
    case O_SET:
      out = map_method(target, method, argv, argc, &ok);
      if (ok) return out;
      break;
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
          Value e = vl_eh_current();
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
      VlWeak *w = (VlWeak *)target.u.o;
      if (eq(method, "upgrade")) {
        if (!w->target) return vl_nil();
        Value v;
        v.t = VL_OBJ;
        v.u.o = w->target;
        return vl_retain(v);
      }
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
    case O_FILE:
      out = vl_file_method(target, method, argv, argc, &ok);
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
    if (eq(method, "unix")) return vl_int(ns / 1000000000);
    if (eq(method, "unix_millis")) return vl_int(ns / 1000000);
  }

  /* 4. a struct field holding a callable */
  if (target.t == VL_OBJ && target.u.o->kind == O_STRUCT) {
    VlStruct *s = (VlStruct *)target.u.o;
    for (int i = 0; i < s->type->nfields; i++)
      if (eq(s->type->fields[i], method)) return vl_call(s->f[i], argv, argc);
  }

  vl_abort("%s has no method `%s`", vl_kind_name(recv), method);
  return vl_nil();
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

Value vl_conv(const char *type, Value v, Value *base) {
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
      if (v.u.o->kind == O_DEC) return vl_float(vl_dec_to_float(v));
      if (v.u.o->kind == O_STR) {
        const char *s = vl_cstr(v);
        char *end;
        double f = strtod(s, &end);
        if (end == s) vl_throw_conv("str", "float", v);
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
