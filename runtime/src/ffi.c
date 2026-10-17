/* ffi.c — std/ffi: raw C memory, and the conversions a foreign call is
 * wrapped in (§12).
 *
 * EVERYTHING HERE IS OUTSIDE THE SAFETY GUARANTEES, and that is the point of
 * the file: it is the one place a Voilà program can name an address. What is
 * kept is what can be kept cheaply:
 *
 *   - ffi.alloc'd blocks are REGISTERED, so ffi.free of an address that is
 *     not a live allocation — a double free, a pointer C handed us, an
 *     offset into a block — aborts with a diagnostic instead of corrupting
 *     the allocator. (A pointer C allocated is freed by C's own function.)
 *   - std/ffi's Ptr carries the block's size when it is known, and its
 *     readers and writers check offsets against it before calling down here.
 *
 * What is NOT kept: a Ptr may outlive its block (use after free), a C
 * function may write past any buffer, and a wrong prototype in an ffi block
 * is undefined behaviour. `voila check --strict` refuses a program that uses
 * std/ffi for exactly this reason.
 *
 * ------------------------------------------------------- the call wrapper
 *
 * The code generator turns each function of an `ffi "C" { … }` block into a
 * C prototype bound to the real symbol by an asm label, and a body that
 * converts the boxed arguments with the vl_ffi_* functions below, makes the
 * call, and boxes the result (cg/instr.voi, emit_ffi). The conversions abort
 * with the function's name and the argument's position on a value of the
 * wrong kind; the static types of the declaration make that unreachable for
 * a checked program, short of a value of unknown type. */
#include "voila_int.h"

#include <stdlib.h>
#include <string.h>

/* =========================================================== allocations */

/* The live-allocation registry: an open-addressing hash set of addresses,
 * grown at 50% load. A block is in it from ffi._alloc until ffi._free. */
static pthread_mutex_t g_ffimu = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t *g_live;
static size_t g_cap, g_n, g_tomb; /* slots, live entries, tombstones */

static size_t slot_of(uintptr_t a, size_t cap) {
  uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ULL;
  return (size_t)(h >> 17) & (cap - 1);
}

/* reg_rehash rebuilds the table at `ncap` slots, dropping every tombstone.
 * Called with g_ffimu held. */
static void reg_rehash(size_t ncap) {
  uintptr_t *nt = (uintptr_t *)calloc(ncap, sizeof(uintptr_t));
  if (!nt) vl_abort("out of memory");
  for (size_t i = 0; i < g_cap; i++) {
    if (g_live[i] > 1) {
      size_t j = slot_of(g_live[i], ncap);
      while (nt[j]) j = (j + 1) & (ncap - 1);
      nt[j] = g_live[i];
    }
  }
  free(g_live);
  g_live = nt;
  g_cap = ncap;
  g_tomb = 0;
}

/* reg_insert adds a. Slot value 0 is empty and 1 a tombstone. The table is
 * rebuilt when live entries PLUS tombstones would pass half of it — counting
 * the tombstones is what keeps an empty slot always reachable, so a probe
 * for a missing address ends instead of circling a table of tombstones
 * after a long alloc/free churn. It doubles only when the LIVE entries need
 * the room; otherwise the rebuild just sweeps the tombstones out. */
static void reg_insert(uintptr_t a) {
  if ((g_n + g_tomb + 1) * 2 > g_cap) {
    size_t ncap = g_cap ? g_cap : 64;
    while ((g_n + 1) * 4 > ncap) ncap *= 2;
    reg_rehash(ncap);
  }
  size_t j = slot_of(a, g_cap);
  while (g_live[j] > 1) j = (j + 1) & (g_cap - 1);
  if (g_live[j] == 1) g_tomb--; /* reusing a tombstone */
  g_live[j] = a;
  g_n++;
}

/* reg_remove takes a out of the registry, reporting whether it was there.
 * The probe is bounded by the table size as a second line of defence. */
static bool reg_remove(uintptr_t a) {
  if (!g_cap) return false;
  size_t j = slot_of(a, g_cap);
  for (size_t k = 0; k < g_cap && g_live[j]; k++) {
    if (g_live[j] == a) {
      g_live[j] = 1; /* tombstone: later probes must walk past it */
      g_n--;
      g_tomb++;
      return true;
    }
    j = (j + 1) & (g_cap - 1);
  }
  return false;
}

