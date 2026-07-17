/* str.c — std/str (§13.2). Indices are 0-based byte offsets; word() and the
 * PARSE column templates keep REXX's 1-based positions. */
#include "voila_int.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* Sb decodes into a CALLER-SUPPLIED buffer. A shared static buffer aliased
 * when two rune arguments appeared in the same call. */
static const char *Sb(Value v, int64_t *n, char *buf) {
  if (v.t == VL_RUNE) {
    uint32_t r = v.u.r;
    int k = 0;
    if (r < 0x80) buf[k++] = (char)r;
    else if (r < 0x800) { buf[k++] = (char)(0xC0 | (r >> 6)); buf[k++] = (char)(0x80 | (r & 0x3F)); }
    else if (r < 0x10000) { buf[k++] = (char)(0xE0 | (r >> 12)); buf[k++] = (char)(0x80 | ((r >> 6) & 0x3F)); buf[k++] = (char)(0x80 | (r & 0x3F)); }
    else { buf[k++] = (char)(0xF0 | (r >> 18)); buf[k++] = (char)(0x80 | ((r >> 12) & 0x3F)); buf[k++] = (char)(0x80 | ((r >> 6) & 0x3F)); buf[k++] = (char)(0x80 | (r & 0x3F)); }
    buf[k] = 0;
    *n = k;
    return buf;
  }
  if (v.t != VL_OBJ || v.u.o->kind != O_STR)
    vl_abort("expected str, got %s", vl_kind_name(v));
  VlStr *s = (VlStr *)v.u.o;
  *n = s->len;
  return s->b;
}

/* S() keeps the old one-argument shape for the many single-subject calls; each
 * call site that decodes TWO values must use its own buffer via Sb(). */
#define S(v, n) Sb((v), (n), (char[8]){0})

static int64_t I(Value v) {
  if (v.t == VL_INT) return v.u.i;
  if (v.t == VL_RUNE) return (int64_t)v.u.r;
  vl_abort("expected int, got %s", vl_kind_name(v));
  return 0;
}

