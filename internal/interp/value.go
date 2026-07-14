package interp

import (
	"fmt"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"voila/internal/ast"
	"voila/internal/dec"
)

// Value is any Voilà runtime value. Concrete types below.
type Value interface{ valueMarker() }

type NilV struct{}
type UnitV struct{}
type BoolV bool
type IntV int64
type FloatV float64
type RuneV rune
type StrV string
type DecV struct{ D *dec.Dec }

func (NilV) valueMarker()   {}
func (UnitV) valueMarker()  {}
func (BoolV) valueMarker()  {}
func (IntV) valueMarker()   {}
func (FloatV) valueMarker() {}
func (RuneV) valueMarker()  {}
func (StrV) valueMarker()   {}
func (DecV) valueMarker()   {}

// Slice has reference semantics at the Go level; Voilà-level move/copy rules
// are enforced at binding sites.
type Slice struct{ Elems []Value }

func (*Slice) valueMarker() {}

// Map preserves insertion order for deterministic iteration and printing.
type Map struct {
	keys []Value
	m    map[any]Value
}

func NewMap() *Map { return &Map{m: map[any]Value{}} }

func (*Map) valueMarker() {}

func (m *Map) Len() int { return len(m.m) }

func (m *Map) Get(k Value) (Value, bool) {
	v, ok := m.m[mapKey(k)]
	return v, ok
}

func (m *Map) Set(k, v Value) {
	kk := mapKey(k)
	if _, exists := m.m[kk]; !exists {
		m.keys = append(m.keys, k)
	}
	m.m[kk] = v
}

func (m *Map) Delete(k Value) {
	kk := mapKey(k)
	if _, ok := m.m[kk]; !ok {
		return
	}
	delete(m.m, kk)
	for i, existing := range m.keys {
		if mapKey(existing) == kk {
			m.keys = append(m.keys[:i], m.keys[i+1:]...)
			break
		}
	}
}

func (m *Map) Keys() []Value { return m.keys }

type Set struct {
	keys []Value
	m    map[any]struct{}
}

func NewSet() *Set { return &Set{m: map[any]struct{}{}} }

func (*Set) valueMarker() {}

func (s *Set) Len() int { return len(s.m) }

func (s *Set) Has(k Value) bool {
	_, ok := s.m[mapKey(k)]
	return ok
}

func (s *Set) Add(k Value) {
	kk := mapKey(k)
	if _, ok := s.m[kk]; !ok {
		s.keys = append(s.keys, k)
		s.m[kk] = struct{}{}
	}
}

func (s *Set) Remove(k Value) {
	kk := mapKey(k)
	if _, ok := s.m[kk]; !ok {
		return
	}
	delete(s.m, kk)
	for i, existing := range s.keys {
		if mapKey(existing) == kk {
			s.keys = append(s.keys[:i], s.keys[i+1:]...)
			break
		}
	}
}

// mapKey produces a comparable Go key for a Voilà value.
func mapKey(v Value) any {
	switch x := v.(type) {
	case StrV:
		return string(x)
	case IntV:
		return int64(x)
	case FloatV:
		return float64(x)
	case BoolV:
		return bool(x)
	case RuneV:
		return "r:" + string(rune(x))
	case DecV:
		return "d:" + x.D.String()
	case NilV:
		return nil
	case *EnumVal:
		key := "e:" + x.Type + ":" + x.Variant
		for _, f := range x.Fields {
			key += ":" + fmt.Sprint(mapKey(f))
		}
		return key
	}
	return fmt.Sprintf("%p", v)
}

type Tuple struct{ Elems []Value }

func (*Tuple) valueMarker() {}

type Struct struct {
	Type   string
	Order  []string
	Fields map[string]Value
}

func (*Struct) valueMarker() {}

type EnumVal struct {
	Type    string
	Variant string
	Names   []string
	Fields  []Value
}

func (*EnumVal) valueMarker() {}

// ErrVal is both an error value (§8.1) and a thrown exception (§8.2).
type ErrVal struct {
	TypeName   string  // exception type; "" for plain err("…")
	Msg        string  // plain message for err()
	Struct     *Struct // exception fields
	Cause      *ErrVal
	Trace      []string
	Suppressed []*ErrVal
}

func (*ErrVal) valueMarker() {}

