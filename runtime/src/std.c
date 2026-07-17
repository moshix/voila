/* std.c — the prelude and the std packages, plus PARSE, pattern matching,
 * and program start-up. */
#include "voila_int.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static Value g_args;

/* ---------------------------------------------------------------- files */

Value vl_file_new(FILE *f, const char *name, bool is_std) {
  VlFile *o = (VlFile *)vl_obj_new(O_FILE, sizeof(VlFile));
  o->f = f;
  o->name = vl_str(name);
  o->is_std = is_std;
  Value v;
  v.t = VL_OBJ;
  v.u.o = &o->hdr;
  return v;
}

static Value read_all_file(FILE *f) {
  Value b = vl_builder_new();
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    Value s = vl_str_n(buf, (int64_t)n);
    vl_builder_write(b, s);
    vl_release(s);
  }
  Value out = vl_builder_done(b);
  vl_release(b);
  return out;
}

Value vl_file_method(Value self, const char *name, Value *argv, int argc,
                     bool *ok) {
  VlFile *f = (VlFile *)self.u.o;
  *ok = true;
  if (eq(name, "close")) {
    if (!f->closed && !f->is_std && f->f) fclose(f->f);
    f->closed = true;
    f->f = NULL;
    return vl_unit();
  }
  if (eq(name, "name")) return vl_retain(f->name);
  if (eq(name, "read_all")) {
    if (!f->f) return vl_err(vl_str("file not open for reading"));
    return read_all_file(f->f);
  }
  if (eq(name, "lines")) {
    if (!f->f) vl_throwf("IOError", "file not open for reading");
    Value text = read_all_file(f->f);
    bool k;
    Value out = vl_str_call("lines", &text, 1, &k);
    vl_release(text);
    return out;
  }
  if (eq(name, "records")) {
    if (!f->f) vl_throwf("IOError", "file not open for reading");
    int64_t width = argv[0].u.i;
    if (width <= 0) vl_abort("record width must be positive");
    Value text = read_all_file(f->f);
    const char *s = vl_cstr(text);
    int64_t n = vl_strlen(text);
    Value out = vl_slice_new(0);
    if (memchr(s, '\n', (size_t)n)) {
      /* line-oriented dataset: each line padded or truncated to width */
      int64_t start = 0;
      for (int64_t i = 0; i <= n; i++) {
        if (i == n && i == start) break;
        if (i < n && s[i] != '\n') continue;
        int64_t len = i - start;
        if (len && s[start + len - 1] == '\r') len--;
        char *rec = (char *)vl_alloc((size_t)width + 1);
        memset(rec, ' ', (size_t)width);
        memcpy(rec, s + start, (size_t)(len < width ? len : width));
        vl_slice_append(out, vl_str_n(rec, width));
        free(rec);
        start = i + 1;
      }
    } else {
      for (int64_t i = 0; i + width <= n; i += width)
        vl_slice_append(out, vl_str_n(s + i, width));
    }
    vl_release(text);
    return out;
  }
  if (eq(name, "write") || eq(name, "writeln")) {
    if (!f->f) return vl_err(vl_str("file not open for writing"));
    vl_fprint(f->f, argv, argc, eq(name, "writeln"));
    return vl_unit();
  }
  *ok = false;
  return vl_nil();
}

/* ---------------------------------------------------------------- prelude */

#define BI(n) static Value n(Value *argv, int argc, VlFrame *up)
#define UNUSED (void)argv, (void)argc, (void)up

BI(bi_len) { UNUSED; return vl_int(vl_len(argv[0])); }

BI(bi_append) {
  UNUSED;
  for (int i = 1; i < argc; i++) vl_slice_append(argv[0], vl_retain(argv[i]));
  return vl_retain(argv[0]);
}

BI(bi_close) { UNUSED; vl_chan_close(argv[0]); return vl_unit(); }

BI(bi_err) { UNUSED; return vl_err(argc ? argv[0] : vl_str("")); }
BI(bi_errf) { UNUSED; return vl_errf(argv, argc); }

BI(bi_assert) {
  UNUSED;
  if (!vl_truthy(argv[0])) {
    if (argc > 1) {
      Value m = vl_tostr(argv[1]);
      vl_abort("assertion failed: %s", vl_cstr(m));
    }
    vl_abort("assertion failed");
  }
  return vl_unit();
}

static Value minmax(Value *argv, int argc, bool want_min) {
  Value *vals = argv;
  int n = argc;
  if (argc == 1 && argv[0].t == VL_OBJ && argv[0].u.o->kind == O_SLICE) {
    VlSlice *s = (VlSlice *)argv[0].u.o;
    vals = s->e;
    n = (int)s->len;
  }
  if (n == 0) vl_abort("min/max of nothing");
  Value best = vals[0];
  for (int i = 1; i < n; i++) {
    Value r = vl_cmplt(vals[i], best);
    bool less = r.t == VL_BOOL && r.u.b;
    vl_release(r);
    if (less == want_min) best = vals[i];
  }
  return vl_retain(best);
}

BI(bi_min) { UNUSED; return minmax(argv, argc, true); }
BI(bi_max) { UNUSED; return minmax(argv, argc, false); }

BI(bi_abs) {
  UNUSED;
  Value v = argv[0];
  if (v.t == VL_INT) return vl_int(v.u.i < 0 ? -v.u.i : v.u.i);
  if (v.t == VL_FLOAT) return vl_float(fabs(v.u.f));
  if (v.t == VL_OBJ && v.u.o->kind == O_DEC) return vl_dec_abs(v);
  vl_abort("abs() of %s", vl_kind_name(v));
  return vl_nil();
}

BI(bi_sum) {
  UNUSED;
  VlSlice *s = (VlSlice *)argv[0].u.o;
  Value acc = vl_int(0);
  for (int64_t i = 0; i < s->len; i++) {
    Value t = vl_add(acc, s->e[i]);
    vl_release(acc);
    acc = t;
  }
  return acc;
}

BI(bi_avg) {
  UNUSED;
  VlSlice *s = (VlSlice *)argv[0].u.o;
  if (s->len == 0) vl_abort("avg() needs a non-empty slice");
  Value acc = vl_int(0);
  for (int64_t i = 0; i < s->len; i++) {
    Value t = vl_add(acc, s->e[i]);
    vl_release(acc);
    acc = t;
  }
  Value n = vl_int(s->len);
  Value out = vl_div(acc, n);
  vl_release(acc);
  return out;
}

