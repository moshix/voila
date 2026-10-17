/* json.c — std/json: encode, and a strict, TYPED decode.
 *
 * Decoding is two passes, and keeping them apart is the design:
 *
 *   1. parse()  the text -> a dynamic value: an object is a map, an array a
 *               slice, a string a str, `true`/`false` a bool, `null` nil, and
 *               a number an int — or a dec when it has a fraction or an
 *               exponent, so money survives (§13.4). The parser is STRICT
 *               (RFC 8259): trailing text, a trailing comma, a bare control
 *               character in a string, a leading zero or an unknown escape
 *               is an error, and so is nesting deeper than JD_MAX_DEPTH.
 *
 *   2. conv()   the dynamic value -> the type the program asked for, by the
 *               TYPE TEXT the code generator hands json.decode[T] (the same
 *               spelling VlType.ftypes carries for every field): `int`,
 *               `?str`, `[]Item`, `map[str]int`, `Box[T]`, a struct, a
 *               newtype, an enum. It recurses through fields and elements
 *               and keeps the JSON PATH it is at, so a mismatch is reported
 *               where it is — `limits.max_mb: expected int, got string` —
 *               instead of surfacing later as "map has no field".
 *
 * The mapping, which json_enc below is the exact inverse of:
 *
 *   int, i8…u64, byte   a JSON integer (range-checked for a sized type); an
 *                       integral number written with a fraction (5.0) too
 *   float, f32          any number
 *   dec                 any number, exactly
 *   str                 a string        rune   a one-character string
 *   bool                true/false      unit   null
 *   ?T                  null or a MISSING field is nil; anything else is a T
 *   []T, set[T]         an array        (T1, T2)  an array of that length
 *   map[K]V             an object; a non-str K is parsed from the key text
 *   Box[T]              a T             a newtype   its underlying value
 *   struct              an object. A field the object lacks is an error
 *                       unless its type is ?T (nil) or it declares a
 *                       default (VlType.defaults); keys the struct does not
 *                       declare are ignored, as Go does
 *   enum                a unit variant is its name, "Red"; a variant with a
 *                       payload is {"Circle": {"r": 1.5}} — or the payload
 *                       as an array, or bare when it has one field
 *
 * A type the runtime cannot see into — a generic parameter, erased by §3.7 —
 * takes the dynamic value as it is.
 *
 * There is no field-renaming tag: the specification has no annotation
 * syntax, so a JSON key IS the field's name. */
#include "voila_int.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#define JD_MAX_DEPTH 512

static bool is_kind(Value v, VlKind k) {
  return v.t == VL_OBJ && v.u.o->kind == k;
}

/* ============================================================== encoding */

static void jput(Value b, const char *s) {
  Value t = vl_str(s);
  vl_builder_write(b, t);
  vl_release(t);
}

static void jput_n(Value b, const char *s, int64_t n) {
  Value t = vl_str_n(s, n);
  vl_builder_write(b, t);
  vl_release(t);
}

/* json_str writes s as a JSON string. Every control character is escaped —
 * a raw one is not legal inside a JSON string — and bytes >= 0x80 pass
 * through untouched, since a str is already UTF-8 (or binary the caller
 * chose to put there). Runs of plain bytes are copied in one write. */
static void json_str(Value b, Value s) {
  Value sv = vl_tostr(s);
  const char *p = vl_cstr(sv);
  int64_t n = vl_strlen(sv);
  jput(b, "\"");
  int64_t run = 0;
  for (int64_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)p[i];
    const char *esc = NULL;
    char hex[8];
    if (c == '"') esc = "\\\"";
    else if (c == '\\') esc = "\\\\";
    else if (c == '\n') esc = "\\n";
    else if (c == '\t') esc = "\\t";
    else if (c == '\r') esc = "\\r";
    else if (c == '\b') esc = "\\b";
    else if (c == '\f') esc = "\\f";
    else if (c < 0x20) {
      snprintf(hex, sizeof hex, "\\u%04x", c);
      esc = hex;
    }
    if (!esc) continue;
    if (i > run) jput_n(b, p + run, i - run);
    jput(b, esc);
    run = i + 1;
  }
  if (n > run) jput_n(b, p + run, n - run);
  jput(b, "\"");
  vl_release(sv);
}

static void json_enc(Value b, Value v, const char *indent, const char *cur);

/* enc_seq writes n values as a JSON array (the shared body of a slice, a
 * set, a tuple and a positional enum payload). */
static void enc_seq(Value b, const Value *e, int64_t n, const char *indent,
                    const char *cur, const char *pad, const char *nl) {
  if (n == 0) {
    jput(b, "[]");
    return;
  }
  jput(b, "[");
  jput(b, nl);
  for (int64_t i = 0; i < n; i++) {
    jput(b, pad);
    json_enc(b, e[i], indent, pad);
    if (i < n - 1) jput(b, ",");
    jput(b, nl);
  }
  jput(b, cur);
  jput(b, "]");
}

/* enc_fields writes named values as a JSON object (a struct, an exception,
 * an enum payload). */
static void enc_fields(Value b, const char *const *names, const Value *f,
                       int n, const char *indent, const char *cur,
                       const char *pad, const char *nl, const char *sep) {
  if (n == 0) {
    jput(b, "{}");
    return;
  }
  jput(b, "{");
  jput(b, nl);
  for (int i = 0; i < n; i++) {
    jput(b, pad);
    Value k = vl_str(names[i]);
    json_str(b, k);
    vl_release(k);
    jput(b, sep);
    json_enc(b, f[i], indent, pad);
    if (i < n - 1) jput(b, ",");
    jput(b, nl);
  }
  jput(b, cur);
  jput(b, "}");
}

