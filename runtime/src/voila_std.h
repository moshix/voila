/* voila_std.h — the prelude and std packages, reached by name.
 *
 * Generated code resolves every builtin and package function ONCE at start
 * up into a table of VlFn pointers, so calls are indirect-but-direct: no
 * string hashing on the hot path.
 */
#ifndef VOILA_STD_H
#define VOILA_STD_H

/* Resolve a prelude or package function: "len", "fmt.printf", "str.upper".
 * Returns NULL when the name is not a function. */
VlFn vl_lookup_fn(const char *name);

/* Resolve a name used as a VALUE: a function reference, or a package
 * constant such as "time.Second", "math.pi", "os.stderr". Throws if the
 * name is unknown. */
Value vl_lookup_value(const char *name);

/* True when the name is provided by this runtime (used by tests to keep the
 * code generator's table honest). */
bool vl_has_name(const char *name);

/* EXTENSION natives. The std packages built on a vendored C library —
 * std/tls (BearSSL), std/compress (miniz), std/sqlite (SQLite) — keep their
 * natives OUT of libvoila: each lives in a separate archive (runtime/src/ext)
 * that is compiled, and linked, only into a program that uses the package.
 * A static table here naming them would drag every one of those libraries
 * into every binary. Instead the generated main() registers the table of
 * each extension the program uses, before it resolves a single name, and
 * vl_lookup_fn searches the registered tables after its own. The table is
 * terminated by a {NULL, NULL} row. */
typedef struct {
  const char *name;
  VlFn fn;
} VlExtEntry;

void vl_ext_register(const VlExtEntry *table);

#endif /* VOILA_STD_H */
