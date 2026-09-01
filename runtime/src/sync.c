/* sync.c — std/sync: mutexes, read/write locks, and atomic integers.
 *
 * Tasks are real OS threads (task.c), so shared state needs real primitives.
 * Until this file there were none a program could reach: `cell` runs a closure
 * under a lock it owns and never lends out, and nothing else in the runtime
 * exposed a lock, an atomic, a waitgroup or a once.
 *
 * THE RULE THIS FILE IS BUILT AROUND: no native below ever calls back into
 * Voilà. Each one takes a lock, or releases one, or touches one machine word,
 * and returns. A lock is therefore never held across user code BY THE RUNTIME,
 * which is the failure io.c learned the hard way — a non-recursive mutex held
 * across vl_tostr self-deadlocked when the user's `show` re-entered, and stayed
 * wedged forever when it threw, because the longjmp skipped the unlock.
 *
 * The closure-shaped API (`mu.with`, `rw.with_read`, `once.do`) lives in
 * std/sync/sync.voi instead, where `defer` expresses it exactly: vl_unwind_to
 * runs a frame's defers on the THROW path as well as the return path, so a
 * critical section that throws still unlocks on its way out. That is a
 * language guarantee, not a convention, and it is why the C side does not need
 * a VL_TRY of its own the way call.c's cell.update does.
 *
 * Mutexes are PTHREAD_MUTEX_ERRORCHECK. It costs a few cycles and buys the two
 * mistakes that are otherwise a hang or undefined behaviour: locking a mutex
 * this thread already holds, and unlocking one it does not own. Both become a
 * thrown RuntimeError, which a program can catch and a test can pin — the same
 * choice task.c already makes for "send on closed channel".
 *
 * Atomics are __atomic_* builtins at __ATOMIC_SEQ_CST. The runtime already
 * depends on these for its refcounts (value.c), so this adds no new
 * requirement on the C compiler, and it keeps <stdatomic.h> — which C11 lets an
 * implementation omit entirely — out of the build. Sequential consistency is
 * the only order offered: an atomic in a language with no memory model to
 * describe the weaker ones would be a footgun with no manual.
 *
 * Portability: pthreads only, which the runtime has assumed since task.c —
 * task.c, value.c, io.c and call.c all use them, so there is no _WIN32 stub
 * arm here the way io.c has one for read(2)/write(2). A Windows port replaces
 * the pthread calls in this one file and the three struct bodies in
 * voila_int.h, and touches nothing else in std/sync.
 */
#include "voila_int.h"

#include <errno.h>
#include <string.h>

/* ---------------------------------------------------------------- objects */

static Value obj(VlObj *o) {
  Value v;
  v.t = VL_OBJ;
  v.u.o = o;
  return v;
}

Value vl_sync_mutex_new(void) {
  VlMutex *m = (VlMutex *)vl_obj_new(O_MUTEX, sizeof(VlMutex));
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&m->mu, &a);
  pthread_mutexattr_destroy(&a);
  return obj(&m->hdr);
}

Value vl_sync_rwlock_new(void) {
  VlRWLock *r = (VlRWLock *)vl_obj_new(O_RWLOCK, sizeof(VlRWLock));
  pthread_rwlock_init(&r->rw, NULL);
  return obj(&r->hdr);
}

Value vl_sync_atomic_new(int64_t n) {
  VlAtomic *a = (VlAtomic *)vl_obj_new(O_ATOMIC, sizeof(VlAtomic));
  a->n = n;
  return obj(&a->hdr);
}

/* obj_free's arms. Dropping the last reference to a lock normally means every
 * critical section over it has finished — a task inside one is holding the
 * Voilà struct that owns the handle. The exception is a lock nobody kept:
 * `sync.mutex().lock()` frees a HELD mutex, and pthread_*_destroy on a held
 * lock is undefined. It is harmless in practice for the one reason that makes
 * it harmless in principle: no other task can have a reference to it either,
 * so nothing is or ever can be blocked on it. Both implementations return
 * EBUSY and destroy nothing; the memory then goes back to the allocator. */
