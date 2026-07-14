/* task.c — structured concurrency (§9): tasks, groups, channels, select.
 *
 * Tasks are OS threads in this runtime. The SEMANTICS of the specification
 * are preserved exactly — a group joins every task it spawned, the first
 * failure cancels the siblings and is re-raised at the boundary, later
 * failures attach as suppressed — but the cost model is not: a million
 * tasks is not yet fine. A green-thread scheduler is a later optimisation
 * (recorded in the manual's deviations).
 */
#include "voila_int.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

/* One global monitor guards every channel and the cancellation flags. It is
 * coarse but correct, and it makes `select` and cancellation trivial: any
 * state change broadcasts, and every waiter re-checks. */
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t vl_now_ns(void) { return now_ns(); }

/* ---------------------------------------------------------------- groups */

static bool group_cancelled(VlGroup *g) {
  for (; g; g = g->parent)
    if (g->cancelled) return true;
  return false;
}

static void cancel_group(VlGroup *g) {
  if (!g) return;
  g->cancelled = true;
  pthread_cond_broadcast(&g_cv);
}

/* wait_ms broadcasts nothing; it waits on the global monitor with a deadline
 * so cancellation and timeouts are always observed. */
static void monitor_wait(void) {
  struct timespec ts;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  int64_t ns = (int64_t)tv.tv_usec * 1000 + 20 * 1000000LL; /* 20 ms */
  ts.tv_sec = tv.tv_sec + ns / 1000000000LL;
  ts.tv_nsec = ns % 1000000000LL;
  pthread_cond_timedwait(&g_cv, &g_mu, &ts);
}

void vl_check_cancelled(void) {
  VlCtx *c = vl_ctx();
  if (group_cancelled(c->group)) vl_throwf("Cancelled", "task cancelled");
}

static void group_fail(VlGroup *g, Value exc, bool is_value) {
  if (!g) return;
  pthread_mutex_lock(&g_mu);
  if (g->failure.t == VL_NIL) {
    g->failure = vl_retain(exc);
    g->failure_is_value = is_value;
    g->cancelled = true;
  } else {
    if (g->suppressed.t != VL_OBJ) g->suppressed = vl_slice_new(0);
    vl_slice_append(g->suppressed, vl_retain(exc));
  }
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
}

bool vl_task_fail(Value exc) {
  (void)exc;
  return false; /* every task installs a handler; see task_main */
}

void vl_group_begin(Value timeout) {
  VlCtx *c = vl_ctx();
  VlGroup *g = (VlGroup *)vl_alloc(sizeof(VlGroup));
  g->parent = c->group;
  g->failure = vl_nil();
  g->suppressed = vl_nil();
  if (timeout.t == VL_DUR) g->deadline_ns = now_ns() + timeout.u.i;
  c->group = g;
}

Value vl_group_end(bool as_error_value) {
  VlCtx *c = vl_ctx();
  VlGroup *g = c->group;
  if (!g) return vl_nil();

  pthread_mutex_lock(&g_mu);
  bool timed_out = false;
  while (g->live > 0) {
    /* Only a deadline that actually FIRED is a timeout — a group whose last
     * task finished just before the deadline succeeded. */
    if (g->deadline_ns && now_ns() >= g->deadline_ns && !g->cancelled) {
      timed_out = true;
      g->cancelled = true;
      pthread_cond_broadcast(&g_cv);
    }
    monitor_wait();
  }
  pthread_mutex_unlock(&g_mu);

  c->group = g->parent;

  Value failure = g->failure;
  Value suppressed = g->suppressed;
  bool is_value = g->failure_is_value;

  /* A deadline that expired outranks the Cancelled failures it caused. */
  if (timed_out) {
    bool only_cancelled = true;
    if (failure.t == VL_OBJ && failure.u.o->kind == O_ERR) {
      const char *tn = vl_cstr(((VlErr *)failure.u.o)->type_name);
      if (strcmp(tn, "Cancelled") != 0) only_cancelled = false;
    }
    if (only_cancelled) {
      Value t = vl_exc_new("Timeout", NULL, 0, NULL, NULL, 0);
      VlErr *te = (VlErr *)t.u.o;
      if (failure.t != VL_NIL) {
        if (te->suppressed.t != VL_OBJ) te->suppressed = vl_slice_new(0);
        vl_slice_append(te->suppressed, vl_retain(failure));
      }
      vl_release(failure);
      failure = t;
      is_value = false;
    }
  }
  if (suppressed.t == VL_OBJ && failure.t == VL_OBJ &&
      failure.u.o->kind == O_ERR) {
    VlErr *fe = (VlErr *)failure.u.o;
    VlSlice *sup = (VlSlice *)suppressed.u.o;
    for (int64_t i = 0; i < sup->len; i++)
      vl_slice_append(fe->suppressed, vl_retain(sup->e[i]));
  }
  vl_release(suppressed);
  free(g);

  if (failure.t == VL_NIL) return vl_nil();
  if (as_error_value) return failure; /* try group → error value (§9.1) */
  vl_throw(failure);                  /* plain group → rethrow */
  return vl_nil();
}

