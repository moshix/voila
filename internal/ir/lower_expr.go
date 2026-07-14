package ir

import (
	"strconv"
	"strings"

	"voila/internal/ast"
	"voila/internal/lexer"
)

var binOps = map[lexer.Kind]string{
	lexer.PLUS: "ADD", lexer.MINUS: "SUB", lexer.STAR: "MUL",
	lexer.SLASH: "DIV", lexer.FLOORDIV: "IDIV", lexer.PERCENT: "MOD",
	lexer.POWER: "POW", lexer.CONCAT: "CAT", lexer.SHL: "SHL",
	lexer.SHR: "SHR", lexer.AMP: "BAND", lexer.PIPE: "BOR",
	lexer.CARET: "BXOR", lexer.EQ: "CMPEQ", lexer.NE: "CMPNE",
	lexer.LT: "CMPLT", lexer.LE: "CMPLE", lexer.GT: "CMPGT",
	lexer.GE: "CMPGE", lexer.IN: "CMPIN",
}

// conversionTypes are the type names callable as checked conversions (§3.2).
var conversionTypes = map[string]bool{
	"int": true, "i8": true, "i16": true, "i32": true, "i64": true,
	"u8": true, "u16": true, "u32": true, "u64": true, "byte": true,
	"float": true, "f32": true, "dec": true, "str": true, "rune": true,
	"bool": true,
}

