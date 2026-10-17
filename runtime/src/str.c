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

/* ---------------------------------------------------------------- unicode */

/* utf8_next decodes ONE well-formed UTF-8 sequence at p and stores its length
 * in *len. It returns -1 (with *len = 1) for anything ill-formed: a stray
 * continuation byte, a truncated sequence, an overlong encoding, a UTF-16
 * surrogate, or a value past U+10FFFF. Callers that transform text copy that
 * one byte through unchanged, so a malformed string is never made worse. */
static int32_t utf8_next(const unsigned char *p, int64_t rem, int *len) {
  *len = 1;
  uint32_t c = p[0];
  if (c < 0x80) return (int32_t)c;
  int n;
  uint32_t min; /* smallest code point that NEEDS n bytes: below it = overlong */
  if ((c & 0xE0) == 0xC0) { n = 2; c &= 0x1F; min = 0x80; }
  else if ((c & 0xF0) == 0xE0) { n = 3; c &= 0x0F; min = 0x800; }
  else if ((c & 0xF8) == 0xF0) { n = 4; c &= 0x07; min = 0x10000; }
  else return -1;
  if ((int64_t)n > rem) return -1;
  for (int k = 1; k < n; k++) {
    if ((p[k] & 0xC0) != 0x80) return -1;
    c = (c << 6) | (p[k] & 0x3F);
  }
  if (c < min || c > 0x10FFFF || (c >= 0xD800 && c <= 0xDFFF)) return -1;
  *len = n;
  return (int32_t)c;
}

