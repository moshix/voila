/* ops.c — the numeric tower (§3.2, §5.1): implicit widening only when it is
 * lossless, overflow traps, exact decimal arithmetic. */
#include "voila_int.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool is_dec(Value v) { return v.t == VL_OBJ && v.u.o->kind == O_DEC; }
static bool is_str(Value v) { return v.t == VL_OBJ && v.u.o->kind == O_STR; }

static Value derune(Value v) {
  if (v.t == VL_RUNE) return vl_int((int64_t)v.u.r);
  return v;
}

/* ------------------------------------------------------- cold traps (-O3)
 * The SINGLE home of every numeric trap message. The boxed helpers below
 * and the inline _ck wrappers in voila.h both land here, so an optimized
 * build cannot emit a different message than an unoptimized one. */

void vl_trap_iovf(const char *op, int64_t a, int64_t b) {
  vl_throwf("OverflowError", "integer overflow: %lld %s %lld",
            (long long)a, op, (long long)b);
  __builtin_unreachable();
}
void vl_trap_div0(void) {
  vl_throwf("RangeError", "division by zero");
  __builtin_unreachable();
}
void vl_trap_itof(int64_t n) {
  vl_throwf("ConvError",
            "int value %lld is not exactly representable as float; use float(x)",
            (long long)n);
  __builtin_unreachable();
}
void vl_trap_shift(int64_t n) {
  vl_throwf("RangeError", "shift count %lld out of range", (long long)n);
  __builtin_unreachable();
}
void vl_trap_inegovf(void) {
  vl_throwf("OverflowError", "integer overflow negating");
  __builtin_unreachable();
}

static const char *sz_name(int t) {
  static const char *const names[] = {"?", "i8", "i16", "i32", "u8", "u16",
                                      "u32", "u64"};
  return (t >= 1 && t <= 7) ? names[t] : "?";
}
void vl_trap_szovf(const char *op, int64_t a, int64_t b, int t) {
  vl_throwf("OverflowError", "integer overflow: %lld %s %lld is out of range for %s",
            (long long)a, op, (long long)b, sz_name(t));
  __builtin_unreachable();
}
void vl_trap_szneg(int64_t a, int t) {
  vl_throwf("OverflowError", "integer overflow: -(%lld) is out of range for %s",
            (long long)a, sz_name(t));
  __builtin_unreachable();
}
void vl_trap_szval(int64_t v, int t) {
  vl_throwf("OverflowError", "integer overflow: %lld is out of range for %s",
            (long long)v, sz_name(t));
  __builtin_unreachable();
}

/* Shared out-of-line cores (declared in voila.h). */
int64_t vl_ifloordiv(int64_t a, int64_t b) {
  if (b == 0) vl_trap_div0();
  if (b == -1 && a == INT64_MIN) vl_trap_iovf("~/", a, -1);
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
  return q;
}
int64_t vl_ifloormod(int64_t a, int64_t b) {
  if (b == 0) vl_trap_div0();
  if (b == -1) return 0;
  int64_t m = a % b;
  if (m != 0 && ((a < 0) != (b < 0))) m += b;
  return m;
}
int64_t vl_ipow_i(int64_t a, int64_t b) {
  if (b < 0)
    vl_throwf("RangeError", "negative integer exponent %lld (use float or dec base)",
              (long long)b);
  int64_t result = 1, base = a;
  for (int64_t e = b; e > 0; e >>= 1) {
    if (e & 1) {
      if (__builtin_mul_overflow(result, base, &result)) vl_trap_iovf("**", a, b);
    }
    if (e > 1) {
      if (__builtin_mul_overflow(base, base, &base)) vl_trap_iovf("**", a, b);
    }
  }
  return result;
}
double vl_ffloormod(double x, double y) {
  if (y == 0) vl_trap_div0();
  double m = fmod(x, y);
  if (m != 0 && ((m < 0) != (y < 0))) m += y;
  return m;
}
int64_t vl_ifloordiv_f(double x, double y) {
  if (y == 0) vl_trap_div0();
  double q = floor(x / y);
  /* The cast would be undefined (and platform-divergent) out of range:
   * arm64 saturates, x86-64 gives INT64_MIN. Trap instead — both the boxed
   * and the -O3 path share this function, so the message cannot diverge. */
  if (!(q >= -9223372036854775808.0 && q < 9223372036854775808.0))
    vl_throwf("OverflowError", "floor division result %g is out of int range", q);
  return (int64_t)q;
}
double vl_fpow(double x, double y) { return pow(x, y); }

