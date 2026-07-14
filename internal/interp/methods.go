package interp

import (
	"time"

	"voila/internal/diag"
	"voila/internal/lexer"
)

// kindMethod dispatches built-in methods by value kind. Returns handled=false
// when the receiver kind has no such method, letting the caller report.
func (t *T) kindMethod(recv Value, name string, args []Value, pos diag.Pos) (Value, bool) {
	// Universal methods.
	switch name {
	case "str":
		if len(args) == 0 {
			return StrV(t.Str(recv)), true
		}
	case "clone":
		return deepClone(recv), true
	}

	switch r := recv.(type) {
	case StrV:
		if fn, ok := strFuncs[name]; ok {
			return fn(t, append([]Value{r}, args...), pos), true
		}
	case *Slice:
		if fn, ok := sliceMethods[name]; ok {
			return fn(t, r, args, pos), true
		}
	case *Map:
		switch name {
		case "keys":
			return &Slice{Elems: append([]Value{}, r.Keys()...)}, true
		case "values":
			var vals []Value
			for _, k := range r.Keys() {
				v, _ := r.Get(k)
				vals = append(vals, v)
			}
			return &Slice{Elems: vals}, true
		case "has":
			t.wantArgs(name, args, 1, pos)
			_, ok := r.Get(args[0])
			return BoolV(ok), true
		case "get":
			if len(args) == 2 {
				if v, ok := r.Get(args[0]); ok {
					return v, true
				}
				return args[1], true
			}
			t.wantArgs(name, args, 1, pos)
			v, ok := r.Get(args[0])
			if !ok {
				return NilV{}, true
			}
			return v, true
		case "set":
			t.wantArgs(name, args, 2, pos)
			r.Set(args[0], args[1])
			return UnitV{}, true
		case "delete", "remove":
			t.wantArgs(name, args, 1, pos)
			r.Delete(args[0])
			return UnitV{}, true
		case "len":
			return IntV(r.Len()), true
		}
	case *Set:
		switch name {
		case "add":
			t.wantArgs(name, args, 1, pos)
			r.Add(args[0])
			return UnitV{}, true
		case "remove", "delete":
			t.wantArgs(name, args, 1, pos)
			r.Remove(args[0])
			return UnitV{}, true
		case "has", "contains":
			t.wantArgs(name, args, 1, pos)
			return BoolV(r.Has(args[0])), true
		case "len":
			return IntV(r.Len()), true
		case "items":
			return &Slice{Elems: append([]Value{}, r.keys...)}, true
		}
	case *ErrVal:
		switch name {
		case "message":
			return StrV(t.errMessage(r)), true
		case "cause":
			if r.Cause == nil {
				return NilV{}, true
			}
			return r.Cause, true
		case "trace":
			var frames []Value
			for _, f := range r.Trace {
				frames = append(frames, StrV(f))
			}
			return &Slice{Elems: frames}, true
		case "suppressed":
			var sup []Value
			for _, s := range r.Suppressed {
				sup = append(sup, s)
			}
			return &Slice{Elems: sup}, true
		}
	case *Task:
		if name == "await" {
			return t.awaitTask(r, pos), true
		}
	case *CellBox:
		switch name {
		case "get":
			r.mu.Lock()
			v := r.V
			r.mu.Unlock()
			return v, true
		case "set":
			t.wantArgs(name, args, 1, pos)
			r.mu.Lock()
			r.V = args[0]
			r.mu.Unlock()
			return UnitV{}, true
		case "update":
			t.wantArgs(name, args, 1, pos)
			return t.cellUpdate(r, args[0], pos), true
		}
	case *Weak:
		if name == "upgrade" {
			if r.Target == nil {
				return NilV{}, true
			}
			return r.Target, true
		}
	case *SharedBox:
		if name == "get" {
			return r.V, true
		}
	case *Builder:
		switch name {
		case "write":
			t.wantArgs(name, args, 1, pos)
			r.SB.WriteString(t.Str(args[0]))
			return UnitV{}, true
		case "writeln":
			for _, a := range args {
				r.SB.WriteString(t.Str(a))
			}
			r.SB.WriteString("\n")
			return UnitV{}, true
		case "done":
			return StrV(r.SB.String()), true
		case "len":
			return IntV(r.SB.Len()), true
		}
	case *File:
		if v, ok := t.fileMethod(r, name, args, pos); ok {
			return v, true
		}
	case DurV:
		d := time.Duration(r)
		switch name {
		case "millis":
			return IntV(d.Milliseconds()), true
		case "seconds":
			return FloatV(d.Seconds()), true
		case "micros":
			return IntV(d.Microseconds()), true
		case "nanos":
			return IntV(d.Nanoseconds()), true
		case "minutes":
			return FloatV(d.Minutes()), true
		}
	case InstantV:
		tm := time.Time(r)
		switch name {
		case "unix":
			return IntV(tm.Unix()), true
		case "unix_millis":
			return IntV(tm.UnixMilli()), true
		case "format":
			t.wantArgs(name, args, 1, pos)
			if s, ok := args[0].(StrV); ok {
				return StrV(tm.Format(string(s))), true
			}
		case "year":
			return IntV(tm.Year()), true
		case "month":
			return IntV(int(tm.Month())), true
		case "day":
			return IntV(tm.Day()), true
		}
	case *CtxV:
		switch name {
		case "done":
			ch := newChan(0)
			go func() {
				<-r.t.ctx.Done()
				ch.mu.Lock()
				if !ch.closed {
					ch.closed = true
					close(ch.ch)
				}
				ch.mu.Unlock()
			}()
			return ch, true
		case "cancelled":
			return BoolV(r.t.ctx.Err() != nil), true
		}
	case DecV:
		switch name {
		case "round":
			if len(args) == 1 {
				places := t.toInt(args[0], pos)
				if places < 0 || places > 1000 {
					t.throwf("ValueError", "round(places) needs a places count in 0..1000")
				}
				return DecV{r.D.RoundPlaces(int(places))}, true
			}
			return DecV{r.D.RoundPlaces(0)}, true
		case "floor":
			return DecV{r.D.Floor()}, true
		case "ceil":
			return DecV{r.D.Ceil()}, true
		case "abs":
			return DecV{r.D.Abs()}, true
		case "format":
			t.wantArgs(name, args, 1, pos)
			places := t.toInt(args[0], pos)
			if places < 0 || places > 1000 {
				t.throwf("ValueError", "format(places) needs a places count in 0..1000")
			}
			return StrV(r.D.FormatFixed(int(places))), true
		}
	case *RegexV:
		if v, ok := t.regexMethod(r, name, args, pos); ok {
			return v, true
		}
	}
	return NilV{}, false
}