static Value rounder(Value v, const char *mode) {
  if (v.t == VL_INT) return v;
  if (v.t == VL_FLOAT) {
    double f = v.u.f, r = 0;
    if (eq(mode, "round")) r = nearbyint(f); /* banker's rounding */
    else if (eq(mode, "floor")) r = floor(f);
    else if (eq(mode, "ceil")) r = ceil(f);
    else r = trunc(f);
    return vl_int((int64_t)r);
  }
  if (v.t == VL_OBJ && v.u.o->kind == O_DEC) {
    Value d;
    if (eq(mode, "round")) d = vl_dec_round(v, 0);
    else if (eq(mode, "floor")) d = vl_dec_floor(v);
    else if (eq(mode, "ceil")) d = vl_dec_ceil(v);
    else d = vl_dec_trunc(v);
    int64_t n;
    if (!vl_dec_to_int(d, &n)) vl_throwf("OverflowError", "%s of dec out of int range", mode);
    vl_release(d);
    return vl_int(n);
  }
  vl_abort("%s() of %s", mode, vl_kind_name(v));
  return vl_nil();
}

BI(bi_round) { UNUSED; return rounder(argv[0], "round"); }
BI(bi_floor) { UNUSED; return rounder(argv[0], "floor"); }
BI(bi_ceil) { UNUSED; return rounder(argv[0], "ceil"); }
BI(bi_trunc) { UNUSED; return rounder(argv[0], "trunc"); }

BI(bi_round_to) {
  UNUSED;
  int places = (int)argv[1].u.i;
  Value v = argv[0];
  if (v.t == VL_OBJ && v.u.o->kind == O_DEC) return vl_dec_round(v, places);
  if (v.t == VL_FLOAT) {
    double shift = pow(10, places);
    return vl_float(nearbyint(v.u.f * shift) / shift);
  }
  return vl_retain(v);
}

/* safe conversions: to_int(x) → ?int (§3.2) */
static Value safe_conv(const char *type, Value *argv, int argc) {
  VlCtx *c = vl_ctx();
  VlHandler h;
  Value out;
  if (VL_TRY(h)) {
    out = vl_nil();
  } else {
    Value base = argc > 1 ? argv[1] : vl_nil();
    out = vl_conv(type, argv[0], argc > 1 ? &base : NULL);
    vl_eh_pop();
  }
  (void)c;
  return out;
}

