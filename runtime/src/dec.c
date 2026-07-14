/* dec.c — exact decimal arithmetic (spec §3.1, §3.2). The coefficient is a
 * decimal digit string, so rounding is exactly what the specification says:
 * half-even, on a digit boundary, under the `numeric digits` context. */
#include "voila_int.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static Value decv(VlDec *d) {
  Value v;
  v.t = VL_OBJ;
  v.u.o = &d->hdr;
  return v;
}

static VlDec *dec_alloc(int nd) {
  if (nd < 1) nd = 1;
  VlDec *d = (VlDec *)vl_obj_new(O_DEC, sizeof(VlDec) + (size_t)nd);
  d->nd = nd;
  return d;
}

static VlDec *D(Value v) { return (VlDec *)v.u.o; }

static bool dec_is_zero(const VlDec *d) {
  for (int i = 0; i < d->nd; i++)
    if (d->d[i] != '0') return false;
  return true;
}

/* make builds a decimal from digits (most significant first), stripping
 * leading zeros. */
static Value make(int neg, const char *digits, int nd, int exp) {
  int start = 0;
  while (start < nd - 1 && digits[start] == '0') start++;
  int n = nd - start;
  VlDec *d = dec_alloc(n);
  memcpy(d->d, digits + start, (size_t)n);
  d->nd = n;
  d->exp = exp;
  d->neg = (int8_t)neg;
  if (dec_is_zero(d)) {
    d->neg = 0;
    d->d[0] = '0';
    d->nd = 1;
    d->exp = 0;
  }
  return decv(d);
}

Value vl_dec_from_int(int64_t n) {
  char buf[24];
  int neg = n < 0;
  unsigned long long m = neg ? (unsigned long long)(-(n + 1)) + 1ULL : (unsigned long long)n;
  int i = (int)sizeof(buf);
  buf[--i] = 0;
  if (m == 0) buf[--i] = '0';
  while (m) {
    buf[--i] = (char)('0' + (int)(m % 10));
    m /= 10;
  }
  return make(neg, buf + i, (int)(sizeof(buf) - 1 - (size_t)i), 0);
}

/* ---------------------------------------------------------------- parse */

static bool parse_into(const char *s, int *neg, char **digits, int *nd, int *exp) {
  while (*s == ' ' || *s == '\t') s++;
  *neg = 0;
  if (*s == '+') s++;
  else if (*s == '-') { *neg = 1; s++; }

  const char *mant = s;
  const char *e = NULL;
  for (const char *p = s; *p; p++)
    if (*p == 'e' || *p == 'E') { e = p; break; }
  const char *mend = e ? e : s + strlen(s);

  int expPart = 0;
  if (e) {
    char *end;
    long v = strtol(e + 1, &end, 10);
    if (end == e + 1) return false;
    expPart = (int)v;
  }

  char *buf = (char *)vl_alloc((size_t)(mend - mant) + 2);
  int n = 0, frac = 0;
  bool seen_dot = false, seen_digit = false;
  for (const char *p = mant; p < mend; p++) {
    if (*p == '.') {
      if (seen_dot) { free(buf); return false; }
      seen_dot = true;
      continue;
    }
    if (*p == '_') continue;
    if (!isdigit((unsigned char)*p)) { free(buf); return false; }
    seen_digit = true;
    buf[n++] = *p;
    if (seen_dot) frac++;
  }
  if (!seen_digit) { free(buf); return false; }
  buf[n] = 0;
  *digits = buf;
  *nd = n;
  *exp = expPart - frac;
  return true;
}

Value vl_dec_parse(const char *s) {
  int neg, nd, exp;
  char *digits;
  if (!parse_into(s, &neg, &digits, &nd, &exp)) {
    Value sv = vl_str(s);
    vl_throw_conv("str", "dec", sv);
  }
  Value v = make(neg, digits, nd, exp);
  free(digits);
  return v;
}

