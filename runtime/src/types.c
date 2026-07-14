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

void vl_register_types(const VlType *types, int ntypes, const VlVariant *vars,
                       int nvars, const VlMethod *methods, int nmethods) {
  g_types = types;
  g_ntypes = ntypes;
  g_vars = vars;
  g_nvars = nvars;
  g_methods = methods;
  g_nmethods = nmethods;
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

const VlMethod *vl_find_method(const char *type, const char *name) {
  if (!type) return NULL;
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
      if (strcmp(names[j], t->fields[i]) == 0) {
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

/* ---------------------------------------------------------------- fields */

static Value *struct_field_ptr(Value obj, const char *name) {
  VlStruct *s = (VlStruct *)obj.u.o;
  for (int i = 0; i < s->type->nfields; i++)
    if (strcmp(s->type->fields[i], name) == 0) return &s->f[i];
  return NULL;
}

Value vl_field(Value o, const char *name) {
  if (o.t != VL_OBJ) vl_abort("%s has no field `%s`", vl_kind_name(o), name);
  switch ((VlKind)o.u.o->kind) {
  case O_STRUCT: {
    Value *p = struct_field_ptr(o, name);
    if (!p) vl_abort("type %s has no field `%s`", vl_kind_name(o), name);
    return vl_retain(*p);
  }
  case O_ENUM: {
    VlEnum *e = (VlEnum *)o.u.o;
    for (int i = 0; i < e->n; i++)
      if (e->var->fields && strcmp(e->var->fields[i], name) == 0)
        return vl_retain(e->f[i]);
    vl_abort("variant %s has no field `%s`", e->var->variant, name);
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
    vl_abort("exception %s has no field `%s`", vl_kind_name(o), name);
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
    vl_abort("%s has no field `%s`", vl_kind_name(o), name);
  }
  return vl_nil();
}

void vl_setfld(Value o, const char *name, Value v) {
  if (o.t != VL_OBJ) vl_abort("cannot assign field `%s` on %s", name, vl_kind_name(o));
  switch ((VlKind)o.u.o->kind) {
  case O_STRUCT: {
    Value *p = struct_field_ptr(o, name);
    if (!p) vl_abort("type %s has no field `%s`", vl_kind_name(o), name);
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
  vl_abort("cannot assign field `%s` on %s", name, vl_kind_name(o));
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
  const VlType *t = vl_find_type(type_text);
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