#define SAFE(T)                                                                \
  BI(bi_to_##T) {                                                              \
    UNUSED;                                                                    \
    return safe_conv(#T, argv, argc);                                          \
  }
SAFE(int)
SAFE(i8)
SAFE(i16)
SAFE(i32)
SAFE(i64)
SAFE(u8)
SAFE(u16)
SAFE(u32)
SAFE(u64)
SAFE(byte)
SAFE(float)
SAFE(dec)
SAFE(rune)
#undef SAFE

static int64_t raw_int(Value v) {
  if (v.t == VL_INT) return v.u.i;
  if (v.t == VL_RUNE) return (int64_t)v.u.r;
  if (v.t == VL_FLOAT) return (int64_t)v.u.f;
  vl_abort("expected integer, got %s", vl_kind_name(v));
  return 0;
}

#define WRAP(T, CT)                                                            \
  BI(bi_wrap_##T) {                                                            \
    UNUSED;                                                                    \
    return vl_int((int64_t)(CT)raw_int(argv[0]));                              \
  }
WRAP(i8, int8_t)
WRAP(i16, int16_t)
WRAP(i32, int32_t)
WRAP(i64, int64_t)
WRAP(u8, uint8_t)
WRAP(u16, uint16_t)
WRAP(u32, uint32_t)
#undef WRAP
BI(bi_wrap_u64) { UNUSED; return vl_int(raw_int(argv[0])); }

static Value sat(Value v, int64_t lo, int64_t hi) {
  int64_t n = raw_int(v);
  if (n < lo) n = lo;
  if (n > hi) n = hi;
  return vl_int(n);
}
BI(bi_sat_i8) { UNUSED; return sat(argv[0], -128, 127); }
BI(bi_sat_i16) { UNUSED; return sat(argv[0], -32768, 32767); }
BI(bi_sat_i32) { UNUSED; return sat(argv[0], -2147483648LL, 2147483647LL); }
BI(bi_sat_i64) { UNUSED; return sat(argv[0], INT64_MIN, INT64_MAX); }
BI(bi_sat_u8) { UNUSED; return sat(argv[0], 0, 255); }
BI(bi_sat_u16) { UNUSED; return sat(argv[0], 0, 65535); }
BI(bi_sat_u32) { UNUSED; return sat(argv[0], 0, 4294967295LL); }
BI(bi_sat_u64) { UNUSED; return sat(argv[0], 0, INT64_MAX); }

BI(bi_wrap_add) { UNUSED; return vl_int((int64_t)((uint64_t)raw_int(argv[0]) + (uint64_t)raw_int(argv[1]))); }
BI(bi_wrap_mul) { UNUSED; return vl_int((int64_t)((uint64_t)raw_int(argv[0]) * (uint64_t)raw_int(argv[1]))); }

BI(bi_range) {
  UNUSED;
  if (argc == 1) return vl_range_new(0, raw_int(argv[0]), 1, false);
  if (argc == 2) return vl_range_new(raw_int(argv[0]), raw_int(argv[1]), 1, false);
  return vl_range_new(raw_int(argv[0]), raw_int(argv[1]), raw_int(argv[2]), false);
}

BI(bi_zip) {
  UNUSED;
  VlSlice *a = (VlSlice *)argv[0].u.o;
  VlSlice *b = (VlSlice *)argv[1].u.o;
  int64_t n = a->len < b->len ? a->len : b->len;
  Value out = vl_slice_new(0);
  for (int64_t i = 0; i < n; i++) {
    Value pair[2] = {a->e[i], b->e[i]};
    vl_slice_append(out, vl_tuple_new(pair, 2));
  }
  return out;
}

BI(bi_enumerate) {
  UNUSED;
  VlSlice *s = (VlSlice *)argv[0].u.o;
  Value out = vl_slice_new(0);
  for (int64_t i = 0; i < s->len; i++) {
    Value pair[2] = {vl_int(i), s->e[i]};
    vl_slice_append(out, vl_tuple_new(pair, 2));
  }
  return out;
}

BI(bi_keys) { UNUSED; return vl_map_keys(argv[0]); }
BI(bi_values) { UNUSED; return vl_map_values(argv[0]); }
BI(bi_shared) { UNUSED; return vl_shared(argv[0]); }
BI(bi_cell) { UNUSED; return vl_cell(argv[0]); }
BI(bi_weak) { UNUSED; return vl_weak(argv[0]); }

BI(bi_set) {
  UNUSED;
  Value s = vl_set_new();
  if (argc == 1 && argv[0].t == VL_OBJ && argv[0].u.o->kind == O_SLICE) {
    VlSlice *sl = (VlSlice *)argv[0].u.o;
    for (int64_t i = 0; i < sl->len; i++) vl_map_set(s, sl->e[i], vl_nil());
    return s;
  }
  for (int i = 0; i < argc; i++) vl_map_set(s, argv[i], vl_nil());
  return s;
}

BI(bi_printf) {
  UNUSED;
  Value s = vl_sprintf_v(argv[0], argv + 1, argc - 1);
  fwrite(vl_cstr(s), 1, (size_t)vl_strlen(s), stdout);
  vl_release(s);
  return vl_unit();
}

BI(bi_sprintf) { UNUSED; return vl_sprintf_v(argv[0], argv + 1, argc - 1); }

BI(bi_eprintf) {
  UNUSED;
  Value s = vl_sprintf_v(argv[0], argv + 1, argc - 1);
  fwrite(vl_cstr(s), 1, (size_t)vl_strlen(s), stderr);
  vl_release(s);
  return vl_unit();
}

BI(bi_fprintf) {
  UNUSED;
  VlFile *f = (VlFile *)argv[0].u.o;
  Value s = vl_sprintf_v(argv[1], argv + 2, argc - 2);
  fwrite(vl_cstr(s), 1, (size_t)vl_strlen(s), f->f ? f->f : stderr);
  vl_release(s);
  return vl_unit();
}

BI(bi_print) { UNUSED; vl_fprint(stdout, argv, argc, false); return vl_unit(); }
BI(bi_println) { UNUSED; vl_fprint(stdout, argv, argc, true); return vl_unit(); }

BI(bi_comma) {
  UNUSED;
  Value s = vl_tostr(argv[0]);
  const char *p = vl_cstr(s);
  bool neg = *p == '-';
  if (neg) p++;
  const char *dot = strchr(p, '.');
  int64_t ilen = dot ? (int64_t)(dot - p) : (int64_t)strlen(p);
  Value b = vl_builder_new();
  if (neg) {
    Value m = vl_str("-");
    vl_builder_write(b, m);
    vl_release(m);
  }
  for (int64_t i = 0; i < ilen; i++) {
    if (i > 0 && (ilen - i) % 3 == 0) {
      Value c = vl_str(",");
      vl_builder_write(b, c);
      vl_release(c);
    }
    Value c = vl_str_n(p + i, 1);
    vl_builder_write(b, c);
    vl_release(c);
  }
  if (dot) {
    Value rest = vl_str(dot);
    vl_builder_write(b, rest);
    vl_release(rest);
  }
  Value out = vl_builder_done(b);
  vl_release(b);
  vl_release(s);
  return out;
}

BI(bi_bytes_human) {
  UNUSED;
  int64_t n = raw_int(argv[0]);
  char buf[64];
  if (n < 1024) snprintf(buf, sizeof buf, "%lld B", (long long)n);
  else {
    const char *u = "KMGTPE";
    double d = (double)n / 1024.0;
    int i = 0;
    while (d >= 1024.0 && i < 5) { d /= 1024.0; i++; }
    snprintf(buf, sizeof buf, "%.1f %ciB", d, u[i]);
  }
  return vl_str(buf);
}

/* ---------------------------------------------------------------- os */

BI(os_read) {
  UNUSED;
  FILE *f = fopen(vl_cstr(argv[0]), "rb");
  if (!f) {
    Value m = vl_str("no such file or directory");
    Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
    vl_release(m);
    return e;
  }
  Value s = read_all_file(f);
  fclose(f);
  return s;
}

BI(os_write) {
  UNUSED;
  FILE *f = fopen(vl_cstr(argv[0]), "wb");
  if (!f) {
    Value m = vl_str("cannot write file");
    Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
    vl_release(m);
    return e;
  }
  Value s = vl_tostr(argv[1]);
  fwrite(vl_cstr(s), 1, (size_t)vl_strlen(s), f);
  fclose(f);
  vl_release(s);
  return vl_unit();
}

BI(os_open) {
  UNUSED;
  FILE *f = fopen(vl_cstr(argv[0]), "rb");
  if (!f) {
    Value m = vl_str("no such file or directory");
    Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
    vl_release(m);
    return e;
  }
  return vl_file_new(f, vl_cstr(argv[0]), false);
}

BI(os_create) {
  UNUSED;
  FILE *f = fopen(vl_cstr(argv[0]), "wb");
  if (!f) {
    Value m = vl_str("cannot create file");
    Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
    vl_release(m);
    return e;
  }
  return vl_file_new(f, vl_cstr(argv[0]), false);
}

BI(os_exists) { UNUSED; return vl_bool(access(vl_cstr(argv[0]), F_OK) == 0); }
BI(os_remove) { UNUSED; remove(vl_cstr(argv[0])); return vl_unit(); }
BI(os_args) { UNUSED; return vl_retain(g_args); }

/* os.isdir, os.listdir and os.run exist for one reason: the Voila compiler is
 * written in Voila. It must walk a package directory and it must invoke `cc`.
 * Without these three the toolchain could not build itself. */
BI(os_isdir) {
  UNUSED;
  struct stat st;
  if (stat(vl_cstr(argv[0]), &st) != 0) return vl_bool(false);
  return vl_bool(S_ISDIR(st.st_mode));
}

/* os.listdir(dir) -> []str!  Entry names only (never "." or ".."), sorted, so
 * a package's merge order does not depend on the file system. */
static int cmp_cstr(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

BI(os_listdir) {
  UNUSED;
  DIR *d = opendir(vl_cstr(argv[0]));
  if (!d) {
    Value m = vl_str("cannot read directory");
    Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
    vl_release(m);
    return e;
  }
  char **names = NULL;
  int n = 0, cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (eq(ent->d_name, ".") || eq(ent->d_name, "..")) continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 16;
      names = (char **)realloc(names, (size_t)cap * sizeof(char *));
      if (!names) vl_abort("out of memory");
    }
    names[n++] = vl_strdup_n(ent->d_name, (int64_t)strlen(ent->d_name));
  }
  closedir(d);
  qsort(names, (size_t)n, sizeof(char *), cmp_cstr);
  Value out = vl_slice_new(0);
  for (int i = 0; i < n; i++) {
    Value s = vl_str(names[i]);
    vl_slice_append(out, s); /* takes ownership */
    free(names[i]);
  }
  free(names);
  return out;
}

