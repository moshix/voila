package interp

import (
	"fmt"
	"math"
	"strconv"
	"strings"

	"voila/internal/ast"
	"voila/internal/dec"
	"voila/internal/diag"
)

func (t *T) evalCall(env *Env, x *ast.CallExpr) Value {
	// Method-call path.
	if fe, ok := x.Fun.(*ast.FieldExpr); ok {
		return t.evalMethodCall(env, fe, x)
	}

	if id, ok := x.Fun.(*ast.Ident); ok {
		switch id.Name {
		case "chan":
			capacity := int64(0)
			if len(x.Args) > 0 {
				capacity = t.toInt(t.eval(env, x.Args[0].Value), x.P)
			}
			return newChan(int(capacity))
		case "make":
			return t.evalMake(env, x)
		}
	}

	callee := t.eval(env, x.Fun)
	args, named := t.evalArgs(env, x)
	t.applyOwnMoves(env, callee, x)
	return t.callValue(callee, args, named, x.P)
}

func (t *T) evalMake(env *Env, x *ast.CallExpr) Value {
	if len(x.TypeArgs) == 0 {
		t.rtErr(x.P, "make requires a type argument")
	}
	switch ty := x.TypeArgs[0].(type) {
	case *ast.SliceType:
		n := int64(0)
		if len(x.Args) > 0 {
			n = t.toInt(t.eval(env, x.Args[0].Value), x.P)
		}
		elems := make([]Value, n)
		for i := range elems {
			elems[i] = t.zeroValue(ty.Elem)
		}
		return &Slice{Elems: elems}
	case *ast.MapType:
		return NewMap()
	case *ast.SetType:
		return NewSet()
	case *ast.ChanType:
		capacity := int64(0)
		if len(x.Args) > 0 {
			capacity = t.toInt(t.eval(env, x.Args[0].Value), x.P)
		}
		return newChan(int(capacity))
	}
	t.rtErr(x.P, "cannot make this type")
	return NilV{}
}

// evalArgs evaluates call arguments: positional, named, and spread.
func (t *T) evalArgs(env *Env, x *ast.CallExpr) ([]Value, map[string]Value) {
	var args []Value
	var named map[string]Value
	for _, a := range x.Args {
		v := t.eval(env, a.Value)
		switch {
		case a.Spread:
			sl, ok := v.(*Slice)
			if !ok {
				t.rtErr(a.Value.Pos(), "cannot spread %s (want slice)", typeName(v))
			}
			args = append(args, sl.Elems...)
		case a.Name != "":
			if named == nil {
				named = map[string]Value{}
			}
			named[a.Name] = v
		default:
			args = append(args, v)
		}
	}
	return args, named
}

// applyOwnMoves marks caller bindings moved for `own` parameters (§6.1).
func (t *T) applyOwnMoves(env *Env, callee Value, x *ast.CallExpr) {
	fn, ok := callee.(*Func)
	if !ok {
		return
	}
	for i, prm := range fn.Params {
		if prm.Mode != ast.ModeOwn || i >= len(x.Args) {
			continue
		}
		if id, ok := x.Args[i].Value.(*ast.Ident); ok {
			if b := env.Lookup(id.Name); b != nil {
				b.Moved = true
				b.MovedPos = id.P
			}
		}
	}
}

// callValue invokes any callable value.
func (t *T) callValue(callee Value, args []Value, named map[string]Value, pos diag.Pos) Value {
	switch fn := callee.(type) {
	case *Func:
		return t.callFunction(fn, args, named, pos)
	case *Builtin:
		if fn.TypeArg != nil {
			args = append([]Value{fn.TypeArg}, args...)
		}
		return fn.Fn(t, args)
	case *TypeV:
		return t.convert(fn.Name, args, pos)
	}
	t.rtErr(pos, "%s is not callable", typeName(callee))
	return NilV{}
}

// callAny is callValue for internal callers with no position.
func (t *T) callAny(callee Value, args []Value, pos diag.Pos) Value {
	return t.callValue(callee, args, nil, pos)
}

