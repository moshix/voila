// Package interp is Voilà's tree-walking evaluator. Semantics follow the
// specification; non-local control flow uses typed panics (control.go),
// exceptions unwind with defers running (§8.3), aborts do not (§8.6).
package interp

import (
	"context"
	"fmt"
	"io"
	"os"
	"strings"
	"sync"

	"voila/internal/ast"
	"voila/internal/dec"
	"voila/internal/diag"
)

type Interp struct {
	File    *ast.File
	Src     *diag.Source
	SrcName string

	Structs    map[string]*ast.StructDecl
	Enums      map[string]*ast.EnumDecl
	Variants   map[string]*ast.EnumDecl // variant name → owning enum
	Traits     map[string]*ast.TraitDecl
	Exceptions map[string]*ast.ExceptionDecl
	Impls      map[string]map[string]*ast.FuncDecl // type name → method → decl
	TraitImpls map[string]map[string]bool          // type name → trait → yes
	Aliases    map[string]ast.TypeExpr

	Globals  *Env
	prelude  map[string]Value
	packages map[string]*Pkg

	Stdout io.Writer
	Stderr io.Writer
	Args   []string

	onAbort Value // os.on_abort handler
	abortMu sync.Mutex
}

// Frame is one call-stack entry.
type Frame struct {
	name    string
	callPos diag.Pos
	defers  []*deferred
	exc     *ErrVal // exception bound by an active catch clause (for bare throw)
}

type deferred struct{ run func() }

// T is per-task interpreter state: every spawned task gets its own T.
type T struct {
	in     *Interp
	ctx    context.Context
	group  *group
	digits int
	frames []*Frame
}

func New(file *ast.File, src *diag.Source) *Interp {
	in := &Interp{
		File:       file,
		Src:        src,
		SrcName:    src.Name,
		Structs:    map[string]*ast.StructDecl{},
		Enums:      map[string]*ast.EnumDecl{},
		Variants:   map[string]*ast.EnumDecl{},
		Traits:     map[string]*ast.TraitDecl{},
		Exceptions: map[string]*ast.ExceptionDecl{},
		Impls:      map[string]map[string]*ast.FuncDecl{},
		TraitImpls: map[string]map[string]bool{},
		Aliases:    map[string]ast.TypeExpr{},
		Globals:    NewEnv(nil),
		Stdout:     os.Stdout,
		Stderr:     os.Stderr,
	}
	in.prelude = buildPrelude()
	in.packages = buildPackages()
	return in
}

// Run executes the program: globals, script statements, then main.
// Returns the process exit code.
func (in *Interp) Run(args []string) (exit int) {
	in.Args = args
	rootCtx, rootCancel := context.WithCancel(context.Background())
	rootGroup := &group{ctx: rootCtx, cancel: rootCancel}
	t := &T{in: in, ctx: rootCtx, group: rootGroup, digits: dec.DefaultDigits}
	defer rootCancel()

	defer func() {
		r := recover()
		switch v := r.(type) {
		case nil:
		case ctlExit:
			exit = v.code
		case ctlThrow:
			// Uncaught exception at the top of main = abort (§8.6).
			in.runAbortHandler(t)
			fmt.Fprintf(in.Stderr, "unhandled exception: %s\n", t.errMessage(v.err))
			for _, fr := range v.err.Trace {
				fmt.Fprintf(in.Stderr, "    %s\n", fr)
			}
			for c := v.err.Cause; c != nil; c = c.Cause {
				fmt.Fprintf(in.Stderr, "caused by: %s\n", t.errMessage(c))
			}
			exit = 1
		case ctlAbort:
			in.runAbortHandler(t)
			fmt.Fprintf(in.Stderr, "abort: %s\n", v.msg)
			for _, fr := range v.trace {
				fmt.Fprintf(in.Stderr, "    %s\n", fr)
			}
			exit = 1
		case ctlPropagate:
			// An error value escaped main via `try` (§14.1 does this).
			if e, ok := v.err2().(*ErrVal); ok {
				fmt.Fprintf(in.Stderr, "error: %s\n", t.errMessage(e))
			} else {
				fmt.Fprintf(in.Stderr, "error: nil value\n")
			}
			exit = 1
		default:
			panic(r)
		}
	}()

	in.register(t)

	// Root frame for script statements.
	root := &Frame{name: "main"}
	t.frames = append(t.frames, root)
	defer t.runFrameDefers(root)

	for _, s := range in.File.ScriptStmts {
		t.execStmt(in.Globals, s)
	}
	if mainBind := in.Globals.Lookup("main"); mainBind != nil {
		if fn, ok := mainBind.Val.(*Func); ok {
			res := t.callFunction(fn, nil, nil, fn.Body.P)
			// An error value that escaped main via `try` (§14.1 pattern).
			if e, ok := res.(*ErrVal); ok {
				fmt.Fprintf(in.Stderr, "error: %s\n", t.errMessage(e))
				rootGroup.wg.Wait()
				return 1
			}
		}
	}
	// Implicit root group: stray spawns are joined before exit (§9.1).
	rootGroup.wg.Wait()
	if f := rootGroup.deliver(); f != nil {
		panic(ctlThrow{f.err})
	}
	return 0
}