/* os.run(prog, args) -> int.  Runs a program with stdio inherited and returns
 * its exit status (128+signal when it was killed). */
BI(os_run) {
  UNUSED;
  Value av = argv[1];
  int n = (int)vl_len(av);
  char **cargv = (char **)calloc((size_t)n + 2, sizeof(char *));
  if (!cargv) vl_abort("out of memory");
  cargv[0] = vl_strdup_n(vl_cstr(argv[0]), vl_strlen(argv[0]));
  Value *data = vl_slice_data(av);
  for (int i = 0; i < n; i++) {
    Value s = vl_tostr(data[i]);
    cargv[i + 1] = vl_strdup_n(vl_cstr(s), vl_strlen(s));
    vl_release(s);
  }
  cargv[n + 1] = NULL;

  /* The child inherits our stdout. Flush first, or our buffered `say` output
   * would appear AFTER the child's — the interpreter, whose stdout is
   * unbuffered, would order them the other way (§12 Equivalence). */
  fflush(NULL);
  pid_t pid = fork();
  if (pid < 0) {
    for (int i = 0; i <= n; i++) free(cargv[i]);
    free(cargv);
    return vl_int(-1);
  }
  if (pid == 0) {
    execvp(cargv[0], cargv);
    _exit(127); /* exec failed: the shell's convention */
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) break;
  }
  for (int i = 0; i <= n; i++) free(cargv[i]);
  free(cargv);
  if (WIFEXITED(status)) return vl_int(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return vl_int(128 + WTERMSIG(status));
  return vl_int(-1);
}

/* os.exe returns the path of the RUNNING executable. The compiler needs it to
 * key its build cache on its own identity: without it, upgrading the toolchain
 * would silently keep running binaries the previous compiler produced. */
BI(os_exe) {
  UNUSED;
  char buf[4096];
#if defined(__APPLE__)
  uint32_t n = (uint32_t)sizeof buf;
  if (_NSGetExecutablePath(buf, &n) != 0) return vl_str("");
  return vl_str(buf);
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n < 0) return vl_str("");
  buf[n] = 0;
  return vl_str(buf);
#endif
}

BI(os_mtime) {
  UNUSED;
  struct stat st;
  if (stat(vl_cstr(argv[0]), &st) != 0) return vl_int(0);
  return vl_int((int64_t)st.st_mtime);
}

BI(os_size) {
  UNUSED;
  struct stat st;
  if (stat(vl_cstr(argv[0]), &st) != 0) return vl_int(-1);
  return vl_int((int64_t)st.st_size);
}

BI(os_pid) {
  UNUSED;
  return vl_int((int64_t)getpid());
}

/* os.hostname -> str!  (IOError when the kernel cannot say). */
BI(os_hostname) {
  UNUSED;
  char buf[256];
  if (gethostname(buf, sizeof buf) != 0) {
    Value m = vl_str("cannot determine the hostname");
    Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
    vl_release(m);
    return e;
  }
  buf[sizeof buf - 1] = 0;
  return vl_str(buf);
}

BI(os_cwd) {
  UNUSED;
  char buf[4096];
  if (!getcwd(buf, sizeof buf)) {
    Value m = vl_str("cannot determine the working directory");
    Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
    vl_release(m);
    return e;
  }
  return vl_str(buf);
}

BI(os_env) {
  UNUSED;
  const char *v = getenv(vl_cstr(argv[0]));
  return v ? vl_str(v) : vl_nil();
}

BI(os_exit) {
  UNUSED;
  fflush(stdout);
  exit(argc ? (int)raw_int(argv[0]) : 0);
}

BI(os_on_abort) { UNUSED; vl_set_abort_handler(argv[0]); return vl_unit(); }

/* ---------------------------------------------------------------- math */

static double asdouble(Value v) {
  if (v.t == VL_FLOAT) return v.u.f;
  if (v.t == VL_INT) return (double)v.u.i;
  if (v.t == VL_OBJ && v.u.o->kind == O_DEC) return vl_dec_to_float(v);
  vl_abort("expected number, got %s", vl_kind_name(v));
  return 0;
}

#define M1(NAME, CFN)                                                          \
  BI(math_##NAME) {                                                            \
    UNUSED;                                                                    \
    return vl_float(CFN(asdouble(argv[0])));                                   \
  }
M1(sqrt, sqrt)
M1(sin, sin)
M1(cos, cos)
M1(tan, tan)
M1(asin, asin)
M1(acos, acos)
M1(atan, atan)
M1(ln, log)
M1(log2, log2)
M1(log10, log10)
M1(exp, exp)
#undef M1

BI(math_pow) { UNUSED; return vl_float(pow(asdouble(argv[0]), asdouble(argv[1]))); }
BI(math_hypot) { UNUSED; return vl_float(hypot(asdouble(argv[0]), asdouble(argv[1]))); }
BI(math_mod) { UNUSED; return vl_float(fmod(asdouble(argv[0]), asdouble(argv[1]))); }

/* ---------------------------------------------------------------- time */

BI(time_now) { UNUSED; return vl_instant(vl_now_ns()); }

BI(time_since) {
  UNUSED;
  return vl_dur(vl_now_ns() - argv[0].u.i);
}

BI(time_sleep) {
  UNUSED;
  vl_sleep_ns(argv[0].t == VL_DUR ? argv[0].u.i : raw_int(argv[0]));
  return vl_unit();
}

BI(time_after) {
  UNUSED;
  return vl_time_after(argv[0].t == VL_DUR ? argv[0].u.i : raw_int(argv[0]));
}

BI(time_millis) { UNUSED; return vl_dur(raw_int(argv[0]) * 1000000LL); }
BI(time_seconds) { UNUSED; return vl_dur(raw_int(argv[0]) * 1000000000LL); }

/* ---------------------------------------------------------------- log */