// evalMethodCall handles recv.name(args): package functions, user impls,
// built-in kind methods, and callable fields — in that order.
func (t *T) evalMethodCall(env *Env, fe *ast.FieldExpr, x *ast.CallExpr) Value {
	// Package call: fmt.printf(…).
	if id, ok := fe.X.(*ast.Ident); ok && env.Lookup(id.Name) == nil {
		if pkg, ok := t.in.packages[id.Name]; ok {
			member, ok := pkg.Funcs[fe.Name]
			if !ok {
				t.rtErr(fe.P, "package %s has no function `%s`", id.Name, fe.Name)
			}
			args, named := t.evalArgs(env, x)
			return t.callValue(member, args, named, x.P)
		}
	}

	recv := t.eval(env, fe.X)
	args, named := t.evalArgs(env, x)
	return t.dispatchMethod(env, recv, fe, args, named, x)
}

func (t *T) dispatchMethod(env *Env, recv Value, fe *ast.FieldExpr, args []Value, named map[string]Value, x *ast.CallExpr) Value {
	name := fe.Name

	// User impl methods on nominal types.
	if tn := nominalType(recv); tn != "" {
		if decl, ok := t.in.Impls[tn][name]; ok {
			self := recv
			if sb, ok := recv.(*SharedBox); ok {
				if decl.SelfMode == ast.ModeMut {
					t.rtErr(x.P, "cannot call mut method `%s` through shared[%s] (§6.3: shared is immutable)", name, tn)
				}
				self = sb.V
			}
			fn := &Func{
				Name: tn + "." + name, Params: decl.Params, Body: decl.Body,
				Self: self, HasSelf: decl.HasSelf, SelfMode: decl.SelfMode, Decl: decl,
				NoThrow: decl.NoThrow,
			}
			if decl.SelfMode == ast.ModeOwn {
				if id, ok := fe.X.(*ast.Ident); ok {
					if b := env.Lookup(id.Name); b != nil {
						b.Moved = true
						b.MovedPos = id.P
					}
				}
			}
			return t.callFunction(fn, args, named, x.P)
		}
		// Static/associated function: Point.origin() — via TypeV receiver.
	}

	if tv, ok := recv.(*TypeV); ok {
		if decl, ok := t.in.Impls[tv.Name][name]; ok && !decl.HasSelf {
			fn := &Func{Name: tv.Name + "." + name, Params: decl.Params, Body: decl.Body, Decl: decl}
			return t.callFunction(fn, args, named, x.P)
		}
	}

	// Built-in kind methods.
	if v, handled := t.kindMethod(recv, name, args, x.P); handled {
		return v
	}

	// A struct field holding a callable.
	if s, ok := recv.(*Struct); ok {
		if f, ok := s.Fields[name]; ok {
			return t.callValue(f, args, named, x.P)
		}
	}

	t.rtErr(x.P, "%s has no method `%s`", typeName(recv), name)
	return NilV{}
}

// nominalType returns the user-type name for impl lookup.
func nominalType(v Value) string {
	switch x := v.(type) {
	case *Struct:
		return x.Type
	case *EnumVal:
		return x.Type
	case *ErrVal:
		return x.TypeName
	case *SharedBox:
		return nominalType(x.V)
	}
	return ""
}

// ---------------------------------------------------------------- conversions

var intRanges = map[string][2]int64{
	"i8":   {math.MinInt8, math.MaxInt8},
	"i16":  {math.MinInt16, math.MaxInt16},
	"i32":  {math.MinInt32, math.MaxInt32},
	"i64":  {math.MinInt64, math.MaxInt64},
	"int":  {math.MinInt64, math.MaxInt64},
	"u8":   {0, math.MaxUint8},
	"u16":  {0, math.MaxUint16},
	"u32":  {0, math.MaxUint32},
	"u64":  {0, math.MaxInt64}, // u64 stored as int64; values above 2^63-1 rejected
	"byte": {0, math.MaxUint8},
}