/* int → float only when exactly representable (§3.2): vl_itof_ck. */
#define int_to_float vl_itof_ck

/* ---------------------------------------------------------------- ints */

static Value int_add(int64_t a, int64_t b) { return vl_int(vl_iadd_ck(a, b)); }
static Value int_sub(int64_t a, int64_t b) { return vl_int(vl_isub_ck(a, b)); }
static Value int_mul(int64_t a, int64_t b) { return vl_int(vl_imul_ck(a, b)); }
static int64_t floor_div(int64_t a, int64_t b) { return vl_ifloordiv(a, b); }
static int64_t floor_mod(int64_t a, int64_t b) { return vl_ifloormod(a, b); }
static Value int_pow(int64_t a, int64_t b) { return vl_int(vl_ipow_i(a, b)); }

/* ---------------------------------------------------------------- durations */

static bool dur_op(int op, Value a, Value b, Value *out);

/* op codes shared by the arithmetic entry points */
enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_IDIV, OP_MOD, OP_POW,
       OP_LT, OP_LE, OP_GT, OP_GE };

static bool dur_op(int op, Value a, Value b, Value *out) {
  bool da = a.t == VL_DUR, db = b.t == VL_DUR;
  if (!da && !db) return false;
  if (da && db) {
    switch (op) {
    case OP_ADD: *out = vl_dur(a.u.i + b.u.i); return true;
    case OP_SUB: *out = vl_dur(a.u.i - b.u.i); return true;
    case OP_DIV:
      if (b.u.i == 0) vl_throwf("RangeError", "division by zero");
      *out = vl_float((double)a.u.i / (double)b.u.i);
      return true;
    case OP_LT: *out = vl_bool(a.u.i < b.u.i); return true;
    case OP_LE: *out = vl_bool(a.u.i <= b.u.i); return true;
    case OP_GT: *out = vl_bool(a.u.i > b.u.i); return true;
    case OP_GE: *out = vl_bool(a.u.i >= b.u.i); return true;
    default: break;
    }
  } else if (da && b.t == VL_INT) {
    switch (op) {
    case OP_MUL: *out = vl_dur(a.u.i * b.u.i); return true;
    case OP_DIV:
      if (b.u.i == 0) vl_throwf("RangeError", "division by zero");
      *out = vl_dur(a.u.i / b.u.i);
      return true;
    default: break;
    }
  } else if (db && a.t == VL_INT && op == OP_MUL) {
    *out = vl_dur(a.u.i * b.u.i);
    return true;
  }
  vl_fault("invalid duration operation");
  return false;
}

/* ---------------------------------------------------------------- generic */

static Value arith(int op, Value a, Value b);

/* sz_code maps a sized integer type name to its VL_SZ_* code, 0 if none. */
static int sz_code(const char *n) {
  if (!n) return 0;
  if (strcmp(n, "i8") == 0) return VL_SZ_I8;
  if (strcmp(n, "i16") == 0) return VL_SZ_I16;
  if (strcmp(n, "i32") == 0) return VL_SZ_I32;
  if (strcmp(n, "u8") == 0 || strcmp(n, "byte") == 0) return VL_SZ_U8;
  if (strcmp(n, "u16") == 0) return VL_SZ_U16;
  if (strcmp(n, "u32") == 0) return VL_SZ_U32;
  if (strcmp(n, "u64") == 0) return VL_SZ_U64;
  return 0;
}