func (in *Interp) runAbortHandler(t *T) {
	in.abortMu.Lock()
	h := in.onAbort
	in.onAbort = nil
	in.abortMu.Unlock()
	if h == nil {
		return
	}
	func() {
		defer func() { recover() }() // last-gasp handler failures are ignored
		t.callAny(h, nil, diag.Pos{})
	}()
}

// err2 lets ctlPropagate carry either an *ErrVal or nil-propagation.
func (c ctlPropagate) err2() Value {
	if c.err == nil {
		return NilV{}
	}
	return c.err
}

// register walks declarations and populates the registries.
func (in *Interp) register(t *T) {
	for _, d := range in.File.Decls {
		switch decl := d.(type) {
		case *ast.StructDecl:
			in.Structs[decl.Name] = decl
		case *ast.EnumDecl:
			in.Enums[decl.Name] = decl
			for _, v := range decl.Variants {
				in.Variants[v.Name] = decl
			}
		case *ast.TraitDecl:
			in.Traits[decl.Name] = decl
		case *ast.ExceptionDecl:
			in.Exceptions[decl.Name] = decl
			for _, m := range decl.Methods {
				in.addImpl(decl.Name, m)
			}
		case *ast.TypeDecl:
			if decl.Struct != nil {
				in.Structs[decl.Name] = decl.Struct
			} else if decl.Target != nil {
				in.Aliases[decl.Name] = decl.Target
			}
		case *ast.ImplDecl:
			typeName := namedTypeName(decl.Type)
			for _, m := range decl.Methods {
				in.addImpl(typeName, m)
			}
			if decl.Trait != "" {
				if in.TraitImpls[typeName] == nil {
					in.TraitImpls[typeName] = map[string]bool{}
				}
				in.TraitImpls[typeName][decl.Trait] = true
			}
		case *ast.FuncDecl:
			fn := &Func{
				Name: decl.Name, Params: decl.Params, Body: decl.Body,
				Decl: decl, NoThrow: decl.NoThrow,
			}
			in.Globals.Define(decl.Name, fn, false)
		}
	}
	// Globals run after all functions are visible.
	for _, d := range in.File.Decls {
		if g, ok := d.(*ast.GlobalDecl); ok {
			t.execStmt(in.Globals, g.Stmt)
		}
	}
}

func (in *Interp) addImpl(typeName string, m *ast.FuncDecl) {
	if in.Impls[typeName] == nil {
		in.Impls[typeName] = map[string]*ast.FuncDecl{}
	}
	in.Impls[typeName][m.Name] = m
}

func namedTypeName(te ast.TypeExpr) string {
	if nt, ok := te.(*ast.NamedType); ok {
		return nt.Name
	}
	return ""
}

// ---------------------------------------------------------------- frames

func (t *T) currentFrame() *Frame { return t.frames[len(t.frames)-1] }

func (t *T) captureTrace() []string {
	var tr []string
	for i := len(t.frames) - 1; i >= 0; i-- {
		fr := t.frames[i]
		if fr.callPos.IsValid() {
			tr = append(tr, fmt.Sprintf("at %s (%s)", fr.name, t.where(fr.callPos)))
		} else {
			tr = append(tr, fmt.Sprintf("at %s", fr.name))
		}
	}
	return tr
}

// runFrameDefers runs a frame's defers LIFO, each exactly once. It returns
// the first control panic raised by a defer (later throws attach as
// suppressed to an earlier throw).
func (t *T) runFrameDefers(fr *Frame) {
	p := t.collectDefers(fr)
	if p != nil {
		panic(p)
	}
}

func (t *T) collectDefers(fr *Frame) any {
	var pending any
	for i := len(fr.defers) - 1; i >= 0; i-- {
		d := fr.defers[i]
		func() {
			defer func() {
				if r := recover(); r != nil {
					if _, isAbort := r.(ctlAbort); isAbort {
						panic(r) // aborts win immediately
					}
					if pending == nil {
						pending = r
					} else if pt, ok := pending.(ctlThrow); ok {
						if rt, ok2 := r.(ctlThrow); ok2 {
							rt.err.Suppressed = append(rt.err.Suppressed, pt.err)
							pending = rt // newer exception wins (frozen decision h)
						}
					}
				}
			}()
			d.run()
		}()
	}
	fr.defers = nil
	return pending
}

