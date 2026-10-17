/* err.c — error values, exceptions, unwinding, aborts (spec §8). */
#include "voila_int.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local VlCtx tls_ctx;
static _Thread_local bool tls_init;

void vl_unwind_to(int depth);

/* vl_ctx_teardown releases a task thread's context arrays. The main thread's
 * context lives for the whole process and is reclaimed by exit(). */
void vl_ctx_teardown(void) {
  if (!tls_init) return;
  free(tls_ctx.cleanups);
  tls_ctx.cleanups = NULL;
  tls_ctx.ncleanups = 0;
  tls_ctx.capcleanups = 0;
  free(tls_ctx.frames);
  free(tls_ctx.digit_stack);
  vl_release(tls_ctx.pending);
  tls_ctx.frames = NULL;
  tls_ctx.digit_stack = NULL;
  tls_ctx.pending = vl_nil();
  tls_ctx.nframes = 0;
  tls_ctx.capframes = 0;
  tls_ctx.ndigits = 0;
  tls_ctx.capdigits = 0;
}

VlCtx *vl_ctx(void) {
  if (!tls_init) {
    tls_init = true;
    tls_ctx.digits = 28; /* §3.1 default */
    tls_ctx.pending = vl_nil();
  }
  return &tls_ctx;
}

/* ---------------------------------------------------------------- numeric */

void vl_numeric_digits(int n) { vl_ctx()->digits = n > 0 ? n : 28; }
int vl_get_digits(void) { return vl_ctx()->digits; }

void vl_digits_save(void) {
  VlCtx *c = vl_ctx();
  if (c->ndigits == c->capdigits) {
    c->capdigits = c->capdigits ? c->capdigits * 2 : 8;
    c->digit_stack = (int *)realloc(c->digit_stack, sizeof(int) * (size_t)c->capdigits);
    if (!c->digit_stack) vl_abort("out of memory");
  }
  c->digit_stack[c->ndigits++] = c->digits;
}

void vl_digits_restore(void) {
  VlCtx *c = vl_ctx();
  if (c->ndigits > 0) c->digits = c->digit_stack[--c->ndigits];
}

/* ---------------------------------------------------------------- errors */

/* Builtin exception layouts, used when the program does not declare them. */
static const char *f_conv[] = {"from", "to", "value"};
static const char *f_msg[] = {"msg"};
static const char *f_op[] = {"op"};
static const char *f_after[] = {"after"};
static const char *f_key[] = {"key"};

static const VlType builtin_excs[] = {
    {"ConvError", 3, f_conv, NULL},   {"RangeError", 1, f_msg, NULL},
    {"OverflowError", 1, f_op, NULL}, {"FormatError", 1, f_msg, NULL},
    {"ValueError", 1, f_msg, NULL},   {"RuntimeError", 1, f_msg, NULL},
    {"IOError", 1, f_msg, NULL},      {"KeyError", 1, f_key, NULL},
    {"Timeout", 1, f_after, NULL},    {"Cancelled", 0, NULL, NULL},
};

const VlType *vl_user_type(const char *name); /* types.c */

static const VlType *exc_type(const char *name) {
  const VlType *t = vl_user_type(name);
  if (t) return t;
  for (size_t i = 0; i < sizeof(builtin_excs) / sizeof(builtin_excs[0]); i++)
    if (strcmp(builtin_excs[i].name, name) == 0) return &builtin_excs[i];
  return NULL;
}

/* vl_find_type answers for the builtin exception types too, so a program can
 * `throw ValueError{msg: "..."}` without declaring it. */
const VlType *vl_find_type(const char *name) { return exc_type(name); }

static Value err_new(const char *type, Value msg) {
  VlErr *e = (VlErr *)vl_obj_new(O_ERR, sizeof(VlErr));
  e->type_name = vl_str(type ? type : "");
  e->msg = msg;
  e->fields = vl_nil();
  e->cause = vl_nil();
  e->suppressed = vl_slice_new(0);
  e->trace = vl_slice_new(0);
  Value v;
  v.t = VL_OBJ;
  v.u.o = &e->hdr;
  return v;
}

Value vl_err(Value msg) { return err_new("", vl_tostr(msg)); }