/* ---------------------------------------------------------------- tasks */

static void *task_main(void *arg) {
  VlTask *t = (VlTask *)arg;
  VlCtx *c = vl_ctx();
  c->group = t->group;
  c->self = t;

  VlHandler h;
  if (VL_TRY(h)) {
    Value exc = vl_eh_current();
    pthread_mutex_lock(&t->mu);
    t->exc = vl_retain(exc);
    pthread_mutex_unlock(&t->mu);
    group_fail(t->group, exc, false);
    vl_release(exc);
  } else {
    Value r;
    if (t->closure.t == VL_OBJ) r = vl_call(t->closure, t->argv, t->argc);
    else r = t->fn(t->argv, t->argc, NULL);
    vl_eh_pop();
    pthread_mutex_lock(&t->mu);
    t->result = r;
    pthread_mutex_unlock(&t->mu);
    /* An error value escaping a task is a group failure too (§9.1). */
    if (r.t == VL_OBJ && r.u.o->kind == O_ERR) group_fail(t->group, r, true);
  }

  pthread_mutex_lock(&g_mu);
  t->done = true;
  if (t->group) t->group->live--;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);

  pthread_mutex_lock(&t->mu);
  pthread_cond_broadcast(&t->cv);
  pthread_mutex_unlock(&t->mu);

  /* Release the reference task_new took on the thread's behalf. */
  Value self;
  self.u.i = 0;
  self.t = VL_OBJ;
  self.u.o = &t->hdr;
  vl_release(self);

  vl_ctx_teardown(); /* the thread is done; free its context arrays */
  return NULL;
}

static Value task_new(VlFn fn, Value closure, Value *argv, int argc) {
  VlCtx *c = vl_ctx();
  if (!c->group) vl_abort("spawn outside a group (wrap in `group { ... }`)");

  VlTask *t = (VlTask *)vl_obj_new(O_TASK, sizeof(VlTask));
  t->fn = fn;
  t->closure = vl_retain(closure);
  t->group = c->group;
  t->result = vl_nil();
  t->exc = vl_nil();
  t->argc = argc;
  t->argv = (Value *)vl_alloc(sizeof(Value) * (size_t)(argc ? argc : 1));
  for (int i = 0; i < argc; i++) t->argv[i] = vl_retain(argv[i]);
  pthread_mutex_init(&t->mu, NULL);
  pthread_cond_init(&t->cv, NULL);

  pthread_mutex_lock(&g_mu);
  t->group->live++;
  pthread_mutex_unlock(&g_mu);

  t->hdr.rc++; /* the thread holds a reference */
  if (pthread_create(&t->th, NULL, task_main, t) != 0)
    vl_abort("cannot create task thread");
  pthread_detach(t->th);

  Value v;
  v.t = VL_OBJ;
  v.u.o = &t->hdr;
  return v;
}

Value vl_spawn_fn(VlFn fn, Value *argv, int argc) {
  return task_new(fn, vl_nil(), argv, argc);
}

Value vl_spawn_closure(Value closure) {
  return task_new(NULL, closure, NULL, 0);
}

