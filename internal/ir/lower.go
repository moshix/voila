package ir

import (
	"fmt"
	"strconv"
	"strings"

	"voila/internal/ast"
	"voila/internal/diag"
	"voila/internal/interp"
)

// lowerPanic unwinds the lowering on the first hard error.
type lowerPanic struct{ err *LowerError }

// scopeLevel is one lexical scope. boundary marks a function frame edge:
// name lookups crossing it resolve as upvalues (^name), mirroring the
// interpreter's by-reference closure capture.
type scopeLevel struct {
	vars     map[string]string // name → register
	boundary bool
}

type loopCtx struct {
	name       string
	breakL     string
	contL      string
	finDepth   int // fins stack depth when the loop began
	scopeDepth int
}

// finRegion tracks one try statement for exit-edge duplication: a
// return/break/continue leaving the region must EHPOP (while the protected
// body is active) and run the finally block (frozen decision: duplicated
// per exit edge).
type finRegion struct {
	finally  *ast.Block
	ehActive bool
	loopSeen int // loops stack depth when pushed

	// A group region: leaving it early must still join and deliver, exactly
	// as the interpreter's execGroup does on a body panic.
	group    bool
	tryGroup bool
}

type switchCtx struct {
	bodyLabels []string
	current    int
}

type variantInfo struct {
	enum    string
	payload bool
}

type lowerer struct {
	mod  *Module
	file *ast.File

	// Global classification tables (source order only; never interp maps).
	globals    map[string]bool
	funcs      map[string]bool
	funcParams map[string][]string // callee → parameter names, for named args
	variants   map[string]variantInfo
	structs    map[string]bool
	enums      map[string]bool
	exceptions map[string]bool
	aliases    map[string]bool
	prelude    map[string]bool
	packages   map[string]bool

	// Per-function state (saved/restored across synthetic closures).
	fn         *Func
	nextReg    int
	nextLabel  int
	pendLabels []string
	scopes     []*scopeLevel
	loops      []loopCtx
	fins       []*finRegion
	switches   []*switchCtx
	catchRegs  []string
	curLine    int
	curFn      string
	closureSeq map[string]int
	inGlobal   bool // lowering $INIT/$SCRIPT: top-level declarations are globals
}

func (lo *lowerer) fail(pos diag.Pos, format string, args ...any) {
	panic(lowerPanic{&LowerError{Line: pos.Line, Col: pos.Col, Msg: fmt.Sprintf(format, args...)}})
}

// ---------------------------------------------------------------- collect

func (lo *lowerer) collect() {
	lo.globals = map[string]bool{}
	lo.funcs = map[string]bool{}
	lo.funcParams = map[string][]string{}
	lo.variants = map[string]variantInfo{}
	lo.structs = map[string]bool{}
	lo.enums = map[string]bool{}
	lo.exceptions = map[string]bool{}
	lo.aliases = map[string]bool{}
	lo.prelude = map[string]bool{}
	lo.packages = map[string]bool{}
	lo.closureSeq = map[string]int{}

	for _, n := range interp.PreludeNames() {
		lo.prelude[n] = true
	}
	for _, n := range interp.PackageNames() {
		lo.packages[n] = true
	}
	// The predeclared exception types are constructible and catchable just
	// like user ones (§8.2); the runtime knows their field layouts.
	for _, n := range []string{
		"ConvError", "RangeError", "OverflowError", "FormatError", "ValueError",
		"RuntimeError", "IOError", "KeyError", "Timeout", "Cancelled",
	} {
		lo.exceptions[n] = true
	}

	for _, d := range lo.file.Decls {
		switch decl := d.(type) {
		case *ast.FuncDecl:
			lo.funcs[decl.Name] = true
			lo.funcParams[decl.Name] = paramNames(decl)
		case *ast.StructDecl:
			lo.structs[decl.Name] = true
		case *ast.EnumDecl:
			lo.enums[decl.Name] = true
			for _, v := range decl.Variants {
				lo.variants[v.Name] = variantInfo{enum: decl.Name, payload: len(v.Fields) > 0}
			}
		case *ast.ExceptionDecl:
			lo.exceptions[decl.Name] = true
		case *ast.TypeDecl:
			if decl.Struct != nil {
				lo.structs[decl.Name] = true
			} else {
				lo.aliases[decl.Name] = true
			}
		case *ast.ImplDecl:
			tn := namedTypeName(decl.Type)
			for _, m := range decl.Methods {
				lo.funcParams[tn+"."+m.Name] = paramNames(m)
			}
		case *ast.GlobalDecl:
			for _, n := range bindingNames(decl.Stmt) {
				lo.globals[n] = true
			}
		}
	}
	// Script-form top-level bindings live in the global scope (the
	// interpreter executes script statements in the Globals env).
	for _, s := range lo.file.ScriptStmts {
		for _, n := range bindingNames(s) {
			lo.globals[n] = true
		}
	}
}

