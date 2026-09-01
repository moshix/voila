/* regex.c — std/regex: a linear-time regular expression engine.
 *
 * The specification (§13.4) promises RE2's guarantee: linear time, and no
 * catastrophic backtracking. That promise is the reason this is a Pike VM —
 * a thread-list simulation of an NFA — and not the recursive backtracker
 * that is easier to write. A backtracker on `(a+)+$` against thirty `a`s
 * explores 2^30 paths and never returns; the machine below runs every
 * alternative ABREAST, one input byte at a time, and de-duplicates threads
 * that have reached the same instruction. At most one thread per instruction
 * can be live at once, so the whole match is O(len(input) * len(program))
 * with no path explosion available to any input. 28_regex.voi pins that case
 * so the guarantee is a test rather than a claim.
 *
 * The pipeline is the classic three stages:
 *
 *   parse()    pattern text -> a tree of Node, in one arena
 *   emit()     tree         -> a flat program of Inst
 *   run()      program      -> leftmost-first match with submatch captures
 *
 * A tree sits in the middle because counted repetition (`a{3,5}`) is compiled
 * by COPYING the body, and copying a subtree is easy where splicing a range
 * of already-emitted instructions and renumbering its jumps is not.
 *
 * PRIORITY, not just membership, is what makes greedy and non-greedy work.
 * Threads are held in the list in the order they were added, and I_SPLIT adds
 * its preferred branch first. When a thread reaches I_MATCH every thread
 * behind it is abandoned, because those are exactly the lower-priority
 * alternatives. Greedy `a*` prefers to consume; `a*?` is the same instruction
 * with its two branches swapped.
 *
 * UTF-8: matching is BYTE-oriented, and a literal in the pattern matches the
 * same bytes in the input, so any UTF-8 literal works. The one place bytes
 * would be wrong is a wildcard: `.` and a NEGATED class consume a whole UTF-8
 * sequence rather than one byte, so a capture can never be cut through the
 * middle of a character and hand back a string that is not valid UTF-8. A
 * POSITIVE class is ASCII-oriented — `[a-z]` cannot name a multi-byte
 * character — and this is stated in the package documentation rather than
 * implied.
 *
 * Errors: a bad pattern is an error VALUE (an IOError-shaped ValueError),
 * never an abort. A pattern usually comes from outside the program — a config
 * file, an argument — so failing to compile one is an ordinary thing a
 * program should be able to catch and report. vl_abort is reserved for a
 * caller bug, like handing a non-regex to a regex method.
 */

#include "voila_int.h"

#include <stdlib.h>
#include <string.h>

static bool eq(const char *a, const char *b) { return strcmp(a, b) == 0; }

/* ------------------------------------------------------------------ limits
 *
 * Both bounds exist to keep a PATTERN from costing unbounded memory, since a
 * pattern is often untrusted input. Neither constrains the INPUT, which is
 * what the linear-time guarantee is about. */
#define RX_MAX_PROG 20000 /* instructions after counted repetition expands */
#define RX_MAX_REP 1000   /* the n in a{n} — expansion is n copies of a */
#define RX_MAX_GROUPS 20  /* capture groups; slot 0 is the whole match */

/* -------------------------------------------------------------------- tree */

enum {
  N_EMPTY,
  N_CHAR,  /* one literal byte */
  N_ANY,   /* `.` — one whole UTF-8 sequence */
  N_CLASS, /* [...] — a 256-bit set, possibly negated */
  N_CAT,
  N_ALT,
  N_REP,  /* min, max (-1 = unbounded), greedy */
  N_GROUP /* capturing when group >= 0, else (?:...) */
};

typedef struct Node Node;
struct Node {
  int type;
  Node *l, *r;
  unsigned char c;      /* N_CHAR */
  unsigned char set[32]; /* N_CLASS bitmap */
  int negated;          /* N_CLASS */
  int min, max, greedy; /* N_REP */
  int group;            /* N_GROUP; -1 for non-capturing */
  int bol, eol;         /* N_EMPTY carrying an anchor */
};

/* The parser allocates every node from one arena and frees the lot at the
 * end of compilation, so no individual node is ever owned by anybody.
 *
 * The arena is a CHAIN OF BLOCKS that are never moved or resized. A growing
 * array would be smaller code, and it is what this file did first: it copied
 * the nodes into a bigger allocation and freed the old one. That silently
 * corrupts every tree over 64 nodes, because the parser holds raw `Node *`
 * in its locals and in `->l`/`->r` while it is still allocating, and every
 * one of them dangles the moment the array moves. A 33-character literal
 * compiled to a program that matched only 32 of them. Blocks never move, so
 * a pointer handed out stays valid until the whole arena is freed. */
#define ARENA_BLOCK 256

typedef struct Block Block;
struct Block {
  Block *next;
  int n;
  Node nodes[ARENA_BLOCK];
};

typedef struct {
  Block *head;
} Arena;

static void arena_free(Arena *a) {
  Block *b = a->head;
  while (b) {
    Block *next = b->next;
    free(b);
    b = next;
  }
  a->head = NULL;
}

typedef struct {
  const char *p; /* cursor */
  const char *end;
  Arena *a;
  int ngroups;
  int depth; /* nesting, so an adversarial pattern cannot exhaust the stack */
  const char *err; /* first error; parsing stops reporting after one */
} Parser;