void vl_sync_mutex_free(VlObj *o) {
  pthread_mutex_destroy(&((VlMutex *)o)->mu);
}

void vl_sync_rwlock_free(VlObj *o) {
  pthread_rwlock_destroy(&((VlRWLock *)o)->rw);
}

int64_t vl_sync_atomic_peek(VlObj *o) {
  return __atomic_load_n(&((VlAtomic *)o)->n, __ATOMIC_SEQ_CST);
}

/* ---------------------------------------------------------------- guards */

/* The guards take (argv, argc) rather than a Value, because the ARITY has to
 * be checked too. These natives are internal to std/sync — the package spells
 * them `sync._lock` and a program has no reason to — but `sync` is a package
 * qualifier the lowerer accepts, so `sync._lock()` with no arguments is a
 * thing a user can WRITE, and indexing off the end of a vector the caller
 * never filled is a segfault rather than a diagnosis. */
static VlMutex *want_mutex(Value *argv, int argc) {
  if (argc < 1) vl_abort("sync._lock/_unlock/_try_lock takes a mutex");
  if (argv[0].t != VL_OBJ || argv[0].u.o->kind != O_MUTEX)
    /* The overwhelmingly likely cause is `sync.Mutex{}`: the struct literal
     * compiles, and leaves the handle field at its zero value. Name the
     * constructor, because "got sync.Handle" on its own tells the reader
     * what went wrong and not one thing about how to fix it. */
    vl_abort("expected a mutex, got %s -- build one with sync.mutex()",
             vl_kind_name(argv[0]));
  return (VlMutex *)argv[0].u.o;
}

static VlRWLock *want_rwlock(Value *argv, int argc) {
  if (argc < 1) vl_abort("the sync._r*/_w* natives take an rwlock");
  if (argv[0].t != VL_OBJ || argv[0].u.o->kind != O_RWLOCK)
    vl_abort("expected an rwlock, got %s -- build one with sync.rwlock()",
             vl_kind_name(argv[0]));
  return (VlRWLock *)argv[0].u.o;
}

static VlAtomic *want_atomic(Value *argv, int argc) {
  if (argc < 1) vl_abort("the sync._load/_store/_add/… natives take an atomic");
  if (argv[0].t != VL_OBJ || argv[0].u.o->kind != O_ATOMIC)
    vl_abort("expected an atomic, got %s -- build one with sync.atomic(n)",
             vl_kind_name(argv[0]));
  return (VlAtomic *)argv[0].u.o;
}

/* want_int reads an integer operand, or aborts if it was never passed. */
static int64_t want_int(Value *argv, int argc, int i) {
  if (i >= argc) vl_abort("std/sync: an integer operand is missing");
  return vl_int_of(argv[i]);
}

/* lock_failed turns a pthread errno into the diagnosis a programmer can act
 * on. These are BUGS in the program's locking, not conditions to handle, so
 * they throw RuntimeError rather than returning an IOError value the way the
 * I/O packages do — the same line task.c draws for a closed channel. */
static void lock_failed(int rc, const char *what) {
  if (rc == EDEADLK)
    vl_throwf("RuntimeError",
              "%s: this task already holds the lock (a Mutex is not "
              "recursive)", what);
  if (rc == EPERM)
    vl_throwf("RuntimeError", "%s: this task does not hold the lock", what);
  if (rc == EBUSY)
    vl_throwf("RuntimeError", "%s: the lock is held", what);
  vl_throwf("RuntimeError", "%s: %s", what, strerror(rc));
}

#define SYNCFN (void)argv, (void)argc, (void)up

/* ---------------------------------------------------------------- Mutex */

Value vl_sync_mutex(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  return vl_sync_mutex_new();
}

Value vl_sync_lock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_mutex_lock(&want_mutex(argv, argc)->mu);
  if (rc != 0) lock_failed(rc, "Mutex.lock");
  return vl_unit();
}

Value vl_sync_unlock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_mutex_unlock(&want_mutex(argv, argc)->mu);
  if (rc != 0) lock_failed(rc, "Mutex.unlock");
  return vl_unit();
}