func paramNames(d *ast.FuncDecl) []string {
	var ns []string
	for _, p := range d.Params {
		ns = append(ns, p.Name)
	}
	return ns
}

func bindingNames(s ast.Stmt) []string {
	switch st := s.(type) {
	case *ast.LetStmt:
		return st.Names
	case *ast.VarStmt:
		return []string{st.Name}
	}
	return nil
}

// ---------------------------------------------------------------- module

func (lo *lowerer) lowerModule() (err error) {
	defer func() {
		if r := recover(); r != nil {
			if lp, ok := r.(lowerPanic); ok {
				err = lp.err
				return
			}
			panic(r)
		}
	}()

	lo.collectTypeDecls()

	// $INIT: global initializers, in declaration order.
	var globalStmts []ast.Stmt
	for _, d := range lo.file.Decls {
		if g, ok := d.(*ast.GlobalDecl); ok {
			globalStmts = append(globalStmts, g.Stmt)
		}
	}
	if len(globalStmts) > 0 {
		lo.lowerSynthetic("$INIT", "module initialization (global bindings)", globalStmts, true)
	}
	if len(lo.file.ScriptStmts) > 0 {
		lo.lowerSynthetic("$SCRIPT", "script-form top-level statements", lo.file.ScriptStmts, true)
	}

	for _, d := range lo.file.Decls {
		switch decl := d.(type) {
		case *ast.FuncDecl:
			lo.lowerFuncDecl(decl.Name, decl, false)
		case *ast.ImplDecl:
			typeName := namedTypeName(decl.Type)
			for _, m := range decl.Methods {
				lo.lowerFuncDecl(typeName+"."+m.Name, m, true)
			}
		case *ast.ExceptionDecl:
			for _, m := range decl.Methods {
				lo.lowerFuncDecl(decl.Name+"."+m.Name, m, true)
			}
		}
	}
	return nil
}

func namedTypeName(te ast.TypeExpr) string {
	if nt, ok := te.(*ast.NamedType); ok {
		return nt.Name
	}
	return "?"
}