type Func struct {
	Name     string
	Params   []*ast.Param
	Body     *ast.Block
	Env      *Env  // closure environment; nil = global
	Self     Value // bound receiver
	SelfMode ast.ParamMode
	HasSelf  bool
	Decl     *ast.FuncDecl // original decl (throws/nothrow metadata)
	NoThrow  bool
	TypeArg  string // bound generic type argument (json.decode[T])
}

func (*Func) valueMarker() {}

type Builtin struct {
	Name    string
	Fn      func(t *T, args []Value) Value
	TypeArg Value // bound via generic instantiation
}

func (*Builtin) valueMarker() {}

// TypeV is a type used as a value (generic instantiation, conversions).
type TypeV struct{ Name string }

func (*TypeV) valueMarker() {}

// Pkg is an imported package reference.
type Pkg struct {
	Name  string
	Funcs map[string]Value
}

func (*Pkg) valueMarker() {}

type Chan struct {
	ch     chan Value
	cap    int
	closed bool
	mu     sync.Mutex
}

func (*Chan) valueMarker() {}

type Task struct {
	done     chan struct{}
	result   Value
	exc      *ErrVal // exception that escaped the task
	isErr    bool    // result is an ErrVal (T! propagation)
	consumed bool
	mu       sync.Mutex
}

func (*Task) valueMarker() {}

type SharedBox struct{ V Value }

func (*SharedBox) valueMarker() {}

type CellBox struct {
	mu sync.Mutex
	V  Value
}

func (*CellBox) valueMarker() {}

type Weak struct{ Target *SharedBox }

func (*Weak) valueMarker() {}

type RangeVal struct {
	Lo, Hi    int64
	Inclusive bool
	By        int64
}

func (*RangeVal) valueMarker() {}

type File struct {
	f      any // *os.File
	name   string
	reader any // *bufio.Reader
	closed bool
}

func (*File) valueMarker() {}

type Builder struct{ SB strings.Builder }

func (*Builder) valueMarker() {}

type DurV time.Duration

func (DurV) valueMarker() {}

type InstantV time.Time

func (InstantV) valueMarker() {}

type CtxV struct{ t *T }

func (*CtxV) valueMarker() {}

// ---------------------------------------------------------------- printing

// Str renders a value the way `say` and string interpolation do (§7.5). The
// interpreter consults user `Show` impls before falling back to these forms
// (see T.Str).
func Str(v Value) string {
	switch x := v.(type) {
	case nil:
		return "nil"
	case NilV:
		return "nil"
	case UnitV:
		return "()"
	case BoolV:
		if x {
			return "true"
		}
		return "false"
	case IntV:
		return strconv.FormatInt(int64(x), 10)
	case FloatV:
		return formatFloat(float64(x))
	case RuneV:
		return string(rune(x))
	case StrV:
		return string(x)
	case DecV:
		return x.D.String()
	case *Slice:
		var parts []string
		for _, e := range x.Elems {
			parts = append(parts, quoted(e))
		}
		return "[" + strings.Join(parts, ", ") + "]"
	case *Map:
		var parts []string
		for _, k := range x.keys {
			val, _ := x.Get(k)
			parts = append(parts, quoted(k)+": "+quoted(val))
		}
		return "{" + strings.Join(parts, ", ") + "}"
	case *Set:
		var parts []string
		for _, k := range x.keys {
			parts = append(parts, quoted(k))
		}
		return "{" + strings.Join(parts, ", ") + "}"
	case *Tuple:
		var parts []string
		for _, e := range x.Elems {
			parts = append(parts, quoted(e))
		}
		return "(" + strings.Join(parts, ", ") + ")"
	case *Struct:
		var parts []string
		for _, name := range x.Order {
			parts = append(parts, name+": "+quoted(x.Fields[name]))
		}
		return x.Type + "{" + strings.Join(parts, ", ") + "}"
	case *EnumVal:
		if len(x.Fields) == 0 {
			return x.Variant
		}
		var parts []string
		for _, f := range x.Fields {
			parts = append(parts, quoted(f))
		}
		return x.Variant + "(" + strings.Join(parts, ", ") + ")"
	case *ErrVal:
		return x.Message()
	case *Func:
		if x.Name != "" {
			return "<func " + x.Name + ">"
		}
		return "<closure>"
	case *Builtin:
		return "<builtin " + x.Name + ">"
	case *TypeV:
		return "<type " + x.Name + ">"
	case *Pkg:
		return "<package " + x.Name + ">"
	case *Chan:
		return "<chan>"
	case *Task:
		return "<task>"
	case *SharedBox:
		return Str(x.V)
	case *CellBox:
		x.mu.Lock()
		defer x.mu.Unlock()
		return Str(x.V)
	case *Weak:
		return "<weak>"
	case *RangeVal:
		op := ".."
		if x.Inclusive {
			op = "..="
		}
		s := fmt.Sprintf("%d%s%d", x.Lo, op, x.Hi)
		if x.By != 1 {
			s += fmt.Sprintf(" by %d", x.By)
		}
		return s
	case *File:
		return "<file " + x.name + ">"
	case *Builder:
		return "<builder>"
	case DurV:
		return time.Duration(x).String()
	case InstantV:
		return time.Time(x).Format("2006-01-02 15:04:05")
	case *CtxV:
		return "<ctx>"
	}
	return fmt.Sprintf("<%T>", v)
}

