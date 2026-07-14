package interp

import (
	"fmt"
	"math"
	"math/rand"
	"regexp"
	"time"

	"voila/internal/diag"
)

// RegexV wraps an RE2 pattern (Go's regexp IS RE2 — linear time, §13.4).
type RegexV struct{ re *regexp.Regexp }

func (*RegexV) valueMarker() {}

// buildPackages registers every std package. All are reachable by name; `use`
// merely documents intent (unused imports are a warning in the spec, §11.2).
func buildPackages() map[string]*Pkg {
	pkgs := map[string]*Pkg{}
	for _, p := range []*Pkg{
		stdFmt(), stdStr(), stdOs(), stdMath(), stdTime(), stdConv(),
		stdJSON(), stdLog(), stdHTTP(), stdRegex(), stdRand(), stdUUID(),
		stdSort(),
	} {
		pkgs[p.Name] = p
	}
	return pkgs
}

func stdMath() *Pkg {
	p := &Pkg{Name: "math", Funcs: map[string]Value{}}
	p.Funcs["pi"] = FloatV(math.Pi)
	p.Funcs["e"] = FloatV(math.E)
	p.Funcs["inf"] = FloatV(math.Inf(1))
	f1 := func(name string, fn func(float64) float64) {
		p.Funcs[name] = bi(name, func(t *T, args []Value) Value {
			t.wantArgs("math."+name, args, 1, diag.Pos{})
			return FloatV(fn(t.wantFloat(args[0])))
		})
	}
	f1("sqrt", math.Sqrt)
	f1("sin", math.Sin)
	f1("cos", math.Cos)
	f1("tan", math.Tan)
	f1("asin", math.Asin)
	f1("acos", math.Acos)
	f1("atan", math.Atan)
	f1("ln", math.Log)
	f1("log2", math.Log2)
	f1("log10", math.Log10)
	f1("exp", math.Exp)
	p.Funcs["pow"] = bi("pow", func(t *T, args []Value) Value {
		t.wantArgs("math.pow", args, 2, diag.Pos{})
		return FloatV(math.Pow(t.wantFloat(args[0]), t.wantFloat(args[1])))
	})
	p.Funcs["hypot"] = bi("hypot", func(t *T, args []Value) Value {
		t.wantArgs("math.hypot", args, 2, diag.Pos{})
		return FloatV(math.Hypot(t.wantFloat(args[0]), t.wantFloat(args[1])))
	})
	p.Funcs["mod"] = bi("mod", func(t *T, args []Value) Value {
		t.wantArgs("math.mod", args, 2, diag.Pos{})
		return FloatV(math.Mod(t.wantFloat(args[0]), t.wantFloat(args[1])))
	})
	return p
}

func (t *T) wantFloat(v Value) float64 {
	switch x := v.(type) {
	case FloatV:
		return float64(x)
	case IntV:
		return float64(x)
	case DecV:
		return x.D.Float64()
	}
	t.rtErr(diag.Pos{}, "expected number, got %s", typeName(v))
	return 0
}

func stdTime() *Pkg {
	p := &Pkg{Name: "time", Funcs: map[string]Value{}}
	p.Funcs["Nanosecond"] = DurV(time.Nanosecond)
	p.Funcs["Microsecond"] = DurV(time.Microsecond)
	p.Funcs["Millisecond"] = DurV(time.Millisecond)
	p.Funcs["Second"] = DurV(time.Second)
	p.Funcs["Minute"] = DurV(time.Minute)
	p.Funcs["Hour"] = DurV(time.Hour)
	p.Funcs["now"] = bi("now", func(t *T, args []Value) Value {
		return InstantV(time.Now())
	})
	p.Funcs["since"] = bi("since", func(t *T, args []Value) Value {
		t.wantArgs("time.since", args, 1, diag.Pos{})
		if inst, ok := args[0].(InstantV); ok {
			return DurV(time.Since(time.Time(inst)))
		}
		t.rtErr(diag.Pos{}, "time.since needs an instant")
		return NilV{}
	})
	p.Funcs["sleep"] = bi("sleep", func(t *T, args []Value) Value {
		t.wantArgs("time.sleep", args, 1, diag.Pos{})
		d := t.toDuration(args[0], diag.Pos{})
		// Cancellation-aware (frozen decision g).
		select {
		case <-time.After(d):
		case <-t.ctx.Done():
			t.throwf("Cancelled", "task cancelled during sleep")
		}
		return UnitV{}
	})
	p.Funcs["after"] = bi("after", func(t *T, args []Value) Value {
		t.wantArgs("time.after", args, 1, diag.Pos{})
		d := t.toDuration(args[0], diag.Pos{})
		ch := newChan(1)
		go func() {
			<-time.After(d)
			ch.mu.Lock()
			if !ch.closed {
				ch.ch <- InstantV(time.Now())
			}
			ch.mu.Unlock()
		}()
		return ch
	})
	p.Funcs["millis"] = bi("millis", func(t *T, args []Value) Value {
		t.wantArgs("time.millis", args, 1, diag.Pos{})
		return DurV(time.Duration(t.toInt(args[0], diag.Pos{})) * time.Millisecond)
	})
	p.Funcs["seconds"] = bi("seconds", func(t *T, args []Value) Value {
		t.wantArgs("time.seconds", args, 1, diag.Pos{})
		return DurV(time.Duration(t.toInt(args[0], diag.Pos{})) * time.Second)
	})
	return p
}

