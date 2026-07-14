package interp

import (
	"math"

	"voila/internal/dec"
	"voila/internal/diag"
	"voila/internal/lexer"
)

func bi(name string, fn func(t *T, args []Value) Value) *Builtin {
	return &Builtin{Name: name, Fn: fn}
}

// stripTypeArg removes a leading TypeV injected by generic instantiation
// (cell[int](0) etc.), returning it separately.
func stripTypeArg(args []Value) (*TypeV, []Value) {
	if len(args) > 0 {
		if tv, ok := args[0].(*TypeV); ok {
			return tv, args[1:]
		}
	}
	return nil, args
}

// buildPrelude defines the names visible without any `use` (§13, §13.3).
func buildPrelude() map[string]Value {
	p := map[string]Value{}
	add := func(b *Builtin) { p[b.Name] = b }

	add(bi("len", func(t *T, args []Value) Value {
		t.wantArgs("len", args, 1, diag.Pos{})
		switch x := args[0].(type) {
		case StrV:
			return IntV(len(x)) // bytes (§13.2)
		case *Slice:
			return IntV(len(x.Elems))
		case *Map:
			return IntV(x.Len())
		case *Set:
			return IntV(x.Len())
		case *Tuple:
			return IntV(len(x.Elems))
		case *Chan:
			return IntV(len(x.ch))
		case NilV:
			return IntV(0)
		}
		t.rtErr(diag.Pos{}, "len() of %s", typeName(args[0]))
		return IntV(0)
	}))

	add(bi("append", func(t *T, args []Value) Value {
		if len(args) < 1 {
			t.rtErr(diag.Pos{}, "append needs a slice")
		}
		s, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(diag.Pos{}, "append needs a slice, got %s", typeName(args[0]))
		}
		s.Elems = append(s.Elems, args[1:]...)
		return s
	}))

	add(bi("close", func(t *T, args []Value) Value {
		t.wantArgs("close", args, 1, diag.Pos{})
		ch, ok := args[0].(*Chan)
		if !ok {
			t.rtErr(diag.Pos{}, "close needs a channel")
		}
		t.closeChan(ch, diag.Pos{})
		return UnitV{}
	}))

	add(bi("err", func(t *T, args []Value) Value {
		msg := ""
		if len(args) > 0 {
			msg = t.Str(args[0])
		}
		return &ErrVal{Msg: msg, Trace: t.captureTrace()}
	}))

	add(bi("errf", func(t *T, args []Value) Value {
		if len(args) == 0 {
			return &ErrVal{Msg: ""}
		}
		f, ok := args[0].(StrV)
		if !ok {
			t.rtErr(diag.Pos{}, "errf needs a format string")
		}
		return &ErrVal{Msg: t.sprintfVerbs(string(f), args[1:], diag.Pos{}), Trace: t.captureTrace()}
	}))

	add(bi("assert", func(t *T, args []Value) Value {
		if len(args) < 1 {
			t.rtErr(diag.Pos{}, "assert needs a condition")
		}
		b, ok := args[0].(BoolV)
		if !ok {
			t.rtErr(diag.Pos{}, "assert condition must be bool")
		}
		if !bool(b) {
			msg := "assertion failed"
			if len(args) > 1 {
				msg = "assertion failed: " + t.Str(args[1])
			}
			t.abort(diag.Pos{}, "%s", msg)
		}
		return UnitV{}
	}))

	add(bi("min", func(t *T, args []Value) Value { return t.minmax(args, true) }))
	add(bi("max", func(t *T, args []Value) Value { return t.minmax(args, false) }))

	add(bi("abs", func(t *T, args []Value) Value {
		t.wantArgs("abs", args, 1, diag.Pos{})
		switch x := args[0].(type) {
		case IntV:
			if x < 0 {
				return IntV(-x)
			}
			return x
		case FloatV:
			return FloatV(math.Abs(float64(x)))
		case DecV:
			return DecV{x.D.Abs()}
		}
		t.rtErr(diag.Pos{}, "abs() of %s", typeName(args[0]))
		return NilV{}
	}))

	add(bi("sum", func(t *T, args []Value) Value {
		t.wantArgs("sum", args, 1, diag.Pos{})
		s, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(diag.Pos{}, "sum() needs a slice")
		}
		var acc Value = IntV(0)
		for _, e := range s.Elems {
			acc = t.binaryOp(lexer.PLUS, acc, e, diag.Pos{})
		}
		return acc
	}))

	add(bi("avg", func(t *T, args []Value) Value {
		t.wantArgs("avg", args, 1, diag.Pos{})
		s, ok := args[0].(*Slice)
		if !ok || len(s.Elems) == 0 {
			t.rtErr(diag.Pos{}, "avg() needs a non-empty slice")
		}
		var acc Value = IntV(0)
		for _, e := range s.Elems {
			acc = t.binaryOp(lexer.PLUS, acc, e, diag.Pos{})
		}
		return t.binaryOp(lexer.SLASH, acc, IntV(len(s.Elems)), diag.Pos{})
	}))

	add(bi("round", func(t *T, args []Value) Value { return t.rounder(args, "round") }))
	add(bi("floor", func(t *T, args []Value) Value { return t.rounder(args, "floor") }))
	add(bi("ceil", func(t *T, args []Value) Value { return t.rounder(args, "ceil") }))
	add(bi("trunc", func(t *T, args []Value) Value { return t.rounder(args, "trunc") }))

	add(bi("round_to", func(t *T, args []Value) Value {
		t.wantArgs("round_to", args, 2, diag.Pos{})
		places := int(t.toInt(args[1], diag.Pos{}))
		switch x := args[0].(type) {
		case FloatV:
			shift := math.Pow(10, float64(places))
			return FloatV(math.Round(float64(x)*shift) / shift)
		case DecV:
			return DecV{x.D.RoundPlaces(places)}
		case IntV:
			return x
		}
		t.rtErr(diag.Pos{}, "round_to() of %s", typeName(args[0]))
		return NilV{}
	}))

	// Safe conversions: to_int(x) → ?int etc. (§3.2).
	for _, name := range []string{"int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "byte", "float", "dec", "rune"} {
		target := name
		add(bi("to_"+name, func(t *T, args []Value) Value {
			return t.safeConvert(target, args)
		}))
	}

	// Wrapping / saturating conversions (§3.2).
	for _, name := range []string{"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"} {
		target := name
		add(bi("wrap_"+target, func(t *T, args []Value) Value {
			t.wantArgs("wrap_"+target, args, 1, diag.Pos{})
			return IntV(wrapInt(t.rawInt(args[0]), target))
		}))
		add(bi("sat_"+target, func(t *T, args []Value) Value {
			t.wantArgs("sat_"+target, args, 1, diag.Pos{})
			rng := intRanges[target]
			n := t.rawInt(args[0])
			if n < rng[0] {
				n = rng[0]
			}
			if n > rng[1] {
				n = rng[1]
			}
			return IntV(n)
		}))
	}

	add(bi("wrap_add", func(t *T, args []Value) Value {
		t.wantArgs("wrap_add", args, 2, diag.Pos{})
		return IntV(t.rawInt(args[0]) + t.rawInt(args[1]))
	}))
	add(bi("wrap_mul", func(t *T, args []Value) Value {
		t.wantArgs("wrap_mul", args, 2, diag.Pos{})
		return IntV(t.rawInt(args[0]) * t.rawInt(args[1]))
	}))

	add(bi("range", func(t *T, args []Value) Value {
		switch len(args) {
		case 1:
			return &RangeVal{Lo: 0, Hi: t.toInt(args[0], diag.Pos{}), By: 1}
		case 2:
			return &RangeVal{Lo: t.toInt(args[0], diag.Pos{}), Hi: t.toInt(args[1], diag.Pos{}), By: 1}
		case 3:
			return &RangeVal{Lo: t.toInt(args[0], diag.Pos{}), Hi: t.toInt(args[1], diag.Pos{}), By: t.toInt(args[2], diag.Pos{})}
		}
		t.rtErr(diag.Pos{}, "range() takes 1-3 arguments")
		return NilV{}
	}))

	add(bi("zip", func(t *T, args []Value) Value {
		t.wantArgs("zip", args, 2, diag.Pos{})
		a, ok1 := args[0].(*Slice)
		b, ok2 := args[1].(*Slice)
		if !ok1 || !ok2 {
			t.rtErr(diag.Pos{}, "zip() needs two slices")
		}
		n := len(a.Elems)
		if len(b.Elems) < n {
			n = len(b.Elems)
		}
		out := &Slice{}
		for i := 0; i < n; i++ {
			out.Elems = append(out.Elems, &Tuple{Elems: []Value{a.Elems[i], b.Elems[i]}})
		}
		return out
	}))

	add(bi("enumerate", func(t *T, args []Value) Value {
		t.wantArgs("enumerate", args, 1, diag.Pos{})
		s, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(diag.Pos{}, "enumerate() needs a slice")
		}
		out := &Slice{}
		for i, e := range s.Elems {
			out.Elems = append(out.Elems, &Tuple{Elems: []Value{IntV(i), e}})
		}
		return out
	}))

	add(bi("keys", func(t *T, args []Value) Value {
		t.wantArgs("keys", args, 1, diag.Pos{})
		m, ok := args[0].(*Map)
		if !ok {
			t.rtErr(diag.Pos{}, "keys() needs a map")
		}
		return &Slice{Elems: append([]Value{}, m.Keys()...)}
	}))

	add(bi("values", func(t *T, args []Value) Value {
		t.wantArgs("values", args, 1, diag.Pos{})
		m, ok := args[0].(*Map)
		if !ok {
			t.rtErr(diag.Pos{}, "values() needs a map")
		}
		var vals []Value
		for _, k := range m.Keys() {
			v, _ := m.Get(k)
			vals = append(vals, v)
		}
		return &Slice{Elems: vals}
	}))

	add(bi("shared", func(t *T, args []Value) Value {
		_, rest := stripTypeArg(args)
		t.wantArgs("shared", rest, 1, diag.Pos{})
		return &SharedBox{V: rest[0]}
	}))

	add(bi("cell", func(t *T, args []Value) Value {
		_, rest := stripTypeArg(args)
		t.wantArgs("cell", rest, 1, diag.Pos{})
		return &CellBox{V: rest[0]}
	}))

	add(bi("weak", func(t *T, args []Value) Value {
		_, rest := stripTypeArg(args)
		t.wantArgs("weak", rest, 1, diag.Pos{})
		if sb, ok := rest[0].(*SharedBox); ok {
			return &Weak{Target: sb}
		}
		t.rtErr(diag.Pos{}, "weak() needs a shared value")
		return NilV{}
	}))

	add(bi("set", func(t *T, args []Value) Value {
		s := NewSet()
		for _, a := range args {
			if sl, ok := a.(*Slice); ok && len(args) == 1 {
				for _, e := range sl.Elems {
					s.Add(e)
				}
				return s
			}
			s.Add(a)
		}
		return s
	}))

	// printf/sprintf are prelude-visible (§13).
	add(bi("printf", func(t *T, args []Value) Value { return t.fmtPrintf(t.in.Stdout, args) }))
	add(bi("sprintf", func(t *T, args []Value) Value { return t.fmtSprintfV(args) }))

	// Bare str helpers used without imports in the spec (§14.2).
	for _, name := range []string{"trim", "pad", "pad_left", "center", "upper", "lower", "split", "join", "contains", "replace", "words", "word", "fields", "lines", "runes", "substr", "left", "right", "repeat", "reverse", "starts_with", "ends_with", "rune_len", "is_digit", "is_alpha", "is_space", "is_upper", "translate", "hex", "unhex", "b64", "unb64", "bytes", "from_bytes", "from_runes", "title", "index"} {
		fname := name
		if fn, ok := strFuncs[fname]; ok {
			f := fn
			add(bi(fname, func(t *T, args []Value) Value { return f(t, args, diag.Pos{}) }))
		}
	}

	return p
}

