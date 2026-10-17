/* ext/sqlite.c — std/sqlite's natives, over the vendored SQLite amalgamation
 * (runtime/src/vendor/sqlite, public domain).
 *
 * This file is a runtime EXTENSION (voila_std.h, vl_ext_register): it is
 * compiled, with SQLite, into its own archive only for a program that uses
 * std/sqlite. SQLite is #included rather than compiled beside it, so the
 * options below are set in exactly one place and every SQLite function is
 * `static` to this translation unit (SQLITE_API) — a program may also reach
 * the system's libsqlite3 through std/ffi without two definitions of
 * sqlite3_open meeting at link time.
 *
 * THREADS. Voilà tasks are OS threads (task.c). SQLite is built
 * SERIALIZED (SQLITE_THREADSAFE=1, and every connection is opened
 * SQLITE_OPEN_FULLMUTEX): each call holds the connection's mutex, so a
 * connection — and its statements — may be used from any task without
 * corrupting it. What serialization does NOT make safe is a CONVERSATION:
 * two tasks stepping one statement interleave its rows, and one task's
 * BEGIN is every task's transaction on that connection. The Voilà layer
 * says so (std/sqlite's header); the usual answer is a connection per task,
 * which the busy timeout set at open makes work against one file.
 *
 * The error message is read UNDER the connection's mutex together with the
 * call that failed (db_lock below): sqlite3_errmsg describes the most
 * recent call on the connection, and without the lock another task's call
 * could replace it in between.
 *
 * Every failure is an IOError VALUE carrying SQLite's own message.
 */
#define SQLITE_THREADSAFE 1           /* serialized: see THREADS above */
#define SQLITE_OMIT_LOAD_EXTENSION 1  /* no dlopen: nothing beyond libc */
#define SQLITE_OMIT_DEPRECATED 1
#define SQLITE_DEFAULT_MEMSTATUS 0    /* no global memory-statistics lock */
/* Every POSIX system the runtime supports has these; without them SQLite's
 * date functions would call the thread-UNSAFE localtime and gmtime, and
 * Voilà tasks are threads. */
#define HAVE_LOCALTIME_R 1
#define HAVE_GMTIME_R 1
#define SQLITE_ENABLE_MATH_FUNCTIONS 1 /* sqrt, pow, ln … in SQL (-lm) */
#define SQLITE_API static
#define SQLITE_EXTERN                 /* `static extern` is not C */
#include "sqlite3.c"

#include "voila_int.h"

#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/time.h>
#include <string.h>

/* ------------------------------------------------------------ helpers */

static Value sq_err(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  Value m = vl_str(buf);
  Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
  vl_release(m);
  return e;
}

static int64_t want_int(Value v, const char *who) {
  v = vl_unbox(v);
  if (v.t == VL_INT) return v.u.i;
  vl_abort("%s: expected an int, got %s", who, vl_kind_name(v));
  return 0;
}

static const char *want_str(Value v, const char *who, int64_t *n) {
  if (v.t != VL_OBJ || v.u.o->kind != O_STR)
    vl_abort("%s: expected a str, got %s", who, vl_kind_name(v));
  *n = vl_strlen(v);
  return vl_cstr(v);
}

/* ------------------------------------------------------------ handles */

/* `mu` guards `closed` together with every use of `db` on a path that
 * holds no statement: once sqlite3_close_v2 has run with no statement
 * alive, `db` is freed, so the check and the use must be one step against
 * a concurrent close. (A statement keeps its connection allocated —
 * close_v2 only makes it a zombie — so statement paths need no `mu`.) */
typedef struct {
  sqlite3 *db;
  bool closed;
  pthread_mutex_t mu;
  /* The TRANSACTION GATE. A transaction is the connection's, not a task's
   * — SQLite has one per connection — so while one task has a transaction
   * open, a statement from any other task would run INSIDE it, and be
   * rolled back with it. So a transaction is taken exclusively: `tx` with
   * `owner` says which thread holds it, every native that touches the
   * connection passes the gate (gate_enter/gate_leave), and the gate
   * stops every thread but the owner until the transaction
   * ends (autocommit is back on, checked as each of the owner's calls
   * leaves). Tasks are threads (task.c), so a thread names a task. */
  pthread_mutex_t gmu;
  pthread_cond_t gcv;
  bool tx;
  pthread_t owner;
  VlCtx *owner_ctx; /* the owner thread's task context (see deadlocked) */
} SqDb;

