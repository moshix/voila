/* ext/compress.c — std/compress's natives, over the vendored miniz
 * (runtime/src/vendor/miniz, MIT): DEFLATE in its three framings — raw
 * (RFC 1951), zlib (RFC 1950) and gzip (RFC 1952) — one-shot and as
 * streams, and zip archives in memory. tar is std/compress's own Voilà.
 *
 * This file is a runtime EXTENSION (voila_std.h, vl_ext_register): it is
 * compiled, with miniz, into its own archive only for a program that uses
 * std/compress, so nothing here is linked into any other binary.
 *
 * miniz is #included, not compiled on its own: it is one .c file, and this
 * way its options below are set in exactly one place. Its symbols keep
 * their own prefixes (mz_, tdefl_, tinfl_) and MINIZ_NO_ZLIB_COMPATIBLE_NAMES
 * keeps it from defining zlib's names, so a program may also link the
 * system zlib through std/ffi without a clash.
 *
 * THE RULES. Every native returns an IOError VALUE on failure and never
 * aborts on bad input: corrupt, truncated or hostile compressed data is an
 * error the program can match on. Decompression is BOUNDED — each decoder
 * takes a maximum output size and stops with an error the moment the output
 * would pass it, so a few kilobytes that inflate to gigabytes (a
 * "decompression bomb") cost a diagnostic, not the machine's memory.
 */
#define MINIZ_NO_STDIO                   /* miniz opens no files itself */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES   /* no `deflate`/`inflate` macros */
#include "voila_int.h"
#include "miniz.c"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* The three framings, as std/compress numbers them. */
enum { FMT_RAW = 0, FMT_ZLIB = 1, FMT_GZIP = 2 };

/* ------------------------------------------------------------ helpers */

static Value cz_err(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Value m = vl_str(buf);
  Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
  vl_release(m);
  return e;
}

static bool is_err(Value v) { return v.t == VL_OBJ && v.u.o->kind == O_ERR; }

static int64_t want_int(Value v, const char *who, int argi) {
  if (v.t == VL_INT) return v.u.i;
  if (v.t == VL_RUNE) return (int64_t)v.u.r;
  vl_abort("%s: argument %d must be an int, got %s", who, argi,
           vl_kind_name(v));
  return 0;
}

/* CzIn is the bytes of a str or a []byte, borrowed from the str or copied
 * out of the slice (whose elements are 16-byte Values, not bytes). */
typedef struct {
  const unsigned char *p;
  size_t n;
  unsigned char *owned;
} CzIn;

static CzIn cz_in(Value v, const char *who) {
  CzIn in = {NULL, 0, NULL};
  if (v.t == VL_OBJ && v.u.o->kind == O_STR) {
    in.p = (const unsigned char *)vl_cstr(v);
    in.n = (size_t)vl_strlen(v);
    return in;
  }
  if (v.t == VL_OBJ && v.u.o->kind == O_SLICE) {
    VlSlice *s = (VlSlice *)v.u.o;
    in.owned = (unsigned char *)vl_alloc((size_t)s->len + 1);
    for (int64_t i = 0; i < s->len; i++) {
      Value e = s->e[i];
      if (e.t != VL_INT || e.u.i < 0 || e.u.i > 255)
        vl_abort("%s: a []byte element must be a byte", who);
      in.owned[i] = (unsigned char)e.u.i;
    }
    in.p = in.owned;
    in.n = (size_t)s->len;
    return in;
  }
  vl_abort("%s: expected a str or []byte, got %s", who, vl_kind_name(v));
  return in;
}

static void cz_in_free(CzIn *in) {
  free(in->owned);
  in->owned = NULL;
}

/* A growable output buffer. `max` (> 0) is the decoder's bound: cz_grow
 * refuses to grow past it, which is what turns a bomb into an error. */
typedef struct {
  unsigned char *b;
  size_t n, cap;
} CzBuf;

static void cz_put(CzBuf *o, const void *p, size_t n) {
  if (o->n + n > o->cap) {
    size_t c = o->cap ? o->cap : 256;
    while (c < o->n + n) c *= 2;
    unsigned char *nb = (unsigned char *)realloc(o->b, c);
    if (!nb) vl_abort("out of memory");
    o->b = nb;
    o->cap = c;
  }
  memcpy(o->b + o->n, p, n);
  o->n += n;
}

static Value cz_done(CzBuf *o) {
  Value s = vl_str_n(o->b ? (const char *)o->b : "", (int64_t)o->n);
  free(o->b);
  o->b = NULL;
  return s;
}

