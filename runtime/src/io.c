/* io.c — std/io: buffered readers, writers, and the process's own standard
 * handles.
 *
 * std/io is a C package, not a Voilà-source one, for a single reason: a
 * program must be able to write
 *
 *     each line in os.stdin { say line }
 *
 * with no `use` at all. `os.stdin` is therefore a package CONSTANT resolved by
 * vl_lookup_value, and the thing it yields is a runtime kind (O_READER) whose
 * methods vl_callm dispatches — the same shape O_FILE has had all along.
 *
 * Three rules govern everything below.
 *
 * 1. Errors are VALUES. Every failure returns an IOError made with
 *    vl_exc_new, exactly as os.read does (std.c), and nothing here throws. It
 *    is what makes `match`, `try`, `else` and `must` all compose over I/O:
 *    the caller chooses whether a failed read is fatal.
 *
 * 2. A reader that came from a Voilà value RETAINS it (VlReader.owner in
 *    voila_int.h) and re-reads the live handle out of it on every operation.
 *    Caching the FILE* instead would leave `io.reader(must os.open(p))`
 *    holding a stream that obj_free has already fclose()d.
 *
 * 3. The standard handles are singletons. vl_lookup_value runs again at every
 *    single use site, so a fresh buffered reader per `os.stdin` would throw
 *    away everything the previous one had read ahead.
 *
 * Portability: read(2)/write(2)/pipe(2)/isatty(3) are POSIX. The runtime has
 * no Windows port today, so the POSIX-only calls sit behind a _WIN32 stub arm
 * here rather than being spread across the runtime — a port becomes additive
 * work in this one file.
 */
#include "voila_int.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
/* No Windows port exists yet. These stubs keep the file COMPILING on a
 * hypothetical Windows build so the port is a matter of filling them in, not
 * of untangling #ifdefs from the reader logic below. */
static int io_read_fd(int fd, void *b, size_t n) {
  (void)fd; (void)b; (void)n;
  errno = ENOSYS;
  return -1;
}
static int io_write_fd(int fd, const void *b, size_t n) {
  (void)fd; (void)b; (void)n;
  errno = ENOSYS;
  return -1;
}
static void io_close_fd(int fd) { (void)fd; }
static bool io_isatty_fd(int fd) { (void)fd; return false; }
#define IO_HAVE_PIPE 0
#else
#include <unistd.h>
static int io_read_fd(int fd, void *b, size_t n) {
  return (int)read(fd, b, n);
}
static int io_write_fd(int fd, const void *b, size_t n) {
  return (int)write(fd, b, n);
}
static void io_close_fd(int fd) { close(fd); }
static bool io_isatty_fd(int fd) { return isatty(fd) == 1; }
#define IO_HAVE_PIPE 1
#endif

/* 8 KiB: one read-ahead per syscall is the whole point of the package, and it
 * is the same chunk read_all_file already uses in std.c. The buffer grows
 * beyond this only for a line (or a peek) longer than it. */
#define IO_BUFSZ 8192
/* A line with no newline in it is still a line, so read_line grows the buffer
 * rather than truncating — but not without bound: past this a stream that is
 * not line-oriented gets a diagnosis instead of the machine's memory. */
#define IO_MAXBUF (64 * 1024 * 1024)

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

static Value obj(VlObj *o) {
  Value v;
  v.t = VL_OBJ;
  v.u.o = o;
  return v;
}

static bool is_err(Value v) {
  return v.t == VL_OBJ && v.u.o->kind == O_ERR;
}

/* ---------------------------------------------------------------- errors */

static Value io_err(const char *msg) {
  Value m = vl_str(msg);
  Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
  vl_release(m);
  return e;
}

static Value io_errf(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  return io_err(buf);
}

/* A blocking read on a socket with SO_RCVTIMEO fails with EAGAIN when the
 * deadline passes. std/net's shim reports that as "timed out" and the Voilà
 * layer matches on it, so a reader over the same socket must say the same. */
static Value io_errno(const char *op) {
  if (errno == EAGAIN || errno == EWOULDBLOCK) return io_err("timed out");
  return io_errf("%s: %s", op, strerror(errno));
}

/* ---------------------------------------------------------------- objects */

static Value reader_new(FILE *f, int fd, Value owner, const char *name,
                        int64_t cap, bool own_src, bool is_std) {
  VlReader *r = (VlReader *)vl_obj_new(O_READER, sizeof(VlReader));
  r->f = f;
  r->fd = fd;
  r->owner = vl_retain(owner);
  r->name = vl_str(name);
  if (cap < 64) cap = 64; /* peek/read_line need room to make progress */
  if (cap > IO_MAXBUF) cap = IO_MAXBUF;
  r->cap = cap;
  r->b = (char *)vl_alloc((size_t)cap);
  r->r = 0;
  r->w = 0;
  r->eof = false;
  r->closed = false;
  r->own_src = own_src;
  r->is_std = is_std;
  pthread_mutex_init(&r->mu, NULL);
  return obj(&r->hdr);
}