func (t *T) minmax(args []Value, isMin bool) Value {
	vals := args
	if len(args) == 1 {
		if s, ok := args[0].(*Slice); ok {
			vals = s.Elems
		}
	}
	if len(vals) == 0 {
		t.rtErr(diag.Pos{}, "min/max of nothing")
	}
	best := vals[0]
	for _, v := range vals[1:] {
		if bool(t.binaryOp(lexer.LT, v, best, diag.Pos{}).(BoolV)) == isMin {
			best = v
		}
	}
	return best
}

func (t *T) rounder(args []Value, mode string) Value {
	t.wantArgs(mode, args, 1, diag.Pos{})
	switch x := args[0].(type) {
	case IntV:
		return x
	case FloatV:
		f := float64(x)
		var r float64
		switch mode {
		case "round":
			r = math.RoundToEven(f)
		case "floor":
			r = math.Floor(f)
		case "ceil":
			r = math.Ceil(f)
		case "trunc":
			r = math.Trunc(f)
		}
		if r > math.MaxInt64 || r < math.MinInt64 {
			t.throwf("OverflowError", "%s(%v) out of int range", mode, f)
		}
		return IntV(int64(r))
	case DecV:
		var d *dec.Dec
		switch mode {
		case "round":
			d = x.D.RoundPlaces(0)
		case "floor":
			d = x.D.Floor()
		case "ceil":
			d = x.D.Ceil()
		case "trunc":
			d = x.D.Trunc()
		}
		n, ok := d.Int64()
		if !ok {
			t.throwf("OverflowError", "%s of dec out of int range", mode)
		}
		return IntV(n)
	}
	t.rtErr(diag.Pos{}, "%s() of %s", mode, typeName(args[0]))
	return NilV{}
}

// safeConvert backs to_int etc.: nil instead of ConvError (§3.2).
func (t *T) safeConvert(target string, args []Value) (result Value) {
	defer func() {
		if r := recover(); r != nil {
			if _, ok := r.(ctlThrow); ok {
				result = NilV{}
				return
			}
			panic(r)
		}
	}()
	return t.convert(target, args, diag.Pos{})
}

func (t *T) rawInt(v Value) int64 {
	switch x := v.(type) {
	case IntV:
		return int64(x)
	case RuneV:
		return int64(x)
	case FloatV:
		return int64(x)
	}
	t.rtErr(diag.Pos{}, "expected integer, got %s", typeName(v))
	return 0
}

func wrapInt(n int64, target string) int64 {
	switch target {
	case "i8":
		return int64(int8(n))
	case "i16":
		return int64(int16(n))
	case "i32":
		return int64(int32(n))
	case "i64":
		return n
	case "u8":
		return int64(uint8(n))
	case "u16":
		return int64(uint16(n))
	case "u32":
		return int64(uint32(n))
	case "u64":
		return int64(uint64(n)) & math.MaxInt64
	}
	return n
}
