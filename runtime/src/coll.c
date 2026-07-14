/* coll.c — maps, sets, indexing, slicing, iteration. Maps and sets preserve
 * insertion order: golden tests and the self-hosted compiler depend on it. */
#include "voila_int.h"

#include <stdlib.h>
#include <string.h>

static Value mapobj(VlMap *m) {
  Value v;
  v.t = VL_OBJ;
  v.u.o = &m->hdr;
  return v;
}

static VlMap *new_map(bool is_set) {
  VlMap *m = (VlMap *)vl_obj_new(is_set ? O_SET : O_MAP, sizeof(VlMap));
  m->is_set = is_set;
  m->capents = 8;
  m->ents = (VlEntry *)vl_alloc(sizeof(VlEntry) * (size_t)m->capents);
  m->nidx = 16;
  m->idx = (int64_t *)vl_alloc(sizeof(int64_t) * (size_t)m->nidx);
  for (int64_t i = 0; i < m->nidx; i++) m->idx[i] = -1;
  return m;
}

Value vl_map_new(void) { return mapobj(new_map(false)); }
Value vl_set_new(void) { return mapobj(new_map(true)); }

static void rehash(VlMap *m) {
  m->nidx *= 2;
  free(m->idx);
  m->idx = (int64_t *)vl_alloc(sizeof(int64_t) * (size_t)m->nidx);
  for (int64_t i = 0; i < m->nidx; i++) m->idx[i] = -1;
  for (int64_t i = 0; i < m->nents; i++) {
    if (!m->ents[i].used) continue;
    int64_t s = (int64_t)(m->ents[i].hash & (uint64_t)(m->nidx - 1));
    while (m->idx[s] != -1) s = (s + 1) & (m->nidx - 1);
    m->idx[s] = i;
  }
}

static int64_t find_slot(VlMap *m, Value k, uint64_t h, int64_t *entry) {
  int64_t s = (int64_t)(h & (uint64_t)(m->nidx - 1));
  while (m->idx[s] != -1) {
    int64_t e = m->idx[s];
    if (m->ents[e].used && m->ents[e].hash == h && vl_equal(m->ents[e].k, k)) {
      *entry = e;
      return s;
    }
    s = (s + 1) & (m->nidx - 1);
  }
  *entry = -1;
  return s;
}

void vl_map_set(Value mv, Value k, Value owned_v) {
  VlMap *m = (VlMap *)mv.u.o;
  uint64_t h = vl_hash(k);
  int64_t entry;
  int64_t slot = find_slot(m, k, h, &entry);
  if (entry >= 0) {
    if (m->is_set) {
      vl_release(owned_v);
      return;
    }
    vl_set(&m->ents[entry].v, owned_v);
    return;
  }
  if (m->nents == m->capents) {
    m->capents *= 2;
    m->ents = (VlEntry *)realloc(m->ents, sizeof(VlEntry) * (size_t)m->capents);
    if (!m->ents) vl_abort("out of memory");
  }
  int64_t e = m->nents++;
  m->ents[e].k = vl_retain(k);
  m->ents[e].v = m->is_set ? vl_nil() : owned_v;
  if (m->is_set) vl_release(owned_v);
  m->ents[e].hash = h;
  m->ents[e].used = true;
  m->idx[slot] = e;
  m->count++;
  if (m->count * 2 > m->nidx) rehash(m);
}

bool vl_map_get(Value mv, Value k, Value *out) {
  VlMap *m = (VlMap *)mv.u.o;
  int64_t entry;
  find_slot(m, k, vl_hash(k), &entry);
  if (entry < 0) return false;
  *out = vl_retain(m->ents[entry].v);
  return true;
}

bool vl_map_has(Value mv, Value k) {
  VlMap *m = (VlMap *)mv.u.o;
  int64_t entry;
  find_slot(m, k, vl_hash(k), &entry);
  return entry >= 0;
}