// lowerExpr lowers an expression and returns the operand holding its value.
func (lo *lowerer) lowerExpr(e ast.Expr) string {
	if e == nil {
		lo.fail(posOnLine(lo.curLine), "internal: nil expression")
	}
	switch x := e.(type) {
	case *ast.BasicLit:
		return lo.lowerLit(x)

	case *ast.InterpStr:
		var parts []string
		for _, p := range x.Parts {
			if p.Expr != nil {
				parts = append(parts, lo.lowerExpr(p.Expr))
			} else {
				parts = append(parts, lo.mod.constant(CStr, p.Lit))
			}
		}
		rd := lo.newReg()
		lo.emit(x.P.Line, "INTERP", rd, "("+strings.Join(parts, ",")+")")
		return rd

	case *ast.Ident:
		return lo.lowerIdentValue(x)

	case *ast.SelfExpr:
		op, _, found := lo.lookup("self")
		if !found {
			lo.fail(x.P, "`self` used outside a method")
		}
		return op

	case *ast.UnaryExpr:
		if x.Op == lexer.ARROW {
			rch := lo.lowerExpr(x.X)
			rd := lo.newReg()
			lo.emit(x.P.Line, "RECV", rd, rch)
			return rd
		}
		ra := lo.lowerExpr(x.X)
		rd := lo.newReg()
		op := map[lexer.Kind]string{lexer.MINUS: "NEG", lexer.NOT: "NOT", lexer.TILDE: "BNOT"}[x.Op]
		lo.emit(x.P.Line, op, rd, ra)
		return rd

	case *ast.BinaryExpr:
		return lo.lowerBinary(x)

	case *ast.RangeExpr:
		rlo := lo.lowerExpr(x.Lo)
		rhi := lo.lowerExpr(x.Hi)
		rby := "1"
		if x.By != nil {
			rby = lo.lowerExpr(x.By)
		}
		mode := "excl"
		if x.Inclusive {
			mode = "incl"
		}
		rd := lo.newReg()
		lo.emit(x.P.Line, "NEWRANGE", rd, rlo, rhi, rby, mode)
		return rd

	case *ast.CallExpr:
		return lo.lowerCall(x)

	case *ast.IndexExpr:
		return lo.lowerIndex(x)

	case *ast.FieldExpr:
		return lo.lowerField(x)

	case *ast.CompositeLit:
		return lo.lowerComposite(x)

	case *ast.SliceLit:
		var regs []string
		for _, el := range x.Elems {
			regs = append(regs, lo.lowerExpr(el))
		}
		rd := lo.newReg()
		lo.emit(x.P.Line, "NEWSLICE", rd, "("+strings.Join(regs, ",")+")")
		return rd

	case *ast.TupleExpr:
		if len(x.Elems) == 0 {
			rd := lo.newReg()
			lo.emit(x.P.Line, "LOADK", rd, lo.mod.constant(CUnit, "()"))
			return rd
		}
		var regs []string
		for _, el := range x.Elems {
			regs = append(regs, lo.lowerExpr(el))
		}
		rd := lo.newReg()
		lo.emit(x.P.Line, "TUPLE", rd, "("+strings.Join(regs, ",")+")")
		return rd

	case *ast.ClosureExpr:
		note := "closure" + closureSigText(x)
		name := lo.lowerClosure(x.Params, x.Body, note, x.P.Line)
		rd := lo.newReg()
		if x.OwnCap && len(x.Captures) > 0 {
			lo.emit(x.P.Line, "MKCLOS", rd, name, "own("+strings.Join(x.Captures, ",")+")")
		} else {
			lo.emit(x.P.Line, "MKCLOS", rd, name)
			lo.note("closure captures enclosing frame by reference")
		}
		return rd

	case *ast.TryExpr:
		ra := lo.lowerExpr(x.X)
		rd := lo.newReg()
		lo.emit(x.P.Line, "TRYP", rd, ra) // err/nil propagates as the fn result
		return rd

	case *ast.MustExpr:
		ra := lo.lowerExpr(x.X)
		rd := lo.newReg()
		lo.emit(x.P.Line, "MUST", rd, ra)
		return rd

	case *ast.AttemptExpr:
		// exception → error value (§8.5): a one-expression protected region.
		catchL := lo.newLabel()
		endL := lo.newLabel()
		rd := lo.newReg()
		lo.emit(x.P.Line, "EHPUSH", catchL)
		ra := lo.lowerExpr(x.X)
		lo.emit(x.P.Line, "MOVE", rd, ra)
		lo.emit(0, "EHPOP")
		lo.emit(0, "JUMP", endL)
		lo.mark(catchL)
		re := lo.newReg()
		lo.emit(x.P.Line, "CATCH", re)
		lo.emit(x.P.Line, "MOVE", rd, re)
		lo.mark(endL)
		return rd

	case *ast.ElseExpr:
		return lo.lowerElse(x)

	case *ast.SpawnExpr:
		return lo.lowerSpawn(x)
	}
	lo.fail(e.Pos(), "cannot lower expression %T", e)
	return ""
}

func (lo *lowerer) lowerLit(x *ast.BasicLit) string {
	var kname string
	switch x.Kind {
	case ast.IntLit:
		n, err := strconv.ParseInt(x.Text, 0, 64)
		if err != nil {
			lo.fail(x.P, "bad integer literal %q", x.Text)
		}
		kname = lo.mod.constant(CInt, strconv.FormatInt(n, 10))
	case ast.FloatLit:
		f, err := strconv.ParseFloat(x.Text, 64)
		if err != nil {
			lo.fail(x.P, "bad float literal %q", x.Text)
		}
		kname = lo.mod.constant(CFloat, strconv.FormatFloat(f, 'g', -1, 64))
	case ast.DecLit:
		kname = lo.mod.constant(CDec, x.Text)
	case ast.StrLit:
		kname = lo.mod.constant(CStr, x.Text)
	case ast.RuneLit:
		kname = lo.mod.constant(CRune, x.Text)
	case ast.BoolLit:
		kname = lo.mod.constant(CBool, x.Text)
	case ast.NilLit:
		kname = lo.mod.constant(CNil, "nil")
	}
	rd := lo.newReg()
	lo.emit(x.P.Line, "LOADK", rd, kname)
	return rd
}