func stdConv() *Pkg {
	p := &Pkg{Name: "conv", Funcs: map[string]Value{}}
	for _, name := range []string{"int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "byte", "float", "dec", "rune", "str"} {
		target := name
		p.Funcs[target] = bi(target, func(t *T, args []Value) Value {
			return t.convert(target, args, diag.Pos{})
		})
		p.Funcs["to_"+target] = bi("to_"+target, func(t *T, args []Value) Value {
			return t.safeConvert(target, args)
		})
	}
	return p
}

func stdLog() *Pkg {
	p := &Pkg{Name: "log", Funcs: map[string]Value{}}
	level := func(name, tag string) {
		p.Funcs[name] = bi(name, func(t *T, args []Value) Value {
			var parts []string
			for _, a := range args {
				parts = append(parts, t.Str(a))
			}
			fmt.Fprintf(t.in.Stderr, "%s %s\n", tag, joinStrings(parts, " "))
			return UnitV{}
		})
	}
	level("info", "INFO ")
	level("warn", "WARN ")
	level("error", "ERROR")
	level("debug", "DEBUG")
	p.Funcs["flush"] = bi("flush", func(t *T, args []Value) Value { return UnitV{} })
	return p
}

func stdRegex() *Pkg {
	p := &Pkg{Name: "regex", Funcs: map[string]Value{}}
	p.Funcs["compile"] = bi("compile", func(t *T, args []Value) Value {
		t.wantArgs("regex.compile", args, 1, diag.Pos{})
		re, err := regexp.Compile(string(t.wantStr(args[0], diag.Pos{})))
		if err != nil {
			return &ErrVal{Msg: "bad regex: " + err.Error(), Trace: t.captureTrace()}
		}
		return &RegexV{re: re}
	})
	p.Funcs["matches"] = bi("matches", func(t *T, args []Value) Value {
		t.wantArgs("regex.matches", args, 2, diag.Pos{})
		re := t.compileRe(args[0])
		return BoolV(re.MatchString(string(t.wantStr(args[1], diag.Pos{}))))
	})
	p.Funcs["find"] = bi("find", func(t *T, args []Value) Value {
		t.wantArgs("regex.find", args, 2, diag.Pos{})
		re := t.compileRe(args[0])
		m := re.FindString(string(t.wantStr(args[1], diag.Pos{})))
		if m == "" && !re.MatchString(string(t.wantStr(args[1], diag.Pos{}))) {
			return NilV{}
		}
		return StrV(m)
	})
	p.Funcs["find_all"] = bi("find_all", func(t *T, args []Value) Value {
		t.wantArgs("regex.find_all", args, 2, diag.Pos{})
		re := t.compileRe(args[0])
		return strSlice(re.FindAllString(string(t.wantStr(args[1], diag.Pos{})), -1))
	})
	p.Funcs["replace"] = bi("replace", func(t *T, args []Value) Value {
		t.wantArgs("regex.replace", args, 3, diag.Pos{})
		re := t.compileRe(args[0])
		return StrV(re.ReplaceAllString(string(t.wantStr(args[1], diag.Pos{})), string(t.wantStr(args[2], diag.Pos{}))))
	})
	return p
}