/* nt_arith: an operator with a Box or a newtype on either side.
 *
 * A Box is transparent: the operator applies to what it holds and the
 * result is a plain value. A newtype (§3.8) is a DISTINCT type: its
 * arithmetic and ordering are its underlying type's, between two values of
 * the SAME newtype only, and an arithmetic result is that newtype again —
 * re-wrapped through vl_conv, so `type Age u8` keeps its range. A newtype
 * mixed with any other type is refused; convert explicitly (`int(c)`). */
static Value nt_arith(int op, Value a, Value b) {
  a = vl_unbox(a);
  b = vl_unbox(b);
  bool na = vl_is_newtype(a), nb = vl_is_newtype(b);
  if (!na && !nb) return arith(op, a, b);
  if (!na || !nb || ((VlStruct *)a.u.o)->type != ((VlStruct *)b.u.o)->type)
    vl_fault("invalid operands: %s and %s (a named type mixes only with "
             "itself; convert explicitly)", vl_kind_name(a), vl_kind_name(b));
  const VlType *t = ((VlStruct *)a.u.o)->type;
  Value ia = ((VlStruct *)a.u.o)->f[0], ib = ((VlStruct *)b.u.o)->f[0];
  /* Over a SIZED integer the arithmetic traps exactly as that type's own
   * does (`type Age u8`: Age(200) + Age(100) is an OverflowError). */
  int sc = sz_code(t->ftypes ? t->ftypes[0] : "");
  int szop = op == OP_ADD ? 0 : op == OP_SUB ? 1 : op == OP_MUL ? 2
           : op == OP_POW ? 3 : op == OP_IDIV ? 4 : -1;
  Value r = (sc && szop >= 0) ? vl_arith_sz(szop, ia, ib, sc) : arith(op, ia, ib);
  /* Comparisons yield a bool, and `/` yields a float whatever the operands
   * (§5.1) — neither is the newtype again. */
  if (op >= OP_LT || op == OP_DIV) return r;
  Value out = vl_conv(t->name, r, NULL);
  vl_release(r);
  return out;
}

/* user_cmp orders two values of ONE user type by its own `cmp(self, other)
 * int` — the `Ord` protocol (§3.6): negative, zero or positive as `a` is
 * less than, equal to or greater than `b`. It applies to a struct (not a
 * newtype, which keeps its underlying type's order, §3.8) or an enum, the
 * values for which every ordering operator used to abort, so no working
 * program changes meaning and none pays a lookup. `*ok` is false when the
 * pair is not such a pair, or the type has no `cmp` with a receiver. */
static int user_cmp(Value a, Value b, bool *ok) {
  *ok = false;
  if (a.t != VL_OBJ || b.t != VL_OBJ) return 0;
  const char *ta = NULL, *tb = NULL;
  if (a.u.o->kind == O_STRUCT && !vl_is_newtype(a))
    ta = ((VlStruct *)a.u.o)->type->name;
  else if (a.u.o->kind == O_ENUM)
    ta = ((VlEnum *)a.u.o)->var->type;
  if (b.u.o->kind == O_STRUCT && !vl_is_newtype(b))
    tb = ((VlStruct *)b.u.o)->type->name;
  else if (b.u.o->kind == O_ENUM)
    tb = ((VlEnum *)b.u.o)->var->type;
  if (!ta || !tb || strcmp(ta, tb) != 0) return 0;
  const VlMethod *m = vl_find_method(ta, "cmp");
  if (!m || !m->has_self) return 0;
  Value args[2] = {a, b};
  vl_set_disp(m->type);
  Value r = m->fn(args, 2, NULL);
  Value d = vl_deref(r);
  if (d.t != VL_INT) vl_fault("%s.cmp must return an int, got %s", ta,
                              vl_kind_name(d));
  int c = d.u.i < 0 ? -1 : d.u.i > 0 ? 1 : 0;
  vl_release(r);
  *ok = true;
  return c;
}