// lowerIdentValue mirrors evalIdent's resolution order (frozen decision 2).
func (lo *lowerer) lowerIdentValue(x *ast.Ident) string {
	if op, _, found := lo.lookup(x.Name); found {
		return op
	}
	if lo.globals[x.Name] {
		rd := lo.newReg()
		lo.emit(x.P.Line, "GETGLB", rd, x.Name)
		return rd
	}
	if vi, ok := lo.variants[x.Name]; ok {
		rd := lo.newReg()
		if !vi.payload {
			lo.emit(x.P.Line, "NEWENUM", rd, vi.enum+"."+x.Name, "()")
		} else {
			lo.emit(x.P.Line, "LOADFN", rd, vi.enum+"."+x.Name)
		}
		return rd
	}
	if lo.funcs[x.Name] || lo.prelude[x.Name] ||
		conversionTypes[x.Name] || lo.structs[x.Name] || lo.enums[x.Name] ||
		lo.exceptions[x.Name] || lo.aliases[x.Name] || lo.packages[x.Name] {
		rd := lo.newReg()
		lo.emit(x.P.Line, "LOADFN", rd, x.Name)
		return rd
	}
	if x.Name == "ctx" {
		rd := lo.newReg()
		lo.emit(x.P.Line, "GETCTX", rd)
		return rd
	}
	lo.fail(x.P, "unknown identifier `%s`", x.Name)
	return ""
}

func (lo *lowerer) lowerBinary(x *ast.BinaryExpr) string {
	switch x.Op {
	case lexer.AND, lexer.OR:
		// Short-circuit lowering.
		rd := lo.newReg()
		shortL := lo.newLabel()
		endL := lo.newLabel()
		ra := lo.lowerExpr(x.X)
		if x.Op == lexer.AND {
			lo.emit(x.P.Line, "JMPF", ra, shortL)
		} else {
			lo.emit(x.P.Line, "JMPT", ra, shortL)
		}
		rb := lo.lowerExpr(x.Y)
		lo.emit(x.P.Line, "MOVE", rd, rb)
		lo.emit(0, "JUMP", endL)
		lo.mark(shortL)
		val := "false"
		if x.Op == lexer.OR {
			val = "true"
		}
		lo.emit(x.P.Line, "LOADK", rd, lo.mod.constant(CBool, val))
		lo.mark(endL)
		return rd

	case lexer.ARROW:
		// Send in expression position (statement form handled in lowerStmt).
		rch := lo.lowerExpr(x.X)
		rv := lo.lowerExpr(x.Y)
		lo.emit(x.P.Line, "SEND", rch, rv)
		rd := lo.newReg()
		lo.emit(x.P.Line, "LOADK", rd, lo.mod.constant(CUnit, "()"))
		return rd
	}

	op, ok := binOps[x.Op]
	if !ok {
		lo.fail(x.P, "cannot lower operator %s", x.Op)
	}
	ra := lo.lowerExpr(x.X)
	rb := lo.lowerExpr(x.Y)
	rd := lo.newReg()
	lo.emit(x.P.Line, op, rd, ra, rb)
	return rd
}

// lowerElse implements `expr else alt` (§9.2): failure (err/nil) selects the
// alternative. A directly-guarded TryExpr is unwrapped — failure is handled
// locally, not propagated (mirrors evalMaybe; frozen decision 4).
func (lo *lowerer) lowerElse(x *ast.ElseExpr) string {
	inner := x.X
	if te, ok := inner.(*ast.TryExpr); ok {
		inner = te.X
	}
	ra := lo.lowerExpr(inner)
	rd := lo.newReg()
	rt := lo.newReg()
	altL := lo.newLabel()
	endL := lo.newLabel()
	lo.emit(x.P.Line, "ISFAIL", rt, ra)
	lo.emit(x.P.Line, "JMPT", rt, altL)
	lo.emit(x.P.Line, "MOVE", rd, ra)
	lo.emit(0, "JUMP", endL)
	lo.mark(altL)
	if x.AltStmt != nil {
		lo.lowerStmt(x.AltStmt) // diverges: rd defined on the success path only
	} else {
		rv := lo.lowerExpr(x.Alt)
		lo.emit(x.P.Line, "MOVE", rd, rv)
	}
	lo.mark(endL)
	return rd
}