/* vl_err_cstr builds an error value from a C string. Native code must use it
 * instead of vl_err(vl_str(m)): vl_err RETAINS its argument (vl_tostr), so the
 * temporary str would never be released and every error would leak it. */
Value vl_err_cstr(const char *msg) { return err_new("", vl_str(msg)); }

Value vl_errf(Value *argv, int argc) {
  if (argc == 0) return err_new("", vl_str(""));
  Value s = vl_sprintf_v(argv[0], argv + 1, argc - 1);
  return err_new("", s);
}

Value vl_exc_new(const char *type, Value *positional, int npos,
                 const char **names, Value *named, int nnamed) {
  Value e = err_new(type, vl_str(""));
  VlErr *ee = (VlErr *)e.u.o;
  Value fields = vl_map_new();
  const VlType *t = exc_type(type);
  if (t) {
    for (int i = 0; i < t->nfields; i++) {
      Value v = vl_nil();
      bool got = false;
      for (int j = 0; j < nnamed; j++) {
        if (strcmp(names[j], t->fields[i]) == 0) {
          v = vl_retain(named[j]);
          got = true;
          break;
        }
      }
      if (!got && i < npos) {
        v = vl_retain(positional[i]);
        got = true;
      }
      Value k = vl_str(t->fields[i]);
      vl_map_set(fields, k, v);
      vl_release(k);
    }
  } else {
    for (int j = 0; j < nnamed; j++) {
      Value k = vl_str(names[j]);
      vl_map_set(fields, k, vl_retain(named[j]));
      vl_release(k);
    }
  }
  vl_set(&ee->fields, fields);
  return e;
}

/* err_message renders the derived message (a user `message` impl is handled
 * by vl_callm before this is reached). */
Value vl_err_message(Value ev) {
  VlErr *e = (VlErr *)ev.u.o;
  const char *tn = vl_cstr(e->type_name);
  if (!tn[0]) return vl_retain(e->msg);

  bool has_fields = false;
  if (e->fields.t == VL_OBJ && e->fields.u.o->kind == O_MAP)
    has_fields = ((VlMap *)e->fields.u.o)->count > 0;

  /* A builtin error whose only field carries its whole message — `msg`
   * (IOError, RangeError, ValueError, …) or `op` (OverflowError) — returns
   * that text bare from `.message()` rather than the structural
   * `Type{msg: "…"}` form. Domain errors with other single fields
   * (NotFound{path}) or several fields (ParseError{line, col}) still render
   * structurally, so `.message()` stays informative for them. */
  if (has_fields) {
    VlMap *fm = (VlMap *)e->fields.u.o;
    if (fm->count == 1) {
      for (int64_t i = 0; i < fm->nents; i++) {
        if (!fm->ents[i].used) continue;
        if (fm->ents[i].k.t == VL_OBJ && fm->ents[i].k.u.o->kind == O_STR) {
          const char *fk = vl_cstr(fm->ents[i].k);
          if (strcmp(fk, "msg") == 0 || strcmp(fk, "op") == 0)
            return vl_tostr(fm->ents[i].v);
        }
        break;
      }
    }
  }

  Value b = vl_builder_new();
  vl_builder_write(b, e->type_name);
  if (has_fields) {
    VlMap *m = (VlMap *)e->fields.u.o;
    Value open = vl_str("{");
    vl_builder_write(b, open);
    vl_release(open);
    bool first = true;
    for (int64_t i = 0; i < m->nents; i++) {
      if (!m->ents[i].used) continue;
      if (!first) {
        Value sep = vl_str(", ");
        vl_builder_write(b, sep);
        vl_release(sep);
      }
      first = false;
      vl_builder_write(b, m->ents[i].k);
      Value colon = vl_str(": ");
      vl_builder_write(b, colon);
      vl_release(colon);
      /* values are quoted the way containers quote them */
      Value q = vl_tostr(m->ents[i].v);
      if (m->ents[i].v.t == VL_OBJ && m->ents[i].v.u.o->kind == O_STR) {
        Value qq = vl_builder_new();
        Value dq = vl_str("\"");
        vl_builder_write(qq, dq);
        vl_builder_write(qq, q);
        vl_builder_write(qq, dq);
        vl_release(dq);
        Value done = vl_builder_done(qq);
        vl_release(qq);
        vl_release(q);
        q = done;
      }
      vl_builder_write(b, q);
      vl_release(q);
    }
    Value close = vl_str("}");
    vl_builder_write(b, close);
    vl_release(close);
  } else if (vl_strlen(e->msg) > 0) {
    Value sep = vl_str(": ");
    vl_builder_write(b, sep);
    vl_release(sep);
    vl_builder_write(b, e->msg);
  }
  Value out = vl_builder_done(b);
  vl_release(b);
  return out;
}

