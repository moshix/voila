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
  vl_abort("invalid duration operation");
  return false;
}

/* ---------------------------------------------------------------- generic */

static Value arith(int op, Value a, Value b) {
  a = derune(a);
  b = derune(b);

  Value d;
  if ((a.t == VL_DUR || b.t == VL_DUR) && dur_op(op, a, b, &d)) return d;

  if (is_dec(a) || is_dec(b)) {
    if (a.t == VL_FLOAT || b.t == VL_FLOAT)
      vl_throwf("ConvError",
                "implicit float->dec conversion is lossy; use dec(x) or a d-suffixed literal");
    if (!is_dec(a) && a.t != VL_INT) vl_abort("invalid operands: %s and dec", vl_kind_name(a));
    if (!is_dec(b) && b.t != VL_INT) vl_abort("invalid operands: dec and %s", vl_kind_name(b));
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
      vl_abort("`+` never concatenates; use `||` (§5.1)");
      break;
    default: break;
    }
  }

  vl_abort("invalid operands: %s and %s", vl_kind_name(a), vl_kind_name(b));
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

Value vl_cmpeq(Value a, Value b) { return vl_bool(vl_equal(a, b)); }
Value vl_cmpne(Value a, Value b) { return vl_bool(!vl_equal(a, b)); }

int vl_compare(Value a, Value b) {
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
  v = derune(v);
  if (v.t != VL_INT) vl_abort("expected int, got %s", vl_kind_name(v));
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
  a = derune(a);
  if (a.t == VL_INT) return vl_int(vl_ineg_ck(a.u.i));
  if (a.t == VL_FLOAT) return vl_float(-a.u.f);
  if (is_dec(a)) return vl_dec_neg_v(a);
  if (a.t == VL_DUR) return vl_dur(-a.u.i);
  vl_abort("cannot negate %s", vl_kind_name(a));
  return vl_nil();
}

Value vl_not(Value a) {
  if (a.t != VL_BOOL) vl_abort("`not` requires bool, got %s", vl_kind_name(a));
  return vl_bool(!a.u.b);
}

bool vl_truthy(Value v) {
  if (v.t != VL_BOOL)
    vl_abort("condition must be bool, got %s (no implicit truthiness)", vl_kind_name(v));
  return v.u.b;
}