Value vl_await(Value tv) {
  VlTask *t = (VlTask *)tv.u.o;
  pthread_mutex_lock(&t->mu);
  while (!t->done) {
    struct timespec ts;
    struct timeval tvv;
    gettimeofday(&tvv, NULL);
    int64_t ns = (int64_t)tvv.tv_usec * 1000 + 20 * 1000000LL;
    ts.tv_sec = tvv.tv_sec + ns / 1000000000LL;
    ts.tv_nsec = ns % 1000000000LL;
    pthread_cond_timedwait(&t->cv, &t->mu, &ts);
  }
  t->consumed = true;
  Value exc = vl_retain(t->exc);
  Value res = vl_retain(t->result);
  pthread_mutex_unlock(&t->mu);

  if (exc.t == VL_OBJ) {
    vl_release(res);
    vl_throw(exc);
  }
  vl_release(exc);
  return res;
}

/* ---------------------------------------------------------------- channels */

Value vl_chan_new(int64_t cap) {
  VlChan *c = (VlChan *)vl_obj_new(O_CHAN, sizeof(VlChan));
  c->cap = cap > 0 ? cap : 1; /* unbuffered: one slot, handed off (below) */
  c->buf = (Value *)vl_alloc(sizeof(Value) * (size_t)c->cap);
  pthread_mutex_init(&c->mu, NULL);
  pthread_cond_init(&c->cv, NULL);
  /* cap == 0 is recorded by head == -1 to mark a rendezvous channel */
  if (cap <= 0) c->head = -1;
  return (Value){.t = VL_OBJ, .u.o = &c->hdr};
}

static bool is_rendezvous(VlChan *c) { return c->head == -1; }

static int64_t ring_head(VlChan *c) { return is_rendezvous(c) ? 0 : c->head; }

void vl_chan_send(Value cv, Value owned) {
  VlChan *c = (VlChan *)cv.u.o;
  pthread_mutex_lock(&g_mu);
  for (;;) {
    if (c->closed) {
      pthread_mutex_unlock(&g_mu);
      vl_release(owned);
      vl_throwf("RuntimeError", "send on closed channel");
    }
    if (group_cancelled(vl_ctx()->group)) {
      pthread_mutex_unlock(&g_mu);
      vl_release(owned);
      vl_throwf("Cancelled", "task cancelled during channel send");
    }
    if (c->count < c->cap) break;
    monitor_wait();
  }
  int64_t h = ring_head(c);
  c->buf[(h + c->count) % c->cap] = owned;
  c->count++;
  pthread_cond_broadcast(&g_cv);

  if (is_rendezvous(c)) {
    /* rendezvous: block until a receiver takes the value */
    while (c->count > 0 && !c->closed) {
      if (group_cancelled(vl_ctx()->group)) break;
      monitor_wait();
    }
  }
  pthread_mutex_unlock(&g_mu);
}

static bool chan_take(VlChan *c, Value *out) {
  if (c->count == 0) return false;
  int64_t h = ring_head(c);
  *out = c->buf[h % c->cap];
  if (!is_rendezvous(c)) c->head = (c->head + 1) % c->cap;
  c->count--;
  pthread_cond_broadcast(&g_cv);
  return true;
}

Value vl_chan_recv_ok(Value cv, Value *ok) {
  VlChan *c = (VlChan *)cv.u.o;
  pthread_mutex_lock(&g_mu);
  for (;;) {
    Value v;
    if (chan_take(c, &v)) {
      pthread_mutex_unlock(&g_mu);
      if (ok) *ok = vl_bool(true);
      return v;
    }
    if (c->closed) {
      pthread_mutex_unlock(&g_mu);
      if (ok) *ok = vl_bool(false);
      return vl_nil();
    }
    if (group_cancelled(vl_ctx()->group)) {
      pthread_mutex_unlock(&g_mu);
      vl_throwf("Cancelled", "task cancelled during channel receive");
    }
    monitor_wait();
  }
}

Value vl_chan_recv(Value cv) { return vl_chan_recv_ok(cv, NULL); }