// ---------------------------------------------------------------- calls

func (lo *lowerer) lowerCall(x *ast.CallExpr) string {
	line := x.P.Line

	if id, ok := x.Fun.(*ast.Ident); ok {
		switch id.Name {
		case "chan":
			rd := lo.newReg()
			if len(x.Args) > 0 {
				rc := lo.lowerExpr(x.Args[0].Value)
				lo.emit(line, "NEWCHAN", rd, rc)
			} else {
				lo.emit(line, "NEWCHAN", rd)
			}
			return rd
		case "make":
			return lo.lowerMake(x)
		}
	}

	// Method-call path: recv.name(args).
	if fe, ok := x.Fun.(*ast.FieldExpr); ok {
		if base, ok := fe.X.(*ast.Ident); ok {
			if _, _, isLocal := lo.lookup(base.Name); !isLocal && lo.packages[base.Name] {
				args := lo.lowerArgs(x)
				rd := lo.newReg()
				lo.emit(line, "CALL", rd, base.Name+"."+fe.Name, args)
				return rd
			}
			// Associated function on a type: `Point.origin()` (§10).
			if _, _, isLocal := lo.lookup(base.Name); !isLocal &&
				(lo.structs[base.Name] || lo.enums[base.Name] || lo.exceptions[base.Name]) {
				args := lo.lowerArgs(x)
				rd := lo.newReg()
				lo.emit(line, "CALL", rd, base.Name+"."+fe.Name, args)
				return rd
			}
		}
		robj := lo.lowerExpr(fe.X)
		args := lo.lowerArgs(x)
		rd := lo.newReg()
		lo.emit(line, "CALLM", rd, robj, fe.Name, args)
		return rd
	}

	// Named callees.
	if id, ok := x.Fun.(*ast.Ident); ok {
		name := id.Name
		if op, _, found := lo.lookup(name); found {
			args := lo.lowerArgs(x)
			rd := lo.newReg()
			lo.emit(line, "CALLR", rd, op, args)
			return rd
		}
		if conversionTypes[name] || lo.aliases[name] {
			// Checked conversion (§3.2): int(x), int(s, 16), dec("…").
			if len(x.Args) == 0 {
				lo.fail(x.P, "%s() needs an argument", name)
			}
			ra := lo.lowerExpr(x.Args[0].Value)
			rd := lo.newReg()
			if len(x.Args) > 1 {
				rb := lo.lowerExpr(x.Args[1].Value)
				lo.emit(line, "CONV", rd, name, ra, rb)
			} else {
				lo.emit(line, "CONV", rd, name, ra)
			}
			return rd
		}
		if vi, ok := lo.variants[name]; ok {
			args := lo.lowerArgs(x)
			rd := lo.newReg()
			lo.emit(line, "NEWENUM", rd, vi.enum+"."+name, args)
			return rd
		}
		if lo.structs[name] || lo.enums[name] || lo.exceptions[name] {
			lo.fail(x.P, "type `%s` cannot be called; use a composite literal", name)
		}
		if lo.funcs[name] || lo.prelude[name] {
			args := lo.lowerArgsFor(x, name)
			rd := lo.newReg()
			lo.emit(line, "CALL", rd, name, args)
			return rd
		}
		lo.fail(x.P, "unknown function `%s`", name)
	}

	// Generic instantiation called directly: cell[int](0), json.decode[T](s).
	if ix, ok := x.Fun.(*ast.IndexExpr); ok {
		if fname, ok := lo.genericFuncName(ix); ok {
			args := lo.lowerArgs(x)
			rd := lo.newReg()
			lo.emit(line, "CALL", rd, fname, args)
			return rd
		}
	}

	// Anything else: evaluate the callee, call indirectly.
	rf := lo.lowerExpr(x.Fun)
	args := lo.lowerArgs(x)
	rd := lo.newReg()
	lo.emit(line, "CALLR", rd, rf, args)
	return rd
}