bool vl_isfail(Value v) {
  if (v.t == VL_NIL) return true;
  return v.t == VL_OBJ && v.u.o->kind == O_ERR;
}

Value vl_tryp(Value v) { return vl_retain(v); }

Value vl_must(Value v) {
  if (v.t == VL_OBJ && v.u.o->kind == O_ERR) vl_throw(v);
  if (v.t == VL_NIL) vl_throwf("ValueError", "must: value is nil");
  return vl_retain(v);
}

bool vl_istype(Value exc, const char *types) {
  if (exc.t != VL_OBJ || exc.u.o->kind != O_ERR) return false;
  const char *tn = vl_cstr(((VlErr *)exc.u.o)->type_name);
  const char *p = types;
  while (*p) {
    const char *comma = strchr(p, ',');
    size_t n = comma ? (size_t)(comma - p) : strlen(p);
    if (n == 5 && strncmp(p, "Error", 5) == 0) return true; /* catch e: Error */
    if (strlen(tn) == n && strncmp(p, tn, n) == 0) return true;
    if (!comma) break;
    p = comma + 1;
  }
  return false;
}

/* ---------------------------------------------------------------- traces */

/* A stack trace is read off the task's live frame stack: every compiled
 * function pushes its frame and records which function it is (VlFnInfo)
 * and the line of the last statement that could fail. Innermost first, as
 * `name (file:line)`. A runaway recursion would print forever, so only the
 * innermost TRACE_MAX frames are shown and the rest are counted. */
enum { TRACE_MAX = 64 };

static void frame_text(const VlFrame *f, char *buf, size_t n) {
  if (f->line > 0)
    snprintf(buf, n, "%s (%s:%d)", f->info->name, f->info->file, f->line);
  else
    snprintf(buf, n, "%s (%s)", f->info->name, f->info->file);
}

/* vl_print_trace writes the current task's stack to `out`. */
static void vl_print_trace(FILE *out) {
  VlCtx *c = vl_ctx();
  int shown = 0, hidden = 0;
  for (int i = c->nframes - 1; i >= 0; i--) {
    const VlFrame *f = c->frames[i];
    if (!f || !f->info) continue;
    if (shown == TRACE_MAX) {
      hidden++;
      continue;
    }
    char buf[600];
    frame_text(f, buf, sizeof buf);
    fprintf(out, "  at %s\n", buf);
    shown++;
  }
  if (hidden) fprintf(out, "  ... %d more\n", hidden);
}

/* capture_trace records where an exception was FIRST thrown, as the slice
 * of strings `e.trace()` returns (§8.2). A rethrow keeps the original. */
static void capture_trace(Value exc) {
  if (exc.t != VL_OBJ || exc.u.o->kind != O_ERR) return;
  VlErr *e = (VlErr *)exc.u.o;
  if (e->trace.t != VL_OBJ || e->trace.u.o->kind != O_SLICE) return;
  if (((VlSlice *)e->trace.u.o)->len > 0) return;
  VlCtx *c = vl_ctx();
  int shown = 0;
  for (int i = c->nframes - 1; i >= 0 && shown < TRACE_MAX; i--) {
    const VlFrame *f = c->frames[i];
    if (!f || !f->info) continue;
    char buf[600];
    frame_text(f, buf, sizeof buf);
    vl_slice_append(e->trace, vl_str(buf));
    shown++;
  }
}

/* ---------------------------------------------------------------- unwind */

static void print_uncaught(Value exc);