static Value writer_new(FILE *f, int fd, Value owner, const char *name,
                        bool own_src, bool is_std, bool discard) {
  VlWriter *w = (VlWriter *)vl_obj_new(O_WRITER, sizeof(VlWriter));
  w->f = f;
  w->fd = fd;
  w->owner = vl_retain(owner);
  w->name = vl_str(name);
  w->closed = false;
  w->own_src = own_src;
  w->is_std = is_std;
  w->discard = discard;
  pthread_mutex_init(&w->mu, NULL);
  return obj(&w->hdr);
}

/* These are obj_free's arms (value.c). They are mandatory: value.c's switch
 * has no default and the build has no -Wswitch, so their absence would leak
 * the buffer, the mutex and the retained owner on every dropped handle. */
void vl_reader_free(VlObj *o) {
  VlReader *r = (VlReader *)o;
  if (!r->closed && !r->is_std && r->own_src) {
    if (r->f) fclose(r->f);
    else if (r->fd >= 0) io_close_fd(r->fd);
  }
  free(r->b);
  vl_release(r->owner);
  vl_release(r->name);
  pthread_mutex_destroy(&r->mu);
}

void vl_writer_free(VlObj *o) {
  VlWriter *w = (VlWriter *)o;
  if (!w->closed && !w->is_std && w->own_src) {
    if (w->f) fclose(w->f);
    else if (w->fd >= 0) io_close_fd(w->fd);
  }
  vl_release(w->owner);
  vl_release(w->name);
  pthread_mutex_destroy(&w->mu);
}

/* ---------------------------------------------------------------- source */

/* sock_src is a socket's byte source: its filter (std/tls's session, see
 * VlSocket.flt) when one is installed, else its descriptor. */
static bool sock_src(VlSocket *s, int *fd, VlHandle **h, const char **why) {
  if (s->closed || s->fd < 0) {
    *why = "the underlying socket is closed";
    return false;
  }
  if (s->flt.t == VL_OBJ) *h = (VlHandle *)s->flt.u.o;
  else *fd = s->fd;
  return true;
}

/* rd_src resolves where the bytes come from RIGHT NOW — see the VlReader
 * comment in voila_int.h for why this is a lookup rather than a cached
 * handle. Exactly one of *f, *fd and *h is set: a FILE*, a descriptor, or
 * an extension's handle (an O_HANDLE owner, or a socket's filter). Returns
 * false with *why set when the source is gone. */
static bool rd_src(VlReader *r, FILE **f, int *fd, VlHandle **h,
                   const char **why) {
  *f = NULL;
  *fd = -1;
  *h = NULL;
  *why = NULL;
  if (r->closed) {
    *why = "reader is closed";
    return false;
  }
  if (r->owner.t == VL_OBJ && r->owner.u.o->kind == O_FILE) {
    VlFile *o = (VlFile *)r->owner.u.o;
    if (o->closed || !o->f) {
      *why = "the underlying file is closed";
      return false;
    }
    *f = o->f;
    return true;
  }
  if (r->owner.t == VL_OBJ && r->owner.u.o->kind == O_SOCKET)
    return sock_src((VlSocket *)r->owner.u.o, fd, h, why);
  if (r->owner.t == VL_OBJ && r->owner.u.o->kind == O_HANDLE) {
    *h = (VlHandle *)r->owner.u.o;
    return true;
  }
  if (r->f) {
    *f = r->f;
    return true;
  }
  if (r->fd >= 0) {
    *fd = r->fd;
    return true;
  }
  *why = "reader has no source";
  return false;
}

static bool wr_src(VlWriter *w, FILE **f, int *fd, VlHandle **h,
                   const char **why) {
  *f = NULL;
  *fd = -1;
  *h = NULL;
  *why = NULL;
  if (w->closed) {
    *why = "writer is closed";
    return false;
  }
  if (w->owner.t == VL_OBJ && w->owner.u.o->kind == O_FILE) {
    VlFile *o = (VlFile *)w->owner.u.o;
    if (o->closed || !o->f) {
      *why = "the underlying file is closed";
      return false;
    }
    *f = o->f;
    return true;
  }
  if (w->owner.t == VL_OBJ && w->owner.u.o->kind == O_SOCKET)
    return sock_src((VlSocket *)w->owner.u.o, fd, h, why);
  if (w->owner.t == VL_OBJ && w->owner.u.o->kind == O_HANDLE) {
    *h = (VlHandle *)w->owner.u.o;
    return true;
  }
  if (w->f) {
    *f = w->f;
    return true;
  }
  if (w->fd >= 0) {
    *fd = w->fd;
    return true;
  }
  *why = "writer has no sink";
  return false;
}