/* Encoding recurses once per level of nesting; past JE_MAX_DEPTH (the
 * decoder's limit too) the encoder stops descending and vl_json_encode
 * throws, instead of a deep value overflowing the C stack. */
#define JE_MAX_DEPTH 512
static __thread int g_je_depth;
static __thread bool g_je_deep;
static void json_enc_body(Value b, Value v, const char *indent,
                          const char *cur);
static void json_enc(Value b, Value v, const char *indent, const char *cur) {
  if (g_je_depth >= JE_MAX_DEPTH) {
    g_je_deep = true;
    jput(b, "null");
    return;
  }
  g_je_depth++;
  json_enc_body(b, v, indent, cur);
  g_je_depth--;
}

static void json_enc_body(Value b, Value v, const char *indent,
                          const char *cur) {
  char pad[1040]; /* JE_MAX_DEPTH levels of a two-space indent */
  const char *nl = indent[0] ? "\n" : "";
  snprintf(pad, sizeof pad, "%s%s", cur, indent);
  const char *sep = indent[0] ? ": " : ":";

  switch (v.t) {
  case VL_NIL:
  case VL_UNIT: jput(b, "null"); return;
  case VL_BOOL: jput(b, v.u.b ? "true" : "false"); return;
  case VL_FLOAT:
    /* JSON has no NaN or infinity; null is the one value that says "no
     * number here" and still parses. */
    if (v.u.f != v.u.f || v.u.f > 1.7976931348623157e308 ||
        v.u.f < -1.7976931348623157e308) {
      jput(b, "null");
      return;
    }
    /* fallthrough */
  case VL_INT: {
    Value s = vl_tostr(v);
    vl_builder_write(b, s);
    vl_release(s);
    return;
  }
  case VL_OBJ: break;
  default: {
    Value s = vl_tostr(v);
    json_str(b, s);
    vl_release(s);
    return;
  }
  }
  switch ((VlKind)v.u.o->kind) {
  case O_DEC: {
    Value s = vl_tostr(v);
    vl_builder_write(b, s);
    vl_release(s);
    return;
  }
  case O_STR:
  case O_RANGE:
    json_str(b, v);
    return;
  case O_SLICE: {
    VlSlice *s = (VlSlice *)v.u.o;
    enc_seq(b, s->e, s->len, indent, cur, pad, nl);
    return;
  }
  case O_TUPLE: {
    VlTuple *t = (VlTuple *)v.u.o;
    enc_seq(b, t->e, t->n, indent, cur, pad, nl);
    return;
  }
  case O_SET: {
    /* A set is an array of its members, in insertion order. */
    Value ks = vl_map_keys(v);
    VlSlice *s = (VlSlice *)ks.u.o;
    enc_seq(b, s->e, s->len, indent, cur, pad, nl);
    vl_release(ks);
    return;
  }
  case O_MAP: {
    VlMap *m = (VlMap *)v.u.o;
    if (m->count == 0) {
      jput(b, "{}");
      return;
    }
    jput(b, "{");
    jput(b, nl);
    int64_t seen = 0;
    for (int64_t i = 0; i < m->nents; i++) {
      if (!m->ents[i].used) continue;
      jput(b, pad);
      json_str(b, m->ents[i].k);
      jput(b, sep);
      json_enc(b, m->ents[i].v, indent, pad);
      if (++seen < m->count) jput(b, ",");
      jput(b, nl);
    }
    jput(b, cur);
    jput(b, "}");
    return;
  }
  case O_STRUCT: {
    VlStruct *s = (VlStruct *)v.u.o;
    /* A newtype is its underlying value (§3.8): `type Id int` is 3, never
     * an object with an unnamed field. */
    if (s->type->newtype) {
      json_enc(b, s->f[0], indent, cur);
      return;
    }
    enc_fields(b, s->type->fields, s->f, s->type->nfields, indent, cur, pad,
               nl, sep);
    return;
  }
  case O_ENUM: {
    /* A unit variant is its name; a variant with a payload is a one-key
     * object, {"Circle": {"r": 1.5}}, named when every field has a name. */
    VlEnum *e = (VlEnum *)v.u.o;
    Value name = vl_str(e->var->variant);
    if (e->n == 0) {
      json_str(b, name);
      vl_release(name);
      return;
    }
    char pad2[1040];
    snprintf(pad2, sizeof pad2, "%s%s", pad, indent);
    jput(b, "{");
    jput(b, nl);
    jput(b, pad);
    json_str(b, name);
    vl_release(name);
    jput(b, sep);
    bool named = e->var->fields != NULL;
    for (int i = 0; named && i < e->n; i++)
      if (!e->var->fields[i] || !e->var->fields[i][0]) named = false;
    if (named)
      enc_fields(b, e->var->fields, e->f, e->n, indent, pad, pad2, nl, sep);
    else
      enc_seq(b, e->f, e->n, indent, pad, pad2, nl);
    jput(b, nl);
    jput(b, cur);
    jput(b, "}");
    return;
  }
  case O_BOX:
    json_enc(b, ((VlBox *)v.u.o)->v, indent, cur);
    return;
  case O_SHARED:
    json_enc(b, ((VlShared *)v.u.o)->v, indent, cur);
    return;
  default: {
    Value s = vl_tostr(v);
    json_str(b, s);
    vl_release(s);
    return;
  }
  }
}