// lowerArgs renders a call's argument list. Named arguments are resolved to
// their PARAMETER POSITION here, while the callee's signature is in scope —
// downstream consumers then only ever see positional arguments.
func (lo *lowerer) lowerArgs(x *ast.CallExpr) string {
	return lo.lowerArgsFor(x, "")
}

func (lo *lowerer) lowerArgsFor(x *ast.CallExpr, callee string) string {
	type arg struct {
		reg    string
		name   string
		spread bool
	}
	var args []arg
	hasNamed := false
	for _, a := range x.Args {
		r := lo.lowerExpr(a.Value)
		args = append(args, arg{reg: r, name: a.Name, spread: a.Spread})
		if a.Name != "" {
			hasNamed = true
		}
	}

	if hasNamed {
		params, known := lo.funcParams[callee]
		if !known {
			// A named argument to a callee we cannot see (a closure, a builtin)
			// has no defined position: reject rather than guess.
			lo.fail(x.P, "named arguments require a statically known callee")
		}
		slots := make([]string, len(params))
		next := 0
		for _, a := range args {
			if a.spread {
				lo.fail(x.P, "cannot mix a spread with named arguments")
			}
			if a.name == "" {
				if next >= len(slots) {
					lo.fail(x.P, "too many positional arguments")
				}
				slots[next] = a.reg
				next++
				continue
			}
			at := -1
			for i, p := range params {
				if p == a.name {
					at = i
				}
			}
			if at < 0 {
				lo.fail(x.P, "no parameter named `%s`", a.name)
			}
			if slots[at] != "" {
				lo.fail(x.P, "parameter `%s` supplied twice", a.name)
			}
			slots[at] = a.reg
		}
		// Trim trailing empties (defaulted params); a HOLE is not expressible.
		end := len(slots)
		for end > 0 && slots[end-1] == "" {
			end--
		}
		for i := 0; i < end; i++ {
			if slots[i] == "" {
				lo.fail(x.P, "parameter `%s` must be supplied when a later one is",
					params[i])
			}
		}
		return "(" + strings.Join(slots[:end], ",") + ")"
	}

	var parts []string
	for _, a := range args {
		switch {
		case a.spread:
			parts = append(parts, a.reg+"...")
		default:
			parts = append(parts, a.reg)
		}
	}
	return "(" + strings.Join(parts, ",") + ")"
}

func (lo *lowerer) lowerMake(x *ast.CallExpr) string {
	if len(x.TypeArgs) == 0 {
		lo.fail(x.P, "make requires a type argument")
	}
	rd := lo.newReg()
	line := x.P.Line
	switch ty := x.TypeArgs[0].(type) {
	case *ast.SliceType:
		if len(x.Args) > 0 {
			rn := lo.lowerExpr(x.Args[0].Value)
			lo.emit(line, "NEWSLICE", rd, "["+rn+"]") // size form, zero-filled
		} else {
			lo.emit(line, "NEWSLICE", rd, "()")
		}
	case *ast.MapType:
		lo.emit(line, "NEWMAP", rd)
	case *ast.SetType:
		lo.emit(line, "NEWSET", rd)
	case *ast.ChanType:
		if len(x.Args) > 0 {
			rc := lo.lowerExpr(x.Args[0].Value)
			lo.emit(line, "NEWCHAN", rd, rc)
		} else {
			lo.emit(line, "NEWCHAN", rd)
		}
	default:
		lo.fail(x.P, "cannot make %s", shortType(ty))
		_ = ty
	}
	return rd
}

