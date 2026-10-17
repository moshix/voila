/* types.c — the type registry the generated program installs, plus struct
 * and enum construction, field access, and user-method dispatch. */
#include "voila_int.h"

#include <stdlib.h>
#include <string.h>

static const VlType *g_types;
static int g_ntypes;
static const VlVariant *g_vars;
static int g_nvars;
static const VlMethod *g_methods;
static int g_nmethods;

/* The method table is searched by (type, method) on EVERY user method call,
 * and a linear scan with two strcmp per row was the single largest cost in a
 * large program (profiled: two thirds of the compiler checking itself). So
 * the table gets an open-addressed hash index, built once at registration —
 * before any task exists, so it is read-only and race-free afterwards. Slot
 * value is row index + 1 (0 = empty). The FIRST row of a duplicated pair is
 * the one indexed, which is the order the linear scan answered in. */
static int *g_mhash;
static uint64_t g_mhmask;

static uint64_t method_key(const char *type, const char *name) {
  uint64_t h = 1469598103934665603ULL;            /* FNV-1a */
  for (const char *p = type; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
  h = (h ^ '|') * 1099511628211ULL;
  for (const char *p = name; *p; p++) h = (h ^ (unsigned char)*p) * 1099511628211ULL;
  return h;
}

static void index_methods(void) {
  uint64_t cap = 16;
  while (cap < (uint64_t)g_nmethods * 2 + 1) cap <<= 1;
  g_mhash = (int *)calloc((size_t)cap, sizeof(int));
  if (!g_mhash) return;                 /* the linear scan still answers */
  g_mhmask = cap - 1;
  for (int i = 0; i < g_nmethods; i++) {
    uint64_t at = method_key(g_methods[i].type, g_methods[i].method) & g_mhmask;
    for (;;) {
      int r = g_mhash[at];
      if (r == 0) { g_mhash[at] = i + 1; break; }
      if (strcmp(g_methods[r - 1].type, g_methods[i].type) == 0 &&
          strcmp(g_methods[r - 1].method, g_methods[i].method) == 0)
        break;                          /* keep the first */
      at = (at + 1) & g_mhmask;
    }
  }
}

void vl_register_types(const VlType *types, int ntypes, const VlVariant *vars,
                       int nvars, const VlMethod *methods, int nmethods) {
  g_types = types;
  g_ntypes = ntypes;
  g_vars = vars;
  g_nvars = nvars;
  g_methods = methods;
  g_nmethods = nmethods;
  free(g_mhash);
  g_mhash = NULL;
  if (nmethods > 0) index_methods();
}

/* vl_user_type looks only at the types the program declared; vl_find_type
 * (err.c) also answers for the predeclared exception layouts. */
const VlType *vl_user_type(const char *name) {
  for (int i = 0; i < g_ntypes; i++)
    if (strcmp(g_types[i].name, name) == 0) return &g_types[i];
  return NULL;
}

const VlVariant *vl_find_variant(const char *type, const char *variant) {
  for (int i = 0; i < g_nvars; i++)
    if (strcmp(g_vars[i].type, type) == 0 &&
        strcmp(g_vars[i].variant, variant) == 0)
      return &g_vars[i];
  return NULL;
}

/* vl_enum_exists reports whether `type` is an enum the program declares —
 * that is, whether any variant names it as its owner. */
bool vl_enum_exists(const char *type) {
  for (int i = 0; i < g_nvars; i++)
    if (strcmp(g_vars[i].type, type) == 0) return true;
  return false;
}

const VlMethod *vl_find_method(const char *type, const char *name) {
  if (!type) return NULL;
  if (g_mhash) {
    uint64_t at = method_key(type, name) & g_mhmask;
    for (;;) {
      int r = g_mhash[at];
      if (r == 0) return NULL;
      const VlMethod *m = &g_methods[r - 1];
      /* Pooled literals compare by pointer first (struct_field_ptr). */
      if ((m->type == type || strcmp(m->type, type) == 0) &&
          (m->method == name || strcmp(m->method, name) == 0))
        return m;
      at = (at + 1) & g_mhmask;
    }
  }
  for (int i = 0; i < g_nmethods; i++)
    if (strcmp(g_methods[i].type, type) == 0 &&
        strcmp(g_methods[i].method, name) == 0)
      return &g_methods[i];
  return NULL;
}

/* ---------------------------------------------------------------- structs */

Value vl_struct_new(const char *type, Value *positional, int npos,
                    const char **names, Value *named, int nnamed) {
  if (strcmp(type, "str.Builder") == 0) return vl_builder_new();
  const VlType *t = vl_find_type(type);
  if (!t) vl_abort("unknown struct type `%s`", type);
  VlStruct *s = (VlStruct *)vl_obj_new(O_STRUCT, sizeof(VlStruct));
  s->type = t;
  s->f = (Value *)vl_alloc(sizeof(Value) * (size_t)(t->nfields ? t->nfields : 1));
  for (int i = 0; i < t->nfields; i++) {
    Value v = vl_nil();
    bool got = false;
    for (int j = 0; j < nnamed; j++) {
      if (names[j] == t->fields[i] || strcmp(names[j], t->fields[i]) == 0) {
        v = vl_retain(named[j]);
        got = true;
        break;
      }
    }
    if (!got && i < npos) { v = vl_retain(positional[i]); got = true; }
    if (!got && t->ftypes) v = vl_zero(t->ftypes[i]);
    s->f[i] = v;
  }
  Value out;
  out.t = VL_OBJ;
  out.u.o = &s->hdr;
  return out;
}

/* The code generator emits field defaults and zero values as an explicit
 * initializer list, so a partially built struct is completed here only for
 * fields the program left out (which the generator fills with ZERO/defaults
 * before calling). */

Value vl_enum_new(const char *type, const char *variant, Value *argv,
                  int argc) {
  const VlVariant *v = vl_find_variant(type, variant);
  if (!v) vl_abort("unknown variant `%s.%s`", type, variant);
  VlEnum *e = (VlEnum *)vl_obj_new(O_ENUM, sizeof(VlEnum));
  e->var = v;
  e->n = v->nfields;
  e->f = (Value *)vl_alloc(sizeof(Value) * (size_t)(e->n ? e->n : 1));
  for (int i = 0; i < e->n; i++)
    e->f[i] = i < argc ? vl_retain(argv[i]) : vl_nil();
  Value out;
  out.t = VL_OBJ;
  out.u.o = &e->hdr;
  return out;
}

/* ---------------------------------------------------------- box, newtype */

Value vl_box_new(Value owned) {
  VlBox *b = (VlBox *)vl_obj_new(O_BOX, sizeof(VlBox));
  b->v = owned;
  Value out;
  out.t = VL_OBJ;
  out.u.o = &b->hdr;
  return out;
}

/* vl_box_unique is a Box's COPY-ON-WRITE (§6.3): a Box is a unique owner
 * with VALUE semantics, but a Value is a shared reference, so `var b = a`
 * leaves two names on one VlBox. Every in-place write through a Box — a
 * `set`, or a field or element assigned through it — is lowered as
 *
 *     UNIQ  b, b, n      write the result back where b came from
 *     …write through b…
 *
 * and this function decides: a Box whose count exceeds `n` (the references
 * the lowerer KNOWS of: the location itself, plus a temporary it loaded the
 * Box into) has another owner, so the write goes to a fresh Box holding a
 * shallow copy of the contents — a struct's fields retained one by one, so a
 * field written through the copy is not seen through the original. Anything
 * that is not a Box, or a Box nobody else holds, comes back unchanged.
 *
 * Over-counting (a stale temporary still holding the Box) only costs a copy,
 * never correctness. And it is what keeps the Acyclicity Rule's promise for
 * Box: `b.set(LNode{next: b})` evaluates the argument first, so the struct
 * already holds `b` and the count is 2 — the set lands in a NEW Box, which
 * holds the node, which holds the OLD Box. A tree, not a cycle.
 *
 * Returns an owned reference. */
/* struct_shallow_copy makes a new struct of the same type whose fields are
 * the original's, each retained: a field ASSIGNED through the copy is not
 * seen through the original, while a field's own contents are shared until
 * they are written in turn (each level of a write path is made unique on its
 * own — see vl_cow_unique). A newtype is a one-field struct and is copied the
 * same way. Returns an owned reference. */
static Value struct_shallow_copy(Value in) {
  const VlStruct *s = (const VlStruct *)in.u.o;
  VlStruct *c = (VlStruct *)vl_obj_new(O_STRUCT, sizeof(VlStruct));
  /* A newtype carries its one value in f[0] whatever its type's nfields. */
  int nf = s->type->newtype ? 1 : s->type->nfields;
  c->type = s->type;
  c->f = (Value *)vl_alloc(sizeof(Value) * (size_t)(nf ? nf : 1));
  for (int i = 0; i < nf; i++) c->f[i] = vl_retain(s->f[i]);
  if (nf == 0) c->f[0] = vl_nil();
  Value cv;
  cv.t = VL_OBJ;
  cv.u.o = &c->hdr;
  return cv;
}

Value vl_box_unique(Value v, int64_t n) {
  if (!vl_is_box(v) || __atomic_load_n(&v.u.o->rc, __ATOMIC_RELAXED) <= n)
    return vl_retain(v);
  Value in = ((VlBox *)v.u.o)->v;
  if (in.t == VL_OBJ && in.u.o->kind == O_STRUCT)
    return vl_box_new(struct_shallow_copy(in));
  return vl_box_new(vl_retain(in));
}

/* container_shallow_copy: a new slice, map or set holding the original's
 * elements (and keys), each retained — a write through the copy (an element
 * stored, an append, a key added) is not seen through the original, while an
 * element's own contents are shared until they are written in turn. A map
 * and a set keep their insertion order. Returns an owned reference. */
static Value container_shallow_copy(Value in) {
  if (in.u.o->kind == O_SLICE) {
    const VlSlice *s = (const VlSlice *)in.u.o;
    Value out = vl_slice_new(0);
    for (int64_t i = 0; i < s->len; i++) vl_slice_append(out, vl_retain(s->e[i]));
    return out;
  }
  const VlMap *m = (const VlMap *)in.u.o;
  Value out = m->is_set ? vl_set_new() : vl_map_new();
  for (int64_t i = 0; i < m->nents; i++)
    if (m->ents[i].used)
      vl_map_set(out, m->ents[i].k, m->is_set ? vl_nil() : vl_retain(m->ents[i].v));
  return out;
}

/* vl_cow_unique is the copy-on-write of every VALUE-semantics object: a Box
 * (vl_box_unique, above) and a STRUCT (§3.4, §6.4). A struct is a shared
 * reference in this runtime, so `var b = a` leaves two names on one VlStruct;
 * the lowerer (low/boxcow.voi) precedes every in-place write through a
 * location — a field or element assigned, a `mut self` method called, a
 * `mut` parameter passed — with UNIQ, which calls this when the count exceeds
 * the references it knows the location itself holds. The write then goes to a
 * shallow copy, written back where the value was read from, and `a` keeps the
 * value it had. Anything else comes back unchanged. Returns an owned
 * reference. */
Value vl_cow_unique(Value v, int64_t n) {
  if (v.t != VL_OBJ || __atomic_load_n(&v.u.o->rc, __ATOMIC_RELAXED) <= n)
    return vl_retain(v);
  if (v.u.o->kind == O_BOX) return vl_box_unique(v, n);
  if (v.u.o->kind == O_STRUCT) return struct_shallow_copy(v);
  /* A slice, a map or a set held by a struct field, an element or a second
   * name: containers have value semantics too (§6.4), and a write through
   * one location must not be seen through another. */
  if (v.u.o->kind == O_SLICE || v.u.o->kind == O_MAP || v.u.o->kind == O_SET) {
    /* Under the collection's lock (call.c, coll_writer): a task may be
     * growing it through a `mut` parameter right now, reallocating the
     * storage this copy reads. */
    VlLock *lk = vl_addr_lock(v.u.o);
    vl_lock_acquire(lk);
    Value c = container_shallow_copy(v);
    vl_lock_release(lk);
    return c;
  }
  return vl_retain(v);
}

/* vl_up_slot is the address of an upvalue's register. */
Value *vl_up_slot(VlFrame *f, int depth, int slot) {
  for (int i = 0; i < depth; i++) {
    if (!f) vl_abort("upvalue depth exceeded");
    f = f->up;
  }
  if (!f || slot >= f->nregs) vl_abort("bad upvalue slot");
  return &f->r[slot];
}

Value vl_newtype_wrap(const VlType *t, Value owned) {
  VlStruct *s = (VlStruct *)vl_obj_new(O_STRUCT, sizeof(VlStruct));
  s->type = t;
  s->f = (Value *)vl_alloc(sizeof(Value));
  s->f[0] = owned;
  Value out;
  out.t = VL_OBJ;
  out.u.o = &s->hdr;
  return out;
}

/* ---------------------------------------------------------------- fields */

static Value *struct_field_ptr(Value obj, const char *name) {
  VlStruct *s = (VlStruct *)obj.u.o;
  /* The field names of the type table and of a generated access are the
   * same C string literal, which the C compiler pools: a pointer match
   * first, the comparison only when that misses. */
  for (int i = 0; i < s->type->nfields; i++)
    if (s->type->fields[i] == name) return &s->f[i];
  for (int i = 0; i < s->type->nfields; i++)
    if (strcmp(s->type->fields[i], name) == 0) return &s->f[i];
  return NULL;
}

Value vl_field(Value o, const char *name) {
  if (o.t != VL_OBJ) vl_fault("%s has no field `%s`", vl_kind_name(o), name);
  switch ((VlKind)o.u.o->kind) {
  case O_BOX:
    /* Auto-deref: a Box is transparent to field access. */
    return vl_field(((VlBox *)o.u.o)->v, name);
  case O_STRUCT: {
    /* A newtype over a struct has its underlying type's fields. */
    if (((VlStruct *)o.u.o)->type->newtype)
      return vl_field(((VlStruct *)o.u.o)->f[0], name);
    Value *p = struct_field_ptr(o, name);
    if (!p) vl_fault("type %s has no field `%s`", vl_kind_name(o), name);
    return vl_retain(*p);
  }
  case O_ENUM: {
    VlEnum *e = (VlEnum *)o.u.o;
    for (int i = 0; i < e->n; i++)
      if (e->var->fields && strcmp(e->var->fields[i], name) == 0)
        return vl_retain(e->f[i]);
    vl_fault("variant %s has no field `%s`", e->var->variant, name);
    break;
  }
  case O_ERR: {
    VlErr *e = (VlErr *)o.u.o;
    if (e->fields.t == VL_OBJ) {
      Value k = vl_str(name);
      Value out;
      bool ok = vl_map_get(e->fields, k, &out);
      vl_release(k);
      if (ok) return out;
    }
    if (strcmp(name, "msg") == 0) return vl_retain(e->msg);
    vl_fault("exception %s has no field `%s`", vl_kind_name(o), name);
    break;
  }
  case O_SHARED:
    return vl_field(((VlShared *)o.u.o)->v, name);
  case O_CELL: {
    VlCell *c = (VlCell *)o.u.o;
    pthread_mutex_lock(&c->mu);
    Value v = vl_field(c->v, name);
    pthread_mutex_unlock(&c->mu);
    return v;
  }
  default:
    vl_fault("%s has no field `%s`", vl_kind_name(o), name);
  }
  return vl_nil();
}

/* own_inner makes the struct a Box or a newtype HOLDS unique before a write
 * reaches through to it: the holder was made unique by the lowerer's UNIQ,
 * but what it holds may still be shared with another name (`var b = Box(p);
 * b.x = 5` must not change `p`). A shared inner struct is replaced, in the
 * holder, by a shallow copy — invisible to everybody else, who keep the
 * original. Anything else is left alone. */
static void own_inner(Value *slot) {
  if (slot->t == VL_OBJ && slot->u.o->kind == O_STRUCT &&
      __atomic_load_n(&slot->u.o->rc, __ATOMIC_RELAXED) > 1) {
    Value c = struct_shallow_copy(*slot);
    vl_set(slot, c);
  }
}

void vl_own_inner(Value holder) {
  if (holder.t != VL_OBJ) return;
  if (holder.u.o->kind == O_BOX) {
    own_inner(&((VlBox *)holder.u.o)->v);
  } else if (holder.u.o->kind == O_STRUCT &&
             ((VlStruct *)holder.u.o)->type->newtype) {
    own_inner(&((VlStruct *)holder.u.o)->f[0]);
  }
}

void vl_setfld(Value o, const char *name, Value v) {
  if (o.t != VL_OBJ) vl_fault("cannot assign field `%s` on %s", name, vl_kind_name(o));
  switch ((VlKind)o.u.o->kind) {
  case O_BOX:
    own_inner(&((VlBox *)o.u.o)->v);
    vl_setfld(((VlBox *)o.u.o)->v, name, v);
    return;
  case O_STRUCT: {
    if (((VlStruct *)o.u.o)->type->newtype) {
      own_inner(&((VlStruct *)o.u.o)->f[0]);
      vl_setfld(((VlStruct *)o.u.o)->f[0], name, v);
      return;
    }
    Value *p = struct_field_ptr(o, name);
    if (!p) vl_fault("type %s has no field `%s`", vl_kind_name(o), name);
    vl_set(p, v);
    return;
  }
  case O_ERR: {
    VlErr *e = (VlErr *)o.u.o;
    if (e->fields.t == VL_OBJ) {
      Value k = vl_str(name);
      vl_map_set(e->fields, k, v);
      vl_release(k);
      return;
    }
    break;
  }
  case O_SHARED:
    vl_setfld(((VlShared *)o.u.o)->v, name, v);
    return;
  case O_CELL: {
    VlCell *c = (VlCell *)o.u.o;
    pthread_mutex_lock(&c->mu);
    vl_setfld(c->v, name, v);
    pthread_mutex_unlock(&c->mu);
    return;
  }
  default:
    break;
  }
  vl_fault("cannot assign field `%s` on %s", name, vl_kind_name(o));
}

/* ---------------------------------------------------------------- show */

/* vl_user_show consults a user `show` implementation (the Show trait). */
Value vl_user_show(Value v, bool *ok) {
  *ok = false;
  const char *tn = NULL;
  if (v.t == VL_OBJ && v.u.o->kind == O_STRUCT) tn = ((VlStruct *)v.u.o)->type->name;
  else if (v.t == VL_OBJ && v.u.o->kind == O_ENUM) tn = ((VlEnum *)v.u.o)->var->type;
  if (!tn) return vl_nil();
  const VlMethod *m = vl_find_method(tn, "show");
  if (!m) return vl_nil();
  Value self = v;
  vl_set_disp(m->type);
  Value out = m->fn(&self, 1, NULL);
  *ok = true;
  return out;
}

/* ---------------------------------------------------------------- zero */

Value vl_zero(const char *type_text) {
  if (!type_text || !type_text[0]) return vl_nil();
  if (strcmp(type_text, "int") == 0 || strcmp(type_text, "i8") == 0 ||
      strcmp(type_text, "i16") == 0 || strcmp(type_text, "i32") == 0 ||
      strcmp(type_text, "i64") == 0 || strcmp(type_text, "u8") == 0 ||
      strcmp(type_text, "u16") == 0 || strcmp(type_text, "u32") == 0 ||
      strcmp(type_text, "u64") == 0 || strcmp(type_text, "byte") == 0)
    return vl_int(0);
  if (strcmp(type_text, "float") == 0 || strcmp(type_text, "f32") == 0)
    return vl_float(0);
  if (strcmp(type_text, "dec") == 0) return vl_dec_from_int(0);
  if (strcmp(type_text, "bool") == 0) return vl_bool(false);
  if (strcmp(type_text, "str") == 0) return vl_str("");
  if (strcmp(type_text, "rune") == 0) return vl_rune(0);
  if (strcmp(type_text, "unit") == 0) return vl_unit();
  if (strncmp(type_text, "[]", 2) == 0) return vl_slice_new(0);
  if (strncmp(type_text, "map[", 4) == 0) return vl_map_new();
  if (strncmp(type_text, "set[", 4) == 0) return vl_set_new();
  if (strncmp(type_text, "?", 1) == 0) return vl_nil();
  if (strncmp(type_text, "weak[", 5) == 0) {
    VlWeak *w = (VlWeak *)vl_obj_new(O_WEAK, sizeof(VlWeak));
    Value v;
    v.t = VL_OBJ;
    v.u.o = &w->hdr;
    return v;
  }
  if (strncmp(type_text, "Box[", 4) == 0 && !vl_find_type("Box")) {
    /* The zero Box holds the zero of its element type. The element is the
     * text between the outer brackets. */
    size_t n = strlen(type_text);
    char *el = vl_strdup_n(type_text + 4, (int64_t)(n > 5 ? n - 5 : 0));
    Value z = vl_zero(el);
    free(el);
    return vl_box_new(z);
  }
  const VlType *t = vl_find_type(type_text);
  if (!t) {
    /* A generic instantiation, `Stack[int]`: types are erased (§3.7), so
     * its descriptor is the generic's own. */
    const char *br = strchr(type_text, '[');
    if (br && br != type_text) {
      char *base = vl_strdup_n(type_text, (int64_t)(br - type_text));
      t = vl_find_type(base);
      free(base);
    }
  }
  if (t && t->newtype) {
    /* A newtype's zero is its underlying type's zero, wrapped. */
    return vl_newtype_wrap(t, vl_zero(t->ftypes ? t->ftypes[0] : ""));
  }
  if (t) {
    /* All fields zero; the generator supplies declared defaults explicitly. */
    VlStruct *s = (VlStruct *)vl_obj_new(O_STRUCT, sizeof(VlStruct));
    s->type = t;
    s->f = (Value *)vl_alloc(sizeof(Value) * (size_t)(t->nfields ? t->nfields : 1));
    for (int i = 0; i < t->nfields; i++) s->f[i] = vl_nil();
    Value v;
    v.t = VL_OBJ;
    v.u.o = &s->hdr;
    return v;
  }
  return vl_nil();
}