/* ---------------------------------------------------------------- buffer */

static bool rd_grow(VlReader *r, int64_t want, Value *err) {
  if (want > IO_MAXBUF) {
    *err = io_errf("record longer than %lld bytes: this stream is not "
                   "line-oriented", (long long)IO_MAXBUF);
    return false;
  }
  char *nb = (char *)realloc(r->b, (size_t)want);
  if (!nb) vl_abort("out of memory");
  r->b = nb;
  r->cap = want;
  return true;
}

/* rd_fill reads one chunk into the buffer. Returns the number of bytes added,
 * 0 at end of stream, or -1 with *err holding an IOError value. It compacts
 * first and grows when the buffer is already full, so a caller may loop until
 * it has what it needs without imposing a length limit of its own. */
static int64_t rd_fill(VlReader *r, Value *err) {
  FILE *f;
  int fd;
  VlHandle *h;
  const char *why;
  if (!rd_src(r, &f, &fd, &h, &why)) {
    *err = io_err(why);
    return -1;
  }
  if (r->r > 0) { /* drop what has already been handed out */
    memmove(r->b, r->b + r->r, (size_t)(r->w - r->r));
    r->w -= r->r;
    r->r = 0;
  }
  if (r->w == r->cap && !rd_grow(r, r->cap * 2, err)) return -1;
  size_t room = (size_t)(r->cap - r->w);
  if (h) {
    if (!h->ops->read) {
      *err = io_errf("%s cannot be read", h->ops->name);
      return -1;
    }
    int64_t n = h->ops->read(h->ctx, r->b + r->w, (int64_t)room, err);
    if (n < 0) return -1;
    if (n == 0) {
      r->eof = true;
      return 0;
    }
    r->w += n;
    return n;
  }
  if (f) {
    size_t n = fread(r->b + r->w, 1, room, f);
    if (n == 0) {
      if (ferror(f)) {
        clearerr(f);
        *err = io_errno("read");
        return -1;
      }
      r->eof = true;
      return 0;
    }
    r->w += (int64_t)n;
    return (int64_t)n;
  }
  for (;;) {
    int n = io_read_fd(fd, r->b + r->w, room);
    if (n < 0) {
      if (errno == EINTR) continue; /* a signal, not a failure */
      *err = io_errno("read");
      return -1;
    }
    if (n == 0) {
      r->eof = true;
      return 0;
    }
    r->w += n;
    return n;
  }
}

/* ---------------------------------------------------------------- reading */

/* rd_line returns the next line INCLUDING its "\n", or "" at end of stream.
 * "" is unambiguous: a blank line reads as "\n". This is the convention
 * std/net's Conn.read_line has always used, and `each` relies on it. */
static Value rd_line(VlReader *r) {
  for (;;) {
    const char *nl =
        (const char *)memchr(r->b + r->r, '\n', (size_t)(r->w - r->r));
    if (nl) {
      int64_t n = (int64_t)(nl - (r->b + r->r)) + 1;
      Value s = vl_str_n(r->b + r->r, n);
      r->r += n;
      return s;
    }
    Value err = vl_nil();
    int64_t got = rd_fill(r, &err);
    if (got < 0) return err;
    if (got == 0) {
      /* End of stream: whatever is left is the last, unterminated line —
       * and "" when there is nothing left, which is the end marker. */
      int64_t n = r->w - r->r;
      Value s = vl_str_n(r->b + r->r, n);
      r->r += n;
      return s;
    }
  }
}

/* rd_read returns UP TO `want` bytes: what is already buffered, else one
 * refill's worth. "" means end of stream. */
static Value rd_read(VlReader *r, int64_t want) {
  if (want <= 0) return vl_str("");
  if (r->r == r->w) {
    Value err = vl_nil();
    int64_t got = rd_fill(r, &err);
    if (got < 0) return err;
    if (got == 0) return vl_str("");
  }
  int64_t have = r->w - r->r;
  int64_t n = have < want ? have : want;
  Value s = vl_str_n(r->b + r->r, n);
  r->r += n;
  return s;
}