typedef struct {
  sqlite3_stmt *st;
  Value db;     /* the SqDb handle, retained: a statement keeps it alive */
  SqDb *dbc;
} SqSt;

static void db_free(void *ctx) {
  SqDb *d = (SqDb *)ctx;
  /* close_v2: if a statement is still alive, the connection lingers until
   * it is finalized, rather than failing. (A statement retains its
   * connection's handle, so in practice none is.) */
  if (!d->closed) sqlite3_close_v2(d->db);
  pthread_mutex_destroy(&d->mu);
  pthread_mutex_destroy(&d->gmu);
  pthread_cond_destroy(&d->gcv);
  free(d);
}

static void st_free(void *ctx) {
  SqSt *s = (SqSt *)ctx;
  if (s->st) sqlite3_finalize(s->st);
  vl_release(s->db);
  free(s);
}

static const VlHandleOps db_ops = {"sqlite.DB", db_free, NULL, NULL, NULL,
                                   NULL};
static const VlHandleOps st_ops = {"sqlite.Stmt", st_free, NULL, NULL, NULL,
                                   NULL};

static SqDb *want_db(Value v, const char *who) {
  SqDb *d = (SqDb *)vl_handle_ctx(v, &db_ops);
  if (!d) vl_abort("%s: expected a database, got %s", who, vl_kind_name(v));
  return d;
}

static SqSt *want_st(Value v, const char *who) {
  SqSt *s = (SqSt *)vl_handle_ctx(v, &st_ops);
  if (!s) vl_abort("%s: expected a statement, got %s", who, vl_kind_name(v));
  return s;
}

/* live_st is a statement that may still be used, or NULL with *err set. */
static SqSt *live_st(Value v, const char *who, Value *err) {
  SqSt *s = want_st(v, who);
  if (s->dbc->closed) {
    *err = sq_err("sqlite: the database is closed");
    return NULL;
  }
  if (!s->st) {
    *err = sq_err("sqlite: the statement is closed");
    return NULL;
  }
  return s;
}

/* db_enter takes the handle's `mu` and reports whether the connection is
 * still open (when false, *err is set and `mu` is already released). */
static bool db_enter(SqDb *d, Value *err) {
  pthread_mutex_lock(&d->mu);
  if (!d->closed) return true;
  pthread_mutex_unlock(&d->mu);
  if (err) *err = sq_err("sqlite: the database is closed");
  return false;
}

static void db_leave(SqDb *d) { pthread_mutex_unlock(&d->mu); }

static void db_lock(sqlite3 *db) { sqlite3_mutex_enter(sqlite3_db_mutex(db)); }
static void db_unlock(sqlite3 *db) {
  sqlite3_mutex_leave(sqlite3_db_mutex(db));
}

/* db_fail is the IOError for the call that just failed on db. Call it with
 * the connection's mutex held. */
static Value db_fail(sqlite3 *db, const char *what) {
  return sq_err("sqlite: %s%s%s", what, what[0] ? ": " : "",
                sqlite3_errmsg(db));
}

/* ------------------------------------------------------------ the gate */

static bool mine(SqDb *d) {
  return d->tx && pthread_equal(d->owner, pthread_self());
}

/* A task that ENDS holding a transaction — it returned, threw or faulted
 * between begin and commit — must not keep the connection forever. Tasks
 * are threads, one each, that exit when the task ends (task.c), so every
 * thread keeps a list of the connections whose transaction it holds, under
 * a pthread key whose destructor runs at the thread's exit: it rolls each
 * one back and opens its gate. The list holds a reference to the handle,
 * so the connection outlives its owner thread's last word on it. */
typedef struct Owned {
  struct Owned *next;
  Value h; /* the DB handle, retained */
  SqDb *d;
} Owned;

static pthread_key_t g_owned_key;
static pthread_once_t g_owned_once = PTHREAD_ONCE_INIT;