void vl_map_delete(Value mv, Value k) {
  VlMap *m = (VlMap *)mv.u.o;
  uint64_t h = vl_hash(k);
  int64_t entry;
  find_slot(m, k, h, &entry);
  if (entry < 0) return;
  vl_release(m->ents[entry].k);
  if (!m->is_set) vl_release(m->ents[entry].v);
  m->ents[entry].used = false;
  m->ents[entry].k = vl_nil();
  m->ents[entry].v = vl_nil();
  m->count--;
  /* Rebuild the index (deletions are rare; keeps probing correct). */
  for (int64_t i = 0; i < m->nidx; i++) m->idx[i] = -1;
  for (int64_t i = 0; i < m->nents; i++) {
    if (!m->ents[i].used) continue;
    int64_t s = (int64_t)(m->ents[i].hash & (uint64_t)(m->nidx - 1));
    while (m->idx[s] != -1) s = (s + 1) & (m->nidx - 1);
    m->idx[s] = i;
  }
}

int64_t vl_map_count(Value mv) { return ((VlMap *)mv.u.o)->count; }

Value vl_map_keys(Value mv) {
  VlMap *m = (VlMap *)mv.u.o;
  Value out = vl_slice_new(0);
  for (int64_t i = 0; i < m->nents; i++)
    if (m->ents[i].used) vl_slice_append(out, vl_retain(m->ents[i].k));
  return out;
}

Value vl_map_values(Value mv) {
  VlMap *m = (VlMap *)mv.u.o;
  Value out = vl_slice_new(0);
  for (int64_t i = 0; i < m->nents; i++)
    if (m->ents[i].used) vl_slice_append(out, vl_retain(m->ents[i].v));
  return out;
}

/* ---------------------------------------------------------------- length */

int64_t vl_len(Value v) {
  if (v.t == VL_NIL) return 0;
  if (v.t != VL_OBJ) vl_abort("len() of %s", vl_kind_name(v));
  switch ((VlKind)v.u.o->kind) {
  case O_STR: return ((VlStr *)v.u.o)->len;
  case O_SLICE: return ((VlSlice *)v.u.o)->len;
  case O_MAP:
  case O_SET: return ((VlMap *)v.u.o)->count;
  case O_TUPLE: return ((VlTuple *)v.u.o)->n;
  case O_CHAN: return ((VlChan *)v.u.o)->count;
  case O_BUILDER: return vl_builder_len(v);
  default: vl_abort("len() of %s", vl_kind_name(v));
  }
  return 0;
}

/* ---------------------------------------------------------------- index */

static int64_t want_int(Value v, const char *what) {
  if (v.t == VL_INT) return v.u.i;
  if (v.t == VL_RUNE) return (int64_t)v.u.r;
  vl_abort("%s must be an integer, got %s", what, vl_kind_name(v));
  return 0;
}

Value vl_index(Value coll, Value idx) {
  if (coll.t == VL_OBJ && coll.u.o->kind == O_SHARED)
    return vl_index(((VlShared *)coll.u.o)->v, idx);
  if (coll.t != VL_OBJ) vl_abort("cannot index %s", vl_kind_name(coll));
  switch ((VlKind)coll.u.o->kind) {
  case O_SLICE: {
    VlSlice *s = (VlSlice *)coll.u.o;
    int64_t i = want_int(idx, "index");
    if (i < 0 || i >= s->len)
      vl_abort("index out of bounds: %lld (len %lld)", (long long)i, (long long)s->len);
    return vl_retain(s->e[i]);
  }
  case O_STR: {
    VlStr *s = (VlStr *)coll.u.o;
    int64_t i = want_int(idx, "index");
    if (i < 0 || i >= s->len)
      vl_abort("string index out of bounds: %lld (len %lld)", (long long)i, (long long)s->len);
    return vl_int((unsigned char)s->b[i]);
  }
  case O_MAP: {
    Value out;
    if (!vl_map_get(coll, idx, &out)) return vl_nil();
    return out;
  }
  case O_TUPLE: {
    VlTuple *t = (VlTuple *)coll.u.o;
    int64_t i = want_int(idx, "index");
    if (i < 0 || i >= t->n) vl_abort("tuple index out of bounds: %lld", (long long)i);
    return vl_retain(t->e[i]);
  }
  default:
    vl_abort("cannot index %s", vl_kind_name(coll));
  }
  return vl_nil();
}