func (lo *lowerer) collectTypeDecls() {
	renderFields := func(fields []*ast.Field) []string {
		var lines []string
		for _, f := range fields {
			line := fmt.Sprintf("%-8s %s", strings.ToUpper(shortType(f.Type)), f.Name)
			if f.Default != nil {
				line += " = " + exprText(f.Default)
			}
			lines = append(lines, line)
		}
		return lines
	}
	for _, d := range lo.file.Decls {
		switch decl := d.(type) {
		case *ast.StructDecl:
			lo.mod.Types = append(lo.mod.Types, TypeDecl{Kind: "STRUCT", Name: decl.Name, Lines: renderFields(decl.Fields)})
		case *ast.TypeDecl:
			if decl.Struct != nil {
				lo.mod.Types = append(lo.mod.Types, TypeDecl{Kind: "STRUCT", Name: decl.Name, Lines: renderFields(decl.Struct.Fields)})
			}
		case *ast.EnumDecl:
			var lines []string
			for _, v := range decl.Variants {
				sig := v.Name
				if len(v.Fields) > 0 {
					var ps []string
					for _, f := range v.Fields {
						ps = append(ps, f.Name+" "+shortType(f.Type))
					}
					sig += "(" + strings.Join(ps, ", ") + ")"
				}
				lines = append(lines, "VARIANT  "+sig)
			}
			lo.mod.Types = append(lo.mod.Types, TypeDecl{Kind: "ENUM", Name: decl.Name, Lines: lines})
		case *ast.ExceptionDecl:
			lo.mod.Types = append(lo.mod.Types, TypeDecl{Kind: "EXCEPTION", Name: decl.Name, Lines: renderFields(decl.Fields)})
		}
	}
}

// ---------------------------------------------------------------- functions

// fnState snapshots per-function lowering state around synthetic closures.
type fnState struct {
	fn         *Func
	nextReg    int
	nextLabel  int
	pendLabels []string
	loops      []loopCtx
	fins       []*finRegion
	switches   []*switchCtx
	catchRegs  []string
	curLine    int
	curFn      string
	inGlobal   bool
	scopeLen   int
}

func (lo *lowerer) saveFn() fnState {
	return fnState{
		fn: lo.fn, nextReg: lo.nextReg, nextLabel: lo.nextLabel,
		pendLabels: lo.pendLabels, loops: lo.loops, fins: lo.fins,
		switches: lo.switches, catchRegs: lo.catchRegs,
		curLine: lo.curLine, curFn: lo.curFn, inGlobal: lo.inGlobal,
		scopeLen: len(lo.scopes),
	}
}

func (lo *lowerer) restoreFn(s fnState) {
	lo.fn, lo.nextReg, lo.nextLabel = s.fn, s.nextReg, s.nextLabel
	lo.pendLabels, lo.loops, lo.fins = s.pendLabels, s.loops, s.fins
	lo.switches, lo.catchRegs = s.switches, s.catchRegs
	lo.curLine, lo.curFn, lo.inGlobal = s.curLine, s.curFn, s.inGlobal
	lo.scopes = lo.scopes[:s.scopeLen]
}

func (lo *lowerer) lowerFuncDecl(name string, decl *ast.FuncDecl, isMethod bool) {
	saved := lo.saveFn()
	lo.fn = &Func{Name: name, Sig: funcSig(decl), SrcLine: decl.P.Line}
	lo.nextReg, lo.nextLabel = 0, 0
	lo.pendLabels, lo.loops, lo.fins, lo.switches, lo.catchRegs = nil, nil, nil, nil, nil
	lo.curLine = 0
	lo.curFn = name
	lo.inGlobal = false
	lo.mod.Funcs = append(lo.mod.Funcs, lo.fn)

	lo.pushScope(true)
	if decl.HasSelf {
		lo.declare("self")
	}
	for _, p := range decl.Params {
		lo.declare(p.Name)
	}
	// Callee-side calling convention: defaults and variadic packing are
	// function prologue (mirrors bindParams).
	for _, p := range decl.Params {
		if p.Variadic {
			lo.emit(decl.P.Line, "VARARG", lo.lookupLocal(p.Name))
		}
		if p.Default != nil {
			done := lo.newLabel()
			lo.emit(decl.P.Line, "ARGDEF", lo.lookupLocal(p.Name), done)
			rv := lo.lowerExpr(p.Default)
			lo.emit(decl.P.Line, "MOVE", lo.lookupLocal(p.Name), rv)
			lo.mark(done)
		}
	}
	if decl.Body != nil {
		lo.lowerBody(decl.Body, false)
	}
	lo.emit(0, "RET")
	lo.fn.NRegs = lo.nextReg
	lo.popScope()
	lo.restoreFn(saved)
}