/* A pattern is untrusted input by this file's own design, and parse_atom ->
 * parse_alt -> parse_cat -> parse_repeat -> parse_atom costs a handful of C
 * frames per `(`. Forty thousand of them segfaulted. */
#define RX_MAX_DEPTH 400

static Node *node(Parser *ps, int type) {
  Arena *a = ps->a;
  if (!a->head || a->head->n == ARENA_BLOCK) {
    Block *b = (Block *)vl_alloc(sizeof(Block));
    b->next = a->head;
    b->n = 0;
    a->head = b;
  }
  Node *n = &a->head->nodes[a->head->n++];
  memset(n, 0, sizeof *n);
  n->type = type;
  n->group = -1;
  n->max = -1;
  return n;
}

static void fail(Parser *ps, const char *why) {
  if (!ps->err) ps->err = why;
}

static int at_end(Parser *ps) { return ps->p >= ps->end; }
static int peek(Parser *ps) { return at_end(ps) ? -1 : (unsigned char)*ps->p; }

static void set_add(unsigned char *set, int c) { set[c >> 3] |= (unsigned char)(1u << (c & 7)); }

static void set_range(unsigned char *set, int lo, int hi) {
  for (int c = lo; c <= hi; c++) set_add(set, c);
}

/* The \d \w \s families, shared by the escape form and the in-class form. */
static int class_escape(unsigned char *set, int e) {
  switch (e) {
  case 'd': set_range(set, '0', '9'); return 1;
  case 'w':
    set_range(set, 'a', 'z');
    set_range(set, 'A', 'Z');
    set_range(set, '0', '9');
    set_add(set, '_');
    return 1;
  case 's':
    set_add(set, ' ');
    set_add(set, '\t');
    set_add(set, '\n');
    set_add(set, '\r');
    set_add(set, '\f');
    set_add(set, '\v');
    return 1;
  default: return 0;
  }
}

/* One escaped byte outside a class: \n \t \r \f \v \0 and \<punct>. */
static int escape_byte(int e) {
  switch (e) {
  case 'n': return '\n';
  case 't': return '\t';
  case 'r': return '\r';
  case 'f': return '\f';
  case 'v': return '\v';
  case '0': return '\0';
  default: return e; /* \. \* \\ \[ … : the character itself */
  }
}

static Node *parse_alt(Parser *ps);

static Node *parse_class(Parser *ps) {
  Node *n = node(ps, N_CLASS);
  if (peek(ps) == '^') {
    n->negated = 1;
    ps->p++;
  }
  /* A `]` first is a literal `]`, the usual convention. */
  int first = 1;
  while (!at_end(ps) && (*ps->p != ']' || first)) {
    first = 0;
    int lo;
    if (*ps->p == '\\') {
      ps->p++;
      if (at_end(ps)) {
        fail(ps, "trailing backslash");
        return n;
      }
      int e = (unsigned char)*ps->p++;
      unsigned char fam[32];
      memset(fam, 0, sizeof fam);
      if (class_escape(fam, e)) {
        for (int i = 0; i < 32; i++) n->set[i] |= fam[i];
        continue;
      }
      if (e == 'D' || e == 'W' || e == 'S') {
        unsigned char pos[32];
        memset(pos, 0, sizeof pos);
        class_escape(pos, e + 32); /* 'D'->'d' */
        for (int i = 0; i < 32; i++) n->set[i] |= (unsigned char)~pos[i];
        continue;
      }
      lo = escape_byte(e);
    } else {
      lo = (unsigned char)*ps->p++;
    }
    /* A `-` that is not last makes a range. */
    if (!at_end(ps) && *ps->p == '-' && ps->p + 1 < ps->end && ps->p[1] != ']') {
      ps->p++;
      int hi;
      if (*ps->p == '\\') {
        ps->p++;
        if (at_end(ps)) {
          fail(ps, "trailing backslash");
          return n;
        }
        hi = escape_byte((unsigned char)*ps->p++);
      } else {
        hi = (unsigned char)*ps->p++;
      }
      if (hi < lo) {
        fail(ps, "character class range is out of order");
        return n;
      }
      set_range(n->set, lo, hi);
    } else {
      set_add(n->set, lo);
    }
  }
  if (at_end(ps)) {
    fail(ps, "unterminated character class");
    return n;
  }
  ps->p++; /* ']' */
  return n;
}