static void owned_exit(void *arg) {
  Owned *o = (Owned *)arg;
  while (o) {
    Owned *next = o->next;
    SqDb *d = o->d;
    pthread_mutex_lock(&d->gmu);
    if (mine(d)) {
      pthread_mutex_lock(&d->mu);
      if (!d->closed) sqlite3_exec(d->db, "ROLLBACK", NULL, NULL, NULL);
      pthread_mutex_unlock(&d->mu);
      d->tx = false;
      pthread_cond_broadcast(&d->gcv);
    }
    pthread_mutex_unlock(&d->gmu);
    vl_release(o->h);
    free(o);
    o = next;
  }
}

static void owned_init(void) { pthread_key_create(&g_owned_key, owned_exit); }

/* owned_add records that this thread now holds d's transaction. */
static void owned_add(Value h, SqDb *d) {
  pthread_once(&g_owned_once, owned_init);
  Owned *o = (Owned *)vl_alloc(sizeof(Owned));
  o->h = vl_retain(h);
  o->d = d;
  o->next = (Owned *)pthread_getspecific(g_owned_key);
  pthread_setspecific(g_owned_key, o);
}

/* owned_drop forgets d (its transaction ended); returns the handle
 * reference to release — AFTER the caller has let go of d's locks. */
static Value owned_drop(SqDb *d) {
  pthread_once(&g_owned_once, owned_init);
  Owned *head = (Owned *)pthread_getspecific(g_owned_key);
  Owned *prev = NULL;
  for (Owned *o = head; o; prev = o, o = o->next) {
    if (o->d != d) continue;
    if (prev) prev->next = o->next;
    else pthread_setspecific(g_owned_key, o->next);
    Value h = o->h;
    free(o);
    return h;
  }
  return vl_nil();
}

/* deadlocked says that waiting for d's gate can never end: its owner is
 * blocked joining a group this task belongs to (directly, or through a
 * group nested inside it) — the owner waits for this task, and this task
 * for the owner. The classic case is a transaction body that spawns tasks
 * using the same connection inside a `group`. Call with gmu held. */
static bool deadlocked(SqDb *d) {
  if (!d->tx || mine(d) || !d->owner_ctx) return false;
  VlGroup *j = __atomic_load_n(&d->owner_ctx->joining, __ATOMIC_ACQUIRE);
  if (!j) return false;
  for (VlGroup *g = vl_ctx()->group; g; g = g->parent)
    if (g == j) return true;
  return false;
}

static Value deadlock_err(void) {
  return sq_err("sqlite: deadlock: this task needs a connection whose "
                "transaction belongs to a task waiting for this one (a "
                "transaction is one task's: pass the work back to it, or "
                "open a separate connection)");
}

/* gate_wait blocks while another task holds d's transaction; false (with
 * gmu still held) when that wait could never end. The wait is in slices so
 * that an owner which only STARTS joining the group later is noticed. */
/* (A call another task has in flight when a transaction begins needs no
 * waiting for: SQLite's own connection mutex runs it whole, before or after
 * the BEGIN, and its NEXT call meets the gate. Nothing here counts calls in
 * flight — a count a contained fault could leave unbalanced forever.) */