static Value rd_all(VlReader *r) {
  Value b = vl_builder_new();
  for (;;) {
    if (r->r < r->w) {
      Value s = vl_str_n(r->b + r->r, r->w - r->r);
      vl_builder_write(b, s);
      vl_release(s);
      r->r = r->w;
    }
    Value err = vl_nil();
    int64_t got = rd_fill(r, &err);
    if (got < 0) {
      vl_release(b);
      return err;
    }
    if (got == 0) break;
  }
  Value out = vl_builder_done(b);
  vl_release(b);
  return out;
}

/* rd_byte returns 0..255, or -1 at end of stream — a byte value and its end
 * marker cannot collide, so no wrapper type is needed. */
static Value rd_byte(VlReader *r) {
  if (r->r == r->w) {
    Value err = vl_nil();
    int64_t got = rd_fill(r, &err);
    if (got < 0) return err;
    if (got == 0) return vl_int(-1);
  }
  return vl_int((unsigned char)r->b[r->r++]);
}

/* rd_peek buffers up to `want` bytes and returns them WITHOUT consuming, so a
 * parser can decide what to do before committing to a read. */
static Value rd_peek(VlReader *r, int64_t want) {
  if (want <= 0) return vl_str("");
  while (r->w - r->r < want) {
    Value err = vl_nil();
    int64_t got = rd_fill(r, &err);
    if (got < 0) return err;
    if (got == 0) break; /* end of stream: return the short tail */
  }
  int64_t have = r->w - r->r;
  int64_t n = have < want ? have : want;
  return vl_str_n(r->b + r->r, n);
}

/* rd_at_eof must actually LOOK: a stream is only at its end once a read has
 * come back empty, so this fills when the buffer is dry. That read blocks, in
 * exactly the way the next read_line would have. */
static Value rd_at_eof(VlReader *r) {
  if (r->r < r->w) return vl_bool(false);
  if (r->eof) return vl_bool(true);
  Value err = vl_nil();
  int64_t got = rd_fill(r, &err);
  if (got < 0) return err;
  return vl_bool(got == 0);
}

/* handle_close ends an extension stream the reader or writer OWNS (own_src:
 * io.reader/io.writer over an O_HANDLE). It is the handle's close op that
 * writes a compressor's trailer, so its error is the close's result.
 * Returns unit, or the owned error value. */
static Value handle_close(bool own_src, Value owner) {
  if (!own_src || owner.t != VL_OBJ || owner.u.o->kind != O_HANDLE)
    return vl_unit();
  VlHandle *h = (VlHandle *)owner.u.o;
  if (!h->ops->close) return vl_unit();
  Value err = vl_nil();
  if (!h->ops->close(h->ctx, &err)) return err;
  return vl_unit();
}

/* rd_close is idempotent, and its real work is releasing `owner`: that is the
 * moment a file the reader was keeping alive is finally fclose()d, in
 * obj_free. A standard handle is exempt — os.stdin is process-wide and shared
 * with every other lookup site, so closing one would poison the rest of the
 * program; close() on it is a no-op by design. */
static Value rd_close(VlReader *r) {
  if (r->is_std || r->closed) return vl_unit();
  if (r->own_src) {
    if (r->f) fclose(r->f);
    else if (r->fd >= 0) io_close_fd(r->fd);
  }
  Value herr = handle_close(r->own_src, r->owner);
  r->f = NULL;
  r->fd = -1;
  r->closed = true;
  r->eof = true;
  r->r = 0;
  r->w = 0;
  Value o = r->owner;
  r->owner = vl_nil();
  vl_release(o);
  return herr;
}

/* ---------------------------------------------------------------- writing */

static Value wr_bytes(VlWriter *w, const char *p, int64_t n) {
  /* Closed BEFORE discard: the close contract must not depend on which kind
   * of writer this is. wr_src reports it for every other sink, and a discard
   * writer that kept accepting writes after close would be the one sink that
   * silently disagreed. */
  if (w->closed) return io_err("writer is closed");
  if (w->discard) return vl_unit();
  FILE *f;
  int fd;
  VlHandle *h;
  const char *why;
  if (!wr_src(w, &f, &fd, &h, &why)) return io_err(why);
  if (h) {
    if (!h->ops->write) return io_errf("%s cannot be written", h->ops->name);
    while (n > 0) {
      Value err = vl_nil();
      int64_t k = h->ops->write(h->ctx, p, n, &err);
      if (k < 0) return err;
      if (k == 0) return io_err("short write: the sink accepted no bytes");
      p += k;
      n -= k;
    }
    return vl_unit();
  }
  if (f) {
    if (n > 0 && fwrite(p, 1, (size_t)n, f) != (size_t)n)
      return io_errno("write");
    return vl_unit();
  }
  while (n > 0) {
    int k = io_write_fd(fd, p, (size_t)n);
    if (k < 0) {
      if (errno == EINTR) continue;
      return io_errno("write");
    }
    if (k == 0) return io_err("short write: the sink accepted no bytes");
    p += k;
    n -= k;
  }
  return vl_unit();
}