void vl_eh_prepare(VlHandler *h) {
  VlCtx *c = vl_ctx();
  h->prev = c->handlers;
  h->frame_depth = c->nframes;
  h->cleanup_depth = c->ncleanups;
  c->handlers = h;
}

/* ----------------------------------------------------- unwind cleanups */

struct VlCleanup {
  int kind; /* 0 a Value slot, 1 malloc'd memory, 2 an int to restore,
              3 a lock to release (vl_lock_acquire), 4 a malloc'd array
              of v Values to release and free */
  void *p;
  int v;
};

static void cleanup_push(int kind, void *p, int v) {
  VlCtx *c = vl_ctx();
  if (c->ncleanups == c->capcleanups) {
    c->capcleanups = c->capcleanups ? c->capcleanups * 2 : 16;
    c->cleanups = (struct VlCleanup *)realloc(
        c->cleanups, sizeof(struct VlCleanup) * (size_t)c->capcleanups);
    if (!c->cleanups) vl_abort("out of memory");
  }
  c->cleanups[c->ncleanups].kind = kind;
  c->cleanups[c->ncleanups].p = p;
  c->cleanups[c->ncleanups].v = v;
  c->ncleanups++;
}

int vl_cleanup_mark(void) { return vl_ctx()->ncleanups; }
void vl_cleanup_slot(Value *slot) { cleanup_push(0, slot, 0); }
void vl_cleanup_mem(void **p) { cleanup_push(1, p, 0); }
void vl_cleanup_int(int *p, int v) { cleanup_push(2, p, v); }
void vl_cleanup_lock(VlLock *l) { cleanup_push(3, l, 0); }
void vl_cleanup_values(Value *a, int64_t n) { cleanup_push(4, a, (int)n); }

void vl_cleanup_to(int mark) {
  VlCtx *c = vl_ctx();
  if (mark < c->ncleanups) c->ncleanups = mark;
}

/* cleanup_unwind runs the entries above `depth`, newest first. It runs
 * BEFORE the longjmp, while the abandoned C frames the entries point into
 * are still intact. Each entry is popped before it runs, so a nested throw
 * (a release that runs a destructor cannot throw today, but a restore
 * might be followed by one) never runs it twice. */
static void cleanup_unwind(VlCtx *c, int depth) {
  while (c->ncleanups > depth) {
    struct VlCleanup e = c->cleanups[--c->ncleanups];
    if (e.kind == 0) {
      Value *slot = (Value *)e.p;
      Value v = *slot;
      *slot = vl_nil();
      vl_release(v);
    } else if (e.kind == 1) {
      void **m = (void **)e.p;
      free(*m);
      *m = NULL;
    } else if (e.kind == 3) {
      vl_lock_release((VlLock *)e.p);
    } else if (e.kind == 4) {
      Value *a = (Value *)e.p;
      for (int i = 0; i < e.v; i++) vl_release(a[i]);
      free(a);
    } else {
      *(int *)e.p = e.v;
    }
  }
}

/* throw_to delivers the in-flight exception (already in c->pending) to
 * handler h. The frames above it are unwound — their defers run — WHILE h
 * IS STILL INSTALLED, so a throw or a fault raised by a defer is delivered
 * to the same h: it becomes the exception (the latest one wins, as in Go
 * and Java), and the unwinding simply continues from where it is. Only then
 * are the natives' cleanups run, h popped, and control transferred. */
static void throw_to(VlCtx *c, VlHandler *h) {
  vl_unwind_to(h->frame_depth);
  cleanup_unwind(c, h->cleanup_depth);
  c->handlers = h->prev;
  longjmp(h->buf, 1);
}

void vl_eh_pop(void) {
  VlCtx *c = vl_ctx();
  if (c->handlers) c->handlers = c->handlers->prev;
}

/* vl_eh_current is what a `catch` block receives. A FAULT — an abort raised
 * under a recovery point, below — is not an exception a program may catch
 * (§8.6: aborts are uncatchable), so it is sent straight on to the next
 * handler, and the recovery point is the only one that will keep it. The
 * runtime's own handlers read the in-flight value with vl_eh_pending. */
Value vl_eh_current(void) {
  Value p = vl_ctx()->pending;
  if (vl_is_fault(p)) vl_rethrow(p);
  return vl_retain(p);
}