static Value log_at(const char *tag, Value *argv, int argc) {
  fprintf(stderr, "%s ", tag);
  vl_fprint(stderr, argv, argc, true);
  return vl_unit();
}
BI(log_info) { UNUSED; return log_at("INFO ", argv, argc); }
BI(log_warn) { UNUSED; return log_at("WARN ", argv, argc); }
BI(log_error) { UNUSED; return log_at("ERROR", argv, argc); }
BI(log_debug) { UNUSED; return log_at("DEBUG", argv, argc); }
BI(log_flush) { UNUSED; fflush(stderr); return vl_unit(); }

/* ---------------------------------------------------------------- sort */

BI(sort_sort) { UNUSED; vl_sort_slice(argv[0], vl_nil()); return vl_unit(); }
BI(sort_sort_by) { UNUSED; vl_sort_slice(argv[0], argv[1]); return vl_unit(); }
BI(sort_reversed) {
  UNUSED;
  VlSlice *s = (VlSlice *)argv[0].u.o;
  Value out = vl_slice_new(0);
  for (int64_t i = s->len - 1; i >= 0; i--) vl_slice_append(out, vl_retain(s->e[i]));
  return out;
}

/* ---------------------------------------------------------------- json */

static void json_enc(Value b, Value v, const char *indent, const char *cur);

static void jput(Value b, const char *s) {
  Value t = vl_str(s);
  vl_builder_write(b, t);
  vl_release(t);
}

static void json_str(Value b, Value s) {
  Value sv = vl_tostr(s);
  const char *p = vl_cstr(sv);
  int64_t n = vl_strlen(sv);
  jput(b, "\"");
  for (int64_t i = 0; i < n; i++) {
    char c = p[i];
    if (c == '"') jput(b, "\\\"");
    else if (c == '\\') jput(b, "\\\\");
    else if (c == '\n') jput(b, "\\n");
    else if (c == '\t') jput(b, "\\t");
    else if (c == '\r') jput(b, "\\r");
    else {
      Value cv = vl_str_n(&c, 1);
      vl_builder_write(b, cv);
      vl_release(cv);
    }
  }
  jput(b, "\"");
  vl_release(sv);
}

static void json_enc(Value b, Value v, const char *indent, const char *cur) {
  char pad[256];
  const char *nl = indent[0] ? "\n" : "";
  snprintf(pad, sizeof pad, "%s%s", cur, indent);
  const char *sep = indent[0] ? ": " : ":";

  switch (v.t) {
  case VL_NIL: jput(b, "null"); return;
  case VL_BOOL: jput(b, v.u.b ? "true" : "false"); return;
  case VL_INT:
  case VL_FLOAT: {
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
    if (s->len == 0) { jput(b, "[]"); return; }
    jput(b, "[");
    jput(b, nl);
    for (int64_t i = 0; i < s->len; i++) {
      jput(b, pad);
      json_enc(b, s->e[i], indent, pad);
      if (i < s->len - 1) jput(b, ",");
      jput(b, nl);
    }
    jput(b, cur);
    jput(b, "]");
    return;
  }
  case O_MAP: {
    VlMap *m = (VlMap *)v.u.o;
    if (m->count == 0) { jput(b, "{}"); return; }
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
    jput(b, "{");
    jput(b, nl);
    for (int i = 0; i < s->type->nfields; i++) {
      jput(b, pad);
      Value k = vl_str(s->type->fields[i]);
      json_str(b, k);
      vl_release(k);
      jput(b, sep);
      json_enc(b, s->f[i], indent, pad);
      if (i < s->type->nfields - 1) jput(b, ",");
      jput(b, nl);
    }
    jput(b, cur);
    jput(b, "}");
    return;
  }
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

BI(json_encode) {
  UNUSED;
  Value b = vl_builder_new();
  json_enc(b, argv[0], "", "");
  Value out = vl_builder_done(b);
  vl_release(b);
  return out;
}

BI(json_pretty) {
  UNUSED;
  Value b = vl_builder_new();
  json_enc(b, argv[0], "  ", "");
  Value out = vl_builder_done(b);
  vl_release(b);
  return out;
}

/* json decode */
typedef struct {
  const char *s;
  int64_t i, n;
} JP;

static void jskip(JP *p) {
  while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\n' ||
                         p->s[p->i] == '\t' || p->s[p->i] == '\r'))
    p->i++;
}

static bool jvalue(JP *p, Value *out);

static bool jstring(JP *p, Value *out) {
  if (p->s[p->i] != '"') return false;
  p->i++;
  Value b = vl_builder_new();
  while (p->i < p->n && p->s[p->i] != '"') {
    char c = p->s[p->i++];
    if (c == '\\' && p->i < p->n) {
      char e = p->s[p->i++];
      if (e == 'n') c = '\n';
      else if (e == 't') c = '\t';
      else if (e == 'r') c = '\r';
      else c = e;
    }
    Value cv = vl_str_n(&c, 1);
    vl_builder_write(b, cv);
    vl_release(cv);
  }
  if (p->i >= p->n) { vl_release(b); return false; }
  p->i++;
  *out = vl_builder_done(b);
  vl_release(b);
  return true;
}

static bool jvalue(JP *p, Value *out) {
  jskip(p);
  if (p->i >= p->n) return false;
  char c = p->s[p->i];
  if (c == '{') {
    p->i++;
    Value m = vl_map_new();
    jskip(p);
    if (p->i < p->n && p->s[p->i] == '}') { p->i++; *out = m; return true; }
    for (;;) {
      jskip(p);
      Value k;
      if (!jstring(p, &k)) { vl_release(m); return false; }
      jskip(p);
      if (p->i >= p->n || p->s[p->i] != ':') { vl_release(k); vl_release(m); return false; }
      p->i++;
      Value v;
      if (!jvalue(p, &v)) { vl_release(k); vl_release(m); return false; }
      vl_map_set(m, k, v);
      vl_release(k);
      jskip(p);
      if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
      if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
      vl_release(m);
      return false;
    }
    *out = m;
    return true;
  }
  if (c == '[') {
    p->i++;
    Value a = vl_slice_new(0);
    jskip(p);
    if (p->i < p->n && p->s[p->i] == ']') { p->i++; *out = a; return true; }
    for (;;) {
      Value v;
      if (!jvalue(p, &v)) { vl_release(a); return false; }
      vl_slice_append(a, v);
      jskip(p);
      if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
      if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
      vl_release(a);
      return false;
    }
    *out = a;
    return true;
  }
  if (c == '"') return jstring(p, out);
  if (strncmp(p->s + p->i, "true", 4) == 0) { p->i += 4; *out = vl_bool(true); return true; }
  if (strncmp(p->s + p->i, "false", 5) == 0) { p->i += 5; *out = vl_bool(false); return true; }
  if (strncmp(p->s + p->i, "null", 4) == 0) { p->i += 4; *out = vl_nil(); return true; }
  /* number: integers stay int, fractions become dec (money survives) */
  int64_t start = p->i;
  bool isfloat = false;
  if (p->s[p->i] == '-' || p->s[p->i] == '+') p->i++;
  while (p->i < p->n && (isdigit((unsigned char)p->s[p->i]) || p->s[p->i] == '.' ||
                         p->s[p->i] == 'e' || p->s[p->i] == 'E' ||
                         p->s[p->i] == '-' || p->s[p->i] == '+')) {
    if (p->s[p->i] == '.' || p->s[p->i] == 'e' || p->s[p->i] == 'E') isfloat = true;
    p->i++;
  }
  if (p->i == start) return false;
  char *tmp = vl_strdup_n(p->s + start, p->i - start);
  if (isfloat) *out = vl_dec_parse(tmp);
  else *out = vl_int(strtoll(tmp, NULL, 10));
  free(tmp);
  return true;
}