// convert implements the checked conversions of §3.2: int(x), float(x),
// dec(x), str(x), sized ints — throwing ConvError on loss.
func (t *T) convert(name string, args []Value, pos diag.Pos) Value {
	if alias, ok := t.in.Aliases[name]; ok {
		if nt, ok := alias.(*ast.NamedType); ok {
			name = nt.Name
		}
	}
	if len(args) == 0 {
		t.rtErr(pos, "%s() needs an argument", name)
	}
	v := args[0]

	if rng, ok := intRanges[name]; ok {
		n := t.convToInt(v, args, name, pos)
		if n < rng[0] || n > rng[1] {
			t.throwConv(v, name, "%d does not fit in %s", n, name)
		}
		return IntV(n)
	}

	switch name {
	case "float", "f32":
		switch x := v.(type) {
		case IntV:
			return FloatV(float64(x)) // explicit conversion allows precision loss
		case FloatV:
			return x
		case DecV:
			return FloatV(x.D.Float64())
		case StrV:
			f, err := strconv.ParseFloat(strings.TrimSpace(string(x)), 64)
			if err != nil {
				t.throwConv(v, "float", "cannot read %q as float", string(x))
			}
			return FloatV(f)
		case RuneV:
			return FloatV(float64(x))
		}
		t.throwConv(v, "float", "cannot convert %s to float", typeName(v))
	case "dec":
		switch x := v.(type) {
		case DecV:
			return x
		case IntV:
			return DecV{dec.FromInt64(int64(x))}
		case StrV:
			d, err := dec.Parse(strings.TrimSpace(string(x)))
			if err != nil {
				t.throwConv(v, "dec", "cannot read %q as dec", string(x))
			}
			return DecV{d}
		case FloatV:
			d, err := dec.FromFloat(float64(x))
			if err != nil {
				t.throwConv(v, "dec", "cannot convert %v to dec", float64(x))
			}
			return DecV{d}
		}
		t.throwConv(v, "dec", "cannot convert %s to dec", typeName(v))
	case "str":
		return StrV(t.Str(v))
	case "rune":
		switch x := v.(type) {
		case RuneV:
			return x
		case IntV:
			return RuneV(rune(x))
		case StrV:
			r := []rune(string(x))
			if len(r) != 1 {
				t.throwConv(v, "rune", "string %q is not a single rune", string(x))
			}
			return RuneV(r[0])
		}
		t.throwConv(v, "rune", "cannot convert %s to rune", typeName(v))
	case "bool":
		if b, ok := v.(BoolV); ok {
			return b
		}
		t.throwConv(v, "bool", "cannot convert %s to bool (no implicit truthiness)", typeName(v))
	}
	t.rtErr(pos, "unknown conversion %s()", name)
	return NilV{}
}

func (t *T) convToInt(v Value, args []Value, target string, pos diag.Pos) int64 {
	switch x := v.(type) {
	case IntV:
		return int64(x)
	case RuneV:
		return int64(x)
	case FloatV:
		f := float64(x)
		if math.IsNaN(f) || math.IsInf(f, 0) || f >= math.MaxInt64 || f <= math.MinInt64 {
			t.throwConv(v, target, "float %v out of int range", f)
		}
		return int64(math.Trunc(f)) // documented truncation toward zero (§3.2)
	case DecV:
		n, ok := x.D.Trunc().Int64()
		if !ok {
			t.throwConv(v, target, "dec %s out of int range", x.D)
		}
		return n
	case StrV:
		s := strings.TrimSpace(string(x))
		base := 10
		if len(args) > 1 {
			base = int(t.toInt(args[1], pos))
		}
		n, err := strconv.ParseInt(s, base, 64)
		if err != nil {
			t.throwConv(v, target, "cannot read %q as %s", string(x), target)
		}
		return n
	case BoolV:
		t.throwConv(v, target, "cannot convert bool to %s", target)
	}
	t.throwConv(v, target, "cannot convert %s to %s", typeName(v), target)
	return 0
}

// throwConv raises ConvError with the §14.4 field shape {from, to, value}.
func (t *T) throwConv(v Value, to string, format string, args ...any) {
	s := &Struct{Type: "ConvError", Order: []string{"from", "to", "value"}, Fields: map[string]Value{
		"from":  StrV(typeName(v)),
		"to":    StrV(to),
		"value": StrV(Str(v)),
	}}
	e := &ErrVal{TypeName: "ConvError", Struct: s, Trace: t.captureTrace()}
	if format != "" {
		e.Msg = fmt.Sprintf(format, args...)
	}
	panic(ctlThrow{e})
}