static Value arith(int op, Value a, Value b) {
  if (op >= OP_LT && a.t == VL_OBJ && b.t == VL_OBJ) {
    /* An ordering between two values of a user type with a `cmp`. */
    bool ok = false;
    int c = user_cmp(vl_unbox(a), vl_unbox(b), &ok);
    if (ok) {
      switch (op) {
      case OP_LT: return vl_bool(c < 0);
      case OP_LE: return vl_bool(c <= 0);
      case OP_GT: return vl_bool(c > 0);
      default: return vl_bool(c >= 0);
      }
    }
  }
  if (a.t == VL_OBJ && b.t == VL_OBJ) {
    if (a.u.o->kind == O_BOX || b.u.o->kind == O_BOX ||
        vl_is_newtype(a) || vl_is_newtype(b))
      return nt_arith(op, a, b);
  } else if ((a.t == VL_OBJ && (a.u.o->kind == O_BOX || vl_is_newtype(a))) ||
             (b.t == VL_OBJ && (b.u.o->kind == O_BOX || vl_is_newtype(b)))) {
    return nt_arith(op, a, b);
  }
  a = derune(a);
  b = derune(b);

  Value d;
  if ((a.t == VL_DUR || b.t == VL_DUR) && dur_op(op, a, b, &d)) return d;

  if (is_dec(a) || is_dec(b)) {
    if (a.t == VL_FLOAT || b.t == VL_FLOAT)
      vl_throwf("ConvError",
                "implicit float->dec conversion is lossy; use dec(x) or a d-suffixed literal");
    if (!is_dec(a) && a.t != VL_INT) vl_fault("invalid operands: %s and dec", vl_kind_name(a));
    if (!is_dec(b) && b.t != VL_INT) vl_fault("invalid operands: dec and %s", vl_kind_name(b));
    Value da = is_dec(a) ? vl_retain(a) : vl_dec_from_int(a.u.i);
    Value db = is_dec(b) ? vl_retain(b) : vl_dec_from_int(b.u.i);
    Value out;
    switch (op) {
    case OP_ADD: out = vl_dec_add(da, db); break;
    case OP_SUB: out = vl_dec_sub(da, db); break;
    case OP_MUL: out = vl_dec_mul(da, db); break;
    case OP_DIV: out = vl_dec_div(da, db); break;
    case OP_IDIV: out = vl_dec_idiv(da, db); break;
    case OP_MOD: out = vl_dec_mod(da, db); break;
    case OP_POW: {
      int64_t e;
      if (!vl_dec_to_int(db, &e)) vl_throwf("RangeError", "dec exponent must be an integer");
      out = vl_dec_pow(da, e);
      break;
    }
    case OP_LT: out = vl_bool(vl_dec_cmp(da, db) < 0); break;
    case OP_LE: out = vl_bool(vl_dec_cmp(da, db) <= 0); break;
    case OP_GT: out = vl_bool(vl_dec_cmp(da, db) > 0); break;
    case OP_GE: out = vl_bool(vl_dec_cmp(da, db) >= 0); break;
    default: out = vl_nil(); break;
    }
    vl_release(da);
    vl_release(db);
    return out;
  }

  if (a.t == VL_INT && b.t == VL_INT) {
    switch (op) {
    case OP_ADD: return int_add(a.u.i, b.u.i);
    case OP_SUB: return int_sub(a.u.i, b.u.i);
    case OP_MUL: return int_mul(a.u.i, b.u.i);
    case OP_DIV: return vl_float(vl_idivf_ck(a.u.i, b.u.i));
    case OP_IDIV: return vl_int(floor_div(a.u.i, b.u.i));
    case OP_MOD: return vl_int(floor_mod(a.u.i, b.u.i));
    case OP_POW: return int_pow(a.u.i, b.u.i);
    case OP_LT: return vl_bool(a.u.i < b.u.i);
    case OP_LE: return vl_bool(a.u.i <= b.u.i);
    case OP_GT: return vl_bool(a.u.i > b.u.i);
    case OP_GE: return vl_bool(a.u.i >= b.u.i);
    }
  }

  if ((a.t == VL_FLOAT || a.t == VL_INT) && (b.t == VL_FLOAT || b.t == VL_INT)) {
    double x = a.t == VL_FLOAT ? a.u.f : int_to_float(a.u.i);
    double y = b.t == VL_FLOAT ? b.u.f : int_to_float(b.u.i);
    switch (op) {
    case OP_ADD: return vl_float(x + y);
    case OP_SUB: return vl_float(x - y);
    case OP_MUL: return vl_float(x * y);
    case OP_DIV: return vl_float(vl_fdiv_ck(x, y));
    case OP_IDIV: return vl_int(vl_ifloordiv_f(x, y));
    case OP_MOD: return vl_float(vl_ffloormod(x, y));
    case OP_POW: return vl_float(vl_fpow(x, y));
    case OP_LT: return vl_bool(x < y);
    case OP_LE: return vl_bool(x <= y);
    case OP_GT: return vl_bool(x > y);
    case OP_GE: return vl_bool(x >= y);
    }
  }

  if (is_str(a) && is_str(b)) {
    int c = strcmp(vl_cstr(a), vl_cstr(b));
    switch (op) {
    case OP_LT: return vl_bool(c < 0);
    case OP_LE: return vl_bool(c <= 0);
    case OP_GT: return vl_bool(c > 0);
    case OP_GE: return vl_bool(c >= 0);
    case OP_ADD:
      vl_fault("`+` never concatenates; use `||` (§5.1)");
      break;
    default: break;
    }
  }

  vl_fault("invalid operands: %s and %s", vl_kind_name(a), vl_kind_name(b));
  return vl_nil();
}