// genericFuncName recognizes fn[T] instantiation (mirrors evalIndex):
// base resolves to a function-ish name and the index is a type name.
func (lo *lowerer) genericFuncName(ix *ast.IndexExpr) (string, bool) {
	id, ok := ix.Index.(*ast.Ident)
	if !ok {
		return "", false
	}
	if !(conversionTypes[id.Name] || lo.structs[id.Name] || lo.enums[id.Name] || lo.aliases[id.Name]) {
		return "", false
	}
	switch base := ix.X.(type) {
	case *ast.Ident:
		if _, _, isLocal := lo.lookup(base.Name); isLocal {
			return "", false
		}
		if lo.funcs[base.Name] || lo.prelude[base.Name] {
			return base.Name + "[" + id.Name + "]", true
		}
	case *ast.FieldExpr:
		if pkgID, ok := base.X.(*ast.Ident); ok {
			if _, _, isLocal := lo.lookup(pkgID.Name); !isLocal && lo.packages[pkgID.Name] {
				return pkgID.Name + "." + base.Name + "[" + id.Name + "]", true
			}
		}
	}
	return "", false
}

func (lo *lowerer) lowerIndex(x *ast.IndexExpr) string {
	// Generic instantiation as a VALUE: `cell[int]` not immediately called.
	if fname, ok := lo.genericFuncName(x); ok {
		rd := lo.newReg()
		lo.emit(x.P.Line, "LOADFN", rd, fname)
		return rd
	}
	rc := lo.lowerExpr(x.X)
	if r, ok := x.Index.(*ast.RangeExpr); ok {
		rr := lo.lowerExpr(r)
		rd := lo.newReg()
		lo.emit(x.P.Line, "SLICE", rd, rc, rr) // range slice THROWS RangeError
		return rd
	}
	ri := lo.lowerExpr(x.Index)
	rd := lo.newReg()
	lo.emit(x.P.Line, "INDEX", rd, rc, ri) // scalar index out of bounds ABORTS
	return rd
}

func (lo *lowerer) lowerField(x *ast.FieldExpr) string {
	// Package member as a value: fmt.printf passed around.
	if base, ok := x.X.(*ast.Ident); ok {
		if _, _, isLocal := lo.lookup(base.Name); !isLocal && lo.packages[base.Name] {
			rd := lo.newReg()
			lo.emit(x.P.Line, "LOADFN", rd, base.Name+"."+x.Name)
			return rd
		}
	}
	ro := lo.lowerExpr(x.X)
	rd := lo.newReg()
	lo.emit(x.P.Line, "FIELD", rd, ro, x.Name)
	return rd
}

func (lo *lowerer) lowerComposite(x *ast.CompositeLit) string {
	name := x.Type.Name
	if x.Type.Pkg != "" {
		name = x.Type.Pkg + "." + name
	}
	if len(x.Type.Args) > 0 {
		var args []string
		for _, a := range x.Type.Args {
			args = append(args, shortType(a))
		}
		name += "[" + strings.Join(args, ",") + "]"
	}
	var fields []string
	for _, f := range x.Fields {
		r := lo.lowerExpr(f.Value)
		if f.Name != "" {
			fields = append(fields, f.Name+":"+r)
		} else {
			fields = append(fields, r)
		}
	}
	rd := lo.newReg()
	op := "NEWSTRUCT"
	// Keyed by the type's DECLARED name — which, in a flattened program, is the
	// qualified one (`low.LowerError`). Stripping a qualifier before the lookup
	// would make `pkg.Foo{}` construct a plain struct that `throw` then aborts
	// on, or the reverse.
	lookup := x.Type.Name
	if x.Type.Pkg != "" {
		lookup = x.Type.Pkg + "." + x.Type.Name
	}
	if lo.exceptions[lookup] {
		op = "NEWEXC"
	}
	lo.emit(x.P.Line, op, rd, name, "("+strings.Join(fields, ",")+")")
	return rd
}