static void put_le32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)v;
  p[1] = (unsigned char)(v >> 8);
  p[2] = (unsigned char)(v >> 16);
  p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_le32(const unsigned char *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

/* The gzip header this package writes: no name, no timestamp (mtime 0, as
 * `gzip -n` does, so equal input gives equal output), OS "unknown". */
static void gzip_header(unsigned char h[10], int level) {
  h[0] = 0x1f;
  h[1] = 0x8b;
  h[2] = 8; /* CM: deflate */
  h[3] = 0; /* FLG */
  put_le32(h + 4, 0);
  h[8] = level >= 9 ? 2 : (level == 1 ? 4 : 0); /* XFL */
  h[9] = 255;                                   /* OS: unknown */
}

/* gzip_header_len parses a gzip member header at p[0..n): its length, 0 when
 * more bytes are needed to decide, or -1 (with *why) when it is not one. */
static int64_t gzip_header_len(const unsigned char *p, size_t n,
                               const char **why) {
  if (n < 10) return 0;
  if (p[0] != 0x1f || p[1] != 0x8b) {
    *why = "not gzip data (bad magic number)";
    return -1;
  }
  if (p[2] != 8) {
    *why = "unsupported gzip compression method";
    return -1;
  }
  unsigned flg = p[3];
  if (flg & 0xe0) {
    *why = "reserved gzip header flags are set";
    return -1;
  }
  size_t at = 10;
  if (flg & 4) { /* FEXTRA: a 2-byte length, then that many bytes */
    if (n < at + 2) return 0;
    size_t xlen = (size_t)p[at] | (size_t)p[at + 1] << 8;
    at += 2 + xlen;
    if (n < at) return 0;
  }
  for (int f = 8; f <= 16; f += 8) { /* FNAME, then FCOMMENT: NUL-ended */
    if (!(flg & (unsigned)f)) continue;
    const unsigned char *z = memchr(p + at, 0, n - at);
    if (!z) return 0;
    at = (size_t)(z - p) + 1;
  }
  if (flg & 2) { /* FHCRC: two bytes of header CRC */
    at += 2;
    if (n < at) return 0;
  }
  return (int64_t)at;
}

static int window_bits(int fmt) {
  /* zlib's framing is miniz's default; raw and gzip are both raw DEFLATE
   * inside (gzip's header and trailer are written here, not by miniz). */
  return fmt == FMT_ZLIB ? MZ_DEFAULT_WINDOW_BITS : -MZ_DEFAULT_WINDOW_BITS;
}

static const char *fmt_name(int fmt) {
  return fmt == FMT_GZIP ? "gzip" : fmt == FMT_ZLIB ? "zlib" : "deflate";
}

/* ------------------------------------------------------------ one-shot */

/* compress._encode(data, level, fmt) -> str. Level 0..9 (the Voilà layer
 * has checked it). Cannot fail on any input. */
static Value cz_encode(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int level = (int)want_int(argv[1], "compress.encode", 2);
  int fmt = (int)want_int(argv[2], "compress.encode", 3);
  CzIn in = cz_in(argv[0], "compress.encode");
  CzBuf o = {NULL, 0, 0};
  if (fmt == FMT_GZIP) {
    unsigned char h[10];
    gzip_header(h, level);
    cz_put(&o, h, 10);
  }
  mz_stream s;
  memset(&s, 0, sizeof s);
  if (mz_deflateInit2(&s, level, MZ_DEFLATED, window_bits(fmt), 9,
                      MZ_DEFAULT_STRATEGY) != MZ_OK) {
    cz_in_free(&in);
    free(o.b);
    return cz_err("compress: cannot start the encoder (level %d)", level);
  }
  s.next_in = in.p;
  /* avail_in is 32-bit; feed a larger input in slices. */
  size_t left = in.n;
  unsigned char chunk[65536];
  for (;;) {
    if (s.avail_in == 0 && left > 0) {
      size_t k = left > UINT_MAX ? UINT_MAX : left;
      s.avail_in = (unsigned)k;
      left -= k;
    }
    s.next_out = chunk;
    s.avail_out = sizeof chunk;
    int rc = mz_deflate(&s, left == 0 ? MZ_FINISH : MZ_NO_FLUSH);
    cz_put(&o, chunk, sizeof chunk - s.avail_out);
    if (rc == MZ_STREAM_END) break;
    if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
      mz_deflateEnd(&s);
      cz_in_free(&in);
      free(o.b);
      return cz_err("compress: the encoder failed (%d)", rc);
    }
  }
  mz_deflateEnd(&s);
  if (fmt == FMT_GZIP) {
    unsigned char t[8];
    put_le32(t, (uint32_t)mz_crc32(MZ_CRC32_INIT, in.p, in.n));
    put_le32(t + 4, (uint32_t)in.n); /* ISIZE is the length mod 2^32 */
    cz_put(&o, t, 8);
  }
  cz_in_free(&in);
  return cz_done(&o);
}

/* inflate_into decodes ONE raw-or-zlib stream from p[0..n) onto o, stopping
 * at its end. *used is the input consumed. Returns NULL, or why it failed:
 * corrupt data, data that ends early, or output past `max`. */
static const char *inflate_into(const unsigned char *p, size_t n, int fmt,
                                CzBuf *o, size_t max, size_t *used,
                                uint32_t *crc) {
  mz_stream s;
  memset(&s, 0, sizeof s);
  if (mz_inflateInit2(&s, window_bits(fmt)) != MZ_OK)
    return "cannot start the decoder";
  s.next_in = p;
  size_t left = n;
  unsigned char chunk[65536];
  const char *why = NULL;
  for (;;) {
    if (s.avail_in == 0 && left > 0) {
      size_t k = left > UINT_MAX ? UINT_MAX : left;
      s.avail_in = (unsigned)k;
      left -= k;
    }
    s.next_out = chunk;
    s.avail_out = sizeof chunk;
    int rc = mz_inflate(&s, MZ_NO_FLUSH);
    size_t got = sizeof chunk - s.avail_out;
    if (got) {
      if (o->n + got > max) {
        why = "LIMIT";
        break;
      }
      if (crc) *crc = (uint32_t)mz_crc32(*crc, chunk, got);
      cz_put(o, chunk, got);
    }
    if (rc == MZ_STREAM_END) break;
    if (rc == MZ_BUF_ERROR && s.avail_in == 0 && left == 0 && got == 0) {
      why = "unexpected end of compressed data";
      break;
    }
    if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
      why = "corrupt compressed data";
      break;
    }
  }
  *used = n - left - s.avail_in;
  mz_inflateEnd(&s);
  return why;
}