// callFunction invokes a user function/closure with the full epilogue:
// defers run on every exit path except aborts; ctlReturn becomes the result;
// ctlPropagate makes the function return the error value (§8.1).
func (t *T) callFunction(fn *Func, args []Value, named map[string]Value, callPos diag.Pos) (result Value) {
	if len(t.frames) > 10000 {
		t.abort(callPos, "stack overflow: recursion too deep")
	}
	env := NewEnv(t.closureBase(fn))
	t.bindParams(env, fn, args, named, callPos)

	name := fn.Name
	if name == "" {
		name = "<closure>"
	}
	frame := &Frame{name: name, callPos: callPos}
	t.frames = append(t.frames, frame)
	savedDigits := t.digits

	result = UnitV{}
	defer func() {
		r := recover()
		t.frames = t.frames[:len(t.frames)-1]
		t.digits = savedDigits
		if r != nil {
			if _, isAbort := r.(ctlAbort); isAbort {
				panic(r) // aborts skip user defers (§8.6)
			}
			if _, isExit := r.(ctlExit); isExit {
				panic(r)
			}
		}
		dpanic := t.collectDefers(frame)
		switch v := r.(type) {
		case nil:
			if dpanic != nil {
				panic(dpanic)
			}
		case ctlReturn:
			result = v.val
			if dpanic != nil {
				panic(dpanic)
			}
		case ctlPropagate:
			if dpanic != nil {
				panic(dpanic)
			}
			result = v.err2()
		case ctlThrow:
			if fn.NoThrow {
				panic(ctlAbort{msg: "exception escaped nothrow function " + name + ": " + v.err.Message(), trace: v.err.Trace})
			}
			if dpanic != nil {
				if nt, ok := dpanic.(ctlThrow); ok {
					nt.err.Suppressed = append(nt.err.Suppressed, v.err)
					panic(nt)
				}
				panic(dpanic)
			}
			panic(v)
		default:
			if dpanic != nil {
				panic(dpanic)
			}
			panic(r)
		}
	}()

	last := t.execBody(env, fn.Body)
	// Closures: the last expression is the value (§10, `fn(x) { x * 2 }`).
	if fn.Name == "" && last != nil {
		result = last
	}
	return result
}

func (t *T) closureBase(fn *Func) *Env {
	if fn.Env != nil {
		return fn.Env
	}
	return t.in.Globals
}

// bindParams evaluates and binds arguments: positional, named, defaults,
// variadic packing, and `own` moves already applied by the caller.
func (t *T) bindParams(env *Env, fn *Func, args []Value, named map[string]Value, callPos diag.Pos) {
	if fn.HasSelf {
		env.Define("self", fn.Self, fn.SelfMode == ast.ModeMut)
	}
	params := fn.Params
	ai := 0
	for pi, prm := range params {
		if prm.Variadic {
			rest := make([]Value, 0, len(args)-ai)
			rest = append(rest, args[ai:]...)
			env.Define(prm.Name, &Slice{Elems: rest}, false)
			ai = len(args)
			continue
		}
		var v Value
		switch {
		case named != nil && named[prm.Name] != nil:
			v = named[prm.Name]
		case ai < len(args):
			v = args[ai]
			ai++
		case prm.Default != nil:
			v = t.eval(env, prm.Default)
		default:
			t.rtErr(callPos, "missing argument `%s` in call to %s", prm.Name, fn.Name)
		}
		mut := prm.Mode == ast.ModeMut
		env.Define(prm.Name, v, mut)
		_ = pi
	}
	if ai < len(args) {
		t.rtErr(callPos, "too many arguments in call to %s (want %d, got %d)", fn.Name, len(params), len(args))
	}
}

// errMessage renders an error's message, honouring a user message() impl.
func (t *T) errMessage(e *ErrVal) string {
	if e.TypeName != "" {
		if impl, ok := t.in.Impls[e.TypeName]["message"]; ok {
			fn := &Func{Name: "message", Params: impl.Params, Body: impl.Body,
				Self: e, HasSelf: true, Decl: impl}
			if s, ok := t.callFunction(fn, nil, nil, e.Struct.pos()).(StrV); ok {
				return string(s)
			}
		}
	}
	return e.Message()
}

func (s *Struct) pos() diag.Pos { return diag.Pos{} }