// lowerSpawn: three forms (frozen decision; mirrors evalSpawn).
func (lo *lowerer) lowerSpawn(x *ast.SpawnExpr) string {
	line := x.P.Line

	// Form 1: spawn f(a, b) with a directly-named callee — the callee and
	// arguments evaluate NOW, in the parent task.
	if call, ok := x.Call.(*ast.CallExpr); ok {
		if id, ok := call.Fun.(*ast.Ident); ok {
			if _, _, isLocal := lo.lookup(id.Name); !isLocal && (lo.funcs[id.Name] || lo.prelude[id.Name]) {
				args := lo.lowerArgs(call)
				rd := lo.newReg()
				lo.emit(line, "SPAWN", rd, id.Name, args)
				return rd
			}
		}
		if fe, ok := call.Fun.(*ast.FieldExpr); ok {
			if base, ok := fe.X.(*ast.Ident); ok {
				if _, _, isLocal := lo.lookup(base.Name); !isLocal && lo.packages[base.Name] {
					args := lo.lowerArgs(call)
					rd := lo.newReg()
					lo.emit(line, "SPAWN", rd, base.Name+"."+fe.Name, args)
					return rd
				}
			}
		}
	}

	// Forms 2 and 3: block body, or any other expression — a synthetic
	// closure run by the task. (Stage-0 listing note: for non-simple calls
	// the arguments evaluate at task start, inside the closure.)
	var body *ast.Block
	if x.Block != nil {
		body = x.Block
	} else {
		body = &ast.Block{Stmts: []ast.Stmt{&ast.ExprStmt{X: x.Call, P: x.P}}, P: x.P}
		lo.note("spawn args evaluate at task start in this form")
	}
	name := lo.lowerClosure(nil, body, "spawn body", line)
	rc := lo.newReg()
	lo.emit(line, "MKCLOS", rc, name)
	rd := lo.newReg()
	lo.emit(line, "SPAWNR", rd, rc)
	return rd
}

// ---------------------------------------------------------------- rendering helpers

// shortType renders a type expression compactly for operands and DSECTs.
func shortType(te ast.TypeExpr) string {
	switch ty := te.(type) {
	case nil:
		return "?"
	case *ast.NamedType:
		s := ty.Name
		if ty.Pkg != "" {
			s = ty.Pkg + "." + s
		}
		if len(ty.Args) > 0 {
			var args []string
			for _, a := range ty.Args {
				args = append(args, shortType(a))
			}
			s += "[" + strings.Join(args, ",") + "]"
		}
		return s
	case *ast.OptionType:
		return "?" + shortType(ty.Elem)
	case *ast.ResultType:
		return shortType(ty.Elem) + "!"
	case *ast.SliceType:
		return "[]" + shortType(ty.Elem)
	case *ast.MapType:
		return "map[" + shortType(ty.Key) + "]" + shortType(ty.Value)
	case *ast.SetType:
		return "set[" + shortType(ty.Elem) + "]"
	case *ast.ChanType:
		return "chan[" + shortType(ty.Elem) + "]"
	case *ast.BoxType:
		kind := map[ast.BoxKind]string{ast.SharedBox: "shared", ast.CellBox: "cell", ast.WeakBox: "weak"}[ty.Kind]
		return kind + "[" + shortType(ty.Elem) + "]"
	case *ast.FuncType:
		return "fn(...)"
	case *ast.TupleType:
		var es []string
		for _, e := range ty.Elems {
			es = append(es, shortType(e))
		}
		return "(" + strings.Join(es, ",") + ")"
	case *ast.BorrowType:
		return "&" + shortType(ty.Elem)
	}
	return "?"
}

