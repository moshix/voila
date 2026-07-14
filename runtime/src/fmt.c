/* fmt.c — the printf verb engine (§13.1). `%f` on a dec is exact and rounds
 * half-even, on every platform. */
#include "voila_int.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static void bput(Value b, const char *s) {
  Value v = vl_str(s);
  vl_builder_write(b, v);
  vl_release(v);
}

static void bputn(Value b, const char *s, int64_t n) {
  Value v = vl_str_n(s, n);
  vl_builder_write(b, v);
  vl_release(v);
}

static int64_t want_int(Value v) {
  if (v.t == VL_INT) return v.u.i;
  if (v.t == VL_RUNE) return (int64_t)v.u.r;
  if (v.t == VL_OBJ && v.u.o->kind == O_DEC) {
    int64_t n;
    if (vl_dec_to_int(v, &n)) return n;
  }
  vl_throwf("FormatError", "integer verb applied to %s", vl_kind_name(v));
  return 0;
}

/* pad_num_into pads a NUMBER: the `0` flag zero-pads whatever the digits look
 * like. pad_into's digit test cannot serve here — a hex number may begin with
 * a letter ("C"), and %06X must still produce "00000C". */
static void pad_num_into(Value b, const char *s, int width, const char *flags) {
  int64_t n = (int64_t)strlen(s);
  bool left = strchr(flags, '-') != NULL;
  bool zero = strchr(flags, '0') != NULL;
  if (width <= n) {
    bput(b, s);
    return;
  }
  int64_t fill = width - n;
  char *p = (char *)vl_alloc((size_t)width + 1);
  if (left) {
    memcpy(p, s, (size_t)n);
    memset(p + n, ' ', (size_t)fill);
  } else if (zero) {
    int64_t o = 0;
    if (n > 0 && s[0] == '-') {
      p[o++] = '-';
      memset(p + o, '0', (size_t)fill);
      memcpy(p + o + fill, s + 1, (size_t)(n - 1));
    } else {
      memset(p, '0', (size_t)fill);
      memcpy(p + fill, s, (size_t)n);
    }
  } else {
    memset(p, ' ', (size_t)fill);
    memcpy(p + fill, s, (size_t)n);
  }
  p[width] = 0;
  bputn(b, p, width);
  free(p);
}

static void pad_into(Value b, const char *s, int width, const char *flags) {
  int64_t n = (int64_t)strlen(s);
  bool left = strchr(flags, '-') != NULL;
  bool zero = strchr(flags, '0') != NULL;
  if (width <= n) {
    bput(b, s);
    return;
  }
  int64_t fill = width - n;
  char *p = (char *)vl_alloc((size_t)width + 1);
  if (left) {
    memcpy(p, s, (size_t)n);
    memset(p + n, ' ', (size_t)fill);
  } else if (zero && n > 0 && (s[0] == '-' || isdigit((unsigned char)s[0]))) {
    int64_t o = 0;
    if (s[0] == '-') {
      p[o++] = '-';
      memset(p + o, '0', (size_t)fill);
      memcpy(p + o + fill, s + 1, (size_t)(n - 1));
    } else {
      memset(p, '0', (size_t)fill);
      memcpy(p + fill, s, (size_t)n);
    }
  } else {
    memset(p, ' ', (size_t)fill);
    memcpy(p + fill, s, (size_t)n);
  }
  p[width] = 0;
  bputn(b, p, width);
  free(p);
}