void vl_setidx(Value coll, Value idx, Value v) {
  if (coll.t != VL_OBJ) vl_abort("cannot index-assign %s", vl_kind_name(coll));
  switch ((VlKind)coll.u.o->kind) {
  case O_SLICE: {
    VlSlice *s = (VlSlice *)coll.u.o;
    int64_t i = want_int(idx, "index");
    if (i < 0 || i >= s->len)
      vl_abort("index out of bounds: %lld (len %lld)", (long long)i, (long long)s->len);
    vl_set(&s->e[i], v);
    return;
  }
  case O_MAP:
    vl_map_set(coll, idx, v);
    return;
  default:
    vl_abort("cannot index-assign %s", vl_kind_name(coll));
  }
}

/* Slicing a range THROWS RangeError (§13.2), unlike scalar indexing. */
Value vl_slice_range(Value coll, Value rv) {
  if (rv.t != VL_OBJ || rv.u.o->kind != O_RANGE)
    vl_abort("slice index must be a range");
  VlRange *r = (VlRange *)rv.u.o;
  int64_t lo = r->lo, hi = r->hi;
  if (r->inclusive) hi++;
  int64_t n = vl_len(coll);
  if (lo < 0 || hi < lo || hi > n)
    vl_throwf("RangeError", "slice bounds [%lld..%lld) out of range (len %lld)",
              (long long)lo, (long long)hi, (long long)n);
  if (coll.t == VL_OBJ && coll.u.o->kind == O_STR) {
    VlStr *s = (VlStr *)coll.u.o;
    return vl_str_n(s->b + lo, hi - lo);
  }
  if (coll.t == VL_OBJ && coll.u.o->kind == O_SLICE) {
    VlSlice *s = (VlSlice *)coll.u.o;
    Value out = vl_slice_new(0);
    for (int64_t i = lo; i < hi; i++) vl_slice_append(out, vl_retain(s->e[i]));
    return out;
  }
  vl_abort("cannot slice %s", vl_kind_name(coll));
  return vl_nil();
}

/* ---------------------------------------------------------------- in */

Value vl_cmpin(Value x, Value coll) {
  if (coll.t != VL_OBJ) vl_abort("`in` not supported on %s", vl_kind_name(coll));
  switch ((VlKind)coll.u.o->kind) {
  case O_SLICE: {
    VlSlice *s = (VlSlice *)coll.u.o;
    for (int64_t i = 0; i < s->len; i++)
      if (vl_equal(x, s->e[i])) return vl_bool(true);
    return vl_bool(false);
  }
  case O_MAP:
  case O_SET:
    return vl_bool(vl_map_has(coll, x));
  case O_STR: {
    Value xs = vl_tostr(x);
    VlStr *hay = (VlStr *)coll.u.o;
    bool found = strstr(hay->b, vl_cstr(xs)) != NULL;
    vl_release(xs);
    return vl_bool(found);
  }
  case O_RANGE: {
    VlRange *r = (VlRange *)coll.u.o;
    if (x.t != VL_INT) return vl_bool(false);
    int64_t v = x.u.i;
    return vl_bool(r->inclusive ? (v >= r->lo && v <= r->hi)
                                : (v >= r->lo && v < r->hi));
  }
  default:
    vl_abort("`in` not supported on %s", vl_kind_name(coll));
  }
  return vl_bool(false);
}

/* ---------------------------------------------------------------- codegen */

/* Helpers the C backend emits calls to. */
int64_t vl_int_of(Value v) { return want_int(v, "value"); }