Value vl_add(Value a, Value b) { return arith(OP_ADD, a, b); }
Value vl_sub(Value a, Value b) { return arith(OP_SUB, a, b); }
Value vl_mul(Value a, Value b) { return arith(OP_MUL, a, b); }
Value vl_div(Value a, Value b) { return arith(OP_DIV, a, b); }
Value vl_idiv(Value a, Value b) { return arith(OP_IDIV, a, b); }
Value vl_mod(Value a, Value b) { return arith(OP_MOD, a, b); }
Value vl_pow(Value a, Value b) { return arith(OP_POW, a, b); }
Value vl_cmplt(Value a, Value b) { return arith(OP_LT, a, b); }
Value vl_cmple(Value a, Value b) { return arith(OP_LE, a, b); }
Value vl_cmpgt(Value a, Value b) { return arith(OP_GT, a, b); }
Value vl_cmpge(Value a, Value b) { return arith(OP_GE, a, b); }

/* The sized forms (voila.h). On two ints the inline _sz core decides, so
 * the boxed and native paths trap identically; any other pairing is the
 * ordinary operator, whose integer result is then range-checked. */
Value vl_arith_sz(int op, Value a, Value b, int t) {
  Value x = derune(vl_deref(a)), y = derune(vl_deref(b));
  if (x.t == VL_INT && y.t == VL_INT) {
    switch (op) {
    case 0: return vl_int(vl_iadd_sz(x.u.i, y.u.i, t));
    case 1: return vl_int(vl_isub_sz(x.u.i, y.u.i, t));
    case 2: return vl_int(vl_imul_sz(x.u.i, y.u.i, t));
    case 3: return vl_int(vl_ipow_sz(x.u.i, y.u.i, t));
    default: return vl_int(vl_idiv_sz(x.u.i, y.u.i, t));
    }
  }
  Value r;
  switch (op) {
  case 0: r = vl_add(a, b); break;
  case 1: r = vl_sub(a, b); break;
  case 2: r = vl_mul(a, b); break;
  case 3: r = vl_pow(a, b); break;
  default: r = vl_idiv(a, b); break;
  }
  if (r.t == VL_INT && (r.u.i < vl_sz_lo(t) || r.u.i > vl_sz_hi(t))) {
    static const char *const ops[] = {"+", "-", "*", "**", "~/"};
    vl_trap_szovf(ops[op >= 0 && op <= 4 ? op : 0],
                  x.t == VL_INT ? x.u.i : 0, y.t == VL_INT ? y.u.i : 0, t);
  }
  return r;
}

