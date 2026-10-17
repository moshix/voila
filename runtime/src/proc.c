/* proc.c — std/os: signals, os.kill, non-blocking process handles
 * (os.start), and the file-system walkers os.walk and os.glob.
 *
 * ------------------------------------------------------------- signals
 *
 * A signal handler may run on ANY thread, between any two instructions, and
 * may call only async-signal-safe functions: no malloc, no lock, nothing in
 * this runtime. So the handler does one thing — it writes the signal's
 * number, as one byte, to a SELF-PIPE whose write end is non-blocking — and
 * restores errno. A dispatcher thread, started the first time os.signals is
 * called, blocks reading the other end and hands each number to every
 * channel subscribed to that signal. That is the whole bridge between the
 * kernel's asynchronous world and the task model: a program receives a
 * signal the way it receives anything else, from a channel, in `select` or
 * `each`, on whichever task it chooses.
 *
 * Delivery never blocks the dispatcher: each subscription channel is
 * buffered, and a signal that arrives while it is full is DROPPED for that
 * subscriber — as Go's signal.Notify does. A burst of SIGINTs is one
 * request to stop, not a queue of them. Until a program calls os.signals the
 * runtime installs no handler at all and every signal keeps its default
 * action; once a signal is subscribed it no longer terminates the process,
 * because the program has said it will deal with it.
 *
 * ------------------------------------------------------------- processes
 *
 * os.start runs a program WITHOUT waiting for it, and hands back a process
 * handle (O_PROC): its pid, `wait()`, `kill(sig)`, and the child's standard
 * input and output as std/io writer and reader. Its standard error is the
 * parent's, so a child that writes diagnostics never blocks on a pipe nobody
 * is reading. The parent's ends of the pipes are close-on-exec, so a second
 * child started later does not inherit them and hold them open. */
#include "voila_int.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* posix_spawnp's environment: the process's own, as os.run's children get. */
#if defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#else
extern char **environ;
#endif

static Value objv(VlObj *o) {
  Value v;
  v.t = VL_OBJ;
  v.u.o = o;
  return v;
}

static Value io_error(const char *msg) {
  Value m = vl_str(msg);
  Value e = vl_exc_new("IOError", &m, 1, NULL, NULL, 0);
  vl_release(m);
  return e;
}

static Value io_errno(const char *op) {
  char buf[512];
  snprintf(buf, sizeof buf, "%s: %s", op, strerror(errno));
  return io_error(buf);
}

/* ================================================================ signals */

/* The names a program may spell: os.SIGINT and the rest are package
 * constants (low.PkgConstantNames), answered by vl_lookup_value through
 * this table so the numbers are the PLATFORM's — SIGUSR1 is 30 on macOS
 * and 10 on Linux. */
static const struct {
  const char *name;
  int sig;
} sig_names[] = {
    {"os.SIGHUP", SIGHUP},   {"os.SIGINT", SIGINT},   {"os.SIGQUIT", SIGQUIT},
    {"os.SIGKILL", SIGKILL}, {"os.SIGUSR1", SIGUSR1}, {"os.SIGUSR2", SIGUSR2},
    {"os.SIGPIPE", SIGPIPE}, {"os.SIGALRM", SIGALRM}, {"os.SIGTERM", SIGTERM},
    {"os.SIGCONT", SIGCONT}, {"os.SIGSTOP", SIGSTOP},
};

bool vl_os_signal_const(const char *name, int64_t *out) {
  for (size_t i = 0; i < sizeof sig_names / sizeof sig_names[0]; i++) {
    if (strcmp(sig_names[i].name, name) == 0) {
      *out = sig_names[i].sig;
      return true;
    }
  }
  return false;
}

#define SIG_MAX 64
#define SIG_BUF 16 /* each subscription channel's buffer */

typedef struct {
  Value ch;      /* retained */
  uint64_t mask; /* bit s: this subscriber wants signal s */
} SigSub;

static int g_sigpipe[2] = {-1, -1};
static pthread_mutex_t g_sigmu = PTHREAD_MUTEX_INITIALIZER;
static SigSub *g_subs;
static int g_nsubs, g_capsubs;
static bool g_sigthread;