/* compress._decode(data, max, fmt) -> str!. Strict: the input must be
 * exactly one stream (gzip: one or more members, as gunzip accepts), with
 * nothing after it, and its checksums must hold. */
static Value cz_decode(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int64_t max = want_int(argv[1], "compress.decode", 2);
  int fmt = (int)want_int(argv[2], "compress.decode", 3);
  if (max <= 0) max = INT64_MAX;
  CzIn in = cz_in(argv[0], "compress.decode");
  CzBuf o = {NULL, 0, 0};
  const char *why = NULL;
  size_t at = 0;
  if (fmt != FMT_GZIP) {
    size_t used = 0;
    why = inflate_into(in.p, in.n, fmt, &o, (size_t)max, &used, NULL);
    if (!why && used != in.n) why = "trailing data after the compressed stream";
  } else {
    do {
      int64_t hl = gzip_header_len(in.p + at, in.n - at, &why);
      if (hl == 0) why = "unexpected end of gzip header";
      if (why) break;
      at += (size_t)hl;
      size_t used = 0, start = o.n;
      uint32_t crc = MZ_CRC32_INIT;
      why = inflate_into(in.p + at, in.n - at, FMT_RAW, &o, (size_t)max,
                         &used, &crc);
      if (why) break;
      at += used;
      if (in.n - at < 8) {
        why = "unexpected end of gzip data (missing trailer)";
        break;
      }
      if (get_le32(in.p + at) != crc) {
        why = "gzip checksum mismatch (corrupt data)";
        break;
      }
      if (get_le32(in.p + at + 4) != (uint32_t)(o.n - start)) {
        why = "gzip length mismatch (corrupt data)";
        break;
      }
      at += 8;
    } while (at < in.n);
  }
  cz_in_free(&in);
  if (why) {
    free(o.b);
    if (strcmp(why, "LIMIT") == 0)
      return cz_err("compress: %s output exceeds the limit of %lld bytes",
                    fmt_name(fmt), (long long)max);
    return cz_err("compress: %s: %s", fmt_name(fmt), why);
  }
  return cz_done(&o);
}

/* compress._crc32(data, crc) -> int: the running CRC-32 (zip, gzip, PNG). */
static Value cz_crc32(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  uint32_t c = (uint32_t)want_int(argv[1], "compress.crc32", 2);
  CzIn in = cz_in(argv[0], "compress.crc32");
  c = (uint32_t)mz_crc32(c, in.p, in.n);
  cz_in_free(&in);
  return vl_int((int64_t)c);
}

/* compress._adler32(data, adler) -> int: the running Adler-32 (zlib). */
static Value cz_adler32(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  uint32_t c = (uint32_t)want_int(argv[1], "compress.adler32", 2);
  CzIn in = cz_in(argv[0], "compress.adler32");
  c = (uint32_t)mz_adler32(c, in.p, in.n);
  cz_in_free(&in);
  return vl_int((int64_t)c);
}

/* ------------------------------------------------------------ streams
 *
 * A stream is an O_HANDLE whose ops std/io's reader and writer drive
 * (io.c): compress._writer hands back a handle with a write op, which the
 * Voilà layer wraps with io.writer, and _reader one with a read op for
 * io.reader. So a compressing writer IS an io.Writer, and a decompressing
 * reader an io.Reader — read_line, read_all, each-line iteration and
 * io.copy all work on them unchanged.
 *
 * The far end — the writer the compressed bytes go to, the reader they
 * come from — is any std/io value, reached by calling its methods
 * (vl_callm), so a file, a socket, a pipe or another stream will do. The
 * io.Reader/io.Writer that wraps this handle holds its mutex across each
 * call, so no two tasks are ever inside one stream's ops at once. */

typedef struct {
  mz_stream s;
  int fmt, level;
  Value dst; /* the io.Writer the compressed bytes go to (retained) */
  uint32_t crc;
  uint64_t isize;
  bool started, finished;
  /* A write to dst failed: the encoder has consumed input whose output is
   * lost, so the stream is corrupt — and every later call says so, rather
   * than letting close() report a clean (and broken) file. */
  bool failed;
} CzWriter;

typedef struct {
  mz_stream s;
  int fmt;
  Value src; /* the io.Reader the compressed bytes come from (retained) */
  unsigned char *in;
  size_t in_len, in_pos;
  bool src_eof, ended, failed, live;
  int64_t max, produced;
  uint32_t crc;
  uint64_t member_out;
  int phase; /* gzip: 0 header, 1 body, 2 trailer */
} CzReader;

/* dst_write hands bytes to the far-end writer; returns an owned error, or
 * nil. */
static Value dst_write(Value dst, const unsigned char *p, size_t n) {
  if (n == 0) return vl_nil();
  Value s = vl_str_n((const char *)p, (int64_t)n);
  Value r = vl_callm(dst, "write_all", &s, 1);
  vl_release(s);
  if (is_err(r)) return r;
  vl_release(r);
  return vl_nil();
}

/* cw_pump runs the encoder over whatever input is pending, with `flush`,
 * until it stops producing, writing every byte it makes to dst. */
static bool cw_pump(CzWriter *w, int flush, Value *err) {
  unsigned char chunk[65536];
  for (;;) {
    w->s.next_out = chunk;
    w->s.avail_out = sizeof chunk;
    int rc = mz_deflate(&w->s, flush);
    size_t got = sizeof chunk - w->s.avail_out;
    Value e = dst_write(w->dst, chunk, got);
    if (e.t != VL_NIL) {
      w->failed = true;
      *err = e;
      return false;
    }
    if (rc == MZ_STREAM_END) return true;
    if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
      w->failed = true;
      *err = cz_err("compress: the encoder failed (%d)", rc);
      return false;
    }
    /* Done when the encoder had room left: it has said all it will say for
     * this input (MZ_BUF_ERROR is "no progress possible", also done). A
     * FINISH is done only at the stream's end, above. */
    if (flush != MZ_FINISH && w->s.avail_in == 0 &&
        (got < sizeof chunk || rc == MZ_BUF_ERROR))
      return true;
  }
}