static Node *parse_atom(Parser *ps) {
  int c = peek(ps);
  if (c < 0) return node(ps, N_EMPTY);

  if (c == '(') {
    if (ps->depth >= RX_MAX_DEPTH) {
      fail(ps, "pattern nests too deeply");
      return node(ps, N_EMPTY);
    }
    ps->p++;
    int group = -1;
    if (ps->p + 1 < ps->end && ps->p[0] == '?' && ps->p[1] == ':') {
      ps->p += 2;
    } else if (!at_end(ps) && *ps->p == '?') {
      fail(ps, "only the non-capturing group `(?:` is supported; "
               "lookaround is not");
      return node(ps, N_EMPTY);
    } else {
      if (ps->ngroups + 1 >= RX_MAX_GROUPS) {
        fail(ps, "too many capture groups");
        return node(ps, N_EMPTY);
      }
      group = ++ps->ngroups;
    }
    ps->depth++;
    Node *body = parse_alt(ps);
    ps->depth--;
    if (peek(ps) != ')') {
      fail(ps, "unbalanced `(`");
      return node(ps, N_EMPTY);
    }
    ps->p++;
    Node *g = node(ps, N_GROUP);
    g->l = body;
    g->group = group;
    return g;
  }
  if (c == '[') {
    ps->p++;
    return parse_class(ps);
  }
  if (c == '.') {
    ps->p++;
    return node(ps, N_ANY);
  }
  if (c == '^') {
    ps->p++;
    Node *n = node(ps, N_EMPTY);
    n->bol = 1;
    return n;
  }
  if (c == '$') {
    ps->p++;
    Node *n = node(ps, N_EMPTY);
    n->eol = 1;
    return n;
  }
  if (c == '\\') {
    ps->p++;
    if (at_end(ps)) {
      fail(ps, "trailing backslash");
      return node(ps, N_EMPTY);
    }
    int e = (unsigned char)*ps->p++;
    if (e >= '1' && e <= '9') {
      fail(ps, "backreferences are not supported: they cannot be matched in "
               "linear time");
      return node(ps, N_EMPTY);
    }
    unsigned char fam[32];
    memset(fam, 0, sizeof fam);
    if (class_escape(fam, e)) {
      Node *n = node(ps, N_CLASS);
      memcpy(n->set, fam, sizeof fam);
      return n;
    }
    if (e == 'D' || e == 'W' || e == 'S') {
      Node *n = node(ps, N_CLASS);
      class_escape(n->set, e + 32);
      n->negated = 1;
      return n;
    }
    /* A letter that named nothing is a mistake, not a literal. \b in
     * particular is a word boundary everywhere else, and silently matching a
     * literal `b` is the worst possible answer: it is quietly wrong rather
     * than loudly unsupported, exactly what the \1 and (?= refusals avoid. */
    if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z')) {
      fail(ps, "unknown escape; \\d \\w \\s and their negations are the "
               "supported letter escapes, and \\b is not a word boundary here");
      return node(ps, N_EMPTY);
    }
    Node *n = node(ps, N_CHAR);
    n->c = (unsigned char)escape_byte(e);
    return n;
  }
  if (c == ')' || c == '|') return node(ps, N_EMPTY);
  if (c == '*' || c == '+' || c == '?') {
    fail(ps, "a quantifier has nothing to repeat");
    return node(ps, N_EMPTY);
  }
  ps->p++;
  Node *n = node(ps, N_CHAR);
  n->c = (unsigned char)c;
  return n;
}

/* {n}, {n,}, {n,m} — anything else after `{` is a literal brace, which is
 * what every other engine does and what a user writing `a{` expects. */
static int parse_counted(Parser *ps, int *lo, int *hi) {
  const char *save = ps->p;
  ps->p++; /* '{' */
  if (at_end(ps) || *ps->p < '0' || *ps->p > '9') {
    ps->p = save;
    return 0;
  }
  int a = 0;
  while (!at_end(ps) && *ps->p >= '0' && *ps->p <= '9') {
    a = a * 10 + (*ps->p++ - '0');
    if (a > RX_MAX_REP) {
      fail(ps, "repetition count is too large");
      return 0;
    }
  }
  int b = a;
  if (!at_end(ps) && *ps->p == ',') {
    ps->p++;
    if (!at_end(ps) && *ps->p == '}') {
      b = -1; /* {n,} */
    } else {
      b = 0;
      while (!at_end(ps) && *ps->p >= '0' && *ps->p <= '9') {
        b = b * 10 + (*ps->p++ - '0');
        if (b > RX_MAX_REP) {
          fail(ps, "repetition count is too large");
          return 0;
        }
      }
    }
  }
  if (at_end(ps) || *ps->p != '}') {
    ps->p = save;
    return 0;
  }
  ps->p++;
  if (b >= 0 && b < a) {
    fail(ps, "repetition range is out of order");
    return 0;
  }
  *lo = a;
  *hi = b;
  return 1;
}

static Node *parse_repeat(Parser *ps) {
  Node *n = parse_atom(ps);
  for (;;) {
    int c = peek(ps);
    int lo, hi;
    if (c == '*') {
      ps->p++;
      lo = 0;
      hi = -1;
    } else if (c == '+') {
      ps->p++;
      lo = 1;
      hi = -1;
    } else if (c == '?') {
      ps->p++;
      lo = 0;
      hi = 1;
    } else if (c == '{' && parse_counted(ps, &lo, &hi)) {
      /* parsed into lo/hi */
    } else {
      return n;
    }
    Node *r = node(ps, N_REP);
    r->l = n;
    r->min = lo;
    r->max = hi;
    r->greedy = 1;
    if (peek(ps) == '?') { /* the non-greedy form */
      ps->p++;
      r->greedy = 0;
    }
    n = r;
  }
}

static Node *parse_cat(Parser *ps) {
  Node *n = NULL;
  while (!at_end(ps) && *ps->p != '|' && *ps->p != ')') {
    Node *r = parse_repeat(ps);
    if (ps->err) return r;
    if (!n) {
      n = r;
    } else {
      Node *cat = node(ps, N_CAT);
      cat->l = n;
      cat->r = r;
      n = cat;
    }
  }
  return n ? n : node(ps, N_EMPTY);
}

static Node *parse_alt(Parser *ps) {
  Node *n = parse_cat(ps);
  while (!ps->err && peek(ps) == '|') {
    ps->p++;
    Node *r = parse_cat(ps);
    Node *alt = node(ps, N_ALT);
    alt->l = n;
    alt->r = r;
    n = alt;
  }
  return n;
}