/* The handler: async-signal-safe by construction. write(2) is on POSIX's
 * list of safe functions; errno is saved because the interrupted code may be
 * about to read it. A full pipe (EAGAIN) drops the byte, which only means a
 * burst was coalesced. */
static void on_signal(int s) {
  int saved = errno;
  unsigned char b = (unsigned char)s;
  ssize_t r = write(g_sigpipe[1], &b, 1);
  (void)r;
  errno = saved;
}

/* The dispatcher: an ordinary pthread, not a Voilà task, so it belongs to no
 * group and never keeps the program from finishing. */
static void *sig_loop(void *arg) {
  (void)arg;
  for (;;) {
    unsigned char b;
    ssize_t r = read(g_sigpipe[0], &b, 1);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return NULL;
    int s = b;
    /* Copy the subscribers out under the lock and deliver outside it: a
     * try_send takes the channel monitor, and no lock is held across
     * another. */
    pthread_mutex_lock(&g_sigmu);
    int n = g_nsubs;
    Value *chs = (Value *)vl_alloc(sizeof(Value) * (size_t)(n ? n : 1));
    int k = 0;
    for (int i = 0; i < n; i++)
      if (g_subs[i].mask & (1ULL << s)) chs[k++] = vl_retain(g_subs[i].ch);
    pthread_mutex_unlock(&g_sigmu);
    for (int i = 0; i < k; i++) {
      vl_chan_try_send(chs[i], vl_int(s));
      vl_release(chs[i]);
    }
    free(chs);
  }
}

/* sig_setup creates the self-pipe and the dispatcher once. Called with
 * g_sigmu held. */
static bool sig_setup(void) {
  if (g_sigthread) return true;
  if (pipe(g_sigpipe) != 0) return false;
  /* The write end must never block a signal handler; neither end may leak
   * into a child. */
  fcntl(g_sigpipe[1], F_SETFL, fcntl(g_sigpipe[1], F_GETFL) | O_NONBLOCK);
  fcntl(g_sigpipe[0], F_SETFD, FD_CLOEXEC);
  fcntl(g_sigpipe[1], F_SETFD, FD_CLOEXEC);
  pthread_t th;
  if (pthread_create(&th, NULL, sig_loop, NULL) != 0) return false;
  pthread_detach(th);
  g_sigthread = true;
  return true;
}

/* os.signals(sig...) -> chan[int]!: a channel that receives each listed
 * signal's number as it arrives. */
Value vl_os_signals(Value *argv, int argc, VlFrame *up) {
  (void)up;
  uint64_t mask = 0;
  /* The arguments may also arrive as one slice: os.signals([os.SIGINT]). */
  Value *xs = argv;
  int n = argc;
  if (argc == 1 && argv[0].t == VL_OBJ && argv[0].u.o->kind == O_SLICE) {
    xs = vl_slice_data(argv[0]);
    n = (int)vl_len(argv[0]);
  }
  if (n == 0) return io_error("os.signals: name at least one signal");
  for (int i = 0; i < n; i++) {
    if (xs[i].t != VL_INT) return io_error("os.signals: a signal is an int");
    int64_t s = xs[i].u.i;
    if (s <= 0 || s >= SIG_MAX || s == SIGKILL || s == SIGSTOP) {
      char buf[96];
      snprintf(buf, sizeof buf, "os.signals: signal %lld cannot be caught",
               (long long)s);
      return io_error(buf);
    }
    mask |= 1ULL << s;
  }
  pthread_mutex_lock(&g_sigmu);
  if (!sig_setup()) {
    pthread_mutex_unlock(&g_sigmu);
    return io_errno("os.signals");
  }
  for (int s = 1; s < SIG_MAX; s++) {
    if (!(mask & (1ULL << s))) continue;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* a blocked read resumes; it is not EINTR'd */
    if (sigaction(s, &sa, NULL) != 0) {
      pthread_mutex_unlock(&g_sigmu);
      return io_errno("os.signals");
    }
  }
  if (g_nsubs == g_capsubs) {
    g_capsubs = g_capsubs ? g_capsubs * 2 : 4;
    g_subs = (SigSub *)realloc(g_subs, sizeof(SigSub) * (size_t)g_capsubs);
    if (!g_subs) vl_abort("out of memory");
  }
  Value ch = vl_chan_new(SIG_BUF);
  g_subs[g_nsubs].ch = vl_retain(ch);
  g_subs[g_nsubs].mask = mask;
  g_nsubs++;
  pthread_mutex_unlock(&g_sigmu);
  return ch;
}