/* cw_ok refuses every operation on a stream that has already failed. */
static bool cw_ok(CzWriter *w, Value *err) {
  if (!w->failed) return true;
  *err = cz_err("compress: the stream failed earlier; its output is "
                "incomplete");
  return false;
}

static bool cw_start(CzWriter *w, Value *err) {
  if (w->started) return true;
  w->started = true;
  if (w->fmt == FMT_GZIP) {
    unsigned char h[10];
    gzip_header(h, w->level);
    Value e = dst_write(w->dst, h, 10);
    if (e.t != VL_NIL) {
      w->failed = true;
      *err = e;
      return false;
    }
  }
  return true;
}

static int64_t cw_write(void *ctx, const char *buf, int64_t n, Value *err) {
  CzWriter *w = (CzWriter *)ctx;
  if (w->finished) {
    *err = cz_err("compress: write after close");
    return -1;
  }
  if (!cw_ok(w, err) || !cw_start(w, err)) return -1;
  int64_t done = 0;
  while (done < n) {
    int64_t k = n - done > (1 << 30) ? (1 << 30) : n - done;
    w->s.next_in = (const unsigned char *)buf + done;
    w->s.avail_in = (unsigned)k;
    if (w->fmt == FMT_GZIP) {
      w->crc = (uint32_t)mz_crc32(w->crc, (const unsigned char *)buf + done,
                                  (size_t)k);
      w->isize += (uint64_t)k;
    }
    if (!cw_pump(w, MZ_NO_FLUSH, err)) return -1;
    done += k;
  }
  return n;
}

/* flush makes everything written so far decodable (a SYNC_FLUSH: the
 * receiver of a streamed response can act on it), at a few bytes' cost. */
static bool cw_flush(void *ctx, Value *err) {
  CzWriter *w = (CzWriter *)ctx;
  if (w->finished) return true;
  if (!cw_ok(w, err) || !cw_start(w, err)) return false;
  w->s.avail_in = 0;
  return cw_pump(w, MZ_SYNC_FLUSH, err);
}

/* close ends the stream: the final block, and for gzip the trailer. It does
 * NOT close the far-end writer — that belongs to whoever opened it. */
static bool cw_close(void *ctx, Value *err) {
  CzWriter *w = (CzWriter *)ctx;
  if (w->finished) return true;
  if (!cw_ok(w, err)) {
    w->finished = true;
    mz_deflateEnd(&w->s);
    return false;
  }
  if (!cw_start(w, err)) return false;
  w->finished = true;
  w->s.avail_in = 0;
  bool ok = cw_pump(w, MZ_FINISH, err);
  mz_deflateEnd(&w->s);
  if (!ok) return false;
  if (w->fmt == FMT_GZIP) {
    unsigned char t[8];
    put_le32(t, w->crc);
    put_le32(t + 4, (uint32_t)w->isize);
    Value e = dst_write(w->dst, t, 8);
    if (e.t != VL_NIL) {
      w->failed = true;
      *err = e;
      return false;
    }
  }
  return true;
}

static void cw_free(void *ctx) {
  CzWriter *w = (CzWriter *)ctx;
  if (!w->finished) mz_deflateEnd(&w->s);
  vl_release(w->dst);
  free(w);
}

static const VlHandleOps cw_ops = {"compress.Writer", cw_free, NULL, cw_write,
                                   cw_flush, cw_close};

/* compress._writer(dst, level, fmt) -> handle. */
static Value cz_writer(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int level = (int)want_int(argv[1], "compress.writer", 2);
  int fmt = (int)want_int(argv[2], "compress.writer", 3);
  CzWriter *w = (CzWriter *)vl_alloc(sizeof(CzWriter));
  if (mz_deflateInit2(&w->s, level, MZ_DEFLATED, window_bits(fmt), 9,
                      MZ_DEFAULT_STRATEGY) != MZ_OK) {
    free(w);
    return cz_err("compress: cannot start the encoder (level %d)", level);
  }
  w->fmt = fmt;
  w->level = level;
  w->dst = vl_retain(argv[0]);
  w->crc = MZ_CRC32_INIT;
  return vl_handle_new(&cw_ops, w);
}

/* cr_more pulls the next chunk of compressed input from the far end. Returns
 * false with *err on a read error; sets src_eof at its end. */
static bool cr_more(CzReader *r, Value *err) {
  if (r->in_pos > 0) { /* drop what the decoder has consumed */
    memmove(r->in, r->in + r->in_pos, r->in_len - r->in_pos);
    r->in_len -= r->in_pos;
    r->in_pos = 0;
  }
  Value n = vl_int(65536);
  Value got = vl_callm(r->src, "read", &n, 1);
  if (is_err(got)) {
    *err = got;
    return false;
  }
  if (got.t != VL_OBJ || got.u.o->kind != O_STR) {
    vl_release(got);
    *err = cz_err("compress: the source's read did not return bytes");
    return false;
  }
  size_t k = (size_t)vl_strlen(got);
  if (k == 0) {
    r->src_eof = true;
  } else {
    unsigned char *nb = (unsigned char *)realloc(r->in, r->in_len + k);
    if (!nb) vl_abort("out of memory");
    r->in = nb;
    memcpy(r->in + r->in_len, vl_cstr(got), k);
    r->in_len += k;
  }
  vl_release(got);
  return true;
}