static bool gate_wait(SqDb *d) {
  while (d->tx && !mine(d)) {
    if (deadlocked(d)) return false;
    struct timespec ts;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t ns = (int64_t)tv.tv_usec * 1000 + 50 * 1000000LL;
    ts.tv_sec = tv.tv_sec + (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    pthread_cond_timedwait(&d->gcv, &d->gmu, &ts);
  }
  return true;
}

static bool gate_enter(SqDb *d, Value *err) {
  pthread_mutex_lock(&d->gmu);
  if (!gate_wait(d)) {
    pthread_mutex_unlock(&d->gmu);
    *err = deadlock_err();
    return false;
  }
  pthread_mutex_unlock(&d->gmu);
  return true;
}

/* gate_leave ends a call; when it was the owner's and the transaction is
 * over (COMMIT, ROLLBACK, a failed BEGIN, a close), the gate opens. */
static void gate_leave(SqDb *d) {
  bool ended = false;
  pthread_mutex_lock(&d->gmu);
  if (mine(d)) {
    pthread_mutex_lock(&d->mu);
    bool over = d->closed || sqlite3_get_autocommit(d->db) != 0;
    pthread_mutex_unlock(&d->mu);
    if (over) {
      d->tx = false;
      ended = true;
    }
  }
  pthread_cond_broadcast(&d->gcv);
  pthread_mutex_unlock(&d->gmu);
  if (ended) vl_release(owned_drop(d));
}

/* ------------------------------------------------------------ natives */

/* sqlite._open(path, flags) -> handle!. flags: 1 read-only, 2 create when
 * missing (the default is read-write and create). ":memory:" is a private
 * in-memory database. */
static Value sq_open(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int64_t n;
  const char *path = want_str(argv[0], "sqlite.open", &n);
  int64_t fl = want_int(argv[1], "sqlite.open");
  if ((int64_t)strlen(path) != n)
    return sq_err("sqlite: open: the path contains a NUL byte");
  int flags = SQLITE_OPEN_FULLMUTEX;
  if (fl & 1) flags |= SQLITE_OPEN_READONLY;
  else flags |= SQLITE_OPEN_READWRITE | ((fl & 2) ? SQLITE_OPEN_CREATE : 0);
  sqlite3 *db = NULL;
  int rc = sqlite3_open_v2(path, &db, flags, NULL);
  if (rc != SQLITE_OK) {
    Value e = db ? sq_err("sqlite: open %s: %s", path, sqlite3_errmsg(db))
                 : sq_err("sqlite: open %s: out of memory", path);
    sqlite3_close_v2(db);
    return e;
  }
  /* Wait up to 5 s for another connection's lock rather than failing at
   * once with "database is locked": several tasks, each with its own
   * connection to one file, is the documented way to share a database. */
  sqlite3_busy_timeout(db, 5000);
  sqlite3_extended_result_codes(db, 1);
  SqDb *d = (SqDb *)vl_alloc(sizeof(SqDb));
  d->db = db;
  pthread_mutex_init(&d->mu, NULL);
  pthread_mutex_init(&d->gmu, NULL);
  pthread_cond_init(&d->gcv, NULL);
  return vl_handle_new(&db_ops, d);
}

/* sqlite._close(db) -> unit. Idempotent. */
static Value sq_close(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.close");
  if (db_enter(d, NULL)) {
    d->closed = true;
    sqlite3_close_v2(d->db);
    db_leave(d);
  }
  return vl_unit();
}

/* sqlite._exec(db, sql) -> unit!: run one or more statements, no rows. */
static Value sq_exec(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.exec");
  int64_t n;
  const char *sql = want_str(argv[1], "sqlite.exec", &n);
  if ((int64_t)strlen(sql) != n)
    return sq_err("sqlite: exec: the SQL contains a NUL byte");
  Value out = vl_unit();
  if (!db_enter(d, &out)) return out;
  char *msg = NULL;
  db_lock(d->db);
  int rc = sqlite3_exec(d->db, sql, NULL, NULL, &msg);
  if (rc != SQLITE_OK)
    out = sq_err("sqlite: %s", msg ? msg : sqlite3_errmsg(d->db));
  db_unlock(d->db);
  db_leave(d);
  sqlite3_free(msg);
  return out;
}

/* only_space reports whether the rest of a statement's text is nothing but
 * whitespace, `;` and comments — what may follow the ONE statement prepare
 * accepts. */
static bool only_space(const char *p) {
  for (;;) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ';')
      p++;
    if (p[0] == '-' && p[1] == '-') {
      while (*p && *p != '\n') p++;
      continue;
    }
    if (p[0] == '/' && p[1] == '*') {
      const char *e = strstr(p + 2, "*/");
      if (!e) return true;
      p = e + 2;
      continue;
    }
    return *p == 0;
  }
}

/* sqlite._prepare(db, sql) -> handle!: exactly ONE statement. A second one
 * after it is refused rather than silently dropped — `exec` is the verb
 * for a script. */