func (t *T) compileRe(v Value) *regexp.Regexp {
	switch x := v.(type) {
	case *RegexV:
		return x.re
	case StrV:
		re, err := regexp.Compile(string(x))
		if err != nil {
			t.throwf("ValueError", "bad regex: %v", err)
		}
		return re
	}
	t.rtErr(diag.Pos{}, "expected regex or pattern string")
	return nil
}

func (t *T) regexMethod(r *RegexV, name string, args []Value, pos diag.Pos) (Value, bool) {
	switch name {
	case "matches":
		t.wantArgs("matches", args, 1, pos)
		return BoolV(r.re.MatchString(string(t.wantStr(args[0], pos)))), true
	case "find":
		t.wantArgs("find", args, 1, pos)
		s := string(t.wantStr(args[0], pos))
		if !r.re.MatchString(s) {
			return NilV{}, true
		}
		return StrV(r.re.FindString(s)), true
	case "find_all":
		t.wantArgs("find_all", args, 1, pos)
		return strSlice(r.re.FindAllString(string(t.wantStr(args[0], pos)), -1)), true
	case "groups":
		t.wantArgs("groups", args, 1, pos)
		m := r.re.FindStringSubmatch(string(t.wantStr(args[0], pos)))
		if m == nil {
			return NilV{}, true
		}
		return strSlice(m), true
	case "replace":
		t.wantArgs("replace", args, 2, pos)
		return StrV(r.re.ReplaceAllString(string(t.wantStr(args[0], pos)), string(t.wantStr(args[1], pos)))), true
	}
	return nil, false
}

func stdRand() *Pkg {
	p := &Pkg{Name: "rand", Funcs: map[string]Value{}}
	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	p.Funcs["seed"] = bi("seed", func(t *T, args []Value) Value {
		t.wantArgs("rand.seed", args, 1, diag.Pos{})
		rng = rand.New(rand.NewSource(t.toInt(args[0], diag.Pos{})))
		return UnitV{}
	})
	p.Funcs["int"] = bi("int", func(t *T, args []Value) Value {
		t.wantArgs("rand.int", args, 1, diag.Pos{})
		n := t.toInt(args[0], diag.Pos{})
		if n <= 0 {
			t.throwf("RangeError", "rand.int needs a positive bound")
		}
		return IntV(rng.Int63n(n))
	})
	p.Funcs["float"] = bi("float", func(t *T, args []Value) Value {
		return FloatV(rng.Float64())
	})
	return p
}

func stdUUID() *Pkg {
	p := &Pkg{Name: "uuid", Funcs: map[string]Value{}}
	p.Funcs["new"] = bi("new", func(t *T, args []Value) Value {
		var b [16]byte
		rand.Read(b[:])
		b[6] = (b[6] & 0x0f) | 0x40 // version 4
		b[8] = (b[8] & 0x3f) | 0x80 // variant
		return StrV(fmt.Sprintf("%x-%x-%x-%x-%x", b[0:4], b[4:6], b[6:8], b[8:10], b[10:16]))
	})
	return p
}

func stdSort() *Pkg {
	p := &Pkg{Name: "sort", Funcs: map[string]Value{}}
	p.Funcs["sort"] = bi("sort", func(t *T, args []Value) Value {
		t.wantArgs("sort.sort", args, 1, diag.Pos{})
		s, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(diag.Pos{}, "sort needs a slice")
		}
		sortValues(s.Elems, func(a, b Value) bool { return t.lessValues(a, b, diag.Pos{}) })
		return UnitV{}
	})
	p.Funcs["sort_by"] = bi("sort_by", func(t *T, args []Value) Value {
		t.wantArgs("sort.sort_by", args, 2, diag.Pos{})
		s, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(diag.Pos{}, "sort_by needs a slice")
		}
		sortValues(s.Elems, func(a, b Value) bool {
			r := t.callAny(args[1], []Value{a, b}, diag.Pos{})
			b2, ok := r.(BoolV)
			return ok && bool(b2)
		})
		return UnitV{}
	})
	p.Funcs["reversed"] = bi("reversed", func(t *T, args []Value) Value {
		t.wantArgs("sort.reversed", args, 1, diag.Pos{})
		s, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(diag.Pos{}, "reversed needs a slice")
		}
		out := make([]Value, len(s.Elems))
		for i, e := range s.Elems {
			out[len(s.Elems)-1-i] = e
		}
		return &Slice{Elems: out}
	})
	return p
}