static bool cr_fail(CzReader *r, Value *err, Value e) {
  r->failed = true;
  *err = e;
  return false;
}

/* cr_restart begins the decoder for one stream (or one gzip member). */
static bool cr_restart(CzReader *r, Value *err) {
  if (r->live) mz_inflateEnd(&r->s);
  memset(&r->s, 0, sizeof r->s);
  if (mz_inflateInit2(&r->s, window_bits(r->fmt)) != MZ_OK)
    return cr_fail(r, err, cz_err("compress: cannot start the decoder"));
  r->live = true;
  r->crc = MZ_CRC32_INIT;
  r->member_out = 0;
  return true;
}

/* cr_gzip_edge handles what lies between gzip bodies: the header before
 * one (phase 0) and the trailer after one (phase 2). Returns false on an
 * error; sets r->ended when the input has ended cleanly after a member. */
static bool cr_gzip_edge(CzReader *r, Value *err) {
  for (;;) {
    if (r->phase == 2) {
      while (r->in_len - r->in_pos < 8 && !r->src_eof)
        if (!cr_more(r, err)) return cr_fail(r, err, *err);
      if (r->in_len - r->in_pos < 8)
        return cr_fail(r, err, cz_err("compress: gzip: unexpected end of "
                                      "gzip data (missing trailer)"));
      const unsigned char *t = r->in + r->in_pos;
      if (get_le32(t) != r->crc)
        return cr_fail(r, err, cz_err("compress: gzip: gzip checksum "
                                      "mismatch (corrupt data)"));
      if (get_le32(t + 4) != (uint32_t)r->member_out)
        return cr_fail(r, err, cz_err("compress: gzip: gzip length "
                                      "mismatch (corrupt data)"));
      r->in_pos += 8;
      /* Another member follows, or the stream is over. */
      while (r->in_pos == r->in_len && !r->src_eof)
        if (!cr_more(r, err)) return cr_fail(r, err, *err);
      if (r->in_pos == r->in_len) {
        r->ended = true;
        return true;
      }
      r->phase = 0;
    }
    if (r->phase == 0) {
      for (;;) {
        const char *why = NULL;
        int64_t hl = gzip_header_len(r->in + r->in_pos, r->in_len - r->in_pos,
                                     &why);
        if (hl < 0) return cr_fail(r, err, cz_err("compress: gzip: %s", why));
        if (hl > 0) {
          r->in_pos += (size_t)hl;
          break;
        }
        if (r->src_eof)
          return cr_fail(r, err, cz_err("compress: gzip: unexpected end of "
                                        "gzip header"));
        if (!cr_more(r, err)) return cr_fail(r, err, *err);
      }
      if (!cr_restart(r, err)) return false;
      r->phase = 1;
      return true;
    }
    return true;
  }
}

static int64_t cr_read(void *ctx, char *buf, int64_t n, Value *err) {
  CzReader *r = (CzReader *)ctx;
  if (r->failed) {
    *err = cz_err("compress: the stream already failed");
    return -1;
  }
  bool stalled = false; /* the decoder made no progress on what it has */
  for (;;) {
    if (r->ended) return 0;
    if (r->fmt == FMT_GZIP && r->phase != 1) {
      if (!cr_gzip_edge(r, err)) return -1;
      continue;
    }
    if ((r->in_pos == r->in_len || stalled) && !r->src_eof) {
      if (!cr_more(r, err)) {
        r->failed = true;
        return -1;
      }
    }
    r->s.next_in = r->in + r->in_pos;
    r->s.avail_in = (unsigned)(r->in_len - r->in_pos);
    r->s.next_out = (unsigned char *)buf;
    unsigned room = n > (1 << 30) ? (1u << 30) : (unsigned)n;
    r->s.avail_out = room;
    int rc = mz_inflate(&r->s, MZ_NO_FLUSH);
    r->in_pos = r->in_len - r->s.avail_in;
    int64_t got = (int64_t)(room - r->s.avail_out);
    if (got > 0) {
      r->produced += got;
      if (r->produced > r->max) {
        r->failed = true;
        *err = cz_err("compress: %s output exceeds the limit of %lld bytes",
                      fmt_name(r->fmt), (long long)r->max);
        return -1;
      }
      r->crc = (uint32_t)mz_crc32(r->crc, (const unsigned char *)buf,
                                  (size_t)got);
      r->member_out += (uint64_t)got;
    }
    if (rc == MZ_STREAM_END) {
      if (r->fmt == FMT_GZIP) {
        r->phase = 2;
      } else {
        /* One stream, and nothing may follow it. */
        while (r->in_pos == r->in_len && !r->src_eof)
          if (!cr_more(r, err)) {
            r->failed = true;
            return -1;
          }
        if (r->in_pos != r->in_len) {
          r->failed = true;
          *err = cz_err("compress: %s: trailing data after the compressed "
                        "stream", fmt_name(r->fmt));
          return -1;
        }
        r->ended = true;
      }
      if (got > 0) return got;
      continue;
    }
    if (rc != MZ_OK && rc != MZ_BUF_ERROR) {
      r->failed = true;
      *err = cz_err("compress: %s: corrupt compressed data",
                    fmt_name(r->fmt));
      return -1;
    }
    if (got > 0) return got;
    /* No output: the decoder needs more input than there is. At the end
     * of the source that is a truncated stream — and it must be said, or
     * this loop would spin on the same bytes forever. */
    stalled = true;
    if (r->src_eof) {
      r->failed = true;
      *err = cz_err("compress: %s: unexpected end of compressed data",
                    fmt_name(r->fmt));
      return -1;
    }
  }
}