/* ----------------------------------------------------------------- program */

/* I_ANY is unused: `.` compiles to a UTF-8 alternation of I_CLASS. */
enum { I_CHAR, I_ANY, I_CLASS, I_MATCH, I_JMP, I_SPLIT, I_SAVE, I_BOL, I_EOL };

typedef struct {
  int op;
  int x, y;        /* I_JMP/I_SPLIT targets; I_SAVE slot */
  unsigned char c; /* I_CHAR */
  int cls;         /* I_CLASS: index into the class table */
} Inst;

typedef struct {
  Inst *prog;
  int n, cap;
  unsigned char *sets; /* nsets * 32 bytes */
  unsigned char *neg;  /* nsets flags */
  int nsets, setcap;
  int overflow;
} Emit;

static int emit(Emit *e, int op) {
  if (e->n == e->cap) {
    int ncap = e->cap ? e->cap * 2 : 64;
    if (ncap > RX_MAX_PROG) ncap = RX_MAX_PROG;
    if (e->n >= ncap) {
      e->overflow = 1;
      return e->n ? e->n - 1 : 0;
    }
    Inst *grown = (Inst *)vl_alloc(sizeof(Inst) * (size_t)ncap);
    if (e->prog) {
      memcpy(grown, e->prog, sizeof(Inst) * (size_t)e->n);
      free(e->prog);
    }
    e->prog = grown;
    e->cap = ncap;
  }
  memset(&e->prog[e->n], 0, sizeof(Inst));
  e->prog[e->n].op = op;
  return e->n++;
}

static int add_set(Emit *e, const unsigned char *set, int negated) {
  if (e->nsets == e->setcap) {
    int ncap = e->setcap ? e->setcap * 2 : 8;
    unsigned char *gs = (unsigned char *)vl_alloc(32 * (size_t)ncap);
    unsigned char *gn = (unsigned char *)vl_alloc((size_t)ncap);
    if (e->sets) {
      memcpy(gs, e->sets, 32 * (size_t)e->nsets);
      memcpy(gn, e->neg, (size_t)e->nsets);
      free(e->sets);
      free(e->neg);
    }
    e->sets = gs;
    e->neg = gn;
    e->setcap = ncap;
  }
  memcpy(e->sets + 32 * (size_t)e->nsets, set, 32);
  e->neg[e->nsets] = (unsigned char)negated;
  return e->nsets++;
}

static void emit_node(Emit *e, Node *n);

/* emit_utf8_class emits a machine matching ONE whole character: a byte from
 * `ascii` (which the caller restricts to 0x00-0x7F), or a well-formed UTF-8
 * sequence of two, three or four bytes.
 *
 * Spelling the multi-byte shapes out as byte sequences is what lets the VM
 * stay a plain one-step-per-byte machine while still never cutting a capture
 * through the middle of a character. The alternative — teaching the VM to
 * skip ahead by a whole sequence — does not work, because a thread added to
 * the next list is by construction resumed one byte later. */
static void emit_utf8_class(Emit *e, const unsigned char *ascii) {
  unsigned char cont[32], l2[32], l3[32], l4[32];
  memset(cont, 0, 32);
  set_range(cont, 0x80, 0xBF);
  memset(l2, 0, 32);
  set_range(l2, 0xC2, 0xDF);
  memset(l3, 0, 32);
  set_range(l3, 0xE0, 0xEF);
  memset(l4, 0, 32);
  set_range(l4, 0xF0, 0xF4);

  int ci = add_set(e, cont, 0);
  int jmps[3];
  int nj = 0;

  /* Four branches, chained by SPLIT: ascii, 2-byte, 3-byte, 4-byte. */
  int sp1 = emit(e, I_SPLIT);
  e->prog[sp1].x = e->n;
  int i0 = emit(e, I_CLASS);
  e->prog[i0].cls = add_set(e, ascii, 0);
  jmps[nj++] = emit(e, I_JMP);

  e->prog[sp1].y = e->n;
  int sp2 = emit(e, I_SPLIT);
  e->prog[sp2].x = e->n;
  int i1 = emit(e, I_CLASS);
  e->prog[i1].cls = add_set(e, l2, 0);
  int i1c = emit(e, I_CLASS);
  e->prog[i1c].cls = ci;
  jmps[nj++] = emit(e, I_JMP);

  e->prog[sp2].y = e->n;
  int sp3 = emit(e, I_SPLIT);
  e->prog[sp3].x = e->n;
  int i2 = emit(e, I_CLASS);
  e->prog[i2].cls = add_set(e, l3, 0);
  for (int k = 0; k < 2; k++) {
    int ic = emit(e, I_CLASS);
    e->prog[ic].cls = ci;
  }
  jmps[nj++] = emit(e, I_JMP);

  e->prog[sp3].y = e->n;
  int i3 = emit(e, I_CLASS);
  e->prog[i3].cls = add_set(e, l4, 0);
  for (int k = 0; k < 3; k++) {
    int ic = emit(e, I_CLASS);
    e->prog[ic].cls = ci;
  }
  for (int k = 0; k < nj; k++) e->prog[jmps[k]].x = e->n;
}

/* One copy of a node's program, used by counted repetition. Emitting the
 * subtree again is simpler and safer than copying instructions and
 * renumbering their jump targets, and it is why the parser keeps a tree. */