/* try_lock reports rather than throws: "someone else has it" is the ANSWER
 * here, not a failure. A self-deadlock still throws, because a task asking for
 * a lock it already holds has a bug either way. */
Value vl_sync_try_lock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_mutex_trylock(&want_mutex(argv, argc)->mu);
  if (rc == 0) return vl_bool(true);
  if (rc == EBUSY) return vl_bool(false);
  lock_failed(rc, "Mutex.try_lock");
  return vl_bool(false);
}

/* ---------------------------------------------------------------- RWLock */

Value vl_sync_rwlock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  return vl_sync_rwlock_new();
}

Value vl_sync_rlock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_rwlock_rdlock(&want_rwlock(argv, argc)->rw);
  if (rc != 0) lock_failed(rc, "RWLock.read_lock");
  return vl_unit();
}

Value vl_sync_wlock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_rwlock_wrlock(&want_rwlock(argv, argc)->rw);
  if (rc != 0) lock_failed(rc, "RWLock.write_lock");
  return vl_unit();
}

/* One unlock for both sides: pthread_rwlock_unlock releases whichever kind of
 * hold the calling thread has, and there is no portable way to ask which that
 * is. std/sync still spells it read_unlock and write_unlock so the code reads
 * in pairs; both land here. */
Value vl_sync_rwunlock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_rwlock_unlock(&want_rwlock(argv, argc)->rw);
  if (rc != 0) lock_failed(rc, "RWLock.unlock");
  return vl_unit();
}

/* The try_ pair answers with a bool rather than throwing, for the same reason
 * Mutex.try_lock does. EDEADLK is folded in with EBUSY: a platform that
 * notices the caller already holds this lock the other way round is telling us
 * the lock cannot be had, which is precisely what `false` means here. */
Value vl_sync_try_rlock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_rwlock_tryrdlock(&want_rwlock(argv, argc)->rw);
  if (rc == 0) return vl_bool(true);
  if (rc == EBUSY || rc == EDEADLK) return vl_bool(false);
  lock_failed(rc, "RWLock.try_read_lock");
  return vl_bool(false);
}

Value vl_sync_try_wlock(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int rc = pthread_rwlock_trywrlock(&want_rwlock(argv, argc)->rw);
  if (rc == 0) return vl_bool(true);
  if (rc == EBUSY || rc == EDEADLK) return vl_bool(false);
  lock_failed(rc, "RWLock.try_write_lock");
  return vl_bool(false);
}

/* ---------------------------------------------------------------- Atomic */

Value vl_sync_atomic(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  return vl_sync_atomic_new(argc ? vl_int_of(argv[0]) : 0);
}

Value vl_sync_load(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  return vl_int(__atomic_load_n(&want_atomic(argv, argc)->n, __ATOMIC_SEQ_CST));
}

Value vl_sync_store(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  __atomic_store_n(&want_atomic(argv, argc)->n, want_int(argv, argc, 1),
                   __ATOMIC_SEQ_CST);
  return vl_unit();
}

/* add returns the NEW value, so `if a.add(-1) == 0` is a race-free "was I the
 * one who brought it to zero" — the whole of WaitGroup.done rests on it. */
Value vl_sync_add(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  return vl_int(__atomic_add_fetch(&want_atomic(argv, argc)->n,
                                   want_int(argv, argc, 1), __ATOMIC_SEQ_CST));
}

/* swap returns the value that was there before, the mirror of add's choice:
 * between them a caller can learn either side of the exchange. */
Value vl_sync_swap(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  return vl_int(__atomic_exchange_n(&want_atomic(argv, argc)->n,
                                    want_int(argv, argc, 1), __ATOMIC_SEQ_CST));
}

/* cas(expect, want) is a STRONG compare-and-swap: it does not fail spuriously,
 * so `while not a.cas(x, x + 1)` is a loop that terminates for the right
 * reason. */
Value vl_sync_cas(Value *argv, int argc, VlFrame *up) {
  SYNCFN;
  int64_t expect = want_int(argv, argc, 1);
  bool won = __atomic_compare_exchange_n(&want_atomic(argv, argc)->n, &expect,
                                         want_int(argv, argc, 2), false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return vl_bool(won);
}