static bool cr_close(void *ctx, Value *err) {
  (void)err;
  CzReader *r = (CzReader *)ctx;
  if (r->live) mz_inflateEnd(&r->s);
  r->live = false;
  r->ended = true;
  return true;
}

static void cr_free(void *ctx) {
  CzReader *r = (CzReader *)ctx;
  if (r->live) mz_inflateEnd(&r->s);
  vl_release(r->src);
  free(r->in);
  free(r);
}

static const VlHandleOps cr_ops = {"compress.Reader", cr_free, cr_read, NULL,
                                   NULL, cr_close};

/* compress._reader(src, max, fmt) -> handle. */
static Value cz_reader(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int64_t max = want_int(argv[1], "compress.reader", 2);
  int fmt = (int)want_int(argv[2], "compress.reader", 3);
  CzReader *r = (CzReader *)vl_alloc(sizeof(CzReader));
  r->fmt = fmt;
  r->src = vl_retain(argv[0]);
  r->max = max > 0 ? max : INT64_MAX;
  r->phase = 0;
  if (fmt != FMT_GZIP) {
    Value err = vl_nil();
    if (!cr_restart(r, &err)) {
      cr_free(r);
      return err;
    }
    r->phase = 1;
  }
  return vl_handle_new(&cr_ops, r);
}

/* ------------------------------------------------------------ zip
 *
 * An archive is read from, or built into, MEMORY: compress._zip_open takes
 * the archive's bytes (os.read gives them) and the writer's finish returns
 * them. miniz checks each entry's CRC-32 as it extracts it. */

/* miniz's archive state (its last error, a writer's heap) is not safe to
 * share, and a ZipReader or ZipWriter value can reach several tasks: each
 * native holds the archive's `mu` for the whole of its work. */
typedef struct {
  mz_zip_archive za;
  unsigned char *data; /* the archive bytes, owned: miniz reads them */
  size_t len;
  bool open;
  pthread_mutex_t mu;
} ZipR;

static void zr_free(void *ctx) {
  ZipR *z = (ZipR *)ctx;
  if (z->open) mz_zip_reader_end(&z->za);
  free(z->data);
  pthread_mutex_destroy(&z->mu);
  free(z);
}

static const VlHandleOps zr_ops = {"compress.ZipReader", zr_free, NULL, NULL,
                                   NULL, NULL};

static Value zip_err(mz_zip_archive *za, const char *what) {
  mz_zip_error e = mz_zip_get_last_error(za);
  /* miniz fails some extractions — data that runs past the size the
   * directory declares, for one — without recording why. */
  if (e == MZ_ZIP_NO_ERROR)
    return cz_err("compress: zip: %s: the member's data does not match the "
                  "archive's directory (corrupt or lying sizes)", what);
  return cz_err("compress: zip: %s: %s", what, mz_zip_get_error_string(e));
}

/* compress._zip_open(data) -> handle!. */
static Value cz_zip_open(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  CzIn in = cz_in(argv[0], "compress.zip_open");
  ZipR *z = (ZipR *)vl_alloc(sizeof(ZipR));
  z->data = (unsigned char *)vl_alloc(in.n ? in.n : 1);
  memcpy(z->data, in.p, in.n);
  size_t n = in.n;
  z->len = n;
  pthread_mutex_init(&z->mu, NULL);
  cz_in_free(&in);
  if (!mz_zip_reader_init_mem(&z->za, z->data, n, 0)) {
    Value e = zip_err(&z->za, "not a readable zip archive");
    zr_free(z);
    return e;
  }
  z->open = true;
  return vl_handle_new(&zr_ops, z);
}

static ZipR *want_zr(Value v, const char *who) {
  ZipR *z = (ZipR *)vl_handle_ctx(v, &zr_ops);
  if (!z) vl_abort("%s: expected a zip reader, got %s", who, vl_kind_name(v));
  return z;
}

/* compress._zip_count(h) -> int. */
static Value cz_zip_count(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  ZipR *z = want_zr(argv[0], "compress.zip_count");
  pthread_mutex_lock(&z->mu);
  Value out = vl_int((int64_t)mz_zip_reader_get_num_files(&z->za));
  pthread_mutex_unlock(&z->mu);
  return out;
}

/* compress._zip_entry(h, i) -> (name, size, csize, is_dir, crc32, mtime,
 * mode)!. mode is the Unix st_mode the archive records — the high half of
 * the external attributes, when the entry was made on Unix — else 0. */
static Value zip_entry_locked(ZipR *z, int64_t i);

static Value cz_zip_entry(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  ZipR *z = want_zr(argv[0], "compress.zip_entry");
  int64_t i = want_int(argv[1], "compress.zip_entry", 2);
  pthread_mutex_lock(&z->mu);
  Value out = zip_entry_locked(z, i);
  pthread_mutex_unlock(&z->mu);
  return out;
}