static int64_t want_int(Value v, const char *fn, int argi) {
  if (v.t != VL_INT) vl_abort("%s: argument %d must be an int", fn, argi);
  return v.u.i;
}

/* ffi._alloc(n) -> int: n zeroed bytes (calloc), registered. */
Value vl_ffi_alloc(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  int64_t n = want_int(argv[0], "ffi.alloc", 1);
  if (n < 0) vl_throwf("RangeError", "ffi.alloc: a size of %lld bytes",
                       (long long)n);
  void *p = calloc(1, n ? (size_t)n : 1);
  if (!p) vl_throwf("RuntimeError", "ffi.alloc: out of memory for %lld bytes",
                    (long long)n);
  pthread_mutex_lock(&g_ffimu);
  reg_insert((uintptr_t)p);
  pthread_mutex_unlock(&g_ffimu);
  return vl_int((int64_t)(uintptr_t)p);
}

/* ffi._free(addr): release a block ffi._alloc returned. 0 (NULL) is a no-op,
 * as for free(3); anything else must be a live allocation. */
Value vl_ffi_free(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  int64_t a = want_int(argv[0], "ffi.free", 1);
  if (a == 0) return vl_unit();
  pthread_mutex_lock(&g_ffimu);
  bool live = reg_remove((uintptr_t)a);
  pthread_mutex_unlock(&g_ffimu);
  if (!live)
    vl_abort("ffi.free: not a block ffi.alloc returned (freed twice, or "
             "allocated by C — free it with C's own function)");
  free((void *)(uintptr_t)a);
  return vl_unit();
}

/* ============================================================ read/write */

/* The element codes std/ffi passes, one per C scalar type. */
enum {
  FC_I8 = 1, FC_U8, FC_I16, FC_U16, FC_I32, FC_U32, FC_I64, FC_U64,
  FC_F32, FC_F64, FC_PTR
};

static void *addr_of(Value v, const char *fn) {
  int64_t a = want_int(v, fn, 1);
  if (a == 0) vl_throwf("RuntimeError", "%s: NULL pointer", fn);
  return (void *)(uintptr_t)a;
}

/* ffi._read(addr, code) -> int | float: one scalar, read with memcpy so an
 * unaligned address is not undefined behaviour. */
Value vl_ffi_read(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  void *p = addr_of(argv[0], "ffi.read");
  switch ((int)want_int(argv[1], "ffi.read", 2)) {
  case FC_I8: { int8_t x; memcpy(&x, p, 1); return vl_int(x); }
  case FC_U8: { uint8_t x; memcpy(&x, p, 1); return vl_int(x); }
  case FC_I16: { int16_t x; memcpy(&x, p, 2); return vl_int(x); }
  case FC_U16: { uint16_t x; memcpy(&x, p, 2); return vl_int(x); }
  case FC_I32: { int32_t x; memcpy(&x, p, 4); return vl_int(x); }
  case FC_U32: { uint32_t x; memcpy(&x, p, 4); return vl_int(x); }
  case FC_I64: { int64_t x; memcpy(&x, p, 8); return vl_int(x); }
  case FC_U64: {
    uint64_t x;
    memcpy(&x, p, 8);
    if (x > (uint64_t)INT64_MAX)
      vl_throwf("RangeError", "ffi.read_u64: %llu does not fit an int",
                (unsigned long long)x);
    return vl_int((int64_t)x);
  }
  case FC_F32: { float x; memcpy(&x, p, 4); return vl_float(x); }
  case FC_F64: { double x; memcpy(&x, p, 8); return vl_float(x); }
  case FC_PTR: {
    void *x;
    memcpy(&x, p, sizeof x);
    return vl_int((int64_t)(uintptr_t)x);
  }
  }
  vl_abort("ffi.read: bad element code");
  return vl_nil();
}

/* ffi._write(addr, code, v): one scalar. The VALUE's range was checked by
 * the Voilà wrapper's parameter type (write_u8 takes a u8, …). */