Value *vl_slice_data(Value s) { return ((VlSlice *)s.u.o)->e; }

void vl_spread(Value dst, Value src) {
  if (src.t != VL_OBJ || src.u.o->kind != O_SLICE)
    vl_abort("cannot spread %s (want slice)", vl_kind_name(src));
  VlSlice *s = (VlSlice *)src.u.o;
  for (int64_t i = 0; i < s->len; i++) vl_slice_append(dst, vl_retain(s->e[i]));
}

/* ---------------------------------------------------------------- iterate */

Value vl_iter(Value src) {
  VlIter *it = (VlIter *)vl_obj_new(O_ITER, sizeof(VlIter));
  it->src = vl_retain(src);
  it->i = 0;
  it->ch = vl_nil();
  Value v;
  v.t = VL_OBJ;
  v.u.o = &it->hdr;
  return v;
}

bool vl_iter_next(Value itv, Value *key, Value *val) {
  VlIter *it = (VlIter *)itv.u.o;
  Value src = it->src;
  if (src.t == VL_NIL) return false;
  if (src.t != VL_OBJ) vl_abort("cannot iterate over %s", vl_kind_name(src));
  switch ((VlKind)src.u.o->kind) {
  case O_RANGE: {
    VlRange *r = (VlRange *)src.u.o;
    if (r->by == 0) vl_abort("range step cannot be 0");
    int64_t n = r->lo + it->i * r->by;
    bool more = r->by > 0 ? (n < r->hi || (r->inclusive && n == r->hi))
                          : (n > r->hi || (r->inclusive && n == r->hi));
    if (!more) return false;
    *key = vl_int(it->i);
    *val = vl_int(n);
    it->i++;
    return true;
  }
  case O_SLICE: {
    VlSlice *s = (VlSlice *)src.u.o;
    if (it->i >= s->len) return false;
    *key = vl_int(it->i);
    *val = vl_retain(s->e[it->i]);
    it->i++;
    return true;
  }
  case O_TUPLE: {
    VlTuple *t = (VlTuple *)src.u.o;
    if (it->i >= t->n) return false;
    *key = vl_int(it->i);
    *val = vl_retain(t->e[it->i]);
    it->i++;
    return true;
  }
  case O_MAP: {
    VlMap *m = (VlMap *)src.u.o;
    while (it->i < m->nents && !m->ents[it->i].used) it->i++;
    if (it->i >= m->nents) return false;
    *key = vl_retain(m->ents[it->i].k);
    *val = vl_retain(m->ents[it->i].v);
    it->i++;
    return true;
  }
  case O_SET: {
    VlMap *m = (VlMap *)src.u.o;
    int64_t seen = 0;
    while (it->i < m->nents && !m->ents[it->i].used) it->i++;
    if (it->i >= m->nents) return false;
    seen = it->i;
    *key = vl_int(it->i);
    *val = vl_retain(m->ents[seen].k);
    it->i++;
    return true;
  }
  case O_STR: {
    VlStr *s = (VlStr *)src.u.o;
    if (it->i >= s->len) return false;
    /* decode one UTF-8 rune */
    const unsigned char *p = (const unsigned char *)s->b + it->i;
    uint32_t r = *p;
    int n = 1;
    if (r >= 0xF0) { r = ((r & 7u) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F); n = 4; }
    else if (r >= 0xE0) { r = ((r & 15u) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F); n = 3; }
    else if (r >= 0xC0) { r = ((r & 31u) << 6) | (p[1] & 0x3F); n = 2; }
    *key = vl_int(it->i);
    *val = vl_rune(r);
    it->i += n;
    return true;
  }
  case O_CHAN: {
    bool ok = false;
    Value v = vl_chan_iter_next(src, &ok);
    if (!ok) return false;
    *key = vl_nil();
    *val = v;
    return true;
  }
  default:
    vl_abort("cannot iterate over %s", vl_kind_name(src));
  }
  return false;
}