/* wr_print renders the arguments the way `say` and fmt.print do — one space
 * between them — so a writer is a drop-in destination for output that used to
 * go to stdout. write_all takes the other route: exactly the bytes given. */
static Value wr_render(Value *argv, int argc, bool newline) {
  Value b = vl_builder_new();
  for (int i = 0; i < argc; i++) {
    if (i) {
      Value sp = vl_str(" ");
      vl_builder_write(b, sp);
      vl_release(sp);
    }
    vl_builder_write(b, argv[i]);
  }
  if (newline) {
    Value nl = vl_str("\n");
    vl_builder_write(b, nl);
    vl_release(nl);
  }
  Value s = vl_builder_done(b);
  vl_release(b);
  return s;
}

static Value wr_flush(VlWriter *w) {
  if (w->closed) return io_err("writer is closed");
  if (w->discard) return vl_unit();
  FILE *f;
  int fd;
  VlHandle *h;
  const char *why;
  if (!wr_src(w, &f, &fd, &h, &why)) return io_err(why);
  if (h && h->ops->flush) {
    Value err = vl_nil();
    if (!h->ops->flush(h->ctx, &err)) return err;
  }
  if (f && fflush(f) != 0) return io_errno("flush");
  return vl_unit();
}

/* close flushes first — an unflushed buffer is data loss, and the caller
 * asked to be done, not to lose the tail. Standard handles are exempt for the
 * same reason readers are: they are shared with `say`. */
static Value wr_close(VlWriter *w) {
  if (w->is_std || w->closed) {
    if (w->is_std) return wr_flush(w);
    return vl_unit();
  }
  Value out = wr_flush(w);
  if (w->own_src) {
    if (w->f) fclose(w->f);
    else if (w->fd >= 0) io_close_fd(w->fd);
  }
  /* An extension stream's close can FAIL (a compressor's trailer is its
   * last write), and that failure is the caller's to see: losing it would
   * report a truncated archive as a clean close. */
  Value herr = handle_close(w->own_src, w->owner);
  /* For an extension stream a failed final flush is the close's failure
   * too (unless the handle's own close already reported one): the data it
   * held is lost, and a compressor's output would be truncated. (A file's
   * or a socket's close keeps its old contract of not reporting it.) */
  bool hstream = w->owner.t == VL_OBJ && w->owner.u.o->kind == O_HANDLE;
  if (hstream && is_err(out) && !is_err(herr)) {
    vl_release(herr);
    herr = out;
  } else {
    vl_release(out);
  }
  w->f = NULL;
  w->fd = -1;
  w->closed = true;
  Value o = w->owner;
  w->owner = vl_nil();
  vl_release(o);
  return herr;
}

/* ---------------------------------------------------------------- methods */

static int64_t want_count(Value *argv, int argc, int64_t dflt) {
  if (argc < 1 || argv[0].t != VL_INT) return dflt;
  return argv[0].u.i;
}

Value vl_reader_method(Value self, const char *name, Value *argv, int argc,
                       bool *ok) {
  VlReader *r = (VlReader *)self.u.o;
  *ok = true;
  /* name is fixed at construction, so it needs no lock. */
  if (eq(name, "name")) return vl_retain(r->name);

  pthread_mutex_lock(&r->mu);
  Value out;
  if (eq(name, "read_line")) {
    out = rd_line(r);
  } else if (eq(name, "read")) {
    out = rd_read(r, want_count(argv, argc, IO_BUFSZ));
  } else if (eq(name, "read_all")) {
    out = rd_all(r);
  } else if (eq(name, "read_byte")) {
    out = rd_byte(r);
  } else if (eq(name, "peek")) {
    out = rd_peek(r, want_count(argv, argc, 1));
  } else if (eq(name, "at_eof")) {
    out = rd_at_eof(r);
  } else if (eq(name, "close")) {
    out = rd_close(r);
  } else if (eq(name, "records")) {
    int64_t width = want_count(argv, argc, 0);
    if (width <= 0) {
      out = io_err("records(width): the record width must be positive");
    } else {
      Value text = rd_all(r);
      if (is_err(text)) out = text;
      else {
        out = vl_records_of(text, width);
        vl_release(text);
      }
    }
  } else {
    *ok = false;
    out = vl_nil();
  }
  pthread_mutex_unlock(&r->mu);
  return out;
}