Value vl_ffi_write(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  void *p = addr_of(argv[0], "ffi.write");
  int code = (int)want_int(argv[1], "ffi.write", 2);
  Value v = argv[2];
  if (code == FC_F32 || code == FC_F64) {
    double d;
    if (v.t == VL_FLOAT) d = v.u.f;
    else if (v.t == VL_INT) d = (double)v.u.i;
    else vl_abort("ffi.write: a float value is wanted");
    if (code == FC_F32) {
      float f = (float)d;
      memcpy(p, &f, 4);
    } else {
      memcpy(p, &d, 8);
    }
    return vl_unit();
  }
  int64_t x = want_int(v, "ffi.write", 3);
  switch (code) {
  case FC_I8: case FC_U8: { uint8_t b = (uint8_t)x; memcpy(p, &b, 1); break; }
  case FC_I16: case FC_U16: { uint16_t h = (uint16_t)x; memcpy(p, &h, 2); break; }
  case FC_I32: case FC_U32: { uint32_t w = (uint32_t)x; memcpy(p, &w, 4); break; }
  case FC_I64: case FC_U64: memcpy(p, &x, 8); break;
  case FC_PTR: {
    void *q = (void *)(uintptr_t)x;
    memcpy(p, &q, sizeof q);
    break;
  }
  default: vl_abort("ffi.write: bad element code");
  }
  return vl_unit();
}

/* ffi._read_str(addr, n) -> str: n bytes, or — when n < 0 — up to (not
 * including) the first NUL. `limit` (argv[2]) bounds the NUL search when
 * the block's size is known; it is -1 when it is not. */
Value vl_ffi_read_str(Value *argv, int argc, VlFrame *up) {
  (void)up;
  const char *p = (const char *)addr_of(argv[0], "ffi.read_str");
  int64_t n = want_int(argv[1], "ffi.read_str", 2);
  if (n >= 0) return vl_str_n(p, n);
  int64_t limit = argc > 2 ? want_int(argv[2], "ffi.read_str", 3) : -1;
  if (limit < 0) return vl_str(p);
  const char *z = memchr(p, 0, (size_t)limit);
  return vl_str_n(p, z ? (int64_t)(z - p) : limit);
}

/* ffi._write_str(addr, data) -> int: the bytes of a str or a []byte, copied
 * to addr (no NUL is added); returns how many. */
Value vl_ffi_write_str(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  char *p = (char *)addr_of(argv[0], "ffi.write_str");
  Value d = argv[1];
  if (d.t == VL_OBJ && d.u.o->kind == O_STR) {
    int64_t n = vl_strlen(d);
    memcpy(p, vl_cstr(d), (size_t)n);
    return vl_int(n);
  }
  if (d.t == VL_OBJ && d.u.o->kind == O_SLICE) {
    int64_t n = vl_len(d);
    Value *e = vl_slice_data(d);
    for (int64_t i = 0; i < n; i++) {
      if (e[i].t != VL_INT || e[i].u.i < 0 || e[i].u.i > 255)
        vl_throwf("RangeError", "ffi.write_bytes: element %lld is not a byte",
                  (long long)i);
      p[i] = (char)(uint8_t)e[i].u.i;
    }
    return vl_int(n);
  }
  vl_abort("ffi.write_str: a str or a []byte is wanted");
  return vl_nil();
}

/* ======================================================= call conversions */

static Value unwrap(Value v) {
  /* A Box or a newtype whose value is of the kind is accepted (§13). */
  if (v.t == VL_OBJ &&
      (v.u.o->kind == O_BOX ||
       (v.u.o->kind == O_STRUCT && ((VlStruct *)v.u.o)->type->newtype)))
    return vl_deref(v);
  return v;
}

int64_t vl_ffi_int(Value v, int argi, const char *fn) {
  v = unwrap(v);
  if (v.t == VL_INT) return v.u.i;
  if (v.t == VL_RUNE) return (int64_t)v.u.r;
  if (v.t == VL_BOOL) return v.u.b ? 1 : 0;
  vl_abort("ffi %s: argument %d must be an int, got %s", fn, argi,
           vl_kind_name(v));
  return 0;
}