/* os.kill(pid, sig) -> unit!: send a signal to a process. */
Value vl_os_kill(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  if (argv[0].t != VL_INT || argv[1].t != VL_INT)
    return io_error("os.kill: pid and signal are ints");
  /* kill(2) reads a pid of 0 as "my process group" and -1 as "every
   * process I may signal" — one arithmetic slip from a bug that takes the
   * whole session down. A process group is os.kill_group's, by name. */
  if (argv[0].u.i <= 0)
    return io_error("os.kill: a pid must be positive (a process group is "
                    "os.kill_group)");
  if (kill((pid_t)argv[0].u.i, (int)argv[1].u.i) != 0)
    return io_errno("os.kill");
  return vl_unit();
}

/* os.kill_group(pgid, sig) -> unit!: signal every process of a group. */
Value vl_os_kill_group(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  if (argv[0].t != VL_INT || argv[1].t != VL_INT)
    return io_error("os.kill_group: group and signal are ints");
  if (argv[0].u.i <= 0)
    return io_error("os.kill_group: a process group id must be positive");
  if (kill(-(pid_t)argv[0].u.i, (int)argv[1].u.i) != 0)
    return io_errno("os.kill_group");
  return vl_unit();
}

/* os.signal_stop(ch) -> unit: end the subscription os.signals made for this
 * channel and close the channel (a reader's `each` ends). A signal nobody
 * subscribes to any more goes back to its default action. */
Value vl_os_signal_stop(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  pthread_mutex_lock(&g_sigmu);
  int found = -1;
  for (int i = 0; i < g_nsubs; i++)
    if (g_subs[i].ch.u.o == argv[0].u.o) found = i;
  if (found < 0) {
    pthread_mutex_unlock(&g_sigmu);
    return io_error("os.signal_stop: not a channel os.signals returned");
  }
  Value ch = g_subs[found].ch;
  uint64_t gone = g_subs[found].mask;
  g_subs[found] = g_subs[--g_nsubs];
  for (int i = 0; i < g_nsubs; i++) gone &= ~g_subs[i].mask;
  for (int s = 1; s < SIG_MAX; s++)
    if (gone & (1ULL << s)) signal(s, SIG_DFL);
  pthread_mutex_unlock(&g_sigmu);
  vl_chan_close(ch);
  vl_release(ch);
  return vl_unit();
}

/* ============================================================== processes */

/* status_code is os.run's exit-status convention: the exit code, or 128 +
 * the signal that killed the child. */
static int64_t status_code(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return -1;
}

static void set_cloexec(int fd) { fcntl(fd, F_SETFD, FD_CLOEXEC); }

/* Serialises os.start's pipe creation with its spawn (see there). */
static pthread_mutex_t g_spawnmu = PTHREAD_MUTEX_INITIALIZER;

/* os.start(prog, args) -> Proc!: start a program and return at once.
 *
 * posix_spawnp, not fork + exec. This process has threads, and after a
 * fork only async-signal-safe calls are allowed in the child; execvp's PATH
 * search may allocate, and an allocator lock some other thread held at the
 * fork is then held forever — the child hangs before it ever runs (seen
 * under the `leaks` tool's allocator). posix_spawn does the dup2s, the
 * signal reset and the PATH search for us, and reports a program that
 * cannot be run as an error code rather than as a child that exits 127. */