static Value sq_prepare(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.prepare");
  int64_t n;
  const char *sql = want_str(argv[1], "sqlite.prepare", &n);
  /* A NUL would end the statement early and hide the rest from the
   * one-statement check below; a length past int is not SQL. */
  if ((int64_t)strlen(sql) != n)
    return sq_err("sqlite: prepare: the SQL contains a NUL byte");
  if (n > INT_MAX) return sq_err("sqlite: prepare: the SQL is too long");
  sqlite3_stmt *st = NULL;
  const char *tail = NULL;
  Value err = vl_nil();
  if (!db_enter(d, &err)) return err;
  db_lock(d->db);
  int rc = sqlite3_prepare_v2(d->db, sql, (int)n, &st, &tail);
  if (rc != SQLITE_OK) err = db_fail(d->db, "");
  db_unlock(d->db);
  db_leave(d);
  if (err.t != VL_NIL) return err;
  if (!st) return sq_err("sqlite: the SQL holds no statement");
  if (tail && tail < sql + n && !only_space(tail)) {
    sqlite3_finalize(st);
    return sq_err("sqlite: the SQL holds more than one statement (exec runs "
                  "a script)");
  }
  SqSt *s = (SqSt *)vl_alloc(sizeof(SqSt));
  s->st = st;
  s->db = vl_retain(argv[0]);
  s->dbc = d;
  return vl_handle_new(&st_ops, s);
}

/* bind_one binds argument i (1-based) from a Voilà value. */
static int bind_one(sqlite3_stmt *st, int i, Value v, const char **what) {
  v = vl_unbox(v);
  switch (v.t) {
  case VL_NIL: return sqlite3_bind_null(st, i);
  case VL_BOOL: return sqlite3_bind_int64(st, i, v.u.b ? 1 : 0);
  case VL_INT: return sqlite3_bind_int64(st, i, v.u.i);
  case VL_FLOAT: return sqlite3_bind_double(st, i, v.u.f);
  case VL_OBJ:
    if (v.u.o->kind == O_STR)
      return sqlite3_bind_text64(st, i, vl_cstr(v), (sqlite3_uint64)vl_strlen(v),
                                 SQLITE_TRANSIENT, SQLITE_UTF8);
    if (v.u.o->kind == O_DEC) {
      /* An exact decimal is stored as its exact TEXT: a REAL would round
       * it, which is the one thing a dec exists to prevent. */
      char *t = vl_dec_str(v);
      int rc = sqlite3_bind_text(st, i, t, -1, SQLITE_TRANSIENT);
      free(t);
      return rc;
    }
    if (v.u.o->kind == O_SLICE) { /* []byte: a BLOB */
      VlSlice *s = (VlSlice *)v.u.o;
      unsigned char *b = (unsigned char *)vl_alloc((size_t)s->len + 1);
      for (int64_t k = 0; k < s->len; k++) {
        Value e = s->e[k];
        if (e.t != VL_INT || e.u.i < 0 || e.u.i > 255) {
          free(b);
          *what = "a []byte element outside 0..255";
          return SQLITE_MISMATCH;
        }
        b[k] = (unsigned char)e.u.i;
      }
      int rc = sqlite3_bind_blob64(st, i, b, (sqlite3_uint64)s->len,
                                   SQLITE_TRANSIENT);
      free(b);
      return rc;
    }
    break;
  default:
    break;
  }
  *what = vl_kind_name(v);
  return SQLITE_MISMATCH;
}

/* sqlite._bind(st, args) -> unit!: rewind the statement and bind every
 * parameter. The count must match: a missing argument would bind NULL
 * silently, an extra one would be ignored, and both are bugs. */
static Value sq_bind(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  Value err = vl_nil();
  SqSt *s = live_st(argv[0], "sqlite.bind", &err);
  if (!s) return err;
  Value args = argv[1];
  if (args.t != VL_OBJ || args.u.o->kind != O_SLICE)
    vl_abort("sqlite.bind: expected a slice of arguments, got %s",
             vl_kind_name(args));
  VlSlice *a = (VlSlice *)args.u.o;
  sqlite3 *db = s->dbc->db;
  db_lock(db);
  sqlite3_reset(s->st);
  sqlite3_clear_bindings(s->st);
  int want = sqlite3_bind_parameter_count(s->st);
  if (want != a->len) {
    db_unlock(db);
    return sq_err("sqlite: the statement has %d parameter%s, but %lld "
                  "argument%s given", want, want == 1 ? "" : "s",
                  (long long)a->len, a->len == 1 ? " is" : "s are");
  }
  for (int64_t i = 0; i < a->len; i++) {
    const char *what = NULL;
    int rc = bind_one(s->st, (int)i + 1, a->e[i], &what);
    if (rc != SQLITE_OK) {
      Value e = what ? sq_err("sqlite: argument %lld: cannot bind %s",
                              (long long)i + 1, what)
                     : db_fail(db, "bind");
      db_unlock(db);
      return e;
    }
  }
  db_unlock(db);
  return vl_unit();
}