double vl_ffi_float(Value v, int argi, const char *fn) {
  v = unwrap(v);
  if (v.t == VL_FLOAT) return v.u.f;
  if (v.t == VL_INT) return (double)v.u.i;
  if (v.t == VL_OBJ && v.u.o->kind == O_DEC) return vl_dec_to_float(v);
  vl_abort("ffi %s: argument %d must be a float, got %s", fn, argi,
           vl_kind_name(v));
  return 0;
}

bool vl_ffi_bool(Value v, int argi, const char *fn) {
  v = unwrap(v);
  if (v.t == VL_BOOL) return v.u.b;
  vl_abort("ffi %s: argument %d must be a bool, got %s", fn, argi,
           vl_kind_name(v));
  return false;
}

/* The bytes stay owned by the str, which the caller's frame holds for the
 * duration of the call; C must not keep the pointer. */
const char *vl_ffi_cstr(Value v, int argi, const char *fn) {
  v = unwrap(v);
  if (v.t == VL_NIL) return NULL;
  if (v.t == VL_OBJ && v.u.o->kind == O_STR) {
    /* C would read a NUL inside the str as its end and silently see less
     * than was passed — refused, as Go's syscall refuses it (EINVAL). */
    if (memchr(vl_cstr(v), 0, (size_t)vl_strlen(v)))
      vl_throwf("ValueError", "ffi %s: argument %d contains a NUL byte, "
                "which a C string cannot hold", fn, argi);
    return vl_cstr(v);
  }
  vl_abort("ffi %s: argument %d must be a str, got %s", fn, argi,
           vl_kind_name(v));
  return NULL;
}

/* A pointer argument: std/ffi's Ptr (its `addr`), an int address, or nil —
 * which is NULL. */
void *vl_ffi_ptr(Value v, int argi, const char *fn) {
  if (v.t == VL_NIL) return NULL;
  if (v.t == VL_INT) return (void *)(uintptr_t)v.u.i;
  if (v.t == VL_OBJ && v.u.o->kind == O_STRUCT) {
    VlStruct *s = (VlStruct *)v.u.o;
    if (strcmp(s->type->name, "ffi.Ptr") == 0 && s->f[0].t == VL_INT)
      return (void *)(uintptr_t)s->f[0].u.i;
  }
  vl_abort("ffi %s: argument %d must be an ffi.Ptr, got %s", fn, argi,
           vl_kind_name(v));
  return NULL;
}

Value vl_ffi_ret_u64(unsigned long long r) {
  if (r > (unsigned long long)INT64_MAX)
    vl_throwf("RangeError", "ffi: the result %llu does not fit an int", r);
  return vl_int((int64_t)r);
}

/* A pointer result is a Ptr whose size is unknown (-1): C did not say. */
Value vl_ffi_ret_ptr(void *p) {
  Value f[2];
  f[0] = vl_int((int64_t)(uintptr_t)p);
  f[1] = vl_int(-1);
  return vl_struct_new("ffi.Ptr", f, 2, NULL, NULL, 0);
}

/* A `*char` result is copied into a str at once; NULL is nil. */
Value vl_ffi_ret_cstr(const char *s) {
  if (!s) return vl_nil();
  return vl_str(s);
}

/* A variadic argument is passed by its run-time kind under C's default
 * promotions: a float as a double, everything else in an integer register —
 * an int as a 64-bit integer, a bool as 0/1, a str as its bytes' address, a
 * Ptr as its address, nil as NULL. vl_ffi_vclass says which. */
int vl_ffi_vclass(Value v) {
  v = unwrap(v);
  return v.t == VL_FLOAT || (v.t == VL_OBJ && v.u.o->kind == O_DEC);
}

double vl_ffi_vdouble(Value v) { return vl_ffi_float(v, 0, "variadic"); }

long long vl_ffi_vword(Value v) {
  v = unwrap(v);
  if (v.t == VL_NIL) return 0;
  if (v.t == VL_INT) return v.u.i;
  if (v.t == VL_RUNE) return (long long)v.u.r;
  if (v.t == VL_BOOL) return v.u.b;
  if (v.t == VL_OBJ && v.u.o->kind == O_STR)
    return (long long)(uintptr_t)vl_cstr(v);
  return (long long)(uintptr_t)vl_ffi_ptr(v, 0, "variadic");
}