Value vl_eh_pending(void) { return vl_retain(vl_ctx()->pending); }

static void print_uncaught(Value exc) {
  if (vl_is_fault(exc)) {
    /* A fault that reached no recovery point (cannot happen: vl_fault only
     * throws under one) still prints as the abort it is — never under the
     * internal `$abort` name. */
    Value fm = ((VlErr *)exc.u.o)->msg;
    fprintf(stderr, "abort: %s\n", vl_cstr(fm));
    return;
  }
  Value m = vl_callm(exc, "message", NULL, 0);
  fprintf(stderr, "unhandled exception: %s\n", vl_cstr(m));
  vl_release(m);
  VlErr *e = (VlErr *)exc.u.o;
  /* Where it was thrown — the trace captured at the first throw, which is
   * still the right one after a rethrow or a delivery from a task. */
  if (e->trace.t == VL_OBJ && e->trace.u.o->kind == O_SLICE) {
    VlSlice *tr = (VlSlice *)e->trace.u.o;
    for (int64_t i = 0; i < tr->len; i++)
      fprintf(stderr, "  at %s\n", vl_cstr(tr->e[i]));
  }
  Value cause = e->cause;
  while (cause.t == VL_OBJ && cause.u.o->kind == O_ERR) {
    Value cm = vl_callm(cause, "message", NULL, 0);
    fprintf(stderr, "caused by: %s\n", vl_cstr(cm));
    vl_release(cm);
    cause = ((VlErr *)cause.u.o)->cause;
  }
}

/* vl_task_fail is defined in task.c; a throw escaping a task is delivered to
 * the group rather than killing the process (§8.7). */
bool vl_task_fail(Value exc);

void vl_throw(Value exc) {
  VlCtx *c = vl_ctx();
  capture_trace(exc);
  VlHandler *h = c->handlers;
  if (!h) {
    if (vl_task_fail(exc)) return; /* handled by the task wrapper */
    print_uncaught(exc);
    fflush(stdout);
    exit(1);
  }
  vl_set(&c->pending, vl_retain(exc));
  throw_to(c, h);
}

void vl_rethrow(Value exc) { vl_throw(exc); }

/* vl_throw_owned consumes the caller's reference: vl_throw longjmps, so a
 * plain vl_throw(e) from a builder site would leak `e` forever. */
static void vl_throw_owned(Value exc) {
  VlCtx *c = vl_ctx();
  capture_trace(exc);
  vl_set(&c->pending, exc); /* takes ownership; released on the next throw */
  VlHandler *h = c->handlers;
  if (!h) {
    print_uncaught(exc);
    fflush(stdout);
    exit(1);
  }
  throw_to(c, h);
}

void vl_throw_from(Value exc, Value cause) {
  if (exc.t == VL_OBJ && exc.u.o->kind == O_ERR)
    vl_set(&((VlErr *)exc.u.o)->cause, vl_retain(cause));
  vl_throw(exc);
}

void vl_throwf(const char *type, const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);

  const VlType *t = exc_type(type);
  Value e;
  if (t && t->nfields == 1 && (strcmp(t->fields[0], "msg") == 0 ||
                               strcmp(t->fields[0], "op") == 0)) {
    Value m = vl_str(buf);
    const char *names[1] = {t->fields[0]};
    Value named[1] = {m};
    e = vl_exc_new(type, NULL, 0, names, named, 1);
    vl_release(m);
  } else {
    e = err_new(type, vl_str(buf));
  }
  vl_throw_owned(e);
}

/* vl_throw_conv raises ConvError with the {from, to, value} shape (§14.4). */
void vl_throw_conv(const char *from, const char *to, Value value) {
  const char *names[3] = {"from", "to", "value"};
  Value sv = vl_tostr(value);
  Value named[3] = {vl_str(from), vl_str(to), sv};
  Value e = vl_exc_new("ConvError", NULL, 0, names, named, 3);
  for (int i = 0; i < 3; i++) vl_release(named[i]);
  vl_throw_owned(e);
}

/* vl_throw_conv_cstr is vl_throw_conv for a value that is still C text: the
 * caller has no Value to borrow, and one made only to be passed here could
 * never be released (the throw does not come back). */