static Value zip_entry_locked(ZipR *z, int64_t i) {
  mz_zip_archive_file_stat st;
  if (i < 0 || i >= (int64_t)mz_zip_reader_get_num_files(&z->za) ||
      !mz_zip_reader_file_stat(&z->za, (mz_uint)i, &st))
    return zip_err(&z->za, "cannot read an entry");
  Value t[7];
  t[0] = vl_str(st.m_filename);
  t[1] = vl_int((int64_t)st.m_uncomp_size);
  t[2] = vl_int((int64_t)st.m_comp_size);
  t[3] = vl_bool(st.m_is_directory != 0);
  t[4] = vl_int((int64_t)st.m_crc32);
  t[5] = vl_int((int64_t)st.m_time);
  bool unix_made = (st.m_version_made_by >> 8) == 3;
  t[6] = vl_int(unix_made ? (int64_t)(st.m_external_attr >> 16) : 0);
  Value tup = vl_tuple_new(t, 7);
  for (int k = 0; k < 7; k++) vl_release(t[k]);
  return tup;
}

/* compress._zip_read(h, i, max) -> str!. The size the archive DECLARES is
 * checked against max before a byte is inflated, and miniz refuses to write
 * past the declared size — so the declaration is a hard bound, and a lying
 * one is a CRC or size error, not an unbounded allocation. */
static Value zip_read_locked(ZipR *z, int64_t i, int64_t max);

static Value cz_zip_read(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  ZipR *z = want_zr(argv[0], "compress.zip_read");
  int64_t i = want_int(argv[1], "compress.zip_read", 2);
  int64_t max = want_int(argv[2], "compress.zip_read", 3);
  if (max <= 0) max = INT64_MAX;
  pthread_mutex_lock(&z->mu);
  Value out = zip_read_locked(z, i, max);
  pthread_mutex_unlock(&z->mu);
  return out;
}

static Value zip_read_locked(ZipR *z, int64_t i, int64_t max) {
  mz_zip_archive_file_stat st;
  if (i < 0 || i >= (int64_t)mz_zip_reader_get_num_files(&z->za) ||
      !mz_zip_reader_file_stat(&z->za, (mz_uint)i, &st))
    return zip_err(&z->za, "cannot read an entry");
  if (st.m_is_encrypted)
    return cz_err("compress: zip: %s is encrypted, which is not supported",
                  st.m_filename);
  if (!st.m_is_supported)
    return cz_err("compress: zip: %s uses an unsupported compression method",
                  st.m_filename);
  if (st.m_uncomp_size > (mz_uint64)max)
    return cz_err("compress: zip: %s: output exceeds the limit of %lld bytes",
                  st.m_filename, (long long)max);
  /* DEFLATE expands at most ~1032:1, so a declared size beyond that of the
   * whole archive is a lie, refused before it becomes an allocation (with
   * max 0, "no limit", nothing else would stop 2^60). */
  if (st.m_uncomp_size / 1032 > (mz_uint64)z->len + 1)
    return cz_err("compress: zip: %s: declared size %llu is impossible for "
                  "this archive", st.m_filename,
                  (unsigned long long)st.m_uncomp_size);
  size_t n = (size_t)st.m_uncomp_size;
  char *buf = (char *)malloc(n ? n : 1);
  if (!buf)
    return cz_err("compress: zip: %s: cannot allocate %llu bytes",
                  st.m_filename, (unsigned long long)st.m_uncomp_size);
  if (n && !mz_zip_reader_extract_to_mem(&z->za, (mz_uint)i, buf, n, 0)) {
    free(buf);
    char what[300];
    snprintf(what, sizeof what, "cannot extract %s", st.m_filename);
    return zip_err(&z->za, what);
  }
  return vl_str_take(buf, (int64_t)n);
}

/* ------------------------------------------------- streamed extraction
 *
 * extract_to's two passes (std/compress zip.voi): the first inflates every
 * member to nowhere, which checks its CRC-32 without keeping a byte; the
 * second inflates each straight into its file. Either way memory is one
 * inflation buffer, whatever the archive holds. The sink also stops at the
 * size the directory declares (and at max): miniz would otherwise inflate a
 * member whose data runs past its declaration to the end before noticing. */
typedef struct {
  FILE *f;        /* NULL: discard (the checking pass) */
  uint64_t n, limit;
  uint32_t crc;   /* of what was inflated: the verdict, in our words */
  bool over, ioerr;
} ZipSink;

static size_t zip_sink(void *opaque, mz_uint64 ofs, const void *buf, size_t n) {
  (void)ofs;
  ZipSink *k = (ZipSink *)opaque;
  if (k->n + n > k->limit) {
    k->over = true;
    return 0;
  }
  k->n += n;
  k->crc = (uint32_t)mz_crc32(k->crc, (const unsigned char *)buf, n);
  if (k->f && fwrite(buf, 1, n, k->f) != n) {
    k->ioerr = true;
    return 0;
  }
  return n;
}