// lowerSynthetic lowers $INIT / $SCRIPT, whose top-level bindings are globals.
func (lo *lowerer) lowerSynthetic(name, sig string, stmts []ast.Stmt, global bool) {
	saved := lo.saveFn()
	lo.fn = &Func{Name: name, Sig: sig}
	lo.nextReg, lo.nextLabel = 0, 0
	lo.pendLabels, lo.loops, lo.fins, lo.switches, lo.catchRegs = nil, nil, nil, nil, nil
	lo.curFn = name
	lo.inGlobal = global
	lo.mod.Funcs = append(lo.mod.Funcs, lo.fn)

	lo.pushScope(true)
	for _, s := range stmts {
		lo.lowerStmt(s)
	}
	lo.emit(0, "RET")
	lo.fn.NRegs = lo.nextReg
	lo.popScope()
	lo.restoreFn(saved)
}

// lowerClosure lowers a closure/defer/spawn body into a synthetic function
// and returns its name. Closures capture the enclosing frame by reference:
// the new scope is pushed ON TOP of the current stack with a boundary, so
// outer names render as ^upvalues.
func (lo *lowerer) lowerClosure(params []*ast.Param, body *ast.Block, note string, srcLine int) string {
	outer := lo.curFn
	lo.closureSeq[outer]++
	name := fmt.Sprintf("%s$fn%d", outer, lo.closureSeq[outer])

	saved := lo.saveFn()
	lo.fn = &Func{Name: name, Sig: note, SrcLine: srcLine}
	lo.nextReg, lo.nextLabel = 0, 0
	lo.pendLabels, lo.loops, lo.fins, lo.switches, lo.catchRegs = nil, nil, nil, nil, nil
	lo.curFn = name
	lo.inGlobal = false
	lo.mod.Funcs = append(lo.mod.Funcs, lo.fn)

	lo.pushScope(true) // boundary: outer locals become ^upvalues
	for _, p := range params {
		lo.declare(p.Name)
	}
	for _, p := range params {
		if p.Variadic {
			lo.emit(srcLine, "VARARG", lo.lookupLocal(p.Name))
		}
		if p.Default != nil {
			done := lo.newLabel()
			lo.emit(srcLine, "ARGDEF", lo.lookupLocal(p.Name), done)
			rv := lo.lowerExpr(p.Default)
			lo.emit(srcLine, "MOVE", lo.lookupLocal(p.Name), rv)
			lo.mark(done)
		}
	}
	last := lo.lowerBody(body, true)
	if last != "" {
		lo.emit(0, "RET", last) // implicit trailing-expression value (§10)
	} else {
		lo.emit(0, "RET")
	}
	lo.fn.NRegs = lo.nextReg
	// Does this closure capture a variable declared inside an enclosing loop?
	// Each iteration is a fresh binding to the interpreter, but a single frame
	// slot to a frame-capturing backend — so the fact must travel in the IR.
	if len(saved.loops) > 0 {
		inner := saved.loops[len(saved.loops)-1]
		// UpOrder, not the map: a Go map's iteration order is random, so a
		// closure capturing two loop-declared variables would name a different
		// one on every run — and the compiler's output would not be
		// reproducible.
		for _, name := range lo.fn.UpOrder {
			if up := lo.fn.Ups[name]; up.Scope >= inner.scopeDepth {
				lo.fn.LoopCapture = up.Name
				break
			}
		}
	}
	lo.popScope()
	lo.restoreFn(saved)
	return name
}

// ---------------------------------------------------------------- emit

func (lo *lowerer) emit(line int, op string, args ...string) *Instr {
	in := Instr{Op: op, Args: args, Src: line}
	if len(lo.pendLabels) > 0 {
		in.Labels = lo.pendLabels
		lo.pendLabels = nil
	}
	lo.fn.Instrs = append(lo.fn.Instrs, in)
	return &lo.fn.Instrs[len(lo.fn.Instrs)-1]
}