/* utf8_put encodes r at out and returns the number of bytes written (1-4). */
static int utf8_put(uint32_t r, char *out) {
  if (r < 0x80) { out[0] = (char)r; return 1; }
  if (r < 0x800) {
    out[0] = (char)(0xC0 | (r >> 6)); out[1] = (char)(0x80 | (r & 0x3F));
    return 2;
  }
  if (r < 0x10000) {
    out[0] = (char)(0xE0 | (r >> 12)); out[1] = (char)(0x80 | ((r >> 6) & 0x3F));
    out[2] = (char)(0x80 | (r & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (r >> 18)); out[1] = (char)(0x80 | ((r >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((r >> 6) & 0x3F)); out[3] = (char)(0x80 | (r & 0x3F));
  return 4;
}

#include "unicode_tab.h"

/* The tables in unicode_tab.h are sorted, non-overlapping rows. Binary search
 * finds the LAST row whose lo <= c; c belongs to it only if it is also within
 * hi and lands on the row's stride (alternating Upper/lower pairs use 2). */
static int uc_row(const void *rows, size_t size, int n, uint32_t c) {
  int lo = 0, hi = n - 1, hit = -1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    const VlRangeRow *r = (const VlRangeRow *)((const char *)rows + (size_t)mid * size);
    if (r->lo <= c) { hit = mid; lo = mid + 1; } else hi = mid - 1;
  }
  if (hit < 0) return -1;
  const VlRangeRow *r = (const VlRangeRow *)((const char *)rows + (size_t)hit * size);
  if (c > r->hi || (c - r->lo) % r->stride != 0) return -1;
  return hit;
}

/* A VlCaseRow's first member is its VlRangeRow, so the row pointer converts
 * to a valid VlRangeRow pointer and one search serves both table shapes. */
static uint32_t uc_case(const VlCaseRow *t, int n, uint32_t c) {
  int i = uc_row(t, sizeof *t, n, c);
  return i < 0 ? c : (uint32_t)((int32_t)c + t[i].delta);
}
static uint32_t uc_upper(uint32_t c) { return uc_case(vl_uc_upper, VL_UC_UPPER_N, c); }
static uint32_t uc_lower(uint32_t c) { return uc_case(vl_uc_lower, VL_UC_LOWER_N, c); }
static bool uc_is_letter(uint32_t c) {
  return uc_row(vl_uc_letter, sizeof vl_uc_letter[0], VL_UC_LETTER_N, c) >= 0;
}
/* Titlecase differs from uppercase only for the DŽ/LJ/NJ/DZ digraphs, so the
 * title table holds just those and everything else falls back to upper. */
static uint32_t uc_title(uint32_t c) {
  int i = uc_row(vl_uc_title, sizeof vl_uc_title[0], VL_UC_TITLE_N, c);
  return i < 0 ? uc_upper(c) : (uint32_t)((int32_t)c + vl_uc_title[i].delta);
}
/* is_upper's two classes (see tools/gen_unicode.py): a rune that rules a
 * string out (Lowercase property, or titlecase Lt), and a rune that counts
 * as upper case (Uppercase property). Uncased runes (digits, 日) are neither. */
static bool uc_not_upper(uint32_t c) {
  return uc_row(vl_uc_notupper, sizeof vl_uc_notupper[0], VL_UC_NOTUPPER_N, c) >= 0;
}
static bool uc_is_upper(uint32_t c) {
  return uc_row(vl_uc_uppercase, sizeof vl_uc_uppercase[0], VL_UC_UPPERCASE_N, c) >= 0;
}

/* Unicode White_Space (PropList.txt): a short, stable list. */
static bool uc_is_space(uint32_t c) {
  return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 ||
         c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
         c == 0x2029 || c == 0x202F || c == 0x205F || c == 0x3000;
}

/* case_map rewrites s rune by rune: the first rune goes through `first`, the
 * rest through `rest` (title() passes titlecase, then lowercase). A case change may change
 * the encoded length (U+0250 ɐ → U+2C6F Ɐ grows 2 → 3 bytes; K U+212A → k
 * shrinks 3 → 1), and no mapping grows by more than half, so 2n+4 is a safe
 * bound for the output buffer. */
static Value case_map(const char *s, int64_t n, uint32_t (*first)(uint32_t),
                      uint32_t (*rest)(uint32_t)) {
  char *b = (char *)vl_alloc((size_t)n * 2 + 4);
  int64_t o = 0;
  bool at_first = true;
  for (int64_t i = 0; i < n;) {
    int len;
    int32_t r = utf8_next((const unsigned char *)s + i, n - i, &len);
    if (r < 0) b[o++] = s[i];  /* ill-formed byte: copy through */
    else o += utf8_put((at_first ? first : rest)((uint32_t)r), b + o);
    at_first = false;
    i += len;
  }
  return vl_str_take(b, o);
}

/* from_bytes_checked builds a str from a []byte. A str is a BYTE string
 * (§13.3: os.read, net.recv and crypto's _raw digests carry binary in one),
 * so any byte sequence is accepted; what is refused, as an ERROR value
 * (§8.1), is an element that is not a byte at all (outside 0..255), which
 * used to be silently truncated. valid_utf8() tells text from binary. */
static Value from_bytes_checked(Value slice) {
  /* A slice, or a set in insertion order (VlSeq). */
  int64_t len = vl_seq_len(slice);
  VlSeq q = vl_seq(slice);
  char *b = (char *)vl_alloc((size_t)len + 1);
  Value ev;
  for (int64_t i = 0; vl_seq_next(&q, &ev); i++) {
    int64_t v = I(ev);
    if (v < 0 || v > 255) {
      free(b);
      char msg[96];
      snprintf(msg, sizeof msg, "from_bytes: element %lld is %lld, not a byte",
               (long long)i, (long long)v);
      return vl_err_cstr(msg);
    }
    b[i] = (char)v;
  }
  return vl_str_take(b, len);
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

/* Every str function's argument count — minimum and maximum, subject included
 * — taken from the signatures in the specification (§13.2) and cross-checked
 * against the argv[] each body actually reads.
 *
 * A native carries no static signature: the code generator knows only a bare
 * name. Both directions of a wrong count were therefore invisible, and both
 * were real bugs. A surplus argument was simply never looked at, so
 * `str.trim_left("..x", ".")` returned "..x", reading like a trim that
 * silently did nothing (the cutset form is `strip(s, cutset)`; trim_left takes
 * the subject alone). A MISSING one was worse: `"hello".substr()` reached
 * `I(argv[1])` and dereferenced past the end of the caller's array — SIGSEGV.
 *
 * THE COMPILER NOW CHECKS THIS FIRST, and this table is what covers the
 * spelling it cannot see. voilac/low/names.voi's NativeArities gives every
 * native both bounds and voilac/chk/native.voi refuses a bad call at compile
 * time — but only where the callee is a NAME (`str.substr(s)`, `substr(s)`).
 * The METHOD spelling `s.substr()` is a call on an ordinary expression whose
 * type is what would decide, which is expression typing and does not exist
 * yet. std/str is the one package with that second spelling, so it is the one
 * package that needs a runtime bound as well. selftest compares the two tables
 * column for column, so they cannot come to disagree about the same call.
 *
 * An optional trailing argument is REAL and rejecting one would be a worse bug
 * than the crash: find_any and skip_any take a start offset, substr and
 * delword and subword a length. Those minima say so.
 *
 * The subject is required even though the body reads it as
 * `argc > 0 ? S(argv[0], &n) : ""`. That guard is defensive — it is what keeps
 * a wrong call out of wild memory — not a claim that the subject is optional;
 * §13.2 spells every one of these with one. */
typedef struct {
  const char *name;
  int min;
  int max;
} StrArity;

static const StrArity str_arity[] = {
    {"upper", 1, 1},       {"lower", 1, 1},      {"title", 1, 1},
    {"trim", 1, 1},        {"trim_left", 1, 1},  {"trim_right", 1, 1},
    {"reverse", 1, 1},     {"lines", 1, 1},      {"fields", 1, 1},
    {"words", 1, 1},       {"bytes", 1, 1},      {"runes", 1, 1},
    {"rune_len", 1, 1},    {"is_digit", 1, 1},   {"is_alpha", 1, 1},
    {"is_space", 1, 1},    {"is_upper", 1, 1},   {"hex", 1, 1},
    {"valid_utf8", 1, 1},
    {"unhex", 1, 1},       {"b64", 1, 1},        {"unb64", 1, 1},
    {"from_bytes", 1, 1},  {"from_runes", 1, 1},
    {"strip", 2, 2},       {"pad", 2, 2},        {"pad_left", 2, 2},
    {"center", 2, 2},      {"contains", 2, 2},   {"index", 2, 2},
    {"last_index", 2, 2},  {"starts_with", 2, 2},{"ends_with", 2, 2},
    {"count", 2, 2},       {"equal_fold", 2, 2}, {"left", 2, 2},
    {"right", 2, 2},       {"split", 2, 2},      {"word", 2, 2},
    {"repeat", 2, 2},      {"join", 2, 2},
    {"trim_prefix", 2, 2}, {"trim_suffix", 2, 2},{"cut", 2, 2},
    {"substr", 2, 3},      {"split_n", 3, 3},    {"delword", 2, 3},
    {"subword", 2, 3},     {"replace", 3, 3},    {"translate", 3, 3},
    {"overlay", 3, 3},     {"insert", 3, 3},     {"delstr", 3, 3},
    {"find_any", 2, 3},    {"skip_any", 2, 3},
    {"replace_n", 4, 4},
    {NULL, 0, 0},
};

static void str_check_arity(const char *name, int argc) {
  for (int i = 0; str_arity[i].name; i++) {
    if (!eq(name, str_arity[i].name)) continue;
    int min = str_arity[i].min, max = str_arity[i].max;
    if (argc < min) {
      vl_abort("str.%s takes at least %d argument%s, but %d %s given", name,
               min, min == 1 ? "" : "s", argc, argc == 1 ? "is" : "are");
    }
    if (argc > max) {
      vl_abort("str.%s takes at most %d argument%s, but %d %s given", name,
               max, max == 1 ? "" : "s", argc, argc == 1 ? "is" : "are");
    }
    return;
  }
}

Value vl_str_call(const char *name, Value *argv, int argc, bool *ok) {
  *ok = true;
  str_check_arity(name, argc);

  /* These three take a slice as their subject, not a string. */
  if (eq(name, "join")) {
    Value sep = vl_tostr(argv[1]);
    /* A slice in order, or a set in insertion order (VlSeq). */
    VlSeq q = vl_seq(argv[0]);
    Value b = vl_builder_new();
    /* an element's `show` may throw (err.c, unwind cleanups) */
    int mark = vl_cleanup_mark();
    vl_cleanup_slot(&b);
    vl_cleanup_slot(&sep);
    Value e;
    for (int64_t i = 0; vl_seq_next(&q, &e); i++) {
      if (i) vl_builder_write(b, sep);
      vl_builder_write(b, e);
    }
    vl_cleanup_to(mark);
    Value out = vl_builder_done(b);
    vl_release(b);
    vl_release(sep);
    return out;
  }
  if (eq(name, "from_bytes")) return from_bytes_checked(argv[0]);
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
    uint32_t (*f)(uint32_t) = eq(name, "upper") ? uc_upper : uc_lower;
    return case_map(s, n, f, f);
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
      Value tv = case_map(ws, wn, uc_title, uc_lower);
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
  /* trim_prefix(s, p) / trim_suffix(s, p): s without a leading / trailing p,
   * or s itself when it does not start / end with p. Exactly one copy of p
   * is removed — unlike strip, which takes a SET of characters. */
  if (eq(name, "trim_prefix") || eq(name, "trim_suffix")) {
    int64_t pn;
    const char *p = S(argv[1], &pn);
    if (pn > n) return vl_str_n(s, n);
    if (eq(name, "trim_prefix")) {
      if (memcmp(s, p, (size_t)pn) == 0) return vl_str_n(s + pn, n - pn);
      return vl_str_n(s, n);
    }
    if (memcmp(s + n - pn, p, (size_t)pn) == 0) return vl_str_n(s, n - pn);
    return vl_str_n(s, n);
  }
  /* cut(s, sep) -> (before, after, found): s split around the FIRST sep. When
   * sep does not occur the answer is (s, "", false), so `before` is always
   * usable on its own. An empty sep is found at offset 0. */
  if (eq(name, "cut")) {
    int64_t pn;
    const char *p = S(argv[1], &pn);
    int64_t at = -1;
    for (int64_t i = 0; i + pn <= n; i++) {
      if (memcmp(s + i, p, (size_t)pn) == 0) {
        at = i;
        break;
      }
    }
    Value parts[3];
    if (at < 0) {
      parts[0] = vl_str_n(s, n);
      parts[1] = vl_str("");
      parts[2] = vl_bool(false);
    } else {
      parts[0] = vl_str_n(s, at);
      parts[1] = vl_str_n(s + at + pn, n - at - pn);
      parts[2] = vl_bool(true);
    }
    Value t = vl_tuple_new(parts, 3); /* retains */
    vl_release(parts[0]);
    vl_release(parts[1]);
    return t;
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
    Value sep = vl_tostr(argv[1]);
    /* A slice in order, or a set in insertion order (VlSeq). */
    VlSeq q = vl_seq(argv[0]);
    Value b = vl_builder_new();
    Value e;
    for (int64_t i = 0; vl_seq_next(&q, &e); i++) {
      if (i) vl_builder_write(b, sep);
      vl_builder_write(b, e);
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
    /* Parenthesised: `/` binds tighter than `<<`, so `1 << 31 / n` was
     * `1 << (31/n)` — for a ten-character string that capped the count at 8,
     * and str.repeat("1234567890", 9) threw over a 90-byte result. The limit
     * meant here is 2^31 bytes of OUTPUT, i.e. (1 << 31) / n. */
    if (n > 0 && k > ((int64_t)1 << 31) / n)
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
  if (eq(name, "from_bytes")) return from_bytes_checked(argv[0]);
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
  if (eq(name, "valid_utf8")) {
    /* True when s is well-formed UTF-8 (no stray, truncated or overlong
     * sequence, no surrogate, nothing past U+10FFFF). A str may hold any
     * bytes (§13.3); this is how a program tells text from binary. */
    for (int64_t i = 0; i < n;) {
      int len;
      if (utf8_next((const unsigned char *)s + i, n - i, &len) < 0)
        return vl_bool(false);
      i += len;
    }
    return vl_bool(true);
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
    /* Each test holds for EVERY rune of s (and s is non-empty). is_digit
     * stays ASCII 0-9 on purpose: it guards int(), which parses only ASCII
     * digits. The others are Unicode: letters are category L*, space is
     * White_Space, and is_upper matches Python's str.isupper: no lowercase
     * or titlecase rune, and at least one uppercase rune (so uncased text
     * like "日本" or "123" is not upper). Ill-formed UTF-8 fails every test. */
    if (n == 0) return vl_bool(false);
    bool has_upper = false;
    for (int64_t i = 0; i < n;) {
      int len;
      int32_t r = utf8_next((const unsigned char *)s + i, n - i, &len);
      if (r < 0) return vl_bool(false);
      uint32_t c = (uint32_t)r;
      if (uc_is_upper(c)) has_upper = true;
      if (eq(name, "is_digit") && !(c >= '0' && c <= '9')) return vl_bool(false);
      if (eq(name, "is_alpha") && !uc_is_letter(c)) return vl_bool(false);
      if (eq(name, "is_space") && !uc_is_space(c)) return vl_bool(false);
      if (eq(name, "is_upper") && uc_not_upper(c)) return vl_bool(false);
      i += len;
    }
    if (eq(name, "is_upper")) return vl_bool(has_upper);
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
    if (n % 2) return vl_err_cstr("bad hex");
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