BI(json_decode) {
  UNUSED;
  const char *type = NULL;
  Value src = argv[0];
  if (argc > 1) { /* json.decode[T](s): the generator passes T's name first */
    type = vl_cstr(argv[0]);
    src = argv[1];
  }
  JP p = {vl_cstr(src), 0, vl_strlen(src)};
  Value v;
  if (!jvalue(&p, &v)) return vl_err(vl_str("bad JSON"));
  if (!type) return v;
  const VlType *t = vl_find_type(type);
  if (!t || v.t != VL_OBJ || v.u.o->kind != O_MAP) return v;
  Value *named = (Value *)vl_alloc(sizeof(Value) * (size_t)(t->nfields ? t->nfields : 1));
  const char **names = (const char **)vl_alloc(sizeof(char *) * (size_t)(t->nfields ? t->nfields : 1));
  int n = 0;
  for (int i = 0; i < t->nfields; i++) {
    Value k = vl_str(t->fields[i]);
    Value fv;
    if (vl_map_get(v, k, &fv)) {
      names[n] = t->fields[i];
      named[n] = fv;
      n++;
    }
    vl_release(k);
  }
  Value out = vl_struct_new(type, NULL, 0, names, named, n);
  for (int i = 0; i < n; i++) vl_release(named[i]);
  free(named);
  free((void *)names);
  vl_release(v);
  return out;
}

/* ---------------------------------------------------------------- rand */

BI(rand_int) { UNUSED; return vl_int(random() % (raw_int(argv[0]) ? raw_int(argv[0]) : 1)); }
BI(rand_float) { UNUSED; return vl_float((double)random() / (double)RAND_MAX); }
BI(rand_seed) { UNUSED; srandom((unsigned)raw_int(argv[0])); return vl_unit(); }

BI(uuid_new) {
  UNUSED;
  char buf[40];
  snprintf(buf, sizeof buf, "%08lx-%04lx-4%03lx-%04lx-%012lx",
           (unsigned long)random() & 0xffffffffUL,
           (unsigned long)random() & 0xffffUL,
           (unsigned long)random() & 0xfffUL,
           ((unsigned long)random() & 0x3fffUL) | 0x8000UL,
           (unsigned long)random() & 0xffffffffffffUL);
  return vl_str(buf);
}

/* ---------------------------------------------------------------- strings */