Value vl_writer_method(Value self, const char *name, Value *argv, int argc,
                       bool *ok) {
  VlWriter *w = (VlWriter *)self.u.o;
  *ok = true;
  if (eq(name, "name")) return vl_retain(w->name);

  /* RENDER BEFORE LOCKING. Turning a value into bytes runs vl_tostr, which
   * runs a user `show` — arbitrary user code — and w->mu is a plain
   * non-recursive mutex over a PROCESS-WIDE singleton for os.stdout and
   * os.stderr. Holding it across that code had two failure modes, both
   * hangs: a `show` that writes to the same handle deadlocked against
   * itself, and a `show` that THREW longjmp'd straight past the unlock, so
   * the exception was caught normally and then the next innocent write
   * anywhere in the program blocked forever on a mutex nobody would ever
   * release. Nothing under the lock below can re-enter or throw. */
  if (eq(name, "write") || eq(name, "writeln")) {
    Value s = wr_render(argv, argc, eq(name, "writeln"));
    pthread_mutex_lock(&w->mu);
    Value out = wr_bytes(w, vl_cstr(s), vl_strlen(s));
    pthread_mutex_unlock(&w->mu);
    vl_release(s);
    return out;
  }
  if (eq(name, "write_all")) {
    Value s = argc ? vl_tostr(argv[0]) : vl_str("");
    pthread_mutex_lock(&w->mu);
    Value out = wr_bytes(w, vl_cstr(s), vl_strlen(s));
    pthread_mutex_unlock(&w->mu);
    vl_release(s);
    return out;
  }

  pthread_mutex_lock(&w->mu);
  Value out;
  if (eq(name, "flush")) {
    out = wr_flush(w);
  } else if (eq(name, "close")) {
    out = wr_close(w);
  } else {
    *ok = false;
    out = vl_nil();
  }
  pthread_mutex_unlock(&w->mu);
  return out;
}

/* ---------------------------------------------------------------- unifier */

/* handle_of picks the field of a struct that names a byte stream, in the
 * direction the caller asked for. It prefers an existing stream of that KIND
 * -- so io.reader(conn) hands back the very reader conn.read_line() is using
 * rather than a rival buffer over the same socket. Failing that it takes `h`,
 * the name std/net gives its opaque socket handle (`struct Conn { h Sock }`),
 * which is how io.reader and io.writer work on a net.Conn with std/net
 * knowing nothing about std/io.
 *
 * `want` is load-bearing, not decoration: a Conn that has been READ from
 * carries a cached O_READER, and a direction-blind search would hand that
 * reader to io.writer, which cannot write to it -- so io.writer(conn) would
 * fail on exactly the connections that had been used. */
static Value handle_of(Value src, VlKind want) {
  VlStruct *s = (VlStruct *)src.u.o;
  const VlType *t = s->type;
  int hi = -1;
  for (int i = 0; i < t->nfields; i++) {
    Value f = s->f[i];
    if (f.t == VL_OBJ && f.u.o->kind == want) return f;
    if (hi < 0 && strcmp(t->fields[i], "h") == 0) hi = i;
  }
  return hi >= 0 ? s->f[hi] : vl_nil();
}

/* reader_over is io.reader's one step: it does NOT recurse, so the struct arm
 * of vl_reader_of can retry exactly once and a struct whose `h` is another
 * struct is reported rather than chased. */
static Value reader_over(Value src, int64_t cap) {
  if (src.t != VL_OBJ) return vl_nil();
  switch ((VlKind)src.u.o->kind) {
  case O_READER:
    return vl_retain(src); /* idempotent: io.reader(io.reader(x)) is x */
  case O_FILE:
    return reader_new(NULL, -1, src, vl_cstr(((VlFile *)src.u.o)->name), cap,
                      false, false);
  case O_SOCKET:
    return reader_new(NULL, -1, src, "<socket>", cap, false, false);
  case O_HANDLE: {
    /* An extension's stream (a decompressor): the reader OWNS it, so the
     * reader's close is the stream's close (handle_close). */
    VlHandle *h = (VlHandle *)src.u.o;
    if (!h->ops->read) return vl_nil();
    return reader_new(NULL, -1, src, h->ops->name, cap, true, false);
  }
  default:
    return vl_nil();
  }
}