Value vl_json_encode(Value v, bool pretty) {
  Value b = vl_builder_new();
  g_je_depth = 0;
  g_je_deep = false;
  int mark = vl_cleanup_mark(); /* a `show` may throw (err.c) */
  vl_cleanup_slot(&b);
  json_enc(b, v, pretty ? "  " : "", "");
  vl_cleanup_to(mark);
  Value out = vl_builder_done(b);
  vl_release(b);
  if (g_je_deep) {
    vl_release(out);
    vl_throwf("ValueError", "json.encode: the value nests deeper than %d "
              "levels", JE_MAX_DEPTH);
  }
  return out;
}

/* =============================================================== parsing */

typedef struct {
  const char *s;
  int64_t i, n;
  const char *why; /* the first syntax error, or NULL */
  int64_t at;      /* where it was found */
  int depth;
} JP;

static bool jfail(JP *p, const char *why) {
  if (!p->why) {
    p->why = why;
    p->at = p->i;
  }
  return false;
}

static void jskip(JP *p) {
  while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\n' ||
                         p->s[p->i] == '\t' || p->s[p->i] == '\r'))
    p->i++;
}

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* read4 reads the four hex digits of a \u escape at p->i. */
static bool read4(JP *p, uint32_t *out) {
  if (p->i + 4 > p->n) return jfail(p, "truncated \\u escape");
  uint32_t v = 0;
  for (int k = 0; k < 4; k++) {
    int h = hexv(p->s[p->i + k]);
    if (h < 0) return jfail(p, "bad hex digit in a \\u escape");
    v = v * 16 + (uint32_t)h;
  }
  p->i += 4;
  *out = v;
  return true;
}