static void emit_rep(Emit *e, Node *n) {
  Node *body = n->l;
  int lo = n->min, hi = n->max;

  for (int i = 0; i < lo; i++) {
    emit_node(e, body);
    if (e->overflow) return;
  }
  if (hi < 0) {
    /* `x*` after the mandatory copies: split, body, jump back. */
    int sp = emit(e, I_SPLIT);
    int start = e->n;
    emit_node(e, body);
    int jp = emit(e, I_JMP);
    e->prog[jp].x = sp;
    if (e->overflow) return;
    if (n->greedy) {
      e->prog[sp].x = start;
      e->prog[sp].y = e->n;
    } else {
      e->prog[sp].x = e->n;
      e->prog[sp].y = start;
    }
    return;
  }
  /* Bounded: (hi - lo) optional copies, each with its own split. */
  int nopt = hi - lo;
  int *splits = nopt > 0 ? (int *)vl_alloc(sizeof(int) * (size_t)nopt) : NULL;
  for (int i = 0; i < nopt; i++) {
    splits[i] = emit(e, I_SPLIT);
    int start = e->n;
    emit_node(e, body);
    if (e->overflow) {
      free(splits);
      return;
    }
    if (n->greedy) {
      e->prog[splits[i]].x = start;
    } else {
      e->prog[splits[i]].y = start;
    }
  }
  /* Every optional copy skips to the same place: the end of the whole
   * repetition. Patched here, once the end is known. */
  for (int i = 0; i < nopt; i++) {
    if (n->greedy) {
      e->prog[splits[i]].y = e->n;
    } else {
      e->prog[splits[i]].x = e->n;
    }
  }
  free(splits);
}

static void emit_node(Emit *e, Node *n) {
  if (!n || e->overflow) return;
  switch (n->type) {
  case N_EMPTY:
    if (n->bol) emit(e, I_BOL);
    if (n->eol) emit(e, I_EOL);
    break;
  case N_CHAR: {
    int i = emit(e, I_CHAR);
    e->prog[i].c = n->c;
    break;
  }
  case N_ANY: {
    /* `.` is every ASCII byte except newline, plus any whole multi-byte
     * character. Conventional, and documented. */
    unsigned char a[32];
    memset(a, 0, sizeof a);
    set_range(a, 0x00, 0x7F);
    a['\n' >> 3] &= (unsigned char)~(1u << ('\n' & 7));
    emit_utf8_class(e, a);
    break;
  }
  case N_CLASS: {
    if (!n->negated) {
      /* A positive class names bytes, and a pattern cannot spell a
       * multi-byte character inside brackets, so this stays a byte test. */
      int idx = add_set(e, n->set, 0);
      int i = emit(e, I_CLASS);
      e->prog[i].cls = idx;
      break;
    }
    /* A NEGATED class means "any character that is not one of these". Its
     * ASCII half is the complement below 0x80; its other half is any whole
     * multi-byte character — which is why it goes through the same UTF-8
     * alternation as `.` rather than complementing all 256 byte values and
     * happily matching a lone continuation byte. */
    unsigned char a[32];
    for (int i = 0; i < 32; i++) a[i] = (unsigned char)~n->set[i];
    for (int c = 0x80; c < 0x100; c++)
      a[c >> 3] &= (unsigned char)~(1u << (c & 7));
    emit_utf8_class(e, a);
    break;
  }
  case N_CAT:
    emit_node(e, n->l);
    emit_node(e, n->r);
    break;
  case N_ALT: {
    int sp = emit(e, I_SPLIT);
    e->prog[sp].x = e->n;
    emit_node(e, n->l);
    int jp = emit(e, I_JMP);
    e->prog[sp].y = e->n;
    emit_node(e, n->r);
    e->prog[jp].x = e->n;
    break;
  }
  case N_REP: emit_rep(e, n); break;
  case N_GROUP: {
    if (n->group < 0) {
      emit_node(e, n->l);
      break;
    }
    int s1 = emit(e, I_SAVE);
    e->prog[s1].x = 2 * n->group;
    emit_node(e, n->l);
    int s2 = emit(e, I_SAVE);
    e->prog[s2].x = 2 * n->group + 1;
    break;
  }
  default: break;
  }
}

/* ------------------------------------------------------------------ object */

typedef struct {
  VlObj hdr;
  Inst *prog;
  int nprog;
  unsigned char *sets;
  unsigned char *neg;
  int nsets;
  int ngroups; /* not counting slot 0 */
  char *src;   /* the pattern text, for str_into and diagnostics */
  int64_t srclen;
} VlRegex;

void vl_regex_free(VlObj *o) {
  VlRegex *r = (VlRegex *)o;
  free(r->prog);
  free(r->sets);
  free(r->neg);
  free(r->src);
}

const char *vl_regex_src(Value v) { return ((VlRegex *)v.u.o)->src; }

/* --------------------------------------------------------------- compiling */

static Value rx_error(const char *why, const char *pat) {
  char buf[512];
  snprintf(buf, sizeof buf, "regex: %s in /%s/", why, pat ? pat : "");
  Value m = vl_str(buf);
  Value e = vl_exc_new("ValueError", &m, 1, NULL, NULL, 0);
  vl_release(m);
  return e;
}