Value vl_sprintf_v(Value fmtv, Value *argv, int argc) {
  if (!(fmtv.t == VL_OBJ && fmtv.u.o->kind == O_STR))
    vl_abort("printf format must be str, got %s", vl_kind_name(fmtv));
  const char *f = vl_cstr(fmtv);
  int64_t fn = vl_strlen(fmtv);
  Value b = vl_builder_new();
  int ai = 0;

  for (int64_t i = 0; i < fn;) {
    if (f[i] != '%') {
      int64_t start = i;
      while (i < fn && f[i] != '%') i++;
      bputn(b, f + start, i - start);
      continue;
    }
    i++;
    if (i < fn && f[i] == '%') {
      bput(b, "%");
      i++;
      continue;
    }
    char flags[8] = {0};
    int nf = 0;
    while (i < fn && strchr("-+0 #", f[i]) && nf < 7) flags[nf++] = f[i++];

    int width = -1;
    if (i < fn && f[i] == '*') {
      if (ai >= argc) vl_throwf("FormatError", "printf: not enough arguments");
      width = (int)want_int(argv[ai++]);
      i++;
    } else {
      int w = 0;
      bool has = false;
      while (i < fn && isdigit((unsigned char)f[i])) { w = w * 10 + (f[i++] - '0'); has = true; }
      if (has) width = w;
    }
    int prec = -1;
    if (i < fn && f[i] == '.') {
      i++;
      if (i < fn && f[i] == '*') {
        if (ai >= argc) vl_throwf("FormatError", "printf: not enough arguments");
        prec = (int)want_int(argv[ai++]);
        i++;
      } else {
        int p = 0;
        while (i < fn && isdigit((unsigned char)f[i])) p = p * 10 + (f[i++] - '0');
        prec = p;
      }
    }
    if (i < fn && f[i] == '+') i++; /* %+v */
    if (i >= fn) vl_throwf("FormatError", "printf: missing verb at end of format");
    char verb = f[i++];

    if (ai >= argc) vl_throwf("FormatError", "printf: not enough arguments for format");
    Value v = argv[ai++];

    char spec[32], buf[512];
    switch (verb) {
    case 'd':
    case 'i':
    case 'u': {
      snprintf(spec, sizeof spec, "%%%s%s%slld", flags, width >= 0 ? "*" : "", "");
      char tmp[64];
      snprintf(tmp, sizeof tmp, "%lld", (long long)want_int(v));
      if (strchr(flags, '+') && tmp[0] != '-') {
        char t2[66];
        snprintf(t2, sizeof t2, "+%s", tmp);
        pad_into(b, t2, width, flags);
      } else {
        pad_into(b, tmp, width, flags);
      }
      break;
    }
    case 'f':
    case 'F': {
      int p = prec < 0 ? 6 : prec;
      if (v.t == VL_OBJ && v.u.o->kind == O_DEC) {
        char *s = vl_dec_fixed(v, p);
        pad_into(b, s, width, flags);
        free(s);
      } else {
        double d = v.t == VL_FLOAT ? v.u.f
                                   : (v.t == VL_INT ? (double)v.u.i : 0);
        if (v.t != VL_FLOAT && v.t != VL_INT)
          vl_throwf("FormatError", "%%f applied to %s", vl_kind_name(v));
        snprintf(buf, sizeof buf, "%.*f", p, d);
        pad_into(b, buf, width, flags);
      }
      break;
    }
    case 'e':
    case 'E':
    case 'g':
    case 'G': {
      double d = v.t == VL_FLOAT ? v.u.f
                 : v.t == VL_INT ? (double)v.u.i
                                 : vl_dec_to_float(v);
      snprintf(spec, sizeof spec, "%%.*%c", verb);
      snprintf(buf, sizeof buf, spec, prec < 0 ? 6 : prec, d);
      pad_into(b, buf, width, flags);
      break;
    }
    case 's': {
      Value s = vl_tostr(v);
      if (prec >= 0 && vl_strlen(s) > prec) {
        Value t = vl_str_n(vl_cstr(s), prec);
        vl_release(s);
        s = t;
      }
      pad_into(b, vl_cstr(s), width, flags);
      vl_release(s);
      break;
    }
    case 'q': {
      Value s = vl_tostr(v);
      Value qb = vl_builder_new();
      bput(qb, "\"");
      const char *p = vl_cstr(s);
      for (int64_t k = 0; k < vl_strlen(s); k++) {
        char c = p[k];
        if (c == '"') bput(qb, "\\\"");
        else if (c == '\\') bput(qb, "\\\\");
        else if (c == '\n') bput(qb, "\\n");
        else if (c == '\t') bput(qb, "\\t");
        else bputn(qb, &c, 1);
      }
      bput(qb, "\"");
      Value done = vl_builder_done(qb);
      pad_into(b, vl_cstr(done), width, flags);
      vl_release(done);
      vl_release(qb);
      vl_release(s);
      break;
    }
    case 'x':
    case 'X': {
      if (v.t == VL_OBJ && v.u.o->kind == O_STR) {
        bool ok;
        Value h = vl_str_call("hex", &v, 1, &ok);
        Value s = vl_tostr(h);
        if (verb == 'X') {
          char *up = vl_strdup_n(vl_cstr(s), vl_strlen(s));
          for (int64_t k = 0; up[k]; k++) up[k] = (char)toupper((unsigned char)up[k]);
          pad_into(b, up, width, flags);
          free(up);
        } else {
          pad_into(b, vl_cstr(s), width, flags);
        }
        vl_release(s);
        vl_release(h);
      } else {
        /* The `#` prefix and the `0` flag interact as they do in Go's fmt: the
         * zeros go AFTER the 0x, and the width counts the digits. C's own
         * "%#06llx" counts the prefix instead, so the padding is done here. */
        char digits[64];
        snprintf(digits, sizeof digits, verb == 'x' ? "%llx" : "%llX",
                 (unsigned long long)want_int(v));
        const char *pfx = "";
        if (strchr(flags, '#')) pfx = (verb == 'x') ? "0x" : "0X";
        if (strchr(flags, '0') && !strchr(flags, '-')) {
          bput(b, pfx);
          pad_num_into(b, digits, width, flags);
        } else {
          char tmp[72];
          snprintf(tmp, sizeof tmp, "%s%s", pfx, digits);
          pad_num_into(b, tmp, width, flags);
        }
      }
      break;
    }
    case 'o': {
      char tmp[64];
      snprintf(tmp, sizeof tmp, "%llo", (unsigned long long)want_int(v));
      pad_into(b, tmp, width, flags);
      break;
    }
    case 'b': {
      uint64_t n = (uint64_t)want_int(v);
      char tmp[72];
      int k = 0;
      if (n == 0) tmp[k++] = '0';
      char rev[72];
      int rn = 0;
      while (n) { rev[rn++] = (char)('0' + (int)(n & 1)); n >>= 1; }
      while (rn) tmp[k++] = rev[--rn];
      tmp[k] = 0;
      pad_into(b, tmp, width, flags);
      break;
    }
    case 'c': {
      Value r = v.t == VL_RUNE ? v : vl_rune((uint32_t)want_int(v));
      Value s = vl_tostr(r);
      pad_into(b, vl_cstr(s), width, flags);
      vl_release(s);
      break;
    }
    case 't': {
      if (v.t != VL_BOOL) vl_throwf("FormatError", "%%t applied to %s", vl_kind_name(v));
      pad_into(b, v.u.b ? "true" : "false", width, flags);
      break;
    }
    case 'v': {
      Value s = vl_tostr(v);
      pad_into(b, vl_cstr(s), width, flags);
      vl_release(s);
      break;
    }
    default:
      vl_throwf("FormatError", "unknown verb %%%c", verb);
    }
  }
  if (ai < argc)
    vl_throwf("FormatError", "printf: %d extra argument(s)", argc - ai);
  Value out = vl_builder_done(b);
  vl_release(b);
  return out;
}

void vl_fprint(FILE *f, Value *argv, int argc, bool newline) {
  Value b = vl_builder_new();
  for (int i = 0; i < argc; i++) {
    if (i) bput(b, " ");
    vl_builder_write(b, argv[i]);
  }
  Value s = vl_builder_done(b);
  fwrite(vl_cstr(s), 1, (size_t)vl_strlen(s), f);
  if (newline) fputc('\n', f);
  vl_release(s);
  vl_release(b);
}