Value vl_os_start(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  int n = (int)vl_len(argv[1]);
  char **cargv = (char **)calloc((size_t)n + 2, sizeof(char *));
  if (!cargv) vl_abort("out of memory");
  cargv[0] = vl_strdup_n(vl_cstr(argv[0]), vl_strlen(argv[0]));
  Value *data = vl_slice_data(argv[1]);
  for (int i = 0; i < n; i++) {
    Value s = vl_tostr(data[i]);
    cargv[i + 1] = vl_strdup_n(vl_cstr(s), vl_strlen(s));
    vl_release(s);
  }
  int inp[2] = {-1, -1}, outp[2] = {-1, -1};
  Value result = vl_nil();
  /* All four ends are close-on-exec — the dup2 file actions below clear the
   * flag on the child's fd 0 and 1, the only copies it should keep. pipe()
   * cannot set the flag atomically, so the pipes are made and the child
   * spawned under one lock: a second os.start on another task can then
   * never spawn in the window and inherit this child's stdout write end
   * (whose reader would wait for an EOF that never comes). */
  pthread_mutex_lock(&g_spawnmu);
  if (pipe(inp) != 0 || pipe(outp) != 0) {
    pthread_mutex_unlock(&g_spawnmu);
    result = io_errno("os.start");
    goto done;
  }
  for (int i = 0; i < 2; i++) {
    set_cloexec(inp[i]);
    set_cloexec(outp[i]);
  }
  /* Writing to a child that has already exited must be an IOError (EPIPE),
   * not SIGPIPE killing this process; so SIGPIPE is ignored from the first
   * os.start on, unless the program has said what it wants (a handler, or
   * already SIG_IGN). The child gets the default back (SETSIGDEF below),
   * because an IGNORED disposition — unlike a caught one — survives exec. */
  struct sigaction cur;
  if (sigaction(SIGPIPE, NULL, &cur) == 0 && cur.sa_handler == SIG_DFL)
    signal(SIGPIPE, SIG_IGN);
  posix_spawn_file_actions_t fa;
  posix_spawnattr_t at;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, inp[0], 0);
  posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
  posix_spawnattr_init(&at);
  sigset_t def;
  sigemptyset(&def);
  sigaddset(&def, SIGPIPE);
  posix_spawnattr_setsigdefault(&at, &def);
  posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSIGDEF);
  fflush(NULL); /* our buffered output goes first, as in os.run */
  pid_t pid;
  int rc = posix_spawnp(&pid, cargv[0], &fa, &at, cargv, environ);
  pthread_mutex_unlock(&g_spawnmu);
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&at);
  if (rc != 0) {
    errno = rc;
    char op[300];
    snprintf(op, sizeof op, "os.start %s", cargv[0]);
    result = io_errno(op);
    goto done;
  }
  close(inp[0]);
  inp[0] = -1;
  close(outp[1]);
  outp[1] = -1;
  VlProc *p = (VlProc *)vl_obj_new(O_PROC, sizeof(VlProc));
  p->pid = pid;
  p->waited = false;
  p->status = 0;
  p->in = vl_io_fd_writer(inp[1], cargv[0]);
  p->out = vl_io_fd_reader(outp[0], cargv[0]);
  inp[1] = -1; /* now owned by the writer and reader */
  outp[0] = -1;
  pthread_mutex_init(&p->mu, NULL);
  result = objv(&p->hdr);
done:
  for (int i = 0; i < 2; i++) {
    if (inp[i] >= 0) close(inp[i]);
    if (outp[i] >= 0) close(outp[i]);
  }
  for (int i = 0; i <= n; i++) free(cargv[i]);
  free(cargv);
  return result;
}

/* proc_wait reaps the child once and caches its status, so wait() may be
 * called again — and from more than one task — and always answers alike. */