// quoted renders nested values inside containers: strings get quotes so
// `["a", "b"]` prints unambiguously.
func quoted(v Value) string {
	switch x := v.(type) {
	case StrV:
		return strconv.Quote(string(x))
	case RuneV:
		return "'" + string(rune(x)) + "'"
	default:
		return Str(x)
	}
}

func formatFloat(f float64) string {
	s := strconv.FormatFloat(f, 'g', -1, 64)
	// Keep whole floats readable but distinct from ints where it matters:
	// Voilà prints 2.0 as "2" like Go's %v for cleanliness.
	if strings.Contains(s, "e") {
		// normalize exponent form e+23 → e+23 (Go's default is fine)
		return s
	}
	return s
}

// Message renders an error's message: user message() impls are consulted at
// call sites in the interpreter; this is the derived fallback (§8.2).
func (e *ErrVal) Message() string {
	if e.TypeName == "" {
		return e.Msg
	}
	if e.Struct != nil && len(e.Struct.Order) > 0 {
		var parts []string
		for _, name := range e.Struct.Order {
			parts = append(parts, name+": "+quoted(e.Struct.Fields[name]))
		}
		return e.TypeName + "{" + strings.Join(parts, ", ") + "}"
	}
	if e.Msg != "" {
		return e.TypeName + ": " + e.Msg
	}
	return e.TypeName
}

// isCopy reports whether a value has Copy semantics (§6.4): primitives and
// structs/tuples/enums made only of Copy values.
func isCopy(v Value) bool {
	switch x := v.(type) {
	case NilV, UnitV, BoolV, IntV, FloatV, RuneV, StrV, DecV, DurV, InstantV,
		*RangeVal, *TypeV, nil:
		return true
	case *SharedBox, *Weak:
		// Handles are cheap to copy; cloning a handle is not cloning the value.
		return true
	case *Struct:
		for _, f := range x.Fields {
			if !isCopy(f) {
				return false
			}
		}
		return true
	case *Tuple:
		for _, e := range x.Elems {
			if !isCopy(e) {
				return false
			}
		}
		return true
	case *EnumVal:
		for _, f := range x.Fields {
			if !isCopy(f) {
				return false
			}
		}
		return true
	}
	return false
}

// copyValue deep-copies a Copy value (value semantics for structs, §3.4).
func copyValue(v Value) Value {
	switch x := v.(type) {
	case *Struct:
		fields := make(map[string]Value, len(x.Fields))
		for k, f := range x.Fields {
			fields[k] = copyValue(f)
		}
		return &Struct{Type: x.Type, Order: x.Order, Fields: fields}
	case *Tuple:
		elems := make([]Value, len(x.Elems))
		for i, e := range x.Elems {
			elems[i] = copyValue(e)
		}
		return &Tuple{Elems: elems}
	case *EnumVal:
		fields := make([]Value, len(x.Fields))
		for i, f := range x.Fields {
			fields[i] = copyValue(f)
		}
		return &EnumVal{Type: x.Type, Variant: x.Variant, Names: x.Names, Fields: fields}
	}
	return v
}

// sortValues sorts a []Value in natural order (used by sort builtin & tests).
func sortValues(vals []Value, less func(a, b Value) bool) {
	sort.SliceStable(vals, func(i, j int) bool { return less(vals[i], vals[j]) })
}