static bool is_space_c(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

/* rune_len_at is clamped by the bytes actually remaining: a truncated
 * multi-byte sequence at the end of a buffer must not be read past. */
static int rune_len_n(const unsigned char *p, int64_t remaining) {
  int n = 1;
  if (*p >= 0xF0) n = 4;
  else if (*p >= 0xE0) n = 3;
  else if (*p >= 0xC0) n = 2;
  if ((int64_t)n > remaining) n = 1;
  return n;
}

/* ---------------------------------------------------------------- helpers */

static Value split_str(const char *s, int64_t n, const char *sep, int64_t sn,
                       int64_t limit) {
  Value out = vl_slice_new(0);
  if (sn == 0) {
    for (int64_t i = 0; i < n;) {
      int len = rune_len_n((const unsigned char *)s + i, n - i);
      vl_slice_append(out, vl_str_n(s + i, len));
      i += len;
    }
    return out;
  }
  int64_t start = 0, parts = 0;
  for (int64_t i = 0; i + sn <= n;) {
    if ((limit > 0 && parts == limit - 1) || memcmp(s + i, sep, (size_t)sn) != 0) {
      i++;
      continue;
    }
    vl_slice_append(out, vl_str_n(s + start, i - start));
    parts++;
    i += sn;
    start = i;
  }
  vl_slice_append(out, vl_str_n(s + start, n - start));
  return out;
}

static Value fields_of(const char *s, int64_t n) {
  Value out = vl_slice_new(0);
  int64_t i = 0;
  while (i < n) {
    while (i < n && is_space_c(s[i])) i++;
    if (i >= n) break;
    int64_t start = i;
    while (i < n && !is_space_c(s[i])) i++;
    vl_slice_append(out, vl_str_n(s + start, i - start));
  }
  return out;
}

/* ---------------------------------------------------------------- api */

Value vl_str_call(const char *name, Value *argv, int argc, bool *ok) {
  *ok = true;

  /* These three take a slice as their subject, not a string. */
  if (eq(name, "join")) {
    Value parts = argv[0];
    Value sep = vl_tostr(argv[1]);
    VlSlice *p = (VlSlice *)parts.u.o;
    Value b = vl_builder_new();
    for (int64_t i = 0; i < p->len; i++) {
      if (i) vl_builder_write(b, sep);
      vl_builder_write(b, p->e[i]);
    }
    Value out = vl_builder_done(b);
    vl_release(b);
    vl_release(sep);
    return out;
  }
  if (eq(name, "from_bytes")) {
    VlSlice *sl = (VlSlice *)argv[0].u.o;
    char *b = (char *)vl_alloc((size_t)sl->len + 1);
    for (int64_t i = 0; i < sl->len; i++) b[i] = (char)I(sl->e[i]);
    return vl_str_take(b, sl->len);
  }
  if (eq(name, "from_runes")) {
    VlSlice *sl = (VlSlice *)argv[0].u.o;
    Value b = vl_builder_new();
    for (int64_t i = 0; i < sl->len; i++) {
      Value r = sl->e[i];
      if (r.t == VL_INT) r = vl_rune((uint32_t)r.u.i);
      int64_t rn;
      const char *rs = S(r, &rn);
      Value rv = vl_str_n(rs, rn);
      vl_builder_write(b, rv);
      vl_release(rv);
    }
    Value out = vl_builder_done(b);
    vl_release(b);
    return out;
  }

  int64_t n = 0;
  const char *s = argc > 0 ? S(argv[0], &n) : "";

  if (eq(name, "upper") || eq(name, "lower")) {
    bool up = eq(name, "upper");
    char *b = vl_strdup_n(s, n);
    for (int64_t i = 0; i < n; i++)
      b[i] = up ? (char)toupper((unsigned char)b[i]) : (char)tolower((unsigned char)b[i]);
    return vl_str_take(b, n);
  }
  if (eq(name, "title")) {
    Value words = fields_of(s, n);
    VlSlice *w = (VlSlice *)words.u.o;
    Value b = vl_builder_new();
    for (int64_t i = 0; i < w->len; i++) {
      if (i) {
        Value sp = vl_str(" ");
        vl_builder_write(b, sp);
        vl_release(sp);
      }
      int64_t wn;
      const char *ws = S(w->e[i], &wn);
      char *t = vl_strdup_n(ws, wn);
      for (int64_t k = 0; k < wn; k++) t[k] = (char)tolower((unsigned char)t[k]);
      if (wn) t[0] = (char)toupper((unsigned char)t[0]);
      Value tv = vl_str_n(t, wn);
      free(t);
      vl_builder_write(b, tv);
      vl_release(tv);
    }
    Value out = vl_builder_done(b);
    vl_release(b);
    vl_release(words);
    return out;
  }
  if (eq(name, "trim") || eq(name, "trim_left") || eq(name, "trim_right")) {
    int64_t a = 0, z = n;
    if (!eq(name, "trim_right"))
      while (a < z && is_space_c(s[a])) a++;
    if (!eq(name, "trim_left"))
      while (z > a && is_space_c(s[z - 1])) z--;
    return vl_str_n(s + a, z - a);
  }
  if (eq(name, "strip")) {
    int64_t cn;
    const char *cut = S(argv[1], &cn);
    int64_t a = 0, z = n;
    while (a < z && memchr(cut, s[a], (size_t)cn)) a++;
    while (z > a && memchr(cut, s[z - 1], (size_t)cn)) z--;
    return vl_str_n(s + a, z - a);
  }
  if (eq(name, "pad") || eq(name, "pad_left") || eq(name, "center")) {
    int64_t w = I(argv[1]);
    if (n >= w) {
      if (eq(name, "pad_left")) return vl_str_n(s + (n - w), w);
      return vl_str_n(s, w);
    }
    int64_t fill = w - n;
    char *b = (char *)vl_alloc((size_t)w + 1);
    if (eq(name, "pad")) {
      memcpy(b, s, (size_t)n);
      memset(b + n, ' ', (size_t)fill);
    } else if (eq(name, "pad_left")) {
      memset(b, ' ', (size_t)fill);
      memcpy(b + fill, s, (size_t)n);
    } else {
      int64_t l = fill / 2, r = fill - l;
      memset(b, ' ', (size_t)l);
      memcpy(b + l, s, (size_t)n);
      memset(b + l + n, ' ', (size_t)r);
    }
    return vl_str_take(b, w);
  }
  if (eq(name, "find_any")) {
    /* find_any(stops, from) -> index of the first byte of the subject at or
     * after `from` that appears in `stops`, or -1. This is the LEXER's
     * scanning primitive: a token scan that visited every character through
     * boxed method calls becomes one C loop with a 256-entry table. */
    if (argc < 2) vl_abort("find_any: missing stops argument");
    int64_t stn;
    const char *st = S(argv[1], &stn);
    int64_t from = argc > 2 ? I(argv[2]) : 0;
    if (from < 0) from = 0;
    bool stop[256] = {false};
    for (int64_t i = 0; i < stn; i++) stop[(unsigned char)st[i]] = true;
    for (int64_t i = from; i < n; i++)
      if (stop[(unsigned char)s[i]]) return vl_int(i);
    return vl_int(-1);
  }
  if (eq(name, "skip_any")) {
    /* skip_any(chars, from) -> index of the first byte of the subject at or
     * after `from` that is NOT in `chars`, or len. The complement of
     * find_any: identifiers, digit runs and whitespace skip in one C loop. */
    if (argc < 2) vl_abort("skip_any: missing chars argument");
    int64_t stn;
    const char *st = S(argv[1], &stn);
    int64_t from = argc > 2 ? I(argv[2]) : 0;
    if (from < 0) from = 0;
    bool in[256] = {false};
    for (int64_t i = 0; i < stn; i++) in[(unsigned char)st[i]] = true;
    for (int64_t i = from; i < n; i++)
      if (!in[(unsigned char)s[i]]) return vl_int(i);
    return vl_int(n);
  }
  if (eq(name, "contains")) {
    int64_t sn;
    const char *sub = S(argv[1], &sn);
    if (sn == 0) return vl_bool(true);
    for (int64_t i = 0; i + sn <= n; i++)
      if (memcmp(s + i, sub, (size_t)sn) == 0) return vl_bool(true);
    return vl_bool(false);
  }
  if (eq(name, "index") || eq(name, "last_index")) {
    int64_t sn;
    const char *sub = S(argv[1], &sn);
    int64_t found = -1;
    for (int64_t i = 0; i + sn <= n; i++) {
      if (memcmp(s + i, sub, (size_t)sn) == 0) {
        found = i;
        if (eq(name, "index")) break;
      }
    }
    return vl_int(found);
  }
  if (eq(name, "starts_with") || eq(name, "ends_with")) {
    int64_t pn;
    const char *p = S(argv[1], &pn);
    if (pn > n) return vl_bool(false);
    if (eq(name, "starts_with")) return vl_bool(memcmp(s, p, (size_t)pn) == 0);
    return vl_bool(memcmp(s + n - pn, p, (size_t)pn) == 0);
  }
  if (eq(name, "count")) {
    int64_t sn;
    const char *sub = S(argv[1], &sn);
    if (sn == 0) return vl_int(n + 1);
    int64_t c = 0;
    for (int64_t i = 0; i + sn <= n;) {
      if (memcmp(s + i, sub, (size_t)sn) == 0) { c++; i += sn; }
      else i++;
    }
    return vl_int(c);
  }
  if (eq(name, "equal_fold")) {
    int64_t bn;
    const char *b = S(argv[1], &bn);
    if (n != bn) return vl_bool(false);
    for (int64_t i = 0; i < n; i++)
      if (tolower((unsigned char)s[i]) != tolower((unsigned char)b[i]))
        return vl_bool(false);
    return vl_bool(true);
  }
  if (eq(name, "substr")) {
    int64_t start = I(argv[1]);
    if (start < 0) start = 0;
    if (start > n) start = n;
    int64_t end = n;
    if (argc > 2) {
      int64_t k = I(argv[2]);
      if (k < 0) k = 0;
      if (start + k < end) end = start + k;
    }
    return vl_str_n(s + start, end - start);
  }
  if (eq(name, "left") || eq(name, "right")) {
    int64_t k = I(argv[1]);
    if (k < 0) k = 0;
    if (k > n) k = n;
    if (eq(name, "left")) return vl_str_n(s, k);
    return vl_str_n(s + n - k, k);
  }
  if (eq(name, "split")) {
    int64_t sn;
    const char *sep = S(argv[1], &sn);
    return split_str(s, n, sep, sn, 0);
  }
  if (eq(name, "split_n")) {
    int64_t sn;
    const char *sep = S(argv[1], &sn);
    return split_str(s, n, sep, sn, I(argv[2]));
  }
  if (eq(name, "lines")) {
    int64_t m = n;
    if (m > 0 && s[m - 1] == '\n') m--;
    if (m == 0) return vl_slice_new(0);
    return split_str(s, m, "\n", 1, 0);
  }
  if (eq(name, "fields")) return fields_of(s, n);
  if (eq(name, "words")) {
    Value f = fields_of(s, n);
    int64_t c = ((VlSlice *)f.u.o)->len;
    vl_release(f);
    return vl_int(c);
  }
  if (eq(name, "word")) {
    Value f = fields_of(s, n);
    VlSlice *w = (VlSlice *)f.u.o;
    int64_t k = I(argv[1]); /* 1-based, REXX */
    Value out = (k >= 1 && k <= w->len) ? vl_retain(w->e[k - 1]) : vl_str("");
    vl_release(f);
    return out;
  }
  if (eq(name, "subword") || eq(name, "delword")) {
    Value f = fields_of(s, n);
    VlSlice *w = (VlSlice *)f.u.o;
    int64_t k = I(argv[1]);
    if (k < 1) k = 1;
    int64_t cnt = argc > 2 ? I(argv[2]) : w->len;
    Value b = vl_builder_new();
    bool first = true;
    for (int64_t i = 0; i < w->len; i++) {
      bool in_range = (i + 1 >= k && i + 1 < k + cnt);
      bool take = eq(name, "subword") ? in_range : !in_range;
      if (!take) continue;
      if (!first) {
        Value sp = vl_str(" ");
        vl_builder_write(b, sp);
        vl_release(sp);
      }
      first = false;
      vl_builder_write(b, w->e[i]);
    }
    Value out = vl_builder_done(b);
    vl_release(b);
    vl_release(f);
    return out;
  }
  if (eq(name, "join")) {
    /* join(parts, sep) — the subject is a slice, not a string */
    Value parts = argv[0];
    Value sep = vl_tostr(argv[1]);
    VlSlice *p = (VlSlice *)parts.u.o;
    Value b = vl_builder_new();
    for (int64_t i = 0; i < p->len; i++) {
      if (i) vl_builder_write(b, sep);
      vl_builder_write(b, p->e[i]);
    }
    Value out = vl_builder_done(b);
    vl_release(b);
    vl_release(sep);
    return out;
  }
  if (eq(name, "replace") || eq(name, "replace_n")) {
    int64_t on, nn;
    const char *old = S(argv[1], &on);
    const char *nw = S(argv[2], &nn);
    int64_t limit = eq(name, "replace_n") ? I(argv[3]) : -1;
    Value b = vl_builder_new();
    int64_t i = 0, done = 0;
    while (i < n) {
      if (on > 0 && i + on <= n && memcmp(s + i, old, (size_t)on) == 0 &&
          (limit < 0 || done < limit)) {
        Value nv = vl_str_n(nw, nn);
        vl_builder_write(b, nv);
        vl_release(nv);
        i += on;
        done++;
      } else {
        Value cv = vl_str_n(s + i, 1);
        vl_builder_write(b, cv);
        vl_release(cv);
        i++;
      }
    }
    Value out = vl_builder_done(b);
    vl_release(b);
    return out;
  }
  if (eq(name, "repeat")) {
    int64_t k = I(argv[1]);
    if (k < 0) k = 0;
    if (n > 0 && k > (int64_t)1 << 31 / (n > 0 ? n : 1))
      vl_throwf("RangeError", "repeat: result too large");
    char *b = (char *)vl_alloc((size_t)(n * k) + 1);
    for (int64_t i = 0; i < k; i++) memcpy(b + i * n, s, (size_t)n);
    return vl_str_take(b, n * k);
  }
  if (eq(name, "reverse")) {
    char *b = (char *)vl_alloc((size_t)n + 1);
    int64_t o = n;
    for (int64_t i = 0; i < n;) {
      int len = rune_len_n((const unsigned char *)s + i, n - i);
      o -= len;
      memcpy(b + o, s + i, (size_t)len);
      i += len;
    }
    return vl_str_take(b, n);
  }
  if (eq(name, "translate")) {
    int64_t fn2, tn;
    const char *from = S(argv[1], &fn2);
    const char *to = S(argv[2], &tn);
    char *b = (char *)vl_alloc((size_t)n + 1);
    int64_t o = 0;
    for (int64_t i = 0; i < n; i++) {
      const char *hit = (const char *)memchr(from, s[i], (size_t)fn2);
      if (!hit) { b[o++] = s[i]; continue; }
      int64_t k = hit - from;
      if (k < tn) b[o++] = to[k]; /* beyond `to` → deleted */
    }
    return vl_str_take(b, o);
  }
  if (eq(name, "overlay")) {
    int64_t nn;
    const char *nw = S(argv[1], &nn);
    int64_t at = I(argv[2]);
    if (at < 0) at = 0;
    int64_t base = n > at ? n : at;
    int64_t total = base > at + nn ? base : at + nn;
    char *b = (char *)vl_alloc((size_t)total + 1);
    memset(b, ' ', (size_t)total);
    memcpy(b, s, (size_t)n);
    memcpy(b + at, nw, (size_t)nn);
    return vl_str_take(b, total);
  }
  if (eq(name, "insert")) {
    int64_t nn;
    const char *nw = S(argv[1], &nn);
    int64_t at = I(argv[2]);
    if (at < 0) at = 0;
    if (at > n) at = n;
    char *b = (char *)vl_alloc((size_t)(n + nn) + 1);
    memcpy(b, s, (size_t)at);
    memcpy(b + at, nw, (size_t)nn);
    memcpy(b + at + nn, s + at, (size_t)(n - at));
    return vl_str_take(b, n + nn);
  }
  if (eq(name, "delstr")) {
    int64_t at = I(argv[1]), k = I(argv[2]);
    if (at < 0) at = 0;
    if (at > n) at = n;
    int64_t end = at + k;
    if (end > n) end = n;
    char *b = (char *)vl_alloc((size_t)n + 1);
    memcpy(b, s, (size_t)at);
    memcpy(b + at, s + end, (size_t)(n - end));
    return vl_str_take(b, n - (end - at));
  }
  if (eq(name, "bytes")) {
    Value out = vl_slice_new(0);
    for (int64_t i = 0; i < n; i++)
      vl_slice_append(out, vl_int((unsigned char)s[i]));
    return out;
  }
  if (eq(name, "from_bytes")) {
    VlSlice *sl = (VlSlice *)argv[0].u.o;
    char *b = (char *)vl_alloc((size_t)sl->len + 1);
    for (int64_t i = 0; i < sl->len; i++) b[i] = (char)I(sl->e[i]);
    return vl_str_take(b, sl->len);
  }
  if (eq(name, "runes")) {
    Value out = vl_slice_new(0);
    for (int64_t i = 0; i < n;) {
      const unsigned char *p = (const unsigned char *)s + i;
      int len = rune_len_n(p, n - i);
      uint32_t r = *p;
      if (len == 2) r = ((r & 31u) << 6) | (p[1] & 0x3F);
      else if (len == 3) r = ((r & 15u) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
      else if (len == 4) r = ((r & 7u) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
      vl_slice_append(out, vl_rune(r));
      i += len;
    }
    return out;
  }
  if (eq(name, "from_runes")) {
    VlSlice *sl = (VlSlice *)argv[0].u.o;
    Value b = vl_builder_new();
    for (int64_t i = 0; i < sl->len; i++) {
      Value r = sl->e[i];
      if (r.t == VL_INT) r = vl_rune((uint32_t)r.u.i);
      int64_t rn;
      const char *rs = S(r, &rn);
      Value rv = vl_str_n(rs, rn);
      vl_builder_write(b, rv);
      vl_release(rv);
    }
    Value out = vl_builder_done(b);
    vl_release(b);
    return out;
  }
  if (eq(name, "rune_len")) {
    int64_t c = 0;
    for (int64_t i = 0; i < n;) {
      i += rune_len_n((const unsigned char *)s + i, n - i);
      c++;
    }
    return vl_int(c);
  }
  if (eq(name, "is_digit") || eq(name, "is_alpha") || eq(name, "is_space") ||
      eq(name, "is_upper")) {
    if (n == 0) return vl_bool(false);
    for (int64_t i = 0; i < n; i++) {
      unsigned char c = (unsigned char)s[i];
      if (eq(name, "is_digit") && !isdigit(c)) return vl_bool(false);
      if (eq(name, "is_alpha") && !isalpha(c)) return vl_bool(false);
      if (eq(name, "is_space") && !is_space_c((char)c)) return vl_bool(false);
      if (eq(name, "is_upper") && islower(c)) return vl_bool(false);
    }
    if (eq(name, "is_upper")) {
      bool has_alpha = false;
      for (int64_t i = 0; i < n; i++)
        if (isalpha((unsigned char)s[i])) has_alpha = true;
      return vl_bool(has_alpha);
    }
    return vl_bool(true);
  }
  if (eq(name, "hex")) {
    static const char *H = "0123456789abcdef";
    if (argv[0].t == VL_INT) {
      char buf[24];
      snprintf(buf, sizeof buf, "%llx", (unsigned long long)argv[0].u.i);
      return vl_str(buf);
    }
    char *b = (char *)vl_alloc((size_t)(n * 2) + 1);
    for (int64_t i = 0; i < n; i++) {
      b[i * 2] = H[(unsigned char)s[i] >> 4];
      b[i * 2 + 1] = H[(unsigned char)s[i] & 15];
    }
    return vl_str_take(b, n * 2);
  }
  if (eq(name, "unhex")) {
    if (n % 2) return vl_err(vl_str("bad hex"));
    char *b = (char *)vl_alloc((size_t)(n / 2) + 1);
    for (int64_t i = 0; i < n / 2; i++) {
      int hi = s[i * 2], lo = s[i * 2 + 1];
      hi = isdigit(hi) ? hi - '0' : (tolower(hi) - 'a' + 10);
      lo = isdigit(lo) ? lo - '0' : (tolower(lo) - 'a' + 10);
      b[i] = (char)((hi << 4) | lo);
    }
    return vl_str_take(b, n / 2);
  }
  if (eq(name, "b64")) {
    static const char *B = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int64_t out_n = ((n + 2) / 3) * 4;
    char *b = (char *)vl_alloc((size_t)out_n + 1);
    int64_t o = 0;
    for (int64_t i = 0; i < n; i += 3) {
      uint32_t v = (uint32_t)(unsigned char)s[i] << 16;
      if (i + 1 < n) v |= (uint32_t)(unsigned char)s[i + 1] << 8;
      if (i + 2 < n) v |= (uint32_t)(unsigned char)s[i + 2];
      b[o++] = B[(v >> 18) & 63];
      b[o++] = B[(v >> 12) & 63];
      b[o++] = i + 1 < n ? B[(v >> 6) & 63] : '=';
      b[o++] = i + 2 < n ? B[v & 63] : '=';
    }
    return vl_str_take(b, out_n);
  }
  if (eq(name, "unb64")) {
    char *b = (char *)vl_alloc((size_t)n + 1);
    int64_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (int64_t i = 0; i < n; i++) {
      char c = s[i];
      int v;
      if (c >= 'A' && c <= 'Z') v = c - 'A';
      else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
      else if (c >= '0' && c <= '9') v = c - '0' + 52;
      else if (c == '+') v = 62;
      else if (c == '/') v = 63;
      else continue;
      acc = (acc << 6) | (uint32_t)v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        b[o++] = (char)((acc >> bits) & 0xFF);
      }
    }
    return vl_str_take(b, o);
  }
  *ok = false;
  return vl_nil();
}

Value vl_str_method(Value self, const char *name, Value *argv, int argc,
                    bool *ok) {
  Value *args = (Value *)vl_alloc(sizeof(Value) * (size_t)(argc + 1));
  args[0] = self;
  for (int i = 0; i < argc; i++) args[i + 1] = argv[i];
  Value r = vl_str_call(name, args, argc + 1, ok);
  free(args);
  return r;
}