/* Returns an O_REGEX value, or an error VALUE for a bad pattern. */
Value vl_regex_compile(Value pat) {
  const char *p = vl_cstr(pat);
  int64_t plen = vl_strlen(pat);

  Arena arena;
  memset(&arena, 0, sizeof arena);
  Parser ps;
  memset(&ps, 0, sizeof ps);
  ps.p = p;
  ps.end = p + plen;
  ps.a = &arena;

  Node *tree = parse_alt(&ps);
  if (!ps.err && !at_end(&ps)) fail(&ps, "unbalanced `)`");
  if (ps.err) {
    const char *why = ps.err;
    arena_free(&arena);
    return rx_error(why, p);
  }

  Emit e;
  memset(&e, 0, sizeof e);
  /* Slot 0/1 are the whole match, so the program brackets everything in an
   * implicit group zero. */
  int s0 = emit(&e, I_SAVE);
  e.prog[s0].x = 0;
  emit_node(&e, tree);
  int s1 = emit(&e, I_SAVE);
  e.prog[s1].x = 1;
  emit(&e, I_MATCH);
  arena_free(&arena);

  if (e.overflow) {
    free(e.prog);
    free(e.sets);
    free(e.neg);
    return rx_error("pattern is too large after expanding repetition", p);
  }

  VlRegex *r = (VlRegex *)vl_obj_new(O_REGEX, sizeof(VlRegex));
  r->prog = e.prog;
  r->nprog = e.n;
  r->sets = e.sets;
  r->neg = e.neg;
  r->nsets = e.nsets;
  r->ngroups = ps.ngroups;
  r->src = (char *)vl_alloc((size_t)plen + 1);
  memcpy(r->src, p, (size_t)plen);
  r->src[plen] = 0;
  r->srclen = plen;

  Value v;
  v.t = VL_OBJ;
  v.u.o = &r->hdr;
  return v;
}

/* ------------------------------------------------------------------ the VM */

typedef struct {
  int *dense;  /* pcs of CONSUMING threads, in priority order */
  int **caps;  /* one capture vector per entry of `dense` */
  int *pool;   /* backing store for those vectors */
  int *seen;   /* visit marks, compared against `gen` */
  int gen;     /* bumped when the list is cleared, so `seen` needs no memset */
  int n;
} TList;

typedef struct {
  const unsigned char *in;
  int64_t len;
  VlRegex *r;
  int ncap;
  TList a, b;
} VM;

/* How many bytes the UTF-8 sequence at `i` occupies. A stray continuation or
 * an impossible lead byte counts as one byte, so malformed input still makes
 * progress instead of looping. */
static int seqlen(const unsigned char *s, int64_t i, int64_t len) {
  unsigned char c = s[i];
  int n = 1;
  if (c >= 0xF0) n = 4;
  else if (c >= 0xE0) n = 3;
  else if (c >= 0xC0) n = 2;
  if (i + n > len) n = 1;
  return n;
}

static int set_has(VlRegex *r, int cls, unsigned char c) {
  const unsigned char *s = r->sets + 32 * (size_t)cls;
  int in = (s[c >> 3] >> (c & 7)) & 1;
  return r->neg[cls] ? !in : in;
}

/* addthread walks the epsilon closure — jumps, splits, saves and anchors are
 * resolved HERE, so the per-byte loop only ever sees instructions that
 * consume input.
 *
 * THE VISIT MARK IS SEPARATE FROM THE LIST. An earlier version marked a pc by
 * writing into the same sparse/dense pair the thread list uses, then undid
 * the write for epsilon instructions because they occupy no slot. That
 * destroyed the mark, so epsilon instructions were never de-duplicated:
 * `(a*)*` recursed until the C stack was gone, and a chain of empty
 * alternations took time exponential in its length. `seen`/`gen` is a mark
 * that nothing else touches, which is what makes the closure both
 * terminating and linear. */
static void addthread(VM *vm, TList *l, int pc, int *cap, int64_t sp) {
  if (l->seen[pc] == l->gen) return;
  l->seen[pc] = l->gen;

  Inst *in = &vm->r->prog[pc];
  switch (in->op) {
  case I_JMP:
    addthread(vm, l, in->x, cap, sp);
    return;
  case I_SPLIT:
    /* x first: priority order is what makes greedy greedy. */
    addthread(vm, l, in->x, cap, sp);
    addthread(vm, l, in->y, cap, sp);
    return;
  case I_SAVE: {
    int old = cap[in->x];
    cap[in->x] = (int)sp;
    addthread(vm, l, pc + 1, cap, sp);
    cap[in->x] = old; /* restore: the caller's vector is borrowed */
    return;
  }
  case I_BOL:
    if (sp == 0) addthread(vm, l, pc + 1, cap, sp);
    return;
  case I_EOL:
    if (sp == vm->len) addthread(vm, l, pc + 1, cap, sp);
    return;
  default:
    break; /* a consuming instruction, or MATCH: this one takes a slot */
  }
  int *slot = l->pool + (size_t)l->n * (size_t)vm->ncap;
  memcpy(slot, cap, sizeof(int) * (size_t)vm->ncap);
  l->dense[l->n] = pc;
  l->caps[l->n] = slot;
  l->n++;
}

static void tl_clear(TList *l) {
  l->n = 0;
  l->gen++;
}