// literalValue normalizes a literal pattern's value: the kind and the exact
// text the constant pool should hold (decoded, not re-quoted).
func literalValue(e ast.Expr) (ConstKind, string) {
	neg := false
	if u, ok := e.(*ast.UnaryExpr); ok && u.Op == lexer.MINUS {
		neg = true
		e = u.X
	}
	lit, ok := e.(*ast.BasicLit)
	if !ok {
		return CNil, "nil"
	}
	sign := ""
	if neg {
		sign = "-"
	}
	switch lit.Kind {
	case ast.IntLit:
		n, err := strconv.ParseInt(lit.Text, 0, 64)
		if err != nil {
			return CInt, "0"
		}
		if neg {
			n = -n
		}
		return CInt, strconv.FormatInt(n, 10)
	case ast.FloatLit:
		f, err := strconv.ParseFloat(lit.Text, 64)
		if err != nil {
			return CFloat, "0"
		}
		if neg {
			f = -f
		}
		return CFloat, strconv.FormatFloat(f, 'g', -1, 64)
	case ast.DecLit:
		return CDec, sign + lit.Text
	case ast.StrLit:
		return CStr, lit.Text
	case ast.RuneLit:
		return CRune, lit.Text
	case ast.BoolLit:
		return CBool, lit.Text
	}
	return CNil, "nil"
}

// patStr renders a structured pattern as a PMATCH operand.
func patStr(p *Pattern) string { return "'" + patInner(p) + "'" }

func patInner(p *Pattern) string {
	switch p.Kind {
	case PatWildcard:
		return "_"
	case PatNil:
		return "nil"
	case PatLiteral:
		return p.Text
	case PatBind:
		return p.Name
	case PatVariant:
		if len(p.Elems) == 0 {
			return p.Name
		}
		var subs []string
		for _, e := range p.Elems {
			subs = append(subs, patInner(e))
		}
		return p.Name + "(" + strings.Join(subs, ",") + ")"
	}
	return "?"
}

// exprText renders small expressions (literals, negations) as text for
// pattern operands and DSECT default commentary.
func exprText(e ast.Expr) string {
	switch x := e.(type) {
	case *ast.BasicLit:
		if x.Kind == ast.StrLit {
			return `"` + x.Text + `"`
		}
		if x.Kind == ast.RuneLit {
			return "'" + x.Text + "'"
		}
		if x.Kind == ast.DecLit {
			return x.Text + "d"
		}
		return x.Text
	case *ast.UnaryExpr:
		return "-" + exprText(x.X)
	case *ast.Ident:
		return x.Name
	case *ast.SliceLit:
		return "[...]"
	case *ast.CallExpr:
		return exprText(x.Fun) + "(...)"
	case *ast.FieldExpr:
		return exprText(x.X) + "." + x.Name
	}
	return "..."
}

// funcSig renders a declaration signature for the CSECT header comment.
func funcSig(d *ast.FuncDecl) string {
	var ps []string
	if d.HasSelf {
		switch d.SelfMode {
		case ast.ModeMut:
			ps = append(ps, "mut self")
		case ast.ModeOwn:
			ps = append(ps, "own self")
		default:
			ps = append(ps, "self")
		}
	}
	for _, p := range d.Params {
		s := p.Name
		switch p.Mode {
		case ast.ModeMut:
			s = "mut " + s
		case ast.ModeOwn:
			s = "own " + s
		}
		if p.Variadic {
			s += " ..."
		}
		if p.Type != nil {
			s += " " + shortType(p.Type)
		}
		ps = append(ps, s)
	}
	sig := "func " + d.Name + "(" + strings.Join(ps, ", ") + ")"
	if len(d.Results) == 1 {
		sig += " " + shortType(d.Results[0])
	} else if len(d.Results) > 1 {
		var rs []string
		for _, r := range d.Results {
			rs = append(rs, shortType(r))
		}
		sig += " (" + strings.Join(rs, ", ") + ")"
	}
	if d.NoThrow {
		sig += " nothrow"
	}
	return sig
}

func closureSigText(x *ast.ClosureExpr) string {
	var ps []string
	for _, p := range x.Params {
		ps = append(ps, p.Name)
	}
	return "(" + strings.Join(ps, ", ") + ")"
}