// cellUpdate runs fn under the cell's lock. A throw escaping the closure
// would leave the protected invariant broken — the spec requires nothrow
// (§8.4), so an escaping throw aborts.
func (t *T) cellUpdate(c *CellBox, fn Value, pos diag.Pos) Value {
	c.mu.Lock()
	defer c.mu.Unlock()
	defer func() {
		if r := recover(); r != nil {
			if thr, ok := r.(ctlThrow); ok {
				panic(ctlAbort{
					msg:   "exception escaped cell.update closure (must be nothrow, §8.4): " + thr.err.Message(),
					trace: thr.err.Trace,
				})
			}
			panic(r)
		}
	}()
	c.V = t.callAny(fn, []Value{c.V}, pos)
	return UnitV{}
}

func (t *T) wantArgs(name string, args []Value, n int, pos diag.Pos) {
	if len(args) != n {
		t.rtErr(pos, "%s expects %d argument(s), got %d", name, n, len(args))
	}
}

// deepClone implements x.clone() — an explicit real copy (§6.4).
func deepClone(v Value) Value {
	switch x := v.(type) {
	case *Slice:
		elems := make([]Value, len(x.Elems))
		for i, e := range x.Elems {
			elems[i] = deepClone(e)
		}
		return &Slice{Elems: elems}
	case *Map:
		m := NewMap()
		for _, k := range x.Keys() {
			val, _ := x.Get(k)
			m.Set(k, deepClone(val))
		}
		return m
	case *Set:
		s := NewSet()
		for _, k := range x.keys {
			s.Add(k)
		}
		return s
	case *Struct:
		fields := make(map[string]Value, len(x.Fields))
		for k, f := range x.Fields {
			fields[k] = deepClone(f)
		}
		return &Struct{Type: x.Type, Order: x.Order, Fields: fields}
	case *Tuple:
		elems := make([]Value, len(x.Elems))
		for i, e := range x.Elems {
			elems[i] = deepClone(e)
		}
		return &Tuple{Elems: elems}
	case *EnumVal:
		fields := make([]Value, len(x.Fields))
		for i, f := range x.Fields {
			fields[i] = deepClone(f)
		}
		return &EnumVal{Type: x.Type, Variant: x.Variant, Names: x.Names, Fields: fields}
	}
	return v
}