// Str renders a value for say/interpolation, honouring user Show impls.
func (t *T) Str(v Value) string {
	switch x := v.(type) {
	case *Struct:
		if impl, ok := t.in.Impls[x.Type]["show"]; ok {
			fn := &Func{Name: "show", Params: impl.Params, Body: impl.Body,
				Self: x, HasSelf: true, Decl: impl}
			if s, ok := t.callFunction(fn, nil, nil, diag.Pos{}).(StrV); ok {
				return string(s)
			}
		}
	case *EnumVal:
		if impl, ok := t.in.Impls[x.Type]["show"]; ok {
			fn := &Func{Name: "show", Params: impl.Params, Body: impl.Body,
				Self: x, HasSelf: true, Decl: impl}
			if s, ok := t.callFunction(fn, nil, nil, diag.Pos{}).(StrV); ok {
				return string(s)
			}
		}
	case *ErrVal:
		return t.errMessage(x)
	case *SharedBox:
		return t.Str(x.V)
	}
	return Str(v)
}

// zeroValue produces the zero value for a declared type (§4).
func (t *T) zeroValue(te ast.TypeExpr) Value {
	switch ty := te.(type) {
	case nil:
		return NilV{}
	case *ast.NamedType:
		switch ty.Name {
		case "int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "byte", "rune":
			if ty.Name == "rune" {
				return RuneV(0)
			}
			return IntV(0)
		case "float", "f32":
			return FloatV(0)
		case "dec":
			return DecV{dec.Zero()}
		case "bool":
			return BoolV(false)
		case "str":
			return StrV("")
		case "unit":
			return UnitV{}
		}
		if alias, ok := t.in.Aliases[ty.Name]; ok {
			return t.zeroValue(alias)
		}
		if sd, ok := t.in.Structs[ty.Name]; ok {
			return t.newStruct(sd, nil, nil)
		}
		return NilV{}
	case *ast.OptionType:
		return NilV{}
	case *ast.ResultType:
		return t.zeroValue(ty.Elem)
	case *ast.SliceType:
		if ty.Len != nil {
			n := t.toInt(t.eval(t.in.Globals, ty.Len), ty.P)
			elems := make([]Value, n)
			for i := range elems {
				elems[i] = t.zeroValue(ty.Elem)
			}
			return &Slice{Elems: elems}
		}
		return &Slice{}
	case *ast.MapType:
		return NewMap()
	case *ast.SetType:
		return NewSet()
	case *ast.TupleType:
		tp := &Tuple{}
		for _, e := range ty.Elems {
			tp.Elems = append(tp.Elems, t.zeroValue(e))
		}
		return tp
	case *ast.BoxType:
		if ty.Kind == ast.WeakBox {
			return &Weak{} // empty handle; upgrade() yields nil
		}
		return NilV{}
	}
	return NilV{}
}

// newStruct builds a struct value from its declaration, positional and named
// initializers, applying field defaults then zero values (§3.4).
func (t *T) newStruct(sd *ast.StructDecl, positional []Value, named map[string]Value) *Struct {
	s := &Struct{Type: sd.Name, Fields: map[string]Value{}}
	for _, f := range sd.Fields {
		s.Order = append(s.Order, f.Name)
	}
	for i, f := range sd.Fields {
		switch {
		case named != nil && named[f.Name] != nil:
			s.Fields[f.Name] = named[f.Name]
		case i < len(positional):
			s.Fields[f.Name] = positional[i]
		case f.Default != nil:
			s.Fields[f.Name] = t.eval(t.in.Globals, f.Default)
		default:
			s.Fields[f.Name] = t.zeroValue(f.Type)
		}
	}
	return s
}

func (t *T) toInt(v Value, pos diag.Pos) int64 {
	switch x := v.(type) {
	case IntV:
		return int64(x)
	case RuneV:
		return int64(x)
	}
	t.rtErr(pos, "expected int, got %s", typeName(v))
	return 0
}

func typeName(v Value) string {
	switch x := v.(type) {
	case NilV:
		return "nil"
	case UnitV:
		return "unit"
	case BoolV:
		return "bool"
	case IntV:
		return "int"
	case FloatV:
		return "float"
	case RuneV:
		return "rune"
	case StrV:
		return "str"
	case DecV:
		return "dec"
	case *Slice:
		return "slice"
	case *Map:
		return "map"
	case *Set:
		return "set"
	case *Tuple:
		return "tuple"
	case *Struct:
		return x.Type
	case *EnumVal:
		return x.Type
	case *ErrVal:
		if x.TypeName != "" {
			return x.TypeName
		}
		return "error"
	case *Func:
		return "func"
	case *Builtin:
		return "func"
	case *Chan:
		return "chan"
	case *Task:
		return "task"
	case *SharedBox:
		return "shared[" + typeName(x.V) + "]"
	case *CellBox:
		return "cell"
	case *Weak:
		return "weak"
	case *RangeVal:
		return "range"
	case *File:
		return "file"
	case *Builder:
		return "builder"
	case DurV:
		return "duration"
	case InstantV:
		return "instant"
	case *Pkg:
		return "package"
	case *TypeV:
		return "type"
	}
	return strings.TrimPrefix(fmt.Sprintf("%T", v), "*interp.")
}