/* put_utf8 appends code point r to buf (which has room for 4 more bytes). */
static int put_utf8(uint32_t r, char *o) {
  if (r < 0x80) {
    o[0] = (char)r;
    return 1;
  }
  if (r < 0x800) {
    o[0] = (char)(0xC0 | (r >> 6));
    o[1] = (char)(0x80 | (r & 0x3F));
    return 2;
  }
  if (r < 0x10000) {
    o[0] = (char)(0xE0 | (r >> 12));
    o[1] = (char)(0x80 | ((r >> 6) & 0x3F));
    o[2] = (char)(0x80 | (r & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | (r >> 18));
  o[1] = (char)(0x80 | ((r >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((r >> 6) & 0x3F));
  o[3] = (char)(0x80 | (r & 0x3F));
  return 4;
}

static bool jstring(JP *p, Value *out) {
  if (p->i >= p->n || p->s[p->i] != '"') return jfail(p, "expected a string");
  p->i++;
  /* Find the closing quote first (a backslash always escapes the next
   * byte), so the buffer is the size of THIS string's source span — the
   * decoded text is never longer than it. Sizing it by the rest of the
   * input instead made decoding quadratic. A string with no escape at all,
   * the common case, is copied straight out of the source. */
  int64_t start = p->i;
  int64_t end = start;
  bool escaped = false;
  while (end < p->n && p->s[end] != '"') {
    unsigned char ch = (unsigned char)p->s[end];
    if (ch < 0x20) break; /* reported by the loop below */
    if (ch == '\\') {
      escaped = true;
      end++;
    }
    end++;
  }
  if (!escaped && end < p->n && p->s[end] == '"') {
    p->i = end + 1;
    *out = vl_str_n(p->s + start, end - start);
    return true;
  }
  int64_t span = (end < p->n ? end : p->n) - start;
  char *buf = (char *)vl_alloc((size_t)span + 8);
  int64_t o = 0;
  while (p->i < p->n && p->s[p->i] != '"') {
    unsigned char c = (unsigned char)p->s[p->i++];
    if (c < 0x20) {
      free(buf);
      p->i--;
      return jfail(p, "control character in a string");
    }
    if (c != '\\') {
      buf[o++] = (char)c;
      continue;
    }
    if (p->i >= p->n) break;
    char e = p->s[p->i++];
    switch (e) {
    case '"': buf[o++] = '"'; break;
    case '\\': buf[o++] = '\\'; break;
    case '/': buf[o++] = '/'; break;
    case 'b': buf[o++] = '\b'; break;
    case 'f': buf[o++] = '\f'; break;
    case 'n': buf[o++] = '\n'; break;
    case 'r': buf[o++] = '\r'; break;
    case 't': buf[o++] = '\t'; break;
    case 'u': {
      uint32_t r;
      if (!read4(p, &r)) {
        free(buf);
        return false;
      }
      /* A UTF-16 surrogate pair spells one code point above U+FFFF; a lone
       * surrogate is not a character and becomes U+FFFD. */
      if (r >= 0xD800 && r <= 0xDBFF && p->i + 6 <= p->n &&
          p->s[p->i] == '\\' && p->s[p->i + 1] == 'u') {
        p->i += 2;
        uint32_t lo;
        if (!read4(p, &lo)) {
          free(buf);
          return false;
        }
        if (lo >= 0xDC00 && lo <= 0xDFFF) {
          r = 0x10000 + ((r - 0xD800) << 10) + (lo - 0xDC00);
        } else {
          o += put_utf8(0xFFFD, buf + o);
          r = lo >= 0xD800 && lo <= 0xDFFF ? 0xFFFD : lo;
        }
      } else if (r >= 0xD800 && r <= 0xDFFF) {
        r = 0xFFFD;
      }
      o += put_utf8(r, buf + o);
      break;
    }
    default:
      free(buf);
      p->i--;
      return jfail(p, "unknown escape in a string");
    }
  }
  if (p->i >= p->n) {
    free(buf);
    return jfail(p, "unterminated string");
  }
  p->i++;
  *out = vl_str_n(buf, o);
  free(buf);
  return true;
}

/* jnumber reads RFC 8259's number grammar exactly:
 *     -? (0 | [1-9][0-9]*) (. [0-9]+)? ([eE] [+-]? [0-9]+)?
 * An integer that fits int64 is an int; any other number is a dec. */
static bool jnumber(JP *p, Value *out) {
  int64_t start = p->i;
  bool isint = true;
  if (p->i < p->n && p->s[p->i] == '-') p->i++;
  if (p->i >= p->n || !isdigit((unsigned char)p->s[p->i]))
    return jfail(p, "invalid value");
  if (p->s[p->i] == '0') {
    p->i++;
    if (p->i < p->n && isdigit((unsigned char)p->s[p->i]))
      return jfail(p, "a number may not have a leading zero");
  } else {
    while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
  }
  if (p->i < p->n && p->s[p->i] == '.') {
    isint = false;
    p->i++;
    if (p->i >= p->n || !isdigit((unsigned char)p->s[p->i]))
      return jfail(p, "a fraction needs a digit after the point");
    while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
  }
  if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
    isint = false;
    p->i++;
    if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
    if (p->i >= p->n || !isdigit((unsigned char)p->s[p->i]))
      return jfail(p, "an exponent needs a digit");
    while (p->i < p->n && isdigit((unsigned char)p->s[p->i])) p->i++;
  }
  char *tmp = vl_strdup_n(p->s + start, p->i - start);
  if (isint) {
    errno = 0;
    long long n = strtoll(tmp, NULL, 10);
    if (errno == ERANGE) isint = false; /* too big for int: exact as a dec */
    else *out = vl_int(n);
  }
  if (!isint && !vl_dec_try_parse(tmp, out)) {
    free(tmp);
    p->i = start;
    return jfail(p, "number out of range (the exponent is beyond 999999999)");
  }
  free(tmp);
  return true;
}

static bool jvalue(JP *p, Value *out);

static bool jobject(JP *p, Value *out) {
  p->i++; /* { */
  Value m = vl_map_new();
  jskip(p);
  if (p->i < p->n && p->s[p->i] == '}') {
    p->i++;
    *out = m;
    return true;
  }
  for (;;) {
    jskip(p);
    Value k;
    if (!jstring(p, &k)) {
      vl_release(m);
      return false;
    }
    jskip(p);
    if (p->i >= p->n || p->s[p->i] != ':') {
      vl_release(k);
      vl_release(m);
      return jfail(p, "expected `:` after an object key");
    }
    p->i++;
    Value v;
    if (!jvalue(p, &v)) {
      vl_release(k);
      vl_release(m);
      return false;
    }
    vl_map_set(m, k, v); /* takes v */
    vl_release(k);
    jskip(p);
    if (p->i < p->n && p->s[p->i] == ',') {
      p->i++;
      continue;
    }
    if (p->i < p->n && p->s[p->i] == '}') {
      p->i++;
      break;
    }
    vl_release(m);
    return jfail(p, "expected `,` or `}` in an object");
  }
  *out = m;
  return true;
}

static bool jarray(JP *p, Value *out) {
  p->i++; /* [ */
  Value a = vl_slice_new(0);
  jskip(p);
  if (p->i < p->n && p->s[p->i] == ']') {
    p->i++;
    *out = a;
    return true;
  }
  for (;;) {
    Value v;
    if (!jvalue(p, &v)) {
      vl_release(a);
      return false;
    }
    vl_slice_append(a, v);
    jskip(p);
    if (p->i < p->n && p->s[p->i] == ',') {
      p->i++;
      continue;
    }
    if (p->i < p->n && p->s[p->i] == ']') {
      p->i++;
      break;
    }
    vl_release(a);
    return jfail(p, "expected `,` or `]` in an array");
  }
  *out = a;
  return true;
}

static bool jlit(JP *p, const char *word, Value v, Value *out) {
  size_t n = strlen(word);
  if ((int64_t)n > p->n - p->i || strncmp(p->s + p->i, word, n) != 0)
    return jfail(p, "invalid value");
  p->i += (int64_t)n;
  *out = v;
  return true;
}

static bool jvalue(JP *p, Value *out) {
  jskip(p);
  if (p->i >= p->n) return jfail(p, "unexpected end of input");
  if (p->depth >= JD_MAX_DEPTH) return jfail(p, "nested too deeply");
  char c = p->s[p->i];
  bool ok;
  p->depth++;
  if (c == '{') ok = jobject(p, out);
  else if (c == '[') ok = jarray(p, out);
  else if (c == '"') ok = jstring(p, out);
  else if (c == 't') ok = jlit(p, "true", vl_bool(true), out);
  else if (c == 'f') ok = jlit(p, "false", vl_bool(false), out);
  else if (c == 'n') ok = jlit(p, "null", vl_nil(), out);
  else ok = jnumber(p, out);
  p->depth--;
  return ok;
}

/* ============================================================ conversion */

typedef struct {
  char *path; /* the JSON path to the value being converted: a.b[2].c */
  size_t len, cap;
  char *err;  /* the first conversion error, already prefixed by its path */
} JD;

static void path_push(JD *d, const char *seg, bool index) {
  size_t n = strlen(seg) + 2;
  if (d->len + n + 1 > d->cap) {
    d->cap = (d->len + n + 1) * 2;
    d->path = (char *)realloc(d->path, d->cap);
    if (!d->path) vl_abort("out of memory");
  }
  if (!index && d->len > 0) d->path[d->len++] = '.';
  memcpy(d->path + d->len, seg, n - 2);
  d->len += n - 2;
  d->path[d->len] = 0;
}

static void path_pop(JD *d, size_t to) {
  d->len = to;
  if (d->path) d->path[to] = 0;
}

static bool cfail(JD *d, const char *fmt, ...) {
  if (d->err) return false;
  char msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);
  size_t n = d->len + strlen(msg) + 4;
  d->err = (char *)vl_alloc(n);
  if (d->len > 0) snprintf(d->err, n, "%s: %s", d->path, msg);
  else snprintf(d->err, n, "%s", msg);
  return false;
}

/* jkind names a parsed value the way the JSON text spelled it. */
static const char *jkind(Value v) {
  if (v.t == VL_NIL) return "null";
  if (v.t == VL_BOOL) return "bool";
  if (v.t == VL_INT || v.t == VL_FLOAT) return "number";
  if (v.t != VL_OBJ) return "value";
  switch ((VlKind)v.u.o->kind) {
  case O_DEC: return "number";
  case O_STR: return "string";
  case O_SLICE: return "array";
  case O_MAP: return "object";
  default: return "value";
  }
}

/* close_bracket finds the `]` matching the `[` at s[open], or -1. */
static int64_t close_bracket(const char *s, int64_t open) {
  int depth = 0;
  for (int64_t i = open; s[i]; i++) {
    if (s[i] == '[' || s[i] == '(') depth++;
    else if (s[i] == ']' || s[i] == ')') {
      if (--depth == 0) return i;
    }
  }
  return -1;
}

static int sized_code(const char *t) {
  if (strcmp(t, "i8") == 0) return VL_SZ_I8;
  if (strcmp(t, "i16") == 0) return VL_SZ_I16;
  if (strcmp(t, "i32") == 0) return VL_SZ_I32;
  if (strcmp(t, "u8") == 0 || strcmp(t, "byte") == 0) return VL_SZ_U8;
  if (strcmp(t, "u16") == 0) return VL_SZ_U16;
  if (strcmp(t, "u32") == 0) return VL_SZ_U32;
  if (strcmp(t, "u64") == 0) return VL_SZ_U64;
  return 0;
}

static bool conv(JD *d, Value v, const char *ty, Value *out);

/* conv_elem converts one element of an array, with `[i]` on the path. */
static bool conv_elem(JD *d, Value v, int64_t i, const char *ty, Value *out) {
  char seg[32];
  snprintf(seg, sizeof seg, "[%lld]", (long long)i);
  size_t mark = d->len;
  path_push(d, seg, true);
  bool ok = conv(d, v, ty, out);
  path_pop(d, mark);
  return ok;
}

/* conv_field converts one member of an object, with `.name` on the path. */
static bool conv_field(JD *d, Value v, const char *name, const char *ty,
                       Value *out) {
  size_t mark = d->len;
  path_push(d, name, false);
  bool ok = conv(d, v, ty, out);
  path_pop(d, mark);
  return ok;
}

static bool conv_struct(JD *d, Value v, const VlType *t, const char *ty,
                        Value *out) {
  if (!is_kind(v, O_MAP))
    return cfail(d, "expected %s (an object), got %s", ty, jkind(v));
  int nf = t->nfields;
  Value *vals = (Value *)vl_alloc(sizeof(Value) * (size_t)(nf ? nf : 1));
  const char **names = (const char **)vl_alloc(sizeof(char *) * (size_t)(nf ? nf : 1));
  bool ok = true;
  int got = 0;
  for (int i = 0; i < nf && ok; i++) {
    const char *fty = t->ftypes ? t->ftypes[i] : "";
    Value key = vl_str(t->fields[i]);
    Value fv;
    bool present = vl_map_get(v, key, &fv);
    vl_release(key);
    Value cv = vl_nil();
    if (present) {
      ok = conv_field(d, fv, t->fields[i], fty, &cv);
      vl_release(fv);
    } else if (t->defaults && t->defaults[i]) {
      /* The declared default — first, so `?int = 5` absent is 5, not nil. */
      cv = vl_retain(*t->defaults[i]);
    } else if (fty[0] == '?') {
      cv = vl_nil(); /* an optional field may simply be absent */
    } else {
      size_t mark = d->len;
      path_push(d, t->fields[i], false);
      ok = cfail(d, "missing field of type %s", fty[0] ? fty : "any");
      path_pop(d, mark);
    }
    if (ok) {
      names[got] = t->fields[i];
      vals[got] = cv;
      got++;
    }
  }
  if (ok) *out = vl_struct_new(t->name, NULL, 0, names, vals, got);
  for (int i = 0; i < got; i++) vl_release(vals[i]);
  free(vals);
  free((void *)names);
  return ok;
}

/* conv_payload fills a variant's fields from its JSON payload: an object by
 * field name, an array by position, or — for a one-field variant — the bare
 * value. */
static bool conv_payload(JD *d, Value pv, const VlVariant *var, Value *out) {
  int nf = var->nfields;
  Value *f = (Value *)vl_alloc(sizeof(Value) * (size_t)(nf ? nf : 1));
  int done = 0;
  bool ok = true;
  size_t mark = d->len;
  path_push(d, var->variant, false);
  for (int i = 0; i < nf && ok; i++) {
    const char *fty = var->ftypes ? var->ftypes[i] : "";
    const char *fname = var->fields && var->fields[i] ? var->fields[i] : "";
    Value raw;
    bool have = false;
    if (is_kind(pv, O_MAP) && fname[0]) {
      Value key = vl_str(fname);
      have = vl_map_get(pv, key, &raw);
      vl_release(key);
    } else if (is_kind(pv, O_SLICE)) {
      VlSlice *s = (VlSlice *)pv.u.o;
      if (s->len != nf) {
        ok = cfail(d, "expected %d payload values, got %lld", nf,
                   (long long)s->len);
        break;
      }
      raw = vl_retain(s->e[i]);
      have = true;
    } else if (nf == 1) {
      raw = vl_retain(pv);
      have = true;
    }
    if (!have) {
      if (fty[0] == '?') {
        f[done++] = vl_nil();
        continue;
      }
      ok = cfail(d, "missing payload field %s", fname[0] ? fname : "?");
      break;
    }
    Value cv;
    if (fname[0]) ok = conv_field(d, raw, fname, fty, &cv);
    else ok = conv_elem(d, raw, i, fty, &cv);
    vl_release(raw);
    if (ok) f[done++] = cv;
  }
  path_pop(d, mark);
  if (ok) *out = vl_enum_new(var->type, var->variant, f, nf);
  for (int i = 0; i < done; i++) vl_release(f[i]);
  free(f);
  return ok;
}

static bool conv_enum(JD *d, Value v, const char *ename, Value *out) {
  if (is_kind(v, O_STR)) {
    const VlVariant *var = vl_find_variant(ename, vl_cstr(v));
    if (!var) return cfail(d, "`%s` is not a variant of %s", vl_cstr(v), ename);
    if (var->nfields > 0)
      return cfail(d, "variant %s carries a payload; expected {\"%s\": …}",
                   var->variant, var->variant);
    *out = vl_enum_new(ename, var->variant, NULL, 0);
    return true;
  }
  if (is_kind(v, O_MAP) && vl_map_count(v) == 1) {
    Value ks = vl_map_keys(v);
    Value k = ((VlSlice *)ks.u.o)->e[0];
    const VlVariant *var = vl_find_variant(ename, vl_cstr(k));
    bool ok;
    if (!var) {
      ok = cfail(d, "`%s` is not a variant of %s", vl_cstr(k), ename);
    } else {
      Value pv;
      vl_map_get(v, k, &pv);
      ok = conv_payload(d, pv, var, out);
      vl_release(pv);
    }
    vl_release(ks);
    return ok;
  }
  return cfail(d, "expected %s (a variant name or a one-key object), got %s",
               ename, jkind(v));
}

/* conv converts a parsed value `v` (borrowed) to type text `ty`, setting
 * *out (owned) — or records an error with the current path and returns
 * false. */
static bool conv(JD *d, Value v, const char *ty, Value *out) {
  size_t tl = strlen(ty);
  /* Erased or unknown: a generic parameter, `any`, an unwritten type. */
  if (tl == 0 || strcmp(ty, "any") == 0) {
    *out = vl_retain(v);
    return true;
  }
  if (ty[0] == '?') {
    if (v.t == VL_NIL) {
      *out = vl_nil();
      return true;
    }
    return conv(d, v, ty + 1, out);
  }
  if (ty[tl - 1] == '!') { /* T! decodes as T */
    char *t2 = vl_strdup_n(ty, (int64_t)tl - 1);
    bool ok = conv(d, v, t2, out);
    free(t2);
    return ok;
  }
  if (ty[0] == '[') {
    /* []T, or a fixed-length [N]T: an array either way. */
    int64_t close = close_bracket(ty, 0);
    if (close < 0) {
      *out = vl_retain(v);
      return true;
    }
    const char *et = ty + close + 1;
    if (!is_kind(v, O_SLICE))
      return cfail(d, "expected %s (an array), got %s", ty, jkind(v));
    VlSlice *s = (VlSlice *)v.u.o;
    /* A fixed length `[N]T` must be matched exactly. */
    if (close > 1) {
      long long want = strtoll(ty + 1, NULL, 10);
      if (want != (long long)s->len)
        return cfail(d, "expected %s (%lld elements), got %lld", ty, want,
                     (long long)s->len);
    }
    Value res = vl_slice_new(0);
    for (int64_t i = 0; i < s->len; i++) {
      Value e;
      if (!conv_elem(d, s->e[i], i, et, &e)) {
        vl_release(res);
        return false;
      }
      vl_slice_append(res, e);
    }
    *out = res;
    return true;
  }
  if (strncmp(ty, "map[", 4) == 0) {
    int64_t close = close_bracket(ty, 3);
    if (close < 0) {
      *out = vl_retain(v);
      return true;
    }
    char *kt = vl_strdup_n(ty + 4, close - 4);
    const char *vt = ty + close + 1;
    if (!is_kind(v, O_MAP)) {
      free(kt);
      return cfail(d, "expected %s (an object), got %s", ty, jkind(v));
    }
    VlMap *m = (VlMap *)v.u.o;
    Value res = vl_map_new();
    bool ok = true;
    for (int64_t i = 0; i < m->nents && ok; i++) {
      if (!m->ents[i].used) continue;
      Value key = m->ents[i].k;
      Value ck;
      /* An object key is always text, and encode wrote a non-str key as
       * its text: 80, true, Red. So the key is read back the same ways —
       * as a JSON number when it spells one and the key type accepts that,
       * as true/false for a bool, and otherwise as the string itself (an
       * enum variant's name, a rune, a newtype over str). */
      if (strcmp(kt, "str") == 0 || kt[0] == 0) {
        ck = vl_retain(key);
      } else {
        bool done = false;
        Value raw;
        JP kp = {vl_cstr(key), 0, vl_strlen(key), NULL, 0, 0};
        bool isnum = jnumber(&kp, &raw);
        if (isnum && kp.i != kp.n) {
          vl_release(raw);
          isnum = false;
        }
        if (isnum) {
          /* Tried on a scratch JD: a number the key type refuses is not
           * yet an error — the key may be text that merely looks numeric. */
          JD tmp = {NULL, 0, 0, NULL};
          done = conv(&tmp, raw, kt, &ck);
          free(tmp.path);
          free(tmp.err);
          vl_release(raw);
        }
        if (!done) {
          if (strcmp(kt, "bool") == 0 && strcmp(vl_cstr(key), "true") == 0)
            raw = vl_bool(true);
          else if (strcmp(kt, "bool") == 0 && strcmp(vl_cstr(key), "false") == 0)
            raw = vl_bool(false);
          else
            raw = vl_retain(key);
          size_t mark = d->len;
          path_push(d, vl_cstr(key), false);
          ok = conv(d, raw, kt, &ck);
          path_pop(d, mark);
          vl_release(raw);
        }
        if (!ok) break;
      }
      Value cv;
      ok = conv_field(d, m->ents[i].v, vl_cstr(key), vt, &cv);
      if (ok) vl_map_set(res, ck, cv);
      vl_release(ck);
    }
    free(kt);
    if (!ok) {
      vl_release(res);
      return false;
    }
    *out = res;
    return true;
  }
  if (strncmp(ty, "set[", 4) == 0 || strncmp(ty, "Box[", 4) == 0) {
    int64_t close = close_bracket(ty, 3);
    bool is_set = ty[0] == 's';
    if (close < 0 || (!is_set && vl_find_type("Box"))) goto named;
    char *et = vl_strdup_n(ty + 4, close - 4);
    bool ok;
    if (is_set) {
      if (!is_kind(v, O_SLICE)) {
        free(et);
        return cfail(d, "expected %s (an array), got %s", ty, jkind(v));
      }
      VlSlice *s = (VlSlice *)v.u.o;
      Value res = vl_set_new();
      ok = true;
      for (int64_t i = 0; i < s->len && ok; i++) {
        Value e;
        ok = conv_elem(d, s->e[i], i, et, &e);
        if (ok) {
          vl_map_set(res, e, vl_nil());
          vl_release(e);
        }
      }
      if (ok) *out = res;
      else vl_release(res);
    } else {
      Value inner;
      ok = conv(d, v, et, &inner);
      if (ok) *out = vl_box_new(inner);
    }
    free(et);
    return ok;
  }
  if (ty[0] == '(' || strcmp(ty, "tuple") == 0) {
    /* A tuple is an array. Written out, `(int,str)`, its members are typed
     * and counted; a field's type text says only "tuple" (the generator
     * does not spell the members), and then they are taken as they come. */
    if (!is_kind(v, O_SLICE))
      return cfail(d, "expected %s (an array), got %s", ty, jkind(v));
    VlSlice *s = (VlSlice *)v.u.o;
    if (ty[0] != '(') {
      *out = vl_tuple_new(s->e, (int)s->len);
      return true;
    }
    /* Split the members at the top-level commas of "(a,b,…)". */
    char *mem[64];
    int nm = 0;
    int64_t close = close_bracket(ty, 0);
    int depth = 0;
    int64_t start = 1;
    for (int64_t i = 1; close > 0 && i <= close && nm < 64; i++) {
      char c = ty[i];
      if (c == '[' || c == '(') depth++;
      else if ((c == ']' || c == ')') && i < close) depth--;
      if ((c == ',' && depth == 0) || i == close) {
        int64_t a = start, z = i;
        while (a < z && ty[a] == ' ') a++;
        while (z > a && ty[z - 1] == ' ') z--;
        mem[nm++] = vl_strdup_n(ty + a, z - a);
        start = i + 1;
      }
    }
    bool ok = true;
    if (nm != s->len) {
      ok = cfail(d, "expected %s (%d elements), got %lld", ty, nm,
                 (long long)s->len);
    }
    Value *e = (Value *)vl_alloc(sizeof(Value) * (size_t)(nm ? nm : 1));
    int done = 0;
    for (int i = 0; ok && i < nm; i++) {
      ok = conv_elem(d, s->e[i], i, mem[i], &e[i]);
      if (ok) done++;
    }
    if (ok) *out = vl_tuple_new(e, nm);
    for (int i = 0; i < done; i++) vl_release(e[i]);
    for (int i = 0; i < nm; i++) free(mem[i]);
    free(e);
    return ok;
  }
  /* ---- primitives */
  if (strcmp(ty, "int") == 0 || strcmp(ty, "i64") == 0 || sized_code(ty)) {
    int64_t n;
    if (v.t == VL_INT) n = v.u.i;
    else if (is_kind(v, O_DEC) && vl_dec_is_int(v) && vl_dec_to_int(v, &n)) {
    } else {
      return cfail(d, "expected %s, got %s", ty, jkind(v));
    }
    int sz = sized_code(ty);
    if (sz && (n < vl_sz_lo(sz) || n > vl_sz_hi(sz)))
      return cfail(d, "%lld is out of range for %s", (long long)n, ty);
    *out = vl_int(n);
    return true;
  }
  if (strcmp(ty, "float") == 0 || strcmp(ty, "f32") == 0) {
    if (v.t == VL_INT) *out = vl_float((double)v.u.i);
    else if (v.t == VL_FLOAT) *out = v;
    else if (is_kind(v, O_DEC)) {
      double f = vl_dec_to_float(v);
      if (isinf(f)) return cfail(d, "number out of range for %s", ty);
      *out = vl_float(f);
    } else return cfail(d, "expected %s, got %s", ty, jkind(v));
    return true;
  }
  if (strcmp(ty, "dec") == 0) {
    if (v.t == VL_INT) *out = vl_dec_from_int(v.u.i);
    else if (is_kind(v, O_DEC)) *out = vl_retain(v);
    else return cfail(d, "expected dec, got %s", jkind(v));
    return true;
  }
  if (strcmp(ty, "str") == 0) {
    if (!is_kind(v, O_STR)) return cfail(d, "expected str, got %s", jkind(v));
    *out = vl_retain(v);
    return true;
  }
  if (strcmp(ty, "bool") == 0) {
    if (v.t != VL_BOOL) return cfail(d, "expected bool, got %s", jkind(v));
    *out = v;
    return true;
  }
  if (strcmp(ty, "unit") == 0) {
    if (v.t != VL_NIL) return cfail(d, "expected null, got %s", jkind(v));
    *out = vl_unit();
    return true;
  }
  if (strcmp(ty, "rune") == 0) {
    /* A code point: 0..U+10FFFF, never a UTF-16 surrogate. */
    if (v.t == VL_INT) {
      if (v.u.i < 0 || v.u.i > 0x10FFFF || (v.u.i >= 0xD800 && v.u.i <= 0xDFFF))
        return cfail(d, "%lld is not a Unicode code point", (long long)v.u.i);
      *out = vl_rune((uint32_t)v.u.i);
      return true;
    }
    if (is_kind(v, O_STR)) {
      /* exactly one well-formed UTF-8 sequence */
      const unsigned char *s = (const unsigned char *)vl_cstr(v);
      int64_t n = vl_strlen(v);
      int len = n == 0 ? 0 : s[0] < 0x80 ? 1 : s[0] < 0xC2 ? 0
              : s[0] < 0xE0 ? 2 : s[0] < 0xF0 ? 3 : s[0] < 0xF5 ? 4 : 0;
      for (int k = 1; k < len && k < n; k++)
        if ((s[k] & 0xC0) != 0x80) len = 0;
      if (n > 0 && n == len) {
        uint32_t r = len == 1 ? s[0]
                   : len == 2 ? ((s[0] & 0x1Fu) << 6) | (s[1] & 0x3Fu)
                   : len == 3 ? ((s[0] & 0x0Fu) << 12) | ((s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu)
                   : ((s[0] & 0x07u) << 18) | ((s[1] & 0x3Fu) << 12) |
                     ((s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
        *out = vl_rune(r);
        return true;
      }
    }
    return cfail(d, "expected rune (a one-character string), got %s", jkind(v));
  }
named:;
  /* ---- a declared type: struct, newtype or enum. A generic instantiation
   * `Stack[int]` is its generic's descriptor (types are erased, §3.7). */
  const char *br = strchr(ty, '[');
  char *base = br && br != ty ? vl_strdup_n(ty, (int64_t)(br - ty)) : vl_strdup_n(ty, (int64_t)tl);
  const VlType *t = vl_find_type(base);
  bool ok;
  if (t && t->newtype) {
    Value inner;
    ok = conv(d, v, t->ftypes ? t->ftypes[0] : "", &inner);
    if (ok) *out = vl_newtype_wrap(t, inner);
  } else if (t) {
    ok = conv_struct(d, v, t, ty, out);
  } else if (vl_enum_exists(base)) {
    ok = conv_enum(d, v, base, out);
  } else {
    *out = vl_retain(v); /* a type parameter: erased, so taken as it is */
    ok = true;
  }
  free(base);
  return ok;
}

/* vl_json_decode parses `src` and, when `type` is not NULL, converts it to
 * that type. A failure of either kind is an error VALUE, never a throw. */
Value vl_json_decode(const char *type, Value src) {
  JP p = {vl_cstr(src), 0, vl_strlen(src), NULL, 0, 0};
  Value v;
  bool ok = jvalue(&p, &v);
  if (ok) {
    jskip(&p);
    if (p.i < p.n) {
      vl_release(v);
      ok = jfail(&p, "unexpected text after the value");
    }
  }
  if (!ok) {
    /* The position as a line and column, which is what an editor can find. */
    int64_t line = 1, col = 1;
    for (int64_t i = 0; i < p.at && i < p.n; i++) {
      if (p.s[i] == '\n') {
        line++;
        col = 1;
      } else {
        col++;
      }
    }
    char msg[256];
    snprintf(msg, sizeof msg, "bad JSON at line %lld, column %lld: %s",
             (long long)line, (long long)col, p.why ? p.why : "invalid");
    return vl_err_cstr(msg);
  }
  if (!type) return v;
  JD d = {NULL, 0, 0, NULL};
  Value out;
  bool cok = conv(&d, v, type, &out);
  vl_release(v);
  free(d.path);
  if (!cok) {
    Value e = vl_err_cstr(d.err);
    free(d.err);
    return e;
  }
  return out;
}