Value vl_dec_from_float(double f) {
  if (f != f || f > 1.7e308 || f < -1.7e308)
    vl_throwf("ConvError", "cannot convert %g to dec", f);
  char *s = vl_fmt_float(f);
  int neg, nd, exp;
  char *digits;
  if (!parse_into(s, &neg, &digits, &nd, &exp)) {
    free(s);
    vl_throwf("ConvError", "cannot convert %g to dec", f);
  }
  Value v = make(neg, digits, nd, exp);
  free(digits);
  free(s);
  return v;
}

/* ---------------------------------------------------------------- digits */

static int cmp_mag(const char *a, int na, const char *b, int nb) {
  /* strip leading zeros */
  while (na > 1 && *a == '0') { a++; na--; }
  while (nb > 1 && *b == '0') { b++; nb--; }
  if (na != nb) return na < nb ? -1 : 1;
  int c = memcmp(a, b, (size_t)na);
  return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

/* add_mag: out must hold max(na,nb)+1 digits. Returns length. */
static int add_mag(const char *a, int na, const char *b, int nb, char *out) {
  int n = (na > nb ? na : nb) + 1;
  int carry = 0, k = n - 1;
  for (int i = 0; i < n; i++) {
    int da = i < na ? a[na - 1 - i] - '0' : 0;
    int db = i < nb ? b[nb - 1 - i] - '0' : 0;
    int s = da + db + carry;
    out[k--] = (char)('0' + s % 10);
    carry = s / 10;
  }
  return n;
}

/* sub_mag: a >= b. out holds na digits. */
static int sub_mag(const char *a, int na, const char *b, int nb, char *out) {
  int borrow = 0, k = na - 1;
  for (int i = 0; i < na; i++) {
    int da = a[na - 1 - i] - '0';
    int db = i < nb ? b[nb - 1 - i] - '0' : 0;
    int s = da - db - borrow;
    if (s < 0) { s += 10; borrow = 1; } else borrow = 0;
    out[k--] = (char)('0' + s);
  }
  return na;
}

/* mul_mag: out holds na+nb digits. */
static int mul_mag(const char *a, int na, const char *b, int nb, char *out) {
  int n = na + nb;
  int *acc = (int *)vl_alloc(sizeof(int) * (size_t)n);
  for (int i = na - 1; i >= 0; i--) {
    int da = a[i] - '0';
    if (!da) continue;
    for (int j = nb - 1; j >= 0; j--)
      acc[i + j + 1] += da * (b[j] - '0');
  }
  int carry = 0;
  for (int i = n - 1; i >= 0; i--) {
    int s = acc[i] + carry;
    acc[i] = s % 10;
    carry = s / 10;
  }
  for (int i = 0; i < n; i++) out[i] = (char)('0' + acc[i]);
  free(acc);
  return n;
}

/* divmod_mag: long division. q must hold na digits, r must hold nb+1. */
static void divmod_mag(const char *a, int na, const char *b, int nb, char *q,
                       int *nq, char *r, int *nr) {
  char *rem = (char *)vl_alloc((size_t)(na + nb + 2));
  int rn = 0;
  for (int i = 0; i < na; i++) {
    /* rem = rem*10 + a[i] */
    if (!(rn == 1 && rem[0] == '0') && rn > 0) {
      rem[rn++] = a[i];
    } else {
      rem[0] = a[i];
      rn = 1;
    }
    /* strip leading zeros of rem */
    int s = 0;
    while (s < rn - 1 && rem[s] == '0') s++;
    if (s) { memmove(rem, rem + s, (size_t)(rn - s)); rn -= s; }

    int digit = 0;
    while (cmp_mag(rem, rn, b, nb) >= 0) {
      char *tmp = (char *)vl_alloc((size_t)rn + 1);
      int tn = sub_mag(rem, rn, b, nb, tmp);
      int t0 = 0;
      while (t0 < tn - 1 && tmp[t0] == '0') t0++;
      memcpy(rem, tmp + t0, (size_t)(tn - t0));
      rn = tn - t0;
      free(tmp);
      digit++;
    }
    q[i] = (char)('0' + digit);
  }
  *nq = na;
  memcpy(r, rem, (size_t)rn);
  *nr = rn;
  free(rem);
}

/* ---------------------------------------------------------------- rounding */

/* reduce_exp rounds d so its exponent becomes target (>= d->exp), half-even. */
static Value reduce_exp(Value dv, int target) {
  VlDec *d = D(dv);
  int drop = target - d->exp;
  if (drop <= 0) return vl_retain(dv);
  if (drop >= d->nd) {
    /* everything is dropped; the rounded result is 0 or 1 ulp */
    char first = d->d[0];
    bool round_up = false;
    if (drop == d->nd && first > '5') round_up = true;
    else if (drop == d->nd && first == '5') {
      bool rest_nonzero = false;
      for (int i = 1; i < d->nd; i++)
        if (d->d[i] != '0') rest_nonzero = true;
      round_up = rest_nonzero; /* half-even: 0 is even, so ties go down */
    }
    char one[1] = {'1'}, zero[1] = {'0'};
    return make(d->neg, round_up ? one : zero, 1, target);
  }
  int keep = d->nd - drop;
  char *digits = (char *)vl_alloc((size_t)keep + 2);
  memcpy(digits, d->d, (size_t)keep);
  digits[keep] = 0;

  char first = d->d[keep];
  bool rest_nonzero = false;
  for (int i = keep + 1; i < d->nd; i++)
    if (d->d[i] != '0') rest_nonzero = true;

  bool up = false;
  if (first > '5') up = true;
  else if (first == '5') {
    if (rest_nonzero) up = true;
    else up = ((digits[keep - 1] - '0') % 2) != 0; /* half-even */
  }
  if (up) {
    int i = keep - 1;
    while (i >= 0) {
      if (digits[i] < '9') { digits[i]++; break; }
      digits[i] = '0';
      i--;
    }
    if (i < 0) {
      memmove(digits + 1, digits, (size_t)keep);
      digits[0] = '1';
      keep++;
    }
  }
  Value out = make(d->neg, digits, keep, target);
  free(digits);
  return out;
}

/* round_to limits d to `digits` significant digits. */
static Value round_to(Value dv, int digits) {
  VlDec *d = D(dv);
  if (digits <= 0) digits = 28;
  if (d->nd <= digits) return vl_retain(dv);
  return reduce_exp(dv, d->exp + (d->nd - digits));
}

/* normalize strips trailing zeros while the exponent is negative. */
static Value normalize(Value dv) {
  VlDec *d = D(dv);
  if (dec_is_zero(d)) return vl_retain(dv);
  int nd = d->nd, exp = d->exp;
  while (exp < 0 && nd > 1 && d->d[nd - 1] == '0') {
    nd--;
    exp++;
  }
  if (nd == d->nd && exp == d->exp) return vl_retain(dv);
  return make(d->neg, d->d, nd, exp);
}

/* ---------------------------------------------------------------- align */

/* scale returns a copy of d with the coefficient multiplied by 10^k. */
static Value scale(Value dv, int k) {
  VlDec *d = D(dv);
  if (k <= 0) return vl_retain(dv);
  char *digits = (char *)vl_alloc((size_t)(d->nd + k) + 1);
  memcpy(digits, d->d, (size_t)d->nd);
  memset(digits + d->nd, '0', (size_t)k);
  Value out = make(d->neg, digits, d->nd + k, d->exp - k);
  free(digits);
  return out;
}

/* ---------------------------------------------------------------- add/sub */

static Value add_signed(Value av, Value bv, int bneg) {
  VlDec *a = D(av), *b = D(bv);
  int exp = a->exp < b->exp ? a->exp : b->exp;
  Value as = scale(av, a->exp - exp);
  Value bs = scale(bv, b->exp - exp);
  VlDec *x = D(as), *y = D(bs);
  int yneg = bneg ? !y->neg : y->neg;

  Value out;
  if (x->neg == yneg) {
    char *buf = (char *)vl_alloc((size_t)(x->nd > y->nd ? x->nd : y->nd) + 2);
    int n = add_mag(x->d, x->nd, y->d, y->nd, buf);
    out = make(x->neg, buf, n, exp);
    free(buf);
  } else {
    int c = cmp_mag(x->d, x->nd, y->d, y->nd);
    if (c == 0) {
      out = vl_dec_from_int(0);
    } else if (c > 0) {
      char *buf = (char *)vl_alloc((size_t)x->nd + 1);
      int n = sub_mag(x->d, x->nd, y->d, y->nd, buf);
      out = make(x->neg, buf, n, exp);
      free(buf);
    } else {
      char *buf = (char *)vl_alloc((size_t)y->nd + 1);
      int n = sub_mag(y->d, y->nd, x->d, x->nd, buf);
      out = make(yneg, buf, n, exp);
      free(buf);
    }
  }
  vl_release(as);
  vl_release(bs);
  Value r = round_to(out, vl_get_digits());
  vl_release(out);
  return r;
}

Value vl_dec_add(Value a, Value b) { return add_signed(a, b, 0); }
Value vl_dec_sub(Value a, Value b) { return add_signed(a, b, 1); }

Value vl_dec_mul(Value a, Value b) {
  VlDec *x = D(a), *y = D(b);
  char *buf = (char *)vl_alloc((size_t)(x->nd + y->nd) + 1);
  int n = mul_mag(x->d, x->nd, y->d, y->nd, buf);
  Value out = make(x->neg != y->neg, buf, n, x->exp + y->exp);
  free(buf);
  Value r = round_to(out, vl_get_digits());
  vl_release(out);
  return r;
}

Value vl_dec_div(Value a, Value b) {
  VlDec *x = D(a), *y = D(b);
  if (dec_is_zero(y)) vl_throwf("RangeError", "division by zero");
  if (dec_is_zero(x)) return vl_dec_from_int(0);
  int digits = vl_get_digits();
  if (digits <= 0) digits = 28;

  int shift = digits + y->nd - x->nd + 1;
  Value num = shift > 0 ? scale(a, shift) : vl_retain(a);
  VlDec *nu = D(num);
  int exp = x->exp - y->exp - (shift > 0 ? shift : 0);

  char *q = (char *)vl_alloc((size_t)nu->nd + 2);
  char *r = (char *)vl_alloc((size_t)y->nd + 2);
  int nq, nr;
  divmod_mag(nu->d, nu->nd, y->d, y->nd, q, &nq, r, &nr);

  /* half-even on the remainder */
  char *twice = (char *)vl_alloc((size_t)nr + 2);
  int n2 = add_mag(r, nr, r, nr, twice);
  int c = cmp_mag(twice, n2, y->d, y->nd);
  bool up = c > 0 || (c == 0 && ((q[nq - 1] - '0') % 2) != 0);
  if (up) {
    int i = nq - 1;
    while (i >= 0) {
      if (q[i] < '9') { q[i]++; break; }
      q[i] = '0';
      i--;
    }
    if (i < 0) {
      memmove(q + 1, q, (size_t)nq);
      q[0] = '1';
      nq++;
    }
  }
  Value out = make(x->neg != y->neg, q, nq, exp);
  free(q);
  free(r);
  free(twice);
  vl_release(num);

  Value rounded = round_to(out, digits);
  vl_release(out);
  Value norm = normalize(rounded);
  vl_release(rounded);
  return norm;
}

Value vl_dec_idiv(Value a, Value b) {
  VlDec *x = D(a), *y = D(b);
  if (dec_is_zero(y)) vl_throwf("RangeError", "division by zero");
  int exp = x->exp < y->exp ? x->exp : y->exp;
  Value as = scale(a, x->exp - exp);
  Value bs = scale(b, y->exp - exp);
  VlDec *p = D(as), *q2 = D(bs);

  char *q = (char *)vl_alloc((size_t)p->nd + 2);
  char *r = (char *)vl_alloc((size_t)q2->nd + 2);
  int nq, nr;
  divmod_mag(p->d, p->nd, q2->d, q2->nd, q, &nq, r, &nr);
  bool neg = p->neg != q2->neg;
  bool rem_nonzero = !(nr == 1 && r[0] == '0');

  Value out = make(neg, q, nq, 0);
  if (neg && rem_nonzero) {
    /* floor: round away from zero for negative quotients */
    Value one = vl_dec_from_int(1);
    Value adj = add_signed(out, one, 1); /* out - 1 (out is negative) */
    vl_release(one);
    vl_release(out);
    out = adj;
  }
  free(q);
  free(r);
  vl_release(as);
  vl_release(bs);
  return out;
}

Value vl_dec_mod(Value a, Value b) {
  Value q = vl_dec_idiv(a, b);
  Value p = vl_dec_mul(q, b);
  Value m = vl_dec_sub(a, p);
  vl_release(q);
  vl_release(p);
  return m;
}

Value vl_dec_pow(Value a, int64_t n) {
  bool invert = n < 0;
  if (invert) n = -n;
  Value result = vl_dec_from_int(1);
  Value base = vl_retain(a);
  while (n > 0) {
    if (n & 1) {
      Value t = vl_dec_mul(result, base);
      vl_release(result);
      result = t;
    }
    n >>= 1;
    if (n) {
      Value t = vl_dec_mul(base, base);
      vl_release(base);
      base = t;
    }
  }
  vl_release(base);
  if (invert) {
    Value one = vl_dec_from_int(1);
    Value inv = vl_dec_div(one, result);
    vl_release(one);
    vl_release(result);
    return inv;
  }
  return result;
}

int vl_dec_cmp(Value av, Value bv) {
  VlDec *a = D(av), *b = D(bv);
  bool za = dec_is_zero(a), zb = dec_is_zero(b);
  if (za && zb) return 0;
  int sa = za ? 0 : (a->neg ? -1 : 1);
  int sb = zb ? 0 : (b->neg ? -1 : 1);
  if (sa != sb) return sa < sb ? -1 : 1;
  if (sa == 0) return 0;
  int exp = a->exp < b->exp ? a->exp : b->exp;
  Value as = scale(av, a->exp - exp);
  Value bs = scale(bv, b->exp - exp);
  int c = cmp_mag(D(as)->d, D(as)->nd, D(bs)->d, D(bs)->nd);
  vl_release(as);
  vl_release(bs);
  if (a->neg) c = -c;
  return c;
}

/* ---------------------------------------------------------------- shape */

Value vl_dec_abs(Value a) {
  VlDec *d = D(a);
  if (!d->neg) return vl_retain(a);
  return make(0, d->d, d->nd, d->exp);
}

Value vl_dec_neg_v(Value a) {
  VlDec *d = D(a);
  if (dec_is_zero(d)) return vl_retain(a);
  return make(!d->neg, d->d, d->nd, d->exp);
}

Value vl_dec_round(Value a, int places) {
  VlDec *d = D(a);
  if (d->exp >= -places) return vl_retain(a);
  return reduce_exp(a, -places);
}

Value vl_dec_trunc(Value a) {
  VlDec *d = D(a);
  if (d->exp >= 0) return vl_retain(a);
  int drop = -d->exp;
  if (drop >= d->nd) return vl_dec_from_int(0);
  return make(d->neg, d->d, d->nd - drop, 0);
}

Value vl_dec_floor(Value a) {
  VlDec *d = D(a);
  Value t = vl_dec_trunc(a);
  if (!d->neg) return t;
  /* negative and not integral → subtract one */
  Value back = vl_dec_sub(a, t);
  bool exact = dec_is_zero(D(back));
  vl_release(back);
  if (exact) return t;
  Value one = vl_dec_from_int(1);
  Value f = vl_dec_sub(t, one);
  vl_release(one);
  vl_release(t);
  return f;
}

Value vl_dec_ceil(Value a) {
  Value n = vl_dec_neg_v(a);
  Value f = vl_dec_floor(n);
  Value c = vl_dec_neg_v(f);
  vl_release(n);
  vl_release(f);
  return c;
}

bool vl_dec_is_int(Value a) {
  VlDec *d = D(a);
  if (d->exp >= 0) return true;
  int drop = -d->exp;
  if (drop >= d->nd) return dec_is_zero(d);
  for (int i = d->nd - drop; i < d->nd; i++)
    if (d->d[i] != '0') return false;
  return true;
}

bool vl_dec_to_int(Value a, int64_t *out) {
  if (!vl_dec_is_int(a)) return false;
  Value t = vl_dec_trunc(a);
  VlDec *d = D(t);
  long double acc = 0;
  for (int i = 0; i < d->nd; i++) acc = acc * 10 + (d->d[i] - '0');
  for (int i = 0; i < d->exp; i++) acc *= 10;
  if (acc > 9.2233720368547758e18L) { vl_release(t); return false; }
  int64_t v = (int64_t)acc;
  if (d->neg) v = -v;
  *out = v;
  vl_release(t);
  return true;
}

double vl_dec_to_float(Value a) {
  char *s = vl_dec_str(a);
  double f = strtod(s, NULL);
  free(s);
  return f;
}

/* ---------------------------------------------------------------- text */

char *vl_dec_str(Value av) {
  Value nv = normalize(av);
  VlDec *d = D(nv);
  int adj = d->exp + d->nd - 1;
  char *out = (char *)vl_alloc((size_t)(d->nd + 40));
  char *o = out;
  if (d->neg) *o++ = '-';

  if (d->exp <= 0 && adj >= -6) {
    int point = d->nd + d->exp;
    if (d->exp == 0) {
      memcpy(o, d->d, (size_t)d->nd);
      o += d->nd;
    } else if (point > 0) {
      memcpy(o, d->d, (size_t)point);
      o += point;
      *o++ = '.';
      memcpy(o, d->d + point, (size_t)(d->nd - point));
      o += d->nd - point;
    } else {
      *o++ = '0';
      *o++ = '.';
      for (int i = 0; i < -point; i++) *o++ = '0';
      memcpy(o, d->d, (size_t)d->nd);
      o += d->nd;
    }
    *o = 0;
    vl_release(nv);
    return out;
  }
  if (d->exp > 0 && adj <= 33) {
    memcpy(o, d->d, (size_t)d->nd);
    o += d->nd;
    for (int i = 0; i < d->exp; i++) *o++ = '0';
    *o = 0;
    vl_release(nv);
    return out;
  }
  /* scientific */
  *o++ = d->d[0];
  if (d->nd > 1) {
    *o++ = '.';
    memcpy(o, d->d + 1, (size_t)(d->nd - 1));
    o += d->nd - 1;
  }
  snprintf(o, 24, "E%c%d", adj < 0 ? '-' : '+', adj < 0 ? -adj : adj);
  vl_release(nv);
  return out;
}

char *vl_dec_fixed(Value av, int places) {
  Value r = vl_dec_round(av, places);
  VlDec *d = D(r);
  /* scale so that exp == -places exactly */
  Value s = d->exp > -places ? scale(r, d->exp + places) : vl_retain(r);
  VlDec *x = D(s);

  int nd = x->nd;
  const char *digits = x->d;
  char *out = (char *)vl_alloc((size_t)(nd + places + 8));
  char *o = out;
  bool zero = dec_is_zero(x);
  if (x->neg && !zero) *o++ = '-';
  if (places == 0) {
    memcpy(o, digits, (size_t)nd);
    o += nd;
    *o = 0;
    vl_release(r);
    vl_release(s);
    return out;
  }
  char *padded = (char *)vl_alloc((size_t)(nd + places + 2));
  int pn = 0;
  while (pn + nd <= places) padded[pn++] = '0';
  memcpy(padded + pn, digits, (size_t)nd);
  pn += nd;
  int point = pn - places;
  memcpy(o, padded, (size_t)point);
  o += point;
  *o++ = '.';
  memcpy(o, padded + point, (size_t)places);
  o += places;
  *o = 0;
  free(padded);
  vl_release(r);
  vl_release(s);
  return out;
}