// sliceMethods: the built-in methods of []T (§13.3 #16-18 and friends).
// Populated in init() to break the initialization cycle with kindMethod.
var sliceMethods map[string]func(t *T, s *Slice, args []Value, pos diag.Pos) Value

func init() {
	sliceMethods = map[string]func(t *T, s *Slice, args []Value, pos diag.Pos) Value{
		"append": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			s.Elems = append(s.Elems, args...)
			return UnitV{}
		},
		"pop": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			if len(s.Elems) == 0 {
				return NilV{}
			}
			v := s.Elems[len(s.Elems)-1]
			s.Elems = s.Elems[:len(s.Elems)-1]
			return v
		},
		"first": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			if len(s.Elems) == 0 {
				return NilV{}
			}
			return s.Elems[0]
		},
		"last": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			if len(s.Elems) == 0 {
				return NilV{}
			}
			return s.Elems[len(s.Elems)-1]
		},
		"len": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			return IntV(len(s.Elems))
		},
		"map": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("map", args, 1, pos)
			out := &Slice{Elems: make([]Value, 0, len(s.Elems))}
			for _, e := range s.Elems {
				out.Elems = append(out.Elems, t.callAny(args[0], []Value{e}, pos))
			}
			return out
		},
		"filter": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("filter", args, 1, pos)
			out := &Slice{}
			for _, e := range s.Elems {
				if b, ok := t.callAny(args[0], []Value{e}, pos).(BoolV); ok && bool(b) {
					out.Elems = append(out.Elems, e)
				}
			}
			return out
		},
		"reduce": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("reduce", args, 2, pos)
			acc := args[1]
			for _, e := range s.Elems {
				acc = t.callAny(args[0], []Value{acc, e}, pos)
			}
			return acc
		},
		"sort": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			sortValues(s.Elems, func(a, b Value) bool { return t.lessValues(a, b, pos) })
			return UnitV{}
		},
		"sort_by": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("sort_by", args, 1, pos)
			sortValues(s.Elems, func(a, b Value) bool {
				r := t.callAny(args[0], []Value{a, b}, pos)
				if b2, ok := r.(BoolV); ok {
					return bool(b2)
				}
				t.rtErr(pos, "sort_by comparator must return bool")
				return false
			})
			return UnitV{}
		},
		"sorted": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			out := append([]Value{}, s.Elems...)
			sortValues(out, func(a, b Value) bool { return t.lessValues(a, b, pos) })
			return &Slice{Elems: out}
		},
		"contains": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("contains", args, 1, pos)
			for _, e := range s.Elems {
				if equalValues(e, args[0]) {
					return BoolV(true)
				}
			}
			return BoolV(false)
		},
		"index_of": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("index_of", args, 1, pos)
			for i, e := range s.Elems {
				if equalValues(e, args[0]) {
					return IntV(i)
				}
			}
			return IntV(-1)
		},
		"reverse": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			for i, j := 0, len(s.Elems)-1; i < j; i, j = i+1, j-1 {
				s.Elems[i], s.Elems[j] = s.Elems[j], s.Elems[i]
			}
			return UnitV{}
		},
		"join": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			sep := ""
			if len(args) > 0 {
				sep = t.Str(args[0])
			}
			var parts []string
			for _, e := range s.Elems {
				parts = append(parts, t.Str(e))
			}
			return StrV(joinStrings(parts, sep))
		},
		"each": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("each", args, 1, pos)
			for _, e := range s.Elems {
				t.callAny(args[0], []Value{e}, pos)
			}
			return UnitV{}
		},
		"any": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("any", args, 1, pos)
			for _, e := range s.Elems {
				if b, ok := t.callAny(args[0], []Value{e}, pos).(BoolV); ok && bool(b) {
					return BoolV(true)
				}
			}
			return BoolV(false)
		},
		"all": func(t *T, s *Slice, args []Value, pos diag.Pos) Value {
			t.wantArgs("all", args, 1, pos)
			for _, e := range s.Elems {
				if b, ok := t.callAny(args[0], []Value{e}, pos).(BoolV); !ok || !bool(b) {
					return BoolV(false)
				}
			}
			return BoolV(true)
		},
	}
}

// lessValues is natural ordering for sort().
func (t *T) lessValues(a, b Value, pos diag.Pos) bool {
	r := t.binaryOp(lexer.LT, a, b, pos)
	if bb, ok := r.(BoolV); ok {
		return bool(bb)
	}
	return false
}

func joinStrings(parts []string, sep string) string {
	out := ""
	for i, p := range parts {
		if i > 0 {
			out += sep
		}
		out += p
	}
	return out
}