Value vl_chan_iter_next(Value cv, bool *ok) {
  Value okv = vl_bool(false);
  Value v = vl_chan_recv_ok(cv, &okv);
  *ok = okv.t == VL_BOOL && okv.u.b;
  return v;
}

void vl_chan_close(Value cv) {
  VlChan *c = (VlChan *)cv.u.o;
  pthread_mutex_lock(&g_mu);
  if (c->closed) {
    pthread_mutex_unlock(&g_mu);
    vl_throwf("RuntimeError", "close of already-closed channel");
  }
  c->closed = true;
  pthread_cond_broadcast(&g_cv);
  pthread_mutex_unlock(&g_mu);
}

/* ---------------------------------------------------------------- select */

int vl_select(VlSelCase *cases, int n, Value *recv_val, bool *recv_ok) {
  int def = -1;
  for (int i = 0; i < n; i++)
    if (cases[i].kind == VL_SEL_DEFAULT) def = i;

  pthread_mutex_lock(&g_mu);
  for (;;) {
    for (int i = 0; i < n; i++) {
      if (cases[i].kind == VL_SEL_RECV) {
        VlChan *c = (VlChan *)cases[i].ch.u.o;
        Value v;
        if (chan_take(c, &v)) {
          pthread_mutex_unlock(&g_mu);
          *recv_val = v;
          *recv_ok = true;
          return i;
        }
        if (c->closed) {
          pthread_mutex_unlock(&g_mu);
          *recv_val = vl_nil();
          *recv_ok = false;
          return i;
        }
      } else if (cases[i].kind == VL_SEL_SEND) {
        VlChan *c = (VlChan *)cases[i].ch.u.o;
        if (!c->closed && c->count < c->cap) {
          int64_t h = ring_head(c);
          c->buf[(h + c->count) % c->cap] = vl_retain(cases[i].send);
          c->count++;
          pthread_cond_broadcast(&g_cv);
          pthread_mutex_unlock(&g_mu);
          return i;
        }
      }
    }
    if (def >= 0) {
      pthread_mutex_unlock(&g_mu);
      return def;
    }
    if (group_cancelled(vl_ctx()->group)) {
      pthread_mutex_unlock(&g_mu);
      vl_throwf("Cancelled", "task cancelled in select");
    }
    monitor_wait();
  }
}

/* ---------------------------------------------------------------- sleep */

void vl_sleep_ns(int64_t ns) {
  int64_t deadline = now_ns() + ns;
  pthread_mutex_lock(&g_mu);
  while (now_ns() < deadline) {
    if (group_cancelled(vl_ctx()->group)) {
      pthread_mutex_unlock(&g_mu);
      vl_throwf("Cancelled", "task cancelled during sleep");
    }
    monitor_wait();
  }
  pthread_mutex_unlock(&g_mu);
}

/* time.after: a channel that receives an instant after the duration. */
typedef struct {
  Value ch;
  int64_t ns;
} AfterArg;

static void *after_main(void *p) {
  AfterArg *a = (AfterArg *)p;
  struct timespec ts;
  ts.tv_sec = a->ns / 1000000000LL;
  ts.tv_nsec = a->ns % 1000000000LL;
  nanosleep(&ts, NULL);
  VlChan *c = (VlChan *)a->ch.u.o;
  pthread_mutex_lock(&g_mu);
  if (!c->closed && c->count < c->cap) {
    int64_t h = ring_head(c);
    c->buf[(h + c->count) % c->cap] = vl_instant(now_ns());
    c->count++;
    pthread_cond_broadcast(&g_cv);
  }
  pthread_mutex_unlock(&g_mu);
  vl_release(a->ch);
  free(a);
  return NULL;
}

Value vl_time_after(int64_t ns) {
  Value ch = vl_chan_new(1);
  AfterArg *a = (AfterArg *)vl_alloc(sizeof(AfterArg));
  a->ch = vl_retain(ch); /* the timer thread outlives the caller's reference */
  a->ns = ns;
  pthread_t th;
  pthread_create(&th, NULL, after_main, a);
  pthread_detach(th);
  return ch;
}