#define STRFN(n)                                                               \
  BI(sf_##n) {                                                                 \
    UNUSED;                                                                    \
    bool ok;                                                                   \
    Value r = vl_str_call(#n, argv, argc, &ok);                                \
    if (!ok) vl_abort("str.%s: bad call", #n);                                 \
    return r;                                                                  \
  }
STRFN(upper) STRFN(lower) STRFN(title) STRFN(trim) STRFN(trim_left)
STRFN(trim_right) STRFN(strip) STRFN(pad) STRFN(pad_left) STRFN(center)
STRFN(contains) STRFN(index) STRFN(last_index) STRFN(starts_with)
STRFN(ends_with) STRFN(count) STRFN(equal_fold) STRFN(substr) STRFN(left)
STRFN(right) STRFN(split) STRFN(split_n) STRFN(lines) STRFN(fields)
STRFN(word) STRFN(words) STRFN(subword) STRFN(delword) STRFN(join)
STRFN(replace) STRFN(replace_n) STRFN(repeat) STRFN(reverse) STRFN(translate)
STRFN(overlay) STRFN(insert) STRFN(delstr) STRFN(bytes) STRFN(from_bytes)
STRFN(runes) STRFN(from_runes) STRFN(rune_len) STRFN(is_digit) STRFN(is_alpha)
STRFN(is_space) STRFN(is_upper) STRFN(hex) STRFN(unhex) STRFN(b64) STRFN(unb64)
STRFN(find_any) STRFN(skip_any)
#undef STRFN

/* ---------------------------------------------------------------- table */

typedef struct {
  const char *name;
  VlFn fn;
} Entry;

static const Entry table[] = {
    /* prelude */
    {"len", bi_len}, {"append", bi_append}, {"close", bi_close},
    {"err", bi_err}, {"errf", bi_errf}, {"assert", bi_assert},
    {"min", bi_min}, {"max", bi_max}, {"abs", bi_abs}, {"sum", bi_sum},
    {"avg", bi_avg}, {"round", bi_round}, {"floor", bi_floor},
    {"ceil", bi_ceil}, {"trunc", bi_trunc}, {"round_to", bi_round_to},
    {"range", bi_range}, {"zip", bi_zip}, {"enumerate", bi_enumerate},
    {"keys", bi_keys}, {"values", bi_values}, {"shared", bi_shared},
    {"cell", bi_cell}, {"weak", bi_weak}, {"set", bi_set},
    {"printf", bi_printf}, {"sprintf", bi_sprintf},
    {"to_int", bi_to_int}, {"to_i8", bi_to_i8}, {"to_i16", bi_to_i16},
    {"to_i32", bi_to_i32}, {"to_i64", bi_to_i64}, {"to_u8", bi_to_u8},
    {"to_u16", bi_to_u16}, {"to_u32", bi_to_u32}, {"to_u64", bi_to_u64},
    {"to_byte", bi_to_byte}, {"to_float", bi_to_float}, {"to_dec", bi_to_dec},
    {"to_rune", bi_to_rune},
    {"wrap_i8", bi_wrap_i8}, {"wrap_i16", bi_wrap_i16},
    {"wrap_i32", bi_wrap_i32}, {"wrap_i64", bi_wrap_i64},
    {"wrap_u8", bi_wrap_u8}, {"wrap_u16", bi_wrap_u16},
    {"wrap_u32", bi_wrap_u32}, {"wrap_u64", bi_wrap_u64},
    {"sat_i8", bi_sat_i8}, {"sat_i16", bi_sat_i16}, {"sat_i32", bi_sat_i32},
    {"sat_i64", bi_sat_i64}, {"sat_u8", bi_sat_u8}, {"sat_u16", bi_sat_u16},
    {"sat_u32", bi_sat_u32}, {"sat_u64", bi_sat_u64},
    {"wrap_add", bi_wrap_add}, {"wrap_mul", bi_wrap_mul},

    /* fmt */
    {"fmt.printf", bi_printf}, {"fmt.sprintf", bi_sprintf},
    {"fmt.eprintf", bi_eprintf}, {"fmt.fprintf", bi_fprintf},
    {"fmt.print", bi_print}, {"fmt.println", bi_println},
    {"fmt.comma", bi_comma}, {"fmt.bytes", bi_bytes_human},
    {"fmt.pad", sf_pad}, {"fmt.pad_left", sf_pad_left},
    {"fmt.center", sf_center},

    /* os */
    {"os.read", os_read}, {"os.write", os_write}, {"os.open", os_open},
    {"os.create", os_create}, {"os.exists", os_exists},
    {"os.remove", os_remove}, {"os.args", os_args}, {"os.env", os_env},
    {"os.exit", os_exit}, {"os.on_abort", os_on_abort},
    {"os.isdir", os_isdir}, {"os.listdir", os_listdir},
    {"os.run", os_run}, {"os.cwd", os_cwd}, {"os.pid", os_pid},
    {"os.exe", os_exe}, {"os.mtime", os_mtime}, {"os.size", os_size},
    {"os.hostname", os_hostname},

    /* math */
    {"math.sqrt", math_sqrt}, {"math.sin", math_sin}, {"math.cos", math_cos},
    {"math.tan", math_tan}, {"math.asin", math_asin}, {"math.acos", math_acos},
    {"math.atan", math_atan}, {"math.ln", math_ln}, {"math.log2", math_log2},
    {"math.log10", math_log10}, {"math.exp", math_exp}, {"math.pow", math_pow},
    {"math.hypot", math_hypot}, {"math.mod", math_mod},

    /* time */
    {"time.now", time_now}, {"time.since", time_since},
    {"time.sleep", time_sleep}, {"time.after", time_after},
    {"time.millis", time_millis}, {"time.seconds", time_seconds},

    /* log */
    {"log.info", log_info}, {"log.warn", log_warn}, {"log.error", log_error},
    {"log.debug", log_debug}, {"log.flush", log_flush},

    /* sort */
    {"sort.sort", sort_sort}, {"sort.sort_by", sort_sort_by},
    {"sort.reversed", sort_reversed},

    /* json */
    {"json.encode", json_encode}, {"json.pretty", json_pretty},
    {"json.decode", json_decode},

    /* rand / uuid */
    {"rand.int", rand_int}, {"rand.float", rand_float},
    {"rand.seed", rand_seed}, {"uuid.new", uuid_new},

    /* conv (package-qualified safe conversions) */
    {"conv.to_int", bi_to_int}, {"conv.to_float", bi_to_float},
    {"conv.to_dec", bi_to_dec},
};

/* Every std/str function, reachable bare (prelude) and qualified. */
static const Entry str_table[] = {
#define E(n) {#n, sf_##n},
    E(upper) E(lower) E(title) E(trim) E(trim_left) E(trim_right) E(strip)
    E(pad) E(pad_left) E(center) E(contains) E(index) E(last_index)
    E(starts_with) E(ends_with) E(count) E(equal_fold) E(substr) E(left)
    E(right) E(split) E(split_n) E(lines) E(fields) E(word) E(words)
    E(subword) E(delword) E(join) E(replace) E(replace_n) E(repeat)
    E(reverse) E(translate) E(overlay) E(insert) E(delstr) E(bytes)
    E(from_bytes) E(runes) E(from_runes) E(rune_len) E(is_digit) E(is_alpha)
    E(is_space) E(is_upper) E(hex) E(unhex) E(b64) E(unb64)
    E(find_any) E(skip_any)
#undef E
};

VlFn vl_lookup_fn(const char *name) {
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
    if (eq(table[i].name, name)) return table[i].fn;
  const char *bare = name;
  if (strncmp(name, "str.", 4) == 0) bare = name + 4;
  for (size_t i = 0; i < sizeof(str_table) / sizeof(str_table[0]); i++)
    if (eq(str_table[i].name, bare)) return str_table[i].fn;
  return NULL;
}

bool vl_has_name(const char *name) {
  if (vl_lookup_fn(name)) return true;
  return eq(name, "time.Second") || eq(name, "time.Millisecond") ||
         eq(name, "time.Nanosecond") || eq(name, "time.Microsecond") ||
         eq(name, "time.Minute") || eq(name, "time.Hour") ||
         eq(name, "math.pi") || eq(name, "math.e") ||
         eq(name, "os.stderr") || eq(name, "os.stdout");
}

Value vl_lookup_value(const char *name) {
  if (eq(name, "time.Nanosecond")) return vl_dur(1);
  if (eq(name, "time.Microsecond")) return vl_dur(1000);
  if (eq(name, "time.Millisecond")) return vl_dur(1000000);
  if (eq(name, "time.Second")) return vl_dur(1000000000LL);
  if (eq(name, "time.Minute")) return vl_dur(60000000000LL);
  if (eq(name, "time.Hour")) return vl_dur(3600000000000LL);
  if (eq(name, "math.pi")) return vl_float(3.14159265358979323846);
  if (eq(name, "math.e")) return vl_float(2.71828182845904523536);
  if (eq(name, "os.stderr")) return vl_file_new(stderr, "<stderr>", true);
  if (eq(name, "os.stdout")) return vl_file_new(stdout, "<stdout>", true);
  VlFn fn = vl_lookup_fn(name);
  if (fn) return vl_native(name, fn);
  vl_abort("unknown name `%s`", name);
  return vl_nil();
}

/* ---------------------------------------------------------------- match */

bool vl_match(VlFrame *f, Value subject, const VlPat *p, const Value *consts) {
  switch (p->kind) {
  case VL_P_WILD:
    return true;
  case VL_P_NIL:
    return subject.t == VL_NIL;
  case VL_P_LIT:
    return vl_equal(subject, consts[p->lit]);
  case VL_P_BIND:
    vl_set(&f->r[p->slot], vl_retain(subject));
    return true;
  case VL_P_VARIANT:
    break;
  }
  /* built-in variants of ?T and T! */
  if (eq(p->name, "Some")) {
    if (subject.t == VL_NIL) return false;
    if (p->nelems == 1) return vl_match(f, subject, &p->elems[0], consts);
    return true;
  }
  if (eq(p->name, "Ok")) {
    if (subject.t == VL_OBJ && subject.u.o->kind == O_ERR) return false;
    if (p->nelems == 1) return vl_match(f, subject, &p->elems[0], consts);
    return true;
  }
  if (eq(p->name, "Err")) {
    if (!(subject.t == VL_OBJ && subject.u.o->kind == O_ERR)) return false;
    if (p->nelems == 1) return vl_match(f, subject, &p->elems[0], consts);
    return true;
  }
  if (subject.t == VL_OBJ && subject.u.o->kind == O_ERR) {
    const char *tn = vl_cstr(((VlErr *)subject.u.o)->type_name);
    return eq(tn, p->name);
  }
  if (!(subject.t == VL_OBJ && subject.u.o->kind == O_ENUM)) return false;
  VlEnum *e = (VlEnum *)subject.u.o;
  if (!eq(e->var->variant, p->name)) return false;
  if (p->nelems == 0) return true;
  if (p->nelems != e->n) return false;
  for (int i = 0; i < p->nelems; i++)
    if (!vl_match(f, e->f[i], &p->elems[i], consts)) return false;
  return true;
}

/* ---------------------------------------------------------------- parse */

/* The general REXX template algorithm (§7.4): pattern items fix the ends of
 * capture regions; within a region, word targets split on blanks and the
 * final target takes the remainder. */
void vl_parse(VlFrame *f, Value srcv, const char *fold, const VlTerm *terms,
              int nterms) {
  Value src = vl_tostr(srcv);
  if (eq(fold, "UPPER") || eq(fold, "LOWER")) {
    bool ok;
    Value t = vl_str_call(eq(fold, "UPPER") ? "upper" : "lower", &src, 1, &ok);
    vl_release(src);
    src = t;
  }
  const char *s = vl_cstr(src);
  int64_t n = vl_strlen(src);

  /* Separator-first templates (`parse line "," a "," b`) are rewritten to the
   * canonical var-then-pattern order. */
  VlTerm *t2 = (VlTerm *)vl_alloc(sizeof(VlTerm) * (size_t)(nterms ? nterms : 1));
  int n2 = 0;
  if (nterms >= 2 && terms[0].kind == VL_T_LIT &&
      (terms[1].kind == VL_T_VAR || terms[1].kind == VL_T_DISCARD)) {
    for (int i = 0; i < nterms;) {
      if (terms[i].kind == VL_T_LIT && i + 1 < nterms &&
          (terms[i + 1].kind == VL_T_VAR || terms[i + 1].kind == VL_T_DISCARD)) {
        t2[n2++] = terms[i + 1];
        t2[n2++] = terms[i];
        i += 2;
      } else {
        t2[n2++] = terms[i++];
      }
    }
  } else {
    for (int i = 0; i < nterms; i++) t2[n2++] = terms[i];
  }

  int64_t pos = 0;
  int i = 0;
  while (i <= n2) {
    /* collect the word targets of this segment */
    int first = i;
    while (i < n2 && (t2[i].kind == VL_T_VAR || t2[i].kind == VL_T_DISCARD)) i++;
    int ntgt = i - first;

    int64_t region_start = pos, region_end = n, next = n;
    if (i < n2) {
      if (t2[i].kind == VL_T_LIT) {
        const char *lit = t2[i].lit;
        int64_t ln = (int64_t)strlen(lit);
        int64_t at = -1;
        for (int64_t k = pos; ln > 0 && k + ln <= n; k++)
          if (memcmp(s + k, lit, (size_t)ln) == 0) { at = k; break; }
        if (at >= 0) { region_end = at; next = at + ln; }
        else { region_end = n; next = n; }
      } else if (t2[i].kind == VL_T_COL) {
        int64_t col = t2[i].col - 1;
        if (col < 0) col = 0;
        if (col > n) col = n;
        region_end = col < region_start ? n : col;
        next = col;
      }
      i++;
    } else {
      i++; /* terminate the loop after the final segment */
    }

    /* distribute the region over its targets */
    if (region_start > n) region_start = n;
    if (region_end > n) region_end = n;
    if (region_end < region_start) region_end = region_start;
    const char *rp = s + region_start;
    int64_t rn = region_end - region_start;

    for (int k = 0; k < ntgt; k++) {
      const VlTerm *tg = &t2[first + k];
      bool last = (k == ntgt - 1);
      if (last) {
        int64_t a = 0;
        if (ntgt > 1)
          while (a < rn && (rp[a] == ' ' || rp[a] == '\t')) a++;
        if (tg->kind == VL_T_VAR)
          vl_set(&f->r[tg->slot], vl_str_n(rp + a, rn - a));
        break;
      }
      int64_t a = 0;
      while (a < rn && (rp[a] == ' ' || rp[a] == '\t')) a++;
      int64_t b = a;
      while (b < rn && rp[b] != ' ' && rp[b] != '\t') b++;
      if (tg->kind == VL_T_VAR)
        vl_set(&f->r[tg->slot], vl_str_n(rp + a, b - a));
      rp += b;
      rn -= b;
    }
    pos = next;
    if (i > n2) break;
  }
  free(t2);
  vl_release(src);
}

/* ---------------------------------------------------------------- entry */

void vl_init(int argc, char **argv) {
  g_args = vl_slice_new(0);
  for (int i = 1; i < argc; i++) vl_slice_append(g_args, vl_str(argv[i]));
  srandom((unsigned)time(NULL));
  vl_group_begin(vl_nil()); /* the implicit root group (§9.1) */
}

int vl_finish(void) {
  Value r = vl_group_end(false);
  vl_release(r);
  fflush(stdout);
  return 0;
}