static Value writer_over(Value dst) {
  if (dst.t != VL_OBJ) return vl_nil();
  switch ((VlKind)dst.u.o->kind) {
  case O_WRITER:
    return vl_retain(dst);
  case O_FILE:
    return writer_new(NULL, -1, dst, vl_cstr(((VlFile *)dst.u.o)->name), false,
                      false, false);
  case O_SOCKET:
    return writer_new(NULL, -1, dst, "<socket>", false, false, false);
  case O_HANDLE: {
    VlHandle *h = (VlHandle *)dst.u.o;
    if (!h->ops->write) return vl_nil();
    return writer_new(NULL, -1, dst, h->ops->name, true, false, false);
  }
  default:
    return vl_nil();
  }
}

static Value reader_of_cap(Value src, int64_t cap) {
  Value r = reader_over(src, cap);
  if (r.t != VL_NIL) return r;
  if (src.t == VL_OBJ && src.u.o->kind == O_STRUCT) {
    Value h = handle_of(src, O_READER);
    r = reader_over(h, cap);
    if (r.t != VL_NIL) return r;
  }
  return io_errf("io.reader: cannot read from %s", vl_kind_name(src));
}

Value vl_reader_of(Value src) { return reader_of_cap(src, IO_BUFSZ); }

Value vl_writer_of(Value dst) {
  Value w = writer_over(dst);
  if (w.t != VL_NIL) return w;
  if (dst.t == VL_OBJ && dst.u.o->kind == O_STRUCT) {
    Value h = handle_of(dst, O_WRITER);
    w = writer_over(h);
    if (w.t != VL_NIL) return w;
  }
  return io_errf("io.writer: cannot write to %s", vl_kind_name(dst));
}

bool vl_io_isatty(Value h) {
  if (h.t != VL_OBJ) return false;
  switch ((VlKind)h.u.o->kind) {
  case O_READER: {
    VlReader *r = (VlReader *)h.u.o;
    FILE *f;
    int fd;
    VlHandle *hh;
    const char *why;
    if (!rd_src(r, &f, &fd, &hh, &why) || hh) return false;
    return f ? io_isatty_fd(fileno(f)) : io_isatty_fd(fd);
  }
  case O_WRITER: {
    VlWriter *w = (VlWriter *)h.u.o;
    FILE *f;
    int fd;
    VlHandle *hh;
    const char *why;
    if (!wr_src(w, &f, &fd, &hh, &why) || hh) return false;
    return f ? io_isatty_fd(fileno(f)) : io_isatty_fd(fd);
  }
  case O_FILE: {
    VlFile *o = (VlFile *)h.u.o;
    return o->f && !o->closed && io_isatty_fd(fileno(o->f));
  }
  case O_SOCKET: {
    VlSocket *s = (VlSocket *)h.u.o;
    return !s->closed && s->fd >= 0 && io_isatty_fd(s->fd);
  }
  default:
    return false;
  }
}

/* ------------------------------------------------------- standard handles */

static Value g_stdin, g_stdout, g_stderr;
static pthread_once_t g_std_once = PTHREAD_ONCE_INIT;

static void make_std_handles(void) {
  Value none = vl_nil();
  g_stdin = reader_new(stdin, -1, none, "<stdin>", IO_BUFSZ, false, true);
  g_stdout = writer_new(stdout, -1, none, "<stdout>", false, true, false);
  g_stderr = writer_new(stderr, -1, none, "<stderr>", false, true, false);
}

/* vl_init calls this, but pthread_once guards it anyway: vl_stdin() is also
 * reached from vl_lookup_value, and nothing in the ABI promises that the
 * generated program's start-up ran first. */
void vl_io_init(void) { pthread_once(&g_std_once, make_std_handles); }

Value vl_stdin(void) {
  vl_io_init();
  return g_stdin; /* borrowed */
}

Value vl_stdout(void) {
  vl_io_init();
  return g_stdout;
}

Value vl_stderr(void) {
  vl_io_init();
  return g_stderr;
}

/* ---------------------------------------------------------------- natives */

#define IOFN (void)argv, (void)argc, (void)up

Value vl_io_reader(Value *argv, int argc, VlFrame *up) {
  IOFN;
  if (argc < 1) return io_err("io.reader(src): a source is required");
  return vl_reader_of(argv[0]);
}

Value vl_io_writer(Value *argv, int argc, VlFrame *up) {
  IOFN;
  if (argc < 1) return io_err("io.writer(dst): a destination is required");
  return vl_writer_of(argv[0]);
}

/* io.buffered(src, cap) is io.reader with the read-ahead size named. Handed
 * a reader that already exists it GROWS that one rather than wrapping it: a
 * second buffer over the same stream would strand whatever the first held. */