/* sqlite._step(st) -> bool!: true with a row ready, false when done. */
static Value sq_step(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  Value err = vl_nil();
  SqSt *s = live_st(argv[0], "sqlite.step", &err);
  if (!s) return err;
  sqlite3 *db = s->dbc->db;
  db_lock(db);
  int rc = sqlite3_step(s->st);
  Value out;
  if (rc == SQLITE_ROW) out = vl_bool(true);
  else if (rc == SQLITE_DONE) out = vl_bool(false);
  else {
    out = db_fail(db, "");
    sqlite3_reset(s->st); /* leave it reusable after the error */
  }
  db_unlock(db);
  return out;
}

/* sqlite._reset(st) -> unit: rewind, keeping the bindings. */
static Value sq_reset(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqSt *s = want_st(argv[0], "sqlite.reset");
  if (s->st && !s->dbc->closed) sqlite3_reset(s->st);
  return vl_unit();
}

/* sqlite._finalize(st) -> unit. Idempotent. */
static Value sq_finalize(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqSt *s = want_st(argv[0], "sqlite.finalize");
  if (s->st) sqlite3_finalize(s->st);
  s->st = NULL;
  return vl_unit();
}

/* sqlite._columns(st) -> []str: the result columns' names. */
static Value sq_columns(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  Value out = vl_slice_new(0);
  Value err = vl_nil();
  SqSt *s = live_st(argv[0], "sqlite.columns", &err);
  if (!s) {
    vl_release(err);
    return out;
  }
  db_lock(s->dbc->db);
  int n = sqlite3_column_count(s->st);
  for (int i = 0; i < n; i++) {
    const char *c = sqlite3_column_name(s->st, i);
    vl_slice_append(out, vl_str(c ? c : ""));
  }
  db_unlock(s->dbc->db);
  return out;
}

/* col checks a column index on a statement that has a row. */
static SqSt *col(Value *argv, const char *who, int *i) {
  SqSt *s = want_st(argv[0], who);
  int64_t k = want_int(argv[1], who);
  if (!s->st || k < 0 || k >= sqlite3_column_count(s->st))
    vl_abort("%s: column %lld is out of range", who, (long long)k);
  *i = (int)k;
  return s;
}

/* sqlite._col_type(st, i) -> int: 1 INTEGER, 2 REAL, 3 TEXT, 4 BLOB, 5 NULL
 * (SQLite's own numbering, which std/sqlite's Value mirrors). */
/* Every column read holds the connection's mutex across the pointer SQLite
 * returns AND the copy out of it: another task stepping or resetting the
 * statement in between would free what the pointer points at. */
static Value sq_col_type(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int i;
  SqSt *s = col(argv, "sqlite.col_type", &i);
  db_lock(s->dbc->db);
  Value out = vl_int(sqlite3_column_type(s->st, i));
  db_unlock(s->dbc->db);
  return out;
}

static Value sq_col_int(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int i;
  SqSt *s = col(argv, "sqlite.col_int", &i);
  db_lock(s->dbc->db);
  Value out = vl_int((int64_t)sqlite3_column_int64(s->st, i));
  db_unlock(s->dbc->db);
  return out;
}

static Value sq_col_float(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int i;
  SqSt *s = col(argv, "sqlite.col_float", &i);
  db_lock(s->dbc->db);
  Value out = vl_float(sqlite3_column_double(s->st, i));
  db_unlock(s->dbc->db);
  return out;
}

/* TEXT is returned byte for byte — its length from SQLite, not strlen, so a
 * value holding a NUL survives. */