/* Finds the leftmost match at or after `from`.
 *
 * ONE pass over the input, not one per starting position. The start thread is
 * injected at every step until something matches, which is what keeps the
 * whole search O(len * prog). Restarting the machine at each start position
 * instead — which this file did first — is O(len^2 * prog): matching `a+b`
 * against sixteen thousand `a`s took 1.4 seconds, and a megabyte would have
 * taken hours. Injecting LAST at each step gives the new thread the lowest
 * priority, so an earlier starting position always wins and the match stays
 * leftmost. */
static int rx_search(VlRegex *r, const unsigned char *in, int64_t len,
                     int64_t from, int *out) {
  VM vm;
  memset(&vm, 0, sizeof vm);
  vm.in = in;
  vm.len = len;
  vm.r = r;
  vm.ncap = 2 * (r->ngroups + 1);

  int np = r->nprog;
  size_t poolsz = sizeof(int) * (size_t)np * (size_t)vm.ncap;
  TList *ls[2] = {&vm.a, &vm.b};
  for (int i = 0; i < 2; i++) {
    ls[i]->dense = (int *)vl_alloc(sizeof(int) * (size_t)np);
    ls[i]->caps = (int **)vl_alloc(sizeof(int *) * (size_t)np);
    ls[i]->pool = (int *)vl_alloc(poolsz);
    ls[i]->seen = (int *)vl_alloc(sizeof(int) * (size_t)np);
    memset(ls[i]->seen, 0, sizeof(int) * (size_t)np);
    ls[i]->gen = 0;
    ls[i]->n = 0;
  }
  int *cap = (int *)vl_alloc(sizeof(int) * (size_t)vm.ncap);

  TList *clist = &vm.a, *nlist = &vm.b;
  tl_clear(clist);
  int matched = 0;

  for (int64_t sp = from; sp <= len; sp++) {
    if (!matched) {
      for (int i = 0; i < vm.ncap; i++) cap[i] = -1;
      addthread(&vm, clist, 0, cap, sp);
    }
    if (clist->n == 0 && matched) break;

    tl_clear(nlist);
    for (int i = 0; i < clist->n; i++) {
      int pc = clist->dense[i];
      int *tc = clist->caps[i];
      Inst *ins = &r->prog[pc];
      switch (ins->op) {
      case I_CHAR:
        if (sp < len && in[sp] == ins->c)
          addthread(&vm, nlist, pc + 1, tc, sp + 1);
        break;
      case I_CLASS:
        /* Always exactly one byte. A wildcard or a negated class was already
         * expanded into a UTF-8 byte sequence at compile time, so the VM
         * never needs to know about characters. */
        if (sp < len && set_has(r, ins->cls, in[sp]))
          addthread(&vm, nlist, pc + 1, tc, sp + 1);
        break;
      case I_MATCH:
        memcpy(out, tc, sizeof(int) * (size_t)vm.ncap);
        matched = 1;
        i = clist->n; /* cut every lower-priority thread */
        break;
      default: break;
      }
    }
    TList *t = clist;
    clist = nlist;
    nlist = t;
  }

  for (int i = 0; i < 2; i++) {
    free(ls[i]->dense);
    free(ls[i]->caps);
    free(ls[i]->pool);
    free(ls[i]->seen);
  }
  free(cap);
  return matched;
}

/* ----------------------------------------------------------------- natives */


/* A pattern argument may be a compiled regex OR a string, so `regex.find(p,
 * s)` reads naturally for one-off use while a loop can hoist the compile.
 * `*owned` tells the caller whether to release what came back. */
static int as_regex(Value v, Value *outv, Value *err) {
  if (v.t == VL_OBJ && v.u.o->kind == O_REGEX) {
    *outv = vl_retain(v);
    return 1;
  }
  Value c = vl_regex_compile(v);
  if (vl_isfail(c)) {
    *err = c;
    return 0;
  }
  *outv = c;
  return 1;
}

static Value caps_slice(VlRegex *r, const unsigned char *in, int *m) {
  Value out = vl_slice_new(0);
  for (int g = 0; g <= r->ngroups; g++) {
    int a = m[2 * g], b = m[2 * g + 1];
    if (a < 0 || b < 0 || b < a) {
      vl_slice_append(out, vl_str(""));
    } else {
      vl_slice_append(out, vl_str_n((const char *)in + a, b - a));
    }
  }
  return out;
}

Value vl_regex_matches(Value pat, Value subject) {
  Value re, err;
  if (!as_regex(pat, &re, &err)) return err;
  VlRegex *r = (VlRegex *)re.u.o;
  const unsigned char *in = (const unsigned char *)vl_cstr(subject);
  int64_t len = vl_strlen(subject);
  int *m = (int *)vl_alloc(sizeof(int) * (size_t)(2 * (r->ngroups + 1)));
  int ok = rx_search(r, in, len, 0, m);
  free(m);
  vl_release(re);
  return vl_bool(ok != 0);
}

Value vl_regex_find(Value pat, Value subject) {
  Value re, err;
  if (!as_regex(pat, &re, &err)) return err;
  VlRegex *r = (VlRegex *)re.u.o;
  const unsigned char *in = (const unsigned char *)vl_cstr(subject);
  int64_t len = vl_strlen(subject);
  int *m = (int *)vl_alloc(sizeof(int) * (size_t)(2 * (r->ngroups + 1)));
  Value out = vl_nil();
  if (rx_search(r, in, len, 0, m)) out = caps_slice(r, in, m);
  free(m);
  vl_release(re);
  return out;
}