void vl_throw_conv_cstr(const char *from, const char *to, const char *text) {
  const char *names[3] = {"from", "to", "value"};
  Value named[3] = {vl_str(from), vl_str(to), vl_str(text)};
  Value e = vl_exc_new("ConvError", NULL, 0, names, named, 3);
  for (int i = 0; i < 3; i++) vl_release(named[i]);
  vl_throw_owned(e);
  __builtin_unreachable(); /* vl_throw_owned longjmps or exits */
}

static Value abort_handler; /* os.on_abort */

void vl_set_abort_handler(Value h) { vl_set(&abort_handler, vl_retain(h)); }

/* ------------------------------------------------------------- faults
 *
 * An abort is a program DEFECT (§8.6): it is not an exception, no `catch`
 * sees it, and ordinarily it ends the process. A server, though, wants one
 * defective request — an index out of bounds, a field of nil, a call of a
 * non-function — to cost that request and not every other one in flight;
 * Go's net/http recovers a handler's panic for the same reason.
 *
 * So the aborts a program's own code can provoke at the language level are
 * raised with vl_fault instead of vl_abort. Outside a RECOVERY POINT the two
 * are identical. Under one (vl_call_contained, which std/http's server wraps
 * around each handler call) a fault is thrown as an error whose type is the
 * unspellable `$abort`: the throw unwinds normally — every frame's defers
 * run, and so does every `finally` (a try with one receives the fault with
 * CATCHF, runs the finally, and throws it on), so a `defer mu.unlock()` or
 * a `finally { mu.unlock() }` still releases its lock — while `catch`
 * clauses never see it (vl_eh_current passes it on), and the recovery point
 * turns it into an error value. A fault or a throw raised BY a defer or a
 * finally during that unwinding goes to the same recovery point and
 * becomes the request's fault (throw_to). A `cell.update` closure that
 * faults has its lock released and the fault carried on.
 *
 * What stays FATAL even under a recovery point: every vl_abort — running
 * out of memory, an internal invariant of the runtime — a stack overflow
 * (a signal, not a call), and a fault on a task the recovery point did not
 * start: recovery is per thread. The internal name `$abort` is never
 * printed: a fault is reported as `abort: <message>`. */
static void fatal(const char *buf) __attribute__((noreturn));

bool vl_is_fault(Value exc) {
  if (exc.t != VL_OBJ || exc.u.o->kind != O_ERR) return false;
  return strcmp(vl_cstr(((VlErr *)exc.u.o)->type_name), "$abort") == 0;
}

void vl_fault(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (vl_ctx()->recover > 0 && vl_ctx()->handlers)
    vl_throw_owned(err_new("$abort", vl_str(buf)));
  fatal(buf);
}

/* vl_call_contained(f, args…) calls f under a recovery point and returns
 * its result — or, when f faulted, an error value "abort: <message>". An
 * ordinary exception is not touched: it carries on to the caller's
 * handlers exactly as if f had been called directly. */
Value vl_call_contained(Value *argv, int argc, VlFrame *up) {
  (void)up;
  VlCtx *c = vl_ctx();
  c->recover++;
  VlHandler h;
  if (VL_TRY(h)) {
    c->recover--;
    Value e = vl_eh_pending();
    if (!vl_is_fault(e)) {
      vl_release(e);
      vl_rethrow(c->pending);
    }
    char msg[600];
    snprintf(msg, sizeof msg, "abort: %s", vl_cstr(((VlErr *)e.u.o)->msg));
    vl_release(e);
    return vl_err_cstr(msg);
  }
  Value r = vl_call(argv[0], argv + 1, argc - 1);
  vl_eh_pop();
  c->recover--;
  return r;
}

void vl_abort(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  fatal(buf);
}

/* fatal is an abort's ending: the on_abort hook, the message, the trace. */
static void fatal(const char *buf) {
  if (abort_handler.t == VL_OBJ) {
    Value h = abort_handler;
    abort_handler = vl_nil();
    vl_call(h, NULL, 0);
    vl_release(h);
  }
  fflush(stdout);
  fprintf(stderr, "abort: %s\n", buf);
  vl_print_trace(stderr);
  exit(1);
}