Value vl_io_buffered(Value *argv, int argc, VlFrame *up) {
  IOFN;
  if (argc < 1) return io_err("io.buffered(src, cap): a source is required");
  int64_t cap = want_count(argv + 1, argc - 1, IO_BUFSZ);
  if (cap <= 0) cap = IO_BUFSZ;
  if (cap > IO_MAXBUF) cap = IO_MAXBUF;
  if (argv[0].t == VL_OBJ && argv[0].u.o->kind == O_READER) {
    VlReader *r = (VlReader *)argv[0].u.o;
    pthread_mutex_lock(&r->mu);
    Value err = vl_nil();
    bool grew = r->cap >= cap || rd_grow(r, cap, &err);
    pthread_mutex_unlock(&r->mu);
    if (!grew) return err;
    return vl_retain(argv[0]);
  }
  return reader_of_cap(argv[0], cap);
}

/* io.copy(dst, src) moves the whole of src into dst and returns the byte
 * count. Both arguments go through the unifiers, so `io.copy(os.stdout,
 * must os.open(p))` is a complete cat. */
Value vl_io_copy(Value *argv, int argc, VlFrame *up) {
  IOFN;
  if (argc < 2) return io_err("io.copy(dst, src): two handles are required");
  Value w = vl_writer_of(argv[0]);
  if (is_err(w)) return w;
  Value r = vl_reader_of(argv[1]);
  if (is_err(r)) {
    vl_release(w);
    return r;
  }
  int64_t total = 0;
  Value out = vl_nil();
  for (;;) {
    bool ok = false;
    Value n = vl_int(IO_BUFSZ);
    Value chunk = vl_reader_method(r, "read", &n, 1, &ok);
    if (is_err(chunk)) {
      out = chunk;
      break;
    }
    int64_t got = vl_strlen(chunk);
    if (got == 0) {
      vl_release(chunk);
      out = vl_int(total);
      break;
    }
    Value res = vl_writer_method(w, "write_all", &chunk, 1, &ok);
    vl_release(chunk);
    if (is_err(res)) {
      out = res;
      break;
    }
    vl_release(res);
    total += got;
  }
  vl_release(r);
  vl_release(w);
  return out;
}

/* io.pipe() -> (reader, writer) over an OS pipe. The two ends own their
 * descriptors, so dropping both closes the pipe. Note the OS pipe buffer is
 * finite: a writer that outruns its reader by more than that blocks, which is
 * the point when the two ends live in different tasks and a bug when they do
 * not. */
Value vl_io_pipe(Value *argv, int argc, VlFrame *up) {
  IOFN;
#if !IO_HAVE_PIPE
  return io_err("io.pipe: not supported on this platform");
#else
  int fds[2];
  if (pipe(fds) != 0) return io_errno("io.pipe");
  /* Close-on-exec, like every descriptor the runtime opens (std.c). */
  fcntl(fds[0], F_SETFD, FD_CLOEXEC);
  fcntl(fds[1], F_SETFD, FD_CLOEXEC);
  Value none = vl_nil();
  Value r = reader_new(NULL, fds[0], none, "<pipe>", IO_BUFSZ, true, false);
  Value w = writer_new(NULL, fds[1], none, "<pipe>", true, false, false);
  Value pair[2];
  pair[0] = r;
  pair[1] = w;
  Value t = vl_tuple_new(pair, 2); /* retains both */
  vl_release(r);
  vl_release(w);
  return t;
#endif
}

/* A reader / writer that owns a raw descriptor: os.start's pipes to a child
 * (proc.c). Dropping it closes the descriptor, as for io.pipe. */
Value vl_io_fd_reader(int fd, const char *name) {
  Value none = vl_nil();
  return reader_new(NULL, fd, none, name, IO_BUFSZ, true, false);
}

Value vl_io_fd_writer(int fd, const char *name) {
  Value none = vl_nil();
  return writer_new(NULL, fd, none, name, true, false, false);
}

Value vl_io_read_all(Value *argv, int argc, VlFrame *up) {
  IOFN;
  if (argc < 1) return io_err("io.read_all(src): a source is required");
  Value r = vl_reader_of(argv[0]);
  if (is_err(r)) return r;
  bool ok = false;
  Value out = vl_reader_method(r, "read_all", NULL, 0, &ok);
  vl_release(r);
  return out;
}

/* io.discard() is the /dev/null of writers: it accepts everything and keeps
 * none of it, for silencing output a program does not want without teaching
 * every caller about an optional destination. */
Value vl_io_discard(Value *argv, int argc, VlFrame *up) {
  IOFN;
  Value none = vl_nil();
  return writer_new(NULL, -1, none, "<discard>", false, false, true);
}

#undef IOFN