Value vl_regex_find_all(Value pat, Value subject) {
  Value re, err;
  if (!as_regex(pat, &re, &err)) return err;
  VlRegex *r = (VlRegex *)re.u.o;
  const unsigned char *in = (const unsigned char *)vl_cstr(subject);
  int64_t len = vl_strlen(subject);
  int nc = 2 * (r->ngroups + 1);
  int *m = (int *)vl_alloc(sizeof(int) * (size_t)nc);
  Value out = vl_slice_new(0);
  int64_t at = 0;
  while (at <= len && rx_search(r, in, len, at, m)) {
    vl_slice_append(out, caps_slice(r, in, m));
    /* An empty match must still advance, or find_all never terminates. */
    at = (m[1] > m[0]) ? m[1] : m[0] + (m[0] < len ? seqlen(in, m[0], len) : 1);
  }
  free(m);
  vl_release(re);
  return out;
}

/* $0..$9 name a capture; $$ is a literal dollar. Anything else after $ is
 * left alone, so a stray `$` in a replacement is not an error. */
static void expand_repl(Value b, const char *rep, int64_t rlen, VlRegex *r,
                        const unsigned char *in, int *m) {
  for (int64_t i = 0; i < rlen; i++) {
    if (rep[i] == '$' && i + 1 < rlen) {
      char n = rep[i + 1];
      if (n == '$') {
        Value s = vl_str("$");
        vl_builder_write(b, s);
        vl_release(s);
        i++;
        continue;
      }
      if (n >= '0' && n <= '9') {
        int g = n - '0';
        if (g <= r->ngroups) {
          int a = m[2 * g], e = m[2 * g + 1];
          if (a >= 0 && e >= a) {
            Value s = vl_str_n((const char *)in + a, e - a);
            vl_builder_write(b, s);
            vl_release(s);
          }
          i++;
          continue;
        }
      }
    }
    Value s = vl_str_n(rep + i, 1);
    vl_builder_write(b, s);
    vl_release(s);
  }
}

Value vl_regex_replace(Value pat, Value subject, Value repl) {
  Value re, err;
  if (!as_regex(pat, &re, &err)) return err;
  VlRegex *r = (VlRegex *)re.u.o;
  const unsigned char *in = (const unsigned char *)vl_cstr(subject);
  int64_t len = vl_strlen(subject);
  const char *rep = vl_cstr(repl);
  int64_t rlen = vl_strlen(repl);
  int nc = 2 * (r->ngroups + 1);
  int *m = (int *)vl_alloc(sizeof(int) * (size_t)nc);

  Value b = vl_builder_new();
  int64_t at = 0;
  while (at <= len && rx_search(r, in, len, at, m)) {
    if (m[0] > at) {
      Value s = vl_str_n((const char *)in + at, m[0] - at);
      vl_builder_write(b, s);
      vl_release(s);
    }
    expand_repl(b, rep, rlen, r, in, m);
    int64_t next = (m[1] > m[0])
                       ? m[1]
                       : m[0] + (m[0] < len ? seqlen(in, m[0], len) : 1);
    if (m[1] == m[0] && m[0] < len) {
      /* An empty match: copy the character it sat before, or it is lost. */
      int k = seqlen(in, m[0], len);
      Value s = vl_str_n((const char *)in + m[0], k);
      vl_builder_write(b, s);
      vl_release(s);
    }
    at = next;
  }
  if (at < len) {
    Value s = vl_str_n((const char *)in + at, len - at);
    vl_builder_write(b, s);
    vl_release(s);
  }
  Value out = vl_builder_done(b);
  vl_release(b);
  free(m);
  vl_release(re);
  return out;
}

Value vl_regex_split(Value pat, Value subject) {
  Value re, err;
  if (!as_regex(pat, &re, &err)) return err;
  VlRegex *r = (VlRegex *)re.u.o;
  const unsigned char *in = (const unsigned char *)vl_cstr(subject);
  int64_t len = vl_strlen(subject);
  int nc = 2 * (r->ngroups + 1);
  int *m = (int *)vl_alloc(sizeof(int) * (size_t)nc);
  Value out = vl_slice_new(0);
  int64_t at = 0, piece = 0;
  while (at <= len && rx_search(r, in, len, at, m)) {
    if (m[1] == m[0]) { /* never split on an empty match */
      if (m[0] >= len) break;
      at = m[0] + seqlen(in, m[0], len);
      continue;
    }
    vl_slice_append(out, vl_str_n((const char *)in + piece, m[0] - piece));
    piece = m[1];
    at = m[1];
  }
  vl_slice_append(out, vl_str_n((const char *)in + piece, len - piece));
  free(m);
  vl_release(re);
  return out;
}

/* Methods on a compiled pattern: the same five, with the pattern implied. */
Value vl_regex_method(Value self, const char *name, Value *argv, int argc,
                      bool *ok) {
  *ok = true;
  if (eq(name, "matches") && argc >= 1) return vl_regex_matches(self, argv[0]);
  if (eq(name, "find") && argc >= 1) return vl_regex_find(self, argv[0]);
  if (eq(name, "find_all") && argc >= 1) return vl_regex_find_all(self, argv[0]);
  if (eq(name, "replace") && argc >= 2)
    return vl_regex_replace(self, argv[0], argv[1]);
  if (eq(name, "split") && argc >= 1) return vl_regex_split(self, argv[0]);
  if (eq(name, "source")) return vl_str(((VlRegex *)self.u.o)->src);
  *ok = false;
  return vl_nil();
}