/* compress._zip_stream(h, i, path, max) -> unit!: path "" checks only. */
static Value cz_zip_stream(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  ZipR *z = want_zr(argv[0], "compress.zip_stream");
  int64_t i = want_int(argv[1], "compress.zip_stream", 2);
  const char *path = vl_cstr(argv[2]);
  int64_t max = want_int(argv[3], "compress.zip_stream", 4);
  pthread_mutex_lock(&z->mu);
  Value out = vl_unit();
  mz_zip_archive_file_stat st;
  if (i < 0 || i >= (int64_t)mz_zip_reader_get_num_files(&z->za) ||
      !mz_zip_reader_file_stat(&z->za, (mz_uint)i, &st)) {
    out = zip_err(&z->za, "cannot read an entry");
  } else if (st.m_is_encrypted || !st.m_is_supported) {
    out = cz_err("compress: zip: %s is encrypted or uses an unsupported "
                 "method", st.m_filename);
  } else if (max > 0 && st.m_uncomp_size > (mz_uint64)max) {
    out = cz_err("compress: zip: %s: output exceeds the limit of %lld bytes",
                 st.m_filename, (long long)max);
  } else {
    ZipSink k = {NULL, 0, st.m_uncomp_size, MZ_CRC32_INIT, false, false};
    if (path[0] && !(k.f = fopen(path, "wb"))) {
      out = cz_err("compress: zip: cannot create %s: %s", path,
                   strerror(errno));
    } else {
      mz_bool ok = mz_zip_reader_extract_to_callback(
          &z->za, (mz_uint)i, zip_sink, &k, 0);
      if (k.f && fclose(k.f) != 0) k.ioerr = true;
      char what[300];
      snprintf(what, sizeof what, "cannot extract %s", st.m_filename);
      if (k.ioerr)
        out = cz_err("compress: zip: writing %s: %s", path, strerror(errno));
      else if (k.over)
        out = cz_err("compress: zip: %s: the member's data does not match "
                     "the archive's directory (corrupt or lying sizes)",
                     st.m_filename);
      else if (k.n == st.m_uncomp_size && k.crc != st.m_crc32)
        out = cz_err("compress: zip: %s: CRC-32 check failed", what);
      else if (!ok)
        out = zip_err(&z->za, what);
    }
  }
  pthread_mutex_unlock(&z->mu);
  return out;
}

typedef struct {
  mz_zip_archive za;
  bool open, finished;
  pthread_mutex_t mu; /* see ZipR */
} ZipW;

static void zw_free(void *ctx) {
  ZipW *z = (ZipW *)ctx;
  if (z->open) mz_zip_writer_end(&z->za);
  pthread_mutex_destroy(&z->mu);
  free(z);
}

static const VlHandleOps zw_ops = {"compress.ZipWriter", zw_free, NULL, NULL,
                                   NULL, NULL};

static ZipW *want_zw(Value v, const char *who) {
  ZipW *z = (ZipW *)vl_handle_ctx(v, &zw_ops);
  if (!z) vl_abort("%s: expected a zip writer, got %s", who, vl_kind_name(v));
  return z;
}

/* compress._zip_writer() -> handle!. */
static Value cz_zip_writer(Value *argv, int argc, VlFrame *up) {
  (void)argv, (void)argc, (void)up;
  ZipW *z = (ZipW *)vl_alloc(sizeof(ZipW));
  if (!mz_zip_writer_init_heap(&z->za, 0, 0)) {
    Value e = zip_err(&z->za, "cannot start an archive");
    free(z);
    return e;
  }
  z->open = true;
  pthread_mutex_init(&z->mu, NULL);
  return vl_handle_new(&zw_ops, z);
}

/* compress._zip_add(h, name, data, level, mtime) -> unit!. A name ending in
 * "/" with no data is a directory entry. mtime 0 means "now". */
static Value cz_zip_add(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  ZipW *z = want_zw(argv[0], "compress.zip_add");
  const char *name = vl_cstr(argv[1]);
  CzIn in = cz_in(argv[2], "compress.zip_add");
  int level = (int)want_int(argv[3], "compress.zip_add", 4);
  int64_t mt = want_int(argv[4], "compress.zip_add", 5);
  MZ_TIME_T t = (MZ_TIME_T)mt;
  pthread_mutex_lock(&z->mu);
  Value out = vl_unit();
  if (z->finished) {
    out = cz_err("compress: zip: the archive is finished");
  } else if (!mz_zip_writer_add_mem_ex_v2(
                 &z->za, name, in.p, in.n, NULL, 0, (mz_uint)level, 0, 0,
                 mt != 0 ? &t : NULL, NULL, 0, NULL, 0)) {
    char what[300];
    snprintf(what, sizeof what, "cannot add %s", name);
    out = zip_err(&z->za, what);
  }
  pthread_mutex_unlock(&z->mu);
  cz_in_free(&in);
  return out;
}

/* compress._zip_finish(h) -> str!: the archive's bytes. */
static Value cz_zip_finish(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  ZipW *z = want_zw(argv[0], "compress.zip_finish");
  pthread_mutex_lock(&z->mu);
  Value s;
  void *buf = NULL;
  size_t n = 0;
  bool was = z->finished;
  z->finished = true; /* finished either way: a failed finish is final */
  if (was) {
    s = cz_err("compress: zip: the archive is finished");
  } else if (!mz_zip_writer_finalize_heap_archive(&z->za, &buf, &n)) {
    s = zip_err(&z->za, "cannot finish the archive");
  } else {
    s = vl_str_n((const char *)buf, (int64_t)n);
    /* finalize hands the heap block over to the caller (it clears its own
     * pointer to it), so it is freed here; writer_end frees the rest. */
    mz_free(buf);
    mz_zip_writer_end(&z->za);
    z->open = false;
  }
  pthread_mutex_unlock(&z->mu);
  return s;
}

/* The natives table (voila_std.h): registered by the program's main() when
 * it uses std/compress. Every row is a name in low.StdSrcNatives. */
const VlExtEntry vl_ext_compress[] = {
    {"compress._encode", cz_encode},
    {"compress._decode", cz_decode},
    {"compress._crc32", cz_crc32},
    {"compress._adler32", cz_adler32},
    {"compress._writer", cz_writer},
    {"compress._reader", cz_reader},
    {"compress._zip_open", cz_zip_open},
    {"compress._zip_count", cz_zip_count},
    {"compress._zip_entry", cz_zip_entry},
    {"compress._zip_read", cz_zip_read},
    {"compress._zip_stream", cz_zip_stream},
    {"compress._zip_writer", cz_zip_writer},
    {"compress._zip_add", cz_zip_add},
    {"compress._zip_finish", cz_zip_finish},
    {NULL, NULL},
};