static Value sq_col_text(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int i;
  SqSt *s = col(argv, "sqlite.col_text", &i);
  db_lock(s->dbc->db);
  const unsigned char *t = sqlite3_column_text(s->st, i);
  int n = sqlite3_column_bytes(s->st, i);
  Value out = vl_str_n(t ? (const char *)t : "", t ? n : 0);
  db_unlock(s->dbc->db);
  return out;
}

static Value sq_col_blob(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  int i;
  SqSt *s = col(argv, "sqlite.col_blob", &i);
  db_lock(s->dbc->db);
  const unsigned char *b = sqlite3_column_blob(s->st, i);
  int n = sqlite3_column_bytes(s->st, i);
  Value out = vl_slice_new(0);
  for (int k = 0; k < n; k++) vl_slice_append(out, vl_int(b[k]));
  db_unlock(s->dbc->db);
  return out;
}

static Value sq_changes(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.changes");
  if (!db_enter(d, NULL)) return vl_int(0);
  Value out = vl_int((int64_t)sqlite3_changes64(d->db));
  db_leave(d);
  return out;
}

static Value sq_last_id(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.last_id");
  if (!db_enter(d, NULL)) return vl_int(0);
  Value out = vl_int((int64_t)sqlite3_last_insert_rowid(d->db));
  db_leave(d);
  return out;
}

static Value sq_busy_timeout(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.busy_timeout");
  int64_t ms = want_int(argv[1], "sqlite.busy_timeout");
  if (db_enter(d, NULL)) {
    sqlite3_busy_timeout(d->db,
                         ms < 0 ? 0 : (ms > INT_MAX ? INT_MAX : (int)ms));
    db_leave(d);
  }
  return vl_unit();
}

/* sqlite._in_tx(db) -> bool: a transaction is open (not autocommit). */
static Value sq_in_tx(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.in_tx");
  /* NOT behind the gate, and never waiting on it: whether THIS task holds
   * the connection's transaction. Another task's is not this task's. */
  pthread_mutex_lock(&d->gmu);
  bool held = mine(d);
  pthread_mutex_unlock(&d->gmu);
  if (!held || !db_enter(d, NULL)) return vl_bool(false);
  Value out = vl_bool(sqlite3_get_autocommit(d->db) == 0);
  db_leave(d);
  return out;
}

/* sqlite._begin(db) -> unit!: take the gate for this task — waiting for
 * any other task's transaction AND for every other task's call in flight
 * to end — then BEGIN IMMEDIATE. The gate stays this task's until the
 * transaction ends. */
static Value sq_begin(Value *argv, int argc, VlFrame *up) {
  (void)argc, (void)up;
  SqDb *d = want_db(argv[0], "sqlite.begin");
  pthread_mutex_lock(&d->gmu);
  if (!gate_wait(d)) {
    pthread_mutex_unlock(&d->gmu);
    return deadlock_err();
  }
  if (d->tx) {
    pthread_mutex_unlock(&d->gmu);
    return sq_err("sqlite: a transaction is already open on this task "
                  "(transactions do not nest)");
  }
  d->tx = true;
  d->owner = pthread_self();
  d->owner_ctx = vl_ctx();
  pthread_mutex_unlock(&d->gmu);
  Value out = vl_unit();
  if (db_enter(d, &out)) {
    char *msg = NULL;
    db_lock(d->db);
    if (sqlite3_exec(d->db, "BEGIN IMMEDIATE", NULL, NULL, &msg) != SQLITE_OK)
      out = sq_err("sqlite: %s", msg ? msg : sqlite3_errmsg(d->db));
    db_unlock(d->db);
    db_leave(d);
    sqlite3_free(msg);
  }
  /* Held from here until the transaction ends — or the thread does. */
  owned_add(argv[0], d);
  gate_leave(d); /* a failed BEGIN leaves autocommit on: the gate opens */
  return out;
}

static Value sq_version(Value *argv, int argc, VlFrame *up) {
  (void)argv, (void)argc, (void)up;
  return vl_str(sqlite3_libversion());
}

/* sqlite.decode[T](row) -> T!: the row, decoded into T exactly as
 * json.decode[T] decodes an object — column names are field names, a field
 * with a default may be absent, an unknown column or a wrong kind is an
 * error. The row renders itself as that object (Row.json, std/sqlite), and
 * json.c's typed decoder does the rest: one decoder, one set of rules. The
 * generator passes T's name as the hidden first argument (cg/instr.voi). */