static Value proc_wait(VlProc *p, bool block) {
  /* A blocking wait must not hold p->mu: kill() needs it, and "kill the
   * child while another task waits for it" is exactly what a timeout does.
   * So the wait happens in two steps. First, unlocked, waitid(WNOWAIT)
   * blocks until the child has exited but leaves it a ZOMBIE — its pid
   * cannot be reused yet, so a concurrent kill() is still aimed at the right
   * process. Then, under the lock, waitpid(WNOHANG) reaps it and records the
   * status once; a second waiter finds `waited` already set. */
  if (block) {
    pthread_mutex_lock(&p->mu);
    bool done = p->waited;
    pthread_mutex_unlock(&p->mu);
    if (!done) {
      siginfo_t info;
      int r;
      do {
        r = waitid(P_PID, (id_t)p->pid, &info, WEXITED | WNOWAIT);
      } while (r < 0 && errno == EINTR);
      /* ECHILD: another waiter reaped it between the two steps; the locked
       * step below reads the status it recorded. */
      if (r < 0 && errno != ECHILD) return io_errno("wait");
    }
  }
  pthread_mutex_lock(&p->mu);
  if (!p->waited) {
    int st = 0;
    pid_t r;
    do {
      r = waitpid(p->pid, &st, WNOHANG);
    } while (r < 0 && errno == EINTR);
    if (r < 0) {
      pthread_mutex_unlock(&p->mu);
      return io_errno("wait");
    }
    if (r == 0) { /* still running (a non-blocking `running()`) */
      pthread_mutex_unlock(&p->mu);
      return vl_nil();
    }
    p->waited = true;
    p->status = status_code(st);
  }
  int64_t s = p->status;
  pthread_mutex_unlock(&p->mu);
  return vl_int(s);
}

Value vl_proc_method(Value self, const char *name, Value *argv, int argc,
                     bool *ok) {
  VlProc *p = (VlProc *)self.u.o;
  *ok = true;
  if (strcmp(name, "pid") == 0) return vl_int(p->pid);
  if (strcmp(name, "stdin") == 0) return vl_retain(p->in);
  if (strcmp(name, "stdout") == 0) return vl_retain(p->out);
  if (strcmp(name, "wait") == 0) {
    /* Our end of the child's stdin is closed first: a filter waits for end
     * of input, and waiting for it while holding its input open would never
     * return. */
    vl_release(vl_callm(p->in, "close", NULL, 0));
    return proc_wait(p, true);
  }
  if (strcmp(name, "running") == 0) {
    Value st = proc_wait(p, false);
    bool running = st.t == VL_NIL;
    vl_release(st);
    return vl_bool(running);
  }
  if (strcmp(name, "kill") == 0) {
    int sig = SIGTERM;
    if (argc > 0) {
      if (argv[0].t != VL_INT) return io_error("kill: a signal is an int");
      sig = (int)argv[0].u.i;
    }
    /* A reaped pid may already belong to somebody else: never signal it.
     * The check and the kill(2) are one step under the lock, and only the
     * locked step of proc_wait reaps, so the pid cannot be reaped (and
     * reused) in between. */
    pthread_mutex_lock(&p->mu);
    if (p->waited) {
      pthread_mutex_unlock(&p->mu);
      return io_error("kill: the process has already been waited for");
    }
    int rc = kill(p->pid, sig);
    int e = errno;
    pthread_mutex_unlock(&p->mu);
    if (rc != 0) {
      errno = e;
      return io_errno("kill");
    }
    return vl_unit();
  }
  *ok = false;
  return vl_nil();
}

void vl_proc_free(VlObj *o) {
  VlProc *p = (VlProc *)o;
  vl_release(p->in);
  vl_release(p->out);
  /* Reap the child if it has already finished, so a dropped handle leaves no
   * zombie behind; a child still running is not waited for — dropping the
   * handle must not block. */
  if (!p->waited) {
    int st;
    waitpid(p->pid, &st, WNOHANG);
  }
  pthread_mutex_destroy(&p->mu);
}

void vl_proc_str(Value b, Value v) {
  VlProc *p = (VlProc *)v.u.o;
  char tmp[64];
  snprintf(tmp, sizeof tmp, "<proc %lld>", (long long)p->pid);
  Value s = vl_str(tmp);
  vl_builder_write(b, s);
  vl_release(s);
}

/* ================================================================ walking */