// note emits a pure comment line into the instruction stream.
func (lo *lowerer) note(text string) {
	lo.fn.Instrs = append(lo.fn.Instrs, Instr{Op: "*", Args: []string{text}})
}

func (lo *lowerer) newReg() string {
	r := fmt.Sprintf("r%d", lo.nextReg)
	lo.nextReg++
	return r
}

func (lo *lowerer) newLabel() string {
	lo.nextLabel++
	return fmt.Sprintf("L%d", lo.nextLabel)
}

func (lo *lowerer) mark(label string) {
	lo.pendLabels = append(lo.pendLabels, label)
}

// ---------------------------------------------------------------- scopes

func (lo *lowerer) pushScope(boundary bool) {
	lo.scopes = append(lo.scopes, &scopeLevel{vars: map[string]string{}, boundary: boundary})
}

func (lo *lowerer) popScope() { lo.scopes = lo.scopes[:len(lo.scopes)-1] }

// declare binds a fresh register to name in the current scope.
func (lo *lowerer) declare(name string) string {
	r := lo.newReg()
	if name != "_" {
		lo.scopes[len(lo.scopes)-1].vars[name] = r
	}
	return r
}

// lookup resolves a name: local register, ^upvalue (crossed a function
// boundary), or "" if unknown. Crossing a boundary records the upvalue's
// (depth, slot) on the current function, which is what a code generator
// needs — the listing keeps the readable `^name` form.
func (lo *lowerer) lookup(name string) (operand string, isLocal, found bool) {
	depth := 0
	for i := len(lo.scopes) - 1; i >= 0; i-- {
		if r, ok := lo.scopes[i].vars[name]; ok {
			if depth > 0 {
				lo.recordUp(name, depth, r, i)
				return "^" + name, false, true
			}
			return r, true, true
		}
		if lo.scopes[i].boundary {
			depth++
		}
	}
	return "", false, false
}

// recordUp notes an upvalue reference on the function being lowered.
func (lo *lowerer) recordUp(name string, depth int, reg string, scope int) {
	if lo.fn.Ups == nil {
		lo.fn.Ups = map[string]UpRef{}
	}
	if _, seen := lo.fn.Ups[name]; seen {
		return
	}
	lo.fn.UpOrder = append(lo.fn.UpOrder, name)
	slot, err := strconv.Atoi(strings.TrimPrefix(reg, "r"))
	if err != nil {
		lo.fail(diag.Pos{}, "internal: bad upvalue register %q", reg)
	}
	lo.fn.Ups[name] = UpRef{Name: name, Depth: depth, Slot: slot, Scope: scope}
}

func (lo *lowerer) lookupLocal(name string) string {
	op, _, ok := lo.lookup(name)
	if !ok {
		return "r?"
	}
	return op
}

// ---------------------------------------------------------------- blocks

// lowerBody lowers a block's statements in a fresh scope. When wantLast is
// true and the final statement is an expression, its register is returned
// (closure trailing-expression value).
func (lo *lowerer) lowerBody(b *ast.Block, wantLast bool) (last string) {
	lo.pushScope(false)
	defer lo.popScope()

	hasNumeric := false
	for _, s := range b.Stmts {
		if _, ok := s.(*ast.NumericStmt); ok {
			hasNumeric = true
		}
	}
	if hasNumeric {
		lo.emit(b.P.Line, "NDSAVE")
	}
	for i, s := range b.Stmts {
		if wantLast && i == len(b.Stmts)-1 {
			if es, ok := s.(*ast.ExprStmt); ok {
				last = lo.lowerExpr(es.X)
				break
			}
		}
		lo.lowerStmt(s)
	}
	if hasNumeric {
		lo.emit(0, "NDREST")
	}
	return last
}
