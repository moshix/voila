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

/* ---------------------------------------------------------------- unwind */

static void print_uncaught(Value exc);

void vl_eh_prepare(VlHandler *h) {
  VlCtx *c = vl_ctx();
  h->prev = c->handlers;
  h->frame_depth = c->nframes;
  c->handlers = h;
}

void vl_eh_pop(void) {
  VlCtx *c = vl_ctx();
  if (c->handlers) c->handlers = c->handlers->prev;
}

Value vl_eh_current(void) { return vl_retain(vl_ctx()->pending); }

static void print_uncaught(Value exc) {
  Value m = vl_callm(exc, "message", NULL, 0);
  fprintf(stderr, "unhandled exception: %s\n", vl_cstr(m));
  vl_release(m);
  VlErr *e = (VlErr *)exc.u.o;
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
  VlHandler *h = c->handlers;
  if (!h) {
    if (vl_task_fail(exc)) return; /* handled by the task wrapper */
    print_uncaught(exc);
    fflush(stdout);
    exit(1);
  }
  vl_set(&c->pending, vl_retain(exc));
  c->handlers = h->prev;
  vl_unwind_to(h->frame_depth);
  longjmp(h->buf, 1);
}

void vl_rethrow(Value exc) { vl_throw(exc); }

/* vl_throw_owned consumes the caller's reference: vl_throw longjmps, so a
 * plain vl_throw(e) from a builder site would leak `e` forever. */
static void vl_throw_owned(Value exc) {
  VlCtx *c = vl_ctx();
  vl_set(&c->pending, exc); /* takes ownership; released on the next throw */
  VlHandler *h = c->handlers;
  if (!h) {
    print_uncaught(exc);
    fflush(stdout);
    exit(1);
  }
  c->handlers = h->prev;
  vl_unwind_to(h->frame_depth);
  longjmp(h->buf, 1);
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

static Value abort_handler; /* os.on_abort */

void vl_set_abort_handler(Value h) { vl_set(&abort_handler, vl_retain(h)); }

void vl_abort(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (abort_handler.t == VL_OBJ) {
    Value h = abort_handler;
    abort_handler = vl_nil();
    vl_call(h, NULL, 0);
    vl_release(h);
  }
  fflush(stdout);
  fprintf(stderr, "abort: %s\n", buf);
  exit(1);
}