static int cmp_names(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* walk_into appends every entry under `dir` to `out`, depth first, each
 * directory's entries in byte order — so the result is the same on every
 * file system. A symbolic link is listed but never followed, so a link to an
 * ancestor cannot loop. Returns an error value, or nil.
 *
 * `skipped`, when it is a slice, makes an unreadable SUBdirectory not an
 * error: the walk records "path: reason" there and goes on with the rest
 * (os.walk_all). The root must still be readable. */
static Value walk_into(const char *dir, Value out, int depth, Value skipped) {
  if (depth > 256) return io_error("os.walk: directories nest too deeply");
  DIR *d = opendir(dir);
  if (!d) {
    char op[600];
    if (depth > 0 && skipped.t == VL_OBJ) {
      snprintf(op, sizeof op, "%s: %s", dir, strerror(errno));
      vl_slice_append(skipped, vl_str(op));
      return vl_nil();
    }
    snprintf(op, sizeof op, "os.walk %s", dir);
    return io_errno(op);
  }
  char **names = NULL;
  int n = 0, cap = 0;
  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;
    if (n == cap) {
      cap = cap ? cap * 2 : 16;
      names = (char **)realloc(names, (size_t)cap * sizeof(char *));
      if (!names) vl_abort("out of memory");
    }
    names[n++] = vl_strdup_n(ent->d_name, (int64_t)strlen(ent->d_name));
  }
  closedir(d);
  qsort(names, (size_t)n, sizeof(char *), cmp_names);
  Value err = vl_nil();
  size_t dl = strlen(dir);
  bool slash = dl > 0 && dir[dl - 1] == '/';
  for (int i = 0; i < n; i++) {
    if (err.t == VL_NIL) {
      size_t len = dl + strlen(names[i]) + 2;
      char *full = (char *)vl_alloc(len);
      snprintf(full, len, "%s%s%s", dir, slash ? "" : "/", names[i]);
      vl_slice_append(out, vl_str(full));
      struct stat st;
      if (lstat(full, &st) == 0 && S_ISDIR(st.st_mode))
        err = walk_into(full, out, depth + 1, skipped);
      free(full);
    }
    free(names[i]);
  }
  free(names);
  return err;
}

/* os.walk(root) -> []str!: every file and directory below root, as paths
 * that begin with root, in depth-first order with each directory's entries
 * sorted. root itself is not in the list. */
Value vl_os_walk(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  Value out = vl_slice_new(0);
  Value err = walk_into(vl_cstr(argv[0]), out, 0, vl_nil());
  if (err.t != VL_NIL) {
    vl_release(out);
    return err;
  }
  return out;
}

/* os.walk_all(root) -> ([]str, []str)!: os.walk that carries on past a
 * subdirectory it cannot read, as Go's WalkDir lets a callback choose to.
 * The first slice is every entry it could list; the second is one
 * "path: reason" line per subdirectory it had to skip. Only an unreadable
 * ROOT is an error. */
Value vl_os_walk_all(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  Value out = vl_slice_new(0);
  Value skipped = vl_slice_new(0);
  Value err = walk_into(vl_cstr(argv[0]), out, 0, skipped);
  if (err.t != VL_NIL) {
    vl_release(out);
    vl_release(skipped);
    return err;
  }
  Value pair[2] = {out, skipped};
  Value t = vl_tuple_new(pair, 2); /* retains both */
  vl_release(out);
  vl_release(skipped);
  return t;
}

/* os.glob(pattern) -> []str!: the paths matching a shell pattern — `*`, `?`
 * and `[…]` within one path segment, as glob(3) defines them — sorted. No
 * match is an empty list, not an error. (`**` is not special: it is `*`.) */
Value vl_os_glob(Value *argv, int argc, VlFrame *up) {
  (void)argc;
  (void)up;
  glob_t g;
  memset(&g, 0, sizeof g);
  int rc = glob(vl_cstr(argv[0]), 0, NULL, &g);
  Value out = vl_slice_new(0);
  if (rc == 0) {
    for (size_t i = 0; i < g.gl_pathc; i++)
      vl_slice_append(out, vl_str(g.gl_pathv[i]));
  } else if (rc != GLOB_NOMATCH) {
    globfree(&g);
    vl_release(out);
    return io_error(rc == GLOB_NOSPACE ? "os.glob: out of memory"
                                       : "os.glob: cannot read a directory");
  }
  globfree(&g);
  return out;
}