static Value sq_decode(Value *argv, int argc, VlFrame *up) {
  (void)up;
  if (argc < 2) vl_abort("sqlite.decode needs a type: sqlite.decode[T](row)");
  /* One Row, nothing else: a []Row (decode[[]U](rows)) is the obvious slip,
   * and is an error VALUE here rather than an abort in the method call. */
  Value row = vl_unbox(argv[1]);
  const char *tn = row.t == VL_OBJ && row.u.o->kind == O_STRUCT
                       ? ((VlStruct *)row.u.o)->type->name
                       : NULL;
  size_t tl = tn ? strlen(tn) : 0;
  if (!tn || !(strcmp(tn, "Row") == 0 ||
               (tl > 4 && strcmp(tn + tl - 4, ".Row") == 0)))
    return sq_err("sqlite.decode: decodes one sqlite.Row, not a %s (decode "
                  "each row of a query in turn)", vl_kind_name(argv[1]));
  Value text = vl_callm(row, "json", NULL, 0);
  if (text.t == VL_OBJ && text.u.o->kind == O_ERR) return text;
  /* `text` is owned across the decoder, which builds user types — should
   * anything there throw, the unwind releases it (err.c, vl_cleanup_*). */
  int mark = vl_cleanup_mark();
  vl_cleanup_slot(&text);
  VlFn dec = vl_lookup_fn("json.decode");
  Value a[2] = {argv[0], text};
  Value out = dec(a, 2, NULL);
  vl_cleanup_to(mark);
  vl_release(text);
  return out;
}

/* db_of is the connection a native's first argument belongs to. */
static SqDb *db_of(Value v) {
  SqDb *d = (SqDb *)vl_handle_ctx(v, &db_ops);
  if (d) return d;
  SqSt *s = (SqSt *)vl_handle_ctx(v, &st_ops);
  return s ? s->dbc : NULL;
}

/* GATED(f) is f behind the transaction gate. */
#define GATED(f)                                                          \
  static Value g_##f(Value *argv, int argc, VlFrame *up) {                 \
    SqDb *d = argc > 0 ? db_of(argv[0]) : NULL;                           \
    Value e_ = vl_nil();                                                  \
    if (d && !gate_enter(d, &e_)) return e_;                              \
    Value r = f(argv, argc, up);                                          \
    if (d) gate_leave(d);                                                 \
    return r;                                                             \
  }
GATED(sq_close) GATED(sq_exec) GATED(sq_prepare) GATED(sq_bind)
GATED(sq_step) GATED(sq_reset) GATED(sq_finalize) GATED(sq_columns)
GATED(sq_col_type) GATED(sq_col_int) GATED(sq_col_float) GATED(sq_col_text)
GATED(sq_col_blob) GATED(sq_changes) GATED(sq_last_id)
GATED(sq_busy_timeout)
#undef GATED

/* The natives table (voila_std.h): registered by the program's main() when
 * it uses std/sqlite. Every row is a name in low.StdSrcNatives. */
const VlExtEntry vl_ext_sqlite[] = {
    {"sqlite._open", sq_open},
    {"sqlite._close", g_sq_close},
    {"sqlite._exec", g_sq_exec},
    {"sqlite._prepare", g_sq_prepare},
    {"sqlite._bind", g_sq_bind},
    {"sqlite._step", g_sq_step},
    {"sqlite._reset", g_sq_reset},
    {"sqlite._finalize", g_sq_finalize},
    {"sqlite._columns", g_sq_columns},
    {"sqlite._col_type", g_sq_col_type},
    {"sqlite._col_int", g_sq_col_int},
    {"sqlite._col_float", g_sq_col_float},
    {"sqlite._col_text", g_sq_col_text},
    {"sqlite._col_blob", g_sq_col_blob},
    {"sqlite._changes", g_sq_changes},
    {"sqlite._last_id", g_sq_last_id},
    {"sqlite._busy_timeout", g_sq_busy_timeout},
    {"sqlite._in_tx", sq_in_tx},
    {"sqlite._version", sq_version},
    {"sqlite._begin", sq_begin},
    {"sqlite.decode", sq_decode},
    {NULL, NULL},
};