Value vl_neg_sz(Value a, int t) {
  Value x = derune(vl_deref(a));
  if (x.t == VL_INT) return vl_int(vl_ineg_sz(x.u.i, t));
  return vl_neg(a);
}

void vl_cksz(Value v, int t) {
  Value x = vl_deref(v);
  if (x.t == VL_INT) vl_cksz_i(x.u.i, t);
}

Value vl_cmpeq(Value a, Value b) { return vl_bool(vl_equal(a, b)); }
Value vl_cmpne(Value a, Value b) { return vl_bool(!vl_equal(a, b)); }

int vl_compare(Value a, Value b) {
  /* A user type's own `cmp` decides equality of ORDER too: two values it
   * calls equal sort as equal even when their fields differ. */
  bool ok = false;
  int c = user_cmp(vl_unbox(a), vl_unbox(b), &ok);
  if (ok) return c;
  if (vl_equal(a, b)) return 0;
  Value lt = arith(OP_LT, a, b);
  int r = lt.u.b ? -1 : 1;
  return r;
}

Value vl_cat(Value a, Value b) {
  Value sa = vl_tostr(a), sb = vl_tostr(b);
  int64_t na = vl_strlen(sa), nb = vl_strlen(sb);
  char *buf = (char *)vl_alloc((size_t)(na + nb + 1));
  memcpy(buf, vl_cstr(sa), (size_t)na);
  memcpy(buf + na, vl_cstr(sb), (size_t)nb);
  Value out = vl_str_n(buf, na + nb);
  free(buf);
  vl_release(sa);
  vl_release(sb);
  return out;
}

Value vl_str_concat(Value a, Value b) { return vl_cat(a, b); }

static int64_t want_i(Value v) {
  v = derune(vl_deref(v));
  if (v.t != VL_INT) vl_fault("expected int, got %s", vl_kind_name(v));
  return v.u.i;
}

/* Original trap order preserved: the count is inspected (and range-checked)
 * before the left operand is even looked at. */
Value vl_shl(Value a, Value b) {
  int64_t n = want_i(b);
  if (n < 0 || n > 63) vl_trap_shift(n);
  return vl_int((int64_t)((uint64_t)want_i(a) << n));
}

Value vl_shr(Value a, Value b) {
  int64_t n = want_i(b);
  if (n < 0 || n > 63) vl_trap_shift(n);
  return vl_int(want_i(a) >> n);
}

Value vl_band(Value a, Value b) { return vl_int(want_i(a) & want_i(b)); }
Value vl_bor(Value a, Value b) { return vl_int(want_i(a) | want_i(b)); }
Value vl_bxor(Value a, Value b) { return vl_int(want_i(a) ^ want_i(b)); }
Value vl_bnot(Value a) { return vl_int(~want_i(a)); }

Value vl_neg(Value a) {
  a = vl_unbox(a);
  if (vl_is_newtype(a)) {
    Value r = vl_neg(((VlStruct *)a.u.o)->f[0]);
    Value out = vl_conv(((VlStruct *)a.u.o)->type->name, r, NULL);
    vl_release(r);
    return out;
  }
  a = derune(a);
  if (a.t == VL_INT) return vl_int(vl_ineg_ck(a.u.i));
  if (a.t == VL_FLOAT) return vl_float(-a.u.f);
  if (is_dec(a)) return vl_dec_neg_v(a);
  if (a.t == VL_DUR) return vl_dur(-a.u.i);
  vl_fault("cannot negate %s", vl_kind_name(a));
  return vl_nil();
}

Value vl_not(Value a) {
  a = vl_deref(a);
  if (a.t != VL_BOOL) vl_fault("`not` requires bool, got %s", vl_kind_name(a));
  return vl_bool(!a.u.b);
}

bool vl_truthy(Value v) {
  if (v.t == VL_OBJ) v = vl_deref(v);
  if (v.t != VL_BOOL)
    vl_fault("condition must be bool, got %s (no implicit truthiness)", vl_kind_name(v));
  return v.u.b;
}
