package ir

import (
	"fmt"
	"strconv"
	"strings"

	"voila/internal/ast"
	"voila/internal/diag"
	"voila/internal/lexer"
)

func (lo *lowerer) lowerStmt(s ast.Stmt) {
	if s == nil {
		return
	}
	lo.curLine = s.Pos().Line
	switch st := s.(type) {
	case *ast.Block:
		lo.lowerBody(st, false)

	case *ast.LetStmt:
		lo.lowerLet(st)

	case *ast.VarStmt:
		line := st.P.Line
		var rv string
		if st.Value != nil {
			before := lo.nextReg
			rv = lo.lowerExpr(st.Value)
			if !lo.isGlobalBind(st.Name) {
				rv = lo.freshen(rv, line, before)
			}
		} else {
			rv = lo.newReg()
			lo.emit(line, "ZERO", rv, shortType(st.Type))
		}
		lo.bindOrSetGlobal(st.Name, rv, line)

	case *ast.AssignStmt:
		lo.lowerAssign(st)

	case *ast.ExprStmt:
		if be, ok := st.X.(*ast.BinaryExpr); ok && be.Op == lexer.ARROW {
			rch := lo.lowerExpr(be.X)
			rv := lo.lowerExpr(be.Y)
			lo.emit(st.P.Line, "SEND", rch, rv) // sending moves (§9.2)
			return
		}
		lo.lowerExpr(st.X)

	case *ast.IfStmt:
		lo.lowerIf(st)

	case *ast.ForStmt:
		if st.Iter == nil {
			lo.lowerInfiniteFor(st)
		} else {
			lo.lowerIterLoop(st.Label, st.P.Line, st.Iter, "", st.Var, st.Body)
		}

	case *ast.EachStmt:
		if st.B != "" {
			lo.lowerIterLoop(st.Label, st.P.Line, st.Iter, st.A, st.B, st.Body)
		} else {
			lo.lowerIterLoop(st.Label, st.P.Line, st.Iter, "", st.A, st.Body)
		}

	case *ast.WhileStmt:
		lo.lowerWhile(st)

	case *ast.SwitchStmt:
		lo.lowerSwitch(st)

	case *ast.MatchStmt:
		lo.lowerMatch(st)

	case *ast.SelectStmt:
		lo.lowerSelect(st)

	case *ast.GroupStmt:
		lo.lowerGroup(st)

	case *ast.ParseStmt:
		lo.lowerParse(st)

	case *ast.SayStmt:
		// `say` renders each argument AS IT IS EVALUATED (§7.5), so a later
		// argument that mutates an earlier one cannot change what was already
		// printed: `say v, v.pop(), v` shows the pre-pop slice first.
		var regs []string
		for _, a := range st.Args {
			rv := lo.lowerExpr(a)
			rs := lo.newReg()
			lo.emit(st.P.Line, "TOSTR", rs, rv)
			regs = append(regs, rs)
		}
		lo.emit(st.P.Line, "SAY", "("+strings.Join(regs, ",")+")")

	case *ast.DeferStmt:
		// Both forms are LATE-evaluated (callee and args at unwind time),
		// so both lower to a synthetic closure (frozen decision 6).
		body := st.Block
		if body == nil {
			body = &ast.Block{Stmts: []ast.Stmt{&ast.ExprStmt{X: st.Call, P: st.P}}, P: st.P}
		}
		name := lo.lowerClosure(nil, body, "defer body (late-evaluated)", st.P.Line)
		rc := lo.newReg()
		lo.emit(st.P.Line, "MKCLOS", rc, name)
		lo.emit(st.P.Line, "DEFER", rc)

	case *ast.DropStmt:
		lo.emit(st.P.Line, "DROP", st.Name)

	case *ast.ReturnStmt:
		var rv string
		switch len(st.Values) {
		case 0:
		case 1:
			rv = lo.lowerExpr(st.Values[0])
		default:
			var regs []string
			for _, v := range st.Values {
				regs = append(regs, lo.lowerExpr(v))
			}
			rv = lo.newReg()
			lo.emit(st.P.Line, "TUPLE", rv, "("+strings.Join(regs, ",")+")")
		}
		lo.unwindFinallies(0, "return")
		if rv == "" {
			lo.emit(st.P.Line, "RET")
		} else {
			lo.emit(st.P.Line, "RET", rv)
		}

	case *ast.BreakStmt:
		loop := lo.findLoop(st.Label, st.P.Line)
		lo.unwindFinallies(loop.finDepth, "break")
		lo.emit(st.P.Line, "JUMP", loop.breakL)

	case *ast.ContinueStmt:
		loop := lo.findLoop(st.Label, st.P.Line)
		lo.unwindFinallies(loop.finDepth, "continue")
		lo.emit(st.P.Line, "JUMP", loop.contL)

	case *ast.FallthroughStmt:
		if len(lo.switches) == 0 {
			lo.fail(st.P, "fallthrough outside switch")
		}
		sw := lo.switches[len(lo.switches)-1]
		if sw.current+1 >= len(sw.bodyLabels) {
			lo.fail(st.P, "fallthrough in final switch case")
		}
		lo.emit(st.P.Line, "JUMP", sw.bodyLabels[sw.current+1])

	case *ast.NumericStmt:
		lo.emit(st.P.Line, "NUMDIG", fmt.Sprintf("%d", st.Digits))

	case *ast.ThrowStmt:
		if st.Value == nil {
			if len(lo.catchRegs) == 0 {
				lo.fail(st.P, "bare `throw` outside a catch clause")
			}
			lo.emit(st.P.Line, "RETHROW", lo.catchRegs[len(lo.catchRegs)-1])
			return
		}
		rv := lo.lowerExpr(st.Value)
		if st.From != nil {
			rc := lo.lowerExpr(st.From)
			lo.emit(st.P.Line, "THROW", rv, rc)
		} else {
			lo.emit(st.P.Line, "THROW", rv)
		}

	case *ast.TryCatchStmt:
		lo.lowerTryCatch(st)

	default:
		lo.fail(s.Pos(), "cannot lower statement %T", s)
	}
}

// bindOrSetGlobal declares a local binding, or stores a global when
// lowering $INIT/$SCRIPT top-level statements.
func (lo *lowerer) bindOrSetGlobal(name, rv string, line int) {
	if lo.inGlobal && lo.globals[name] && lo.scopeDepthTop() {
		lo.emit(line, "SETGLB", name, rv)
		return
	}
	if name == "_" {
		return
	}
	// Bind the name directly to the value's register. Registers are allocated
	// monotonically, so an expression's result register is normally fresh and
	// owned by nobody else — but see freshen: a bare identifier hands back the
	// register of an EXISTING binding, and binding a second name to it would
	// alias the two.
	lo.scopes[len(lo.scopes)-1].vars[name] = rv
}

// isGlobalBind reports whether this declaration writes a module global rather
// than a frame slot; a global is stored by SETGLB and needs no freshening.
func (lo *lowerer) isGlobalBind(name string) bool {
	return lo.inGlobal && lo.globals[name] && lo.scopeDepthTop()
}

// freshen guarantees that the register about to be bound to a NEW name is one
// no other name already owns. `lowerExpr` of a bare identifier returns that
// identifier's own register (or `^name` for an upvalue), so
//
//	var a = b
//	a = 1        // would write b's register, and b would change too
//
// aliases the two. A MOVE into a fresh register costs one instruction and
// restores the interpreter's semantics, where `var a = b` binds a copy.
func (lo *lowerer) freshen(rv string, line, before int) string {
	if strings.HasPrefix(rv, "r") {
		if n, err := strconv.Atoi(rv[1:]); err == nil && n >= before {
			return rv // produced by this expression; nobody else owns it
		}
	}
	r := lo.newReg()
	lo.emit(line, "MOVE", r, rv)
	return r
}

// scopeDepthTop reports whether we are at the top-level scope of a
// synthetic global function (one boundary scope + one body scope at most).
func (lo *lowerer) scopeDepthTop() bool { return len(lo.scopes) <= 2 }

func (lo *lowerer) lowerLet(st *ast.LetStmt) {
	line := st.P.Line
	// Comma-ok receive: exactly the interpreter's syntactic test (§9.2).
	if len(st.Names) == 2 {
		if ux, ok := st.Value.(*ast.UnaryExpr); ok && ux.Op == lexer.ARROW {
			rch := lo.lowerExpr(ux.X)
			rv, rok := lo.newReg(), lo.newReg()
			lo.emit(line, "RECVOK", rv, rok, rch)
			lo.bindOrSetGlobal(st.Names[0], rv, line)
			lo.bindOrSetGlobal(st.Names[1], rok, line)
			return
		}
	}
	before := lo.nextReg
	rv := lo.lowerExpr(st.Value)
	if len(st.Names) == 1 {
		if !lo.isGlobalBind(st.Names[0]) {
			rv = lo.freshen(rv, line, before)
		}
		lo.bindOrSetGlobal(st.Names[0], rv, line)
		return
	}
	var regs []string
	for _, n := range st.Names {
		if n == "_" {
			regs = append(regs, "_")
			continue
		}
		regs = append(regs, lo.newReg())
	}
	lo.emit(line, "UNTUPLE", "("+strings.Join(regs, ",")+")", rv)
	for i, n := range st.Names {
		if n != "_" {
			lo.bindOrSetGlobal(n, regs[i], line)
		}
	}
}

var compoundOps = map[lexer.Kind]string{
	lexer.PLUS_ASSIGN:    "ADD",
	lexer.MINUS_ASSIGN:   "SUB",
	lexer.STAR_ASSIGN:    "MUL",
	lexer.SLASH_ASSIGN:   "DIV",
	lexer.PERCENT_ASSIGN: "MOD",
	lexer.CONCAT_ASSIGN:  "CAT",
}

func (lo *lowerer) lowerAssign(st *ast.AssignStmt) {
	line := st.P.Line
	rv := lo.lowerExpr(st.RHS)
	target := st.LHS[0]

	switch e := target.(type) {
	case *ast.Ident:
		op, isLocal, found := lo.lookup(e.Name)
		switch {
		case found && isLocal:
			if st.Op == lexer.ASSIGN {
				lo.emit(line, "MOVE", op, rv)
			} else {
				lo.emit(line, compoundOps[st.Op], op, op, rv)
			}
		case found: // upvalue
			if st.Op == lexer.ASSIGN {
				lo.emit(line, "UPSET", e.Name, rv)
			} else {
				rt := lo.newReg()
				lo.emit(line, compoundOps[st.Op], rt, "^"+e.Name, rv)
				lo.emit(line, "UPSET", e.Name, rt)
			}
		case lo.globals[e.Name]:
			if st.Op == lexer.ASSIGN {
				lo.emit(line, "SETGLB", e.Name, rv)
			} else {
				rc := lo.newReg()
				lo.emit(line, "GETGLB", rc, e.Name)
				rt := lo.newReg()
				lo.emit(line, compoundOps[st.Op], rt, rc, rv)
				lo.emit(line, "SETGLB", e.Name, rt)
			}
		default:
			lo.fail(e.P, "assignment to undeclared variable `%s`", e.Name)
		}

	case *ast.FieldExpr:
		// Single receiver evaluation (documented divergence: the interpreter
		// evaluates the receiver twice for compound ops).
		ro := lo.lowerExpr(e.X)
		if st.Op == lexer.ASSIGN {
			lo.emit(line, "SETFLD", ro, e.Name, rv)
		} else {
			rc := lo.newReg()
			lo.emit(line, "FIELD", rc, ro, e.Name)
			rt := lo.newReg()
			lo.emit(line, compoundOps[st.Op], rt, rc, rv)
			lo.emit(line, "SETFLD", ro, e.Name, rt)
		}

	case *ast.IndexExpr:
		rc := lo.lowerExpr(e.X)
		ri := lo.lowerExpr(e.Index)
		if st.Op == lexer.ASSIGN {
			lo.emit(line, "SETIDX", rc, ri, rv)
		} else {
			rcur := lo.newReg()
			lo.emit(line, "INDEX", rcur, rc, ri)
			rt := lo.newReg()
			lo.emit(line, compoundOps[st.Op], rt, rcur, rv)
			lo.emit(line, "SETIDX", rc, ri, rt)
		}

	default:
		lo.fail(target.Pos(), "invalid assignment target")
	}
}

func (lo *lowerer) lowerIf(st *ast.IfStmt) {
	elseL := lo.newLabel()
	endL := lo.newLabel()

	if st.Pattern != nil {
		rs := lo.lowerExpr(st.Value)
		lo.pushScope(false)
		pat := lo.buildPattern(st.Pattern)
		binds := patBinds(pat)
		rf := lo.newReg()
		lo.emit(st.P.Line, "PMATCH", rf, rs, patStr(pat), "("+strings.Join(binds, ",")+")").Pat = pat
		lo.emit(st.P.Line, "JMPF", rf, elseL)
		lo.lowerBody(st.Then, false)
		lo.popScope() // pattern binds visible to Then only
	} else {
		rc := lo.lowerExpr(st.Cond)
		lo.emit(st.P.Line, "JMPF", rc, elseL)
		lo.lowerBody(st.Then, false)
	}

	if st.Else != nil {
		lo.emit(0, "JUMP", endL)
		lo.mark(elseL)
		lo.lowerStmt(st.Else)
		lo.mark(endL)
	} else {
		lo.mark(elseL)
	}
	// A dangling endL with no else: both labels may mark the next instr.
	if st.Else == nil {
		lo.mark(endL)
	}
}

func (lo *lowerer) lowerInfiniteFor(st *ast.ForStmt) {
	head := lo.newLabel()
	end := lo.newLabel()
	lo.pushLoop(st.Label, end, head)
	lo.mark(head)
	lo.emit(st.P.Line, "CKCANC") // cancellation poll (mirrors the interpreter)
	lo.lowerBody(st.Body, false)
	lo.emit(0, "JUMP", head)
	lo.mark(end)
	lo.popLoop()
}

func (lo *lowerer) lowerWhile(st *ast.WhileStmt) {
	head := lo.newLabel()
	end := lo.newLabel()
	lo.pushLoop(st.Label, end, head)
	lo.mark(head)
	lo.emit(st.P.Line, "CKCANC")
	rc := lo.lowerExpr(st.Cond)
	lo.emit(st.P.Line, "JMPF", rc, end)
	lo.lowerBody(st.Body, false)
	lo.emit(0, "JUMP", head)
	lo.mark(end)
	lo.popLoop()
}

// lowerIterLoop lowers `for v in X` / `each [k,] v in X` via ITER/ITNEXT.
func (lo *lowerer) lowerIterLoop(label string, line int, iter ast.Expr, kName, vName string, body *ast.Block) {
	rsrc := lo.lowerExpr(iter)
	rit := lo.newReg()
	lo.emit(line, "ITER", rit, rsrc)
	head := lo.newLabel()
	end := lo.newLabel()
	lo.pushLoop(label, end, head)
	lo.mark(head)
	lo.pushScope(false)
	rk, rv := lo.newReg(), lo.newReg()
	lo.emit(line, "ITNEXT", rk, rv, rit, end)
	if kName != "" && kName != "_" {
		lo.scopes[len(lo.scopes)-1].vars[kName] = rk
	}
	if vName != "" && vName != "_" {
		lo.scopes[len(lo.scopes)-1].vars[vName] = rv
	}
	lo.lowerBody(body, false)
	lo.popScope()
	lo.emit(0, "JUMP", head)
	lo.mark(end)
	lo.popLoop()
}

func (lo *lowerer) pushLoop(name, breakL, contL string) {
	lo.loops = append(lo.loops, loopCtx{
		name: name, breakL: breakL, contL: contL,
		finDepth: len(lo.fins), scopeDepth: len(lo.scopes),
	})
}

func (lo *lowerer) popLoop() { lo.loops = lo.loops[:len(lo.loops)-1] }

func (lo *lowerer) findLoop(label string, line int) loopCtx {
	if len(lo.loops) == 0 {
		lo.fail(posOnLine(line), "break/continue outside a loop")
	}
	if label == "" {
		return lo.loops[len(lo.loops)-1]
	}
	for i := len(lo.loops) - 1; i >= 0; i-- {
		if lo.loops[i].name == label {
			return lo.loops[i]
		}
	}
	lo.fail(posOnLine(line), "unknown loop label `%s`", label)
	return loopCtx{}
}

func (lo *lowerer) lowerSwitch(st *ast.SwitchStmt) {
	line := st.P.Line
	rs := lo.lowerExpr(st.Subject)
	end := lo.newLabel()

	// Bodies are labeled in source order; fallthrough jumps to the next
	// body, including default (mirrors execSwitch).
	labels := make([]string, len(st.Cases))
	for i := range st.Cases {
		labels[i] = lo.newLabel()
	}
	defaultL := end
	// Dispatch chain: case values evaluated lazily in source order.
	for i, c := range st.Cases {
		if c.Values == nil {
			defaultL = labels[i]
			continue
		}
		for _, ve := range c.Values {
			rv := lo.lowerExpr(ve)
			rt := lo.newReg()
			lo.emit(line, "CMPEQ", rt, rs, rv)
			lo.emit(line, "JMPT", rt, labels[i])
		}
	}
	lo.emit(line, "JUMP", defaultL)

	sw := &switchCtx{bodyLabels: labels}
	lo.switches = append(lo.switches, sw)
	for i, c := range st.Cases {
		sw.current = i
		lo.mark(labels[i])
		lo.pushScope(false)
		for _, s := range c.Body {
			lo.lowerStmt(s)
		}
		lo.popScope()
		lo.emit(0, "JUMP", end)
	}
	lo.switches = lo.switches[:len(lo.switches)-1]
	lo.mark(end)
}

func (lo *lowerer) lowerMatch(st *ast.MatchStmt) {
	line := st.P.Line
	rs := lo.lowerExpr(st.Subject)
	end := lo.newLabel()

	for _, arm := range st.Arms {
		next := lo.newLabel()
		lo.pushScope(false)
		pat := lo.buildPattern(arm.Pattern)
		binds := patBinds(pat)
		rf := lo.newReg()
		lo.emit(arm.P.Line, "PMATCH", rf, rs, patStr(pat), "("+strings.Join(binds, ",")+")").Pat = pat
		lo.emit(arm.P.Line, "JMPF", rf, next)
		if arm.Guard != nil {
			rg := lo.lowerExpr(arm.Guard) // guard runs in arm scope, after binds
			lo.emit(arm.P.Line, "JMPF", rg, next)
		}
		for _, s := range arm.Body {
			lo.lowerStmt(s)
		}
		lo.popScope()
		lo.emit(0, "JUMP", end)
		lo.mark(next)
	}
	// Exhaustion is a bug: uncatchable abort (§8.6).
	lo.emit(line, "ABORT", "'no match arm matched'")
	lo.mark(end)
}

// buildPattern converts an AST pattern to the structured IR pattern,
// allocating a register for each binding in pattern order.
func (lo *lowerer) buildPattern(p ast.Pattern) *Pattern {
	switch pt := p.(type) {
	case *ast.WildcardPat:
		return &Pattern{Kind: PatWildcard}
	case *ast.NilPat:
		return &Pattern{Kind: PatNil}
	case *ast.LiteralPat:
		kind, text := literalValue(pt.Value)
		return &Pattern{Kind: PatLiteral, Text: exprText(pt.Value), LitKind: kind, LitText: text}
	case *ast.BindPat:
		return &Pattern{Kind: PatBind, Name: pt.Name, Reg: lo.declare(pt.Name)}
	case *ast.VariantPat:
		v := &Pattern{Kind: PatVariant, Name: pt.Name}
		for _, e := range pt.Elems {
			v.Elems = append(v.Elems, lo.buildPattern(e))
		}
		return v
	}
	lo.fail(p.Pos(), "cannot lower pattern %T", p)
	return nil
}

// patBinds lists a pattern's binding registers in pattern order.
func patBinds(p *Pattern) []string {
	var regs []string
	var walk func(*Pattern)
	walk = func(p *Pattern) {
		if p.Kind == PatBind {
			regs = append(regs, p.Reg)
			return
		}
		for _, e := range p.Elems {
			walk(e)
		}
	}
	walk(p)
	return regs
}

func (lo *lowerer) lowerSelect(st *ast.SelectStmt) {
	line := st.P.Line
	end := lo.newLabel()

	// All channel operands and send values evaluate BEFORE blocking (§9.3).
	type armInfo struct {
		label         string
		rch, rsend    string
		bindV, bindOk string
	}
	arms := make([]armInfo, len(st.Cases))
	for i, c := range st.Cases {
		arms[i].label = lo.newLabel()
		switch c.Kind {
		case ast.SelectRecv:
			arms[i].rch = lo.lowerExpr(c.Ch)
			if c.Bind != "" {
				arms[i].bindV = lo.newReg()
			}
			if c.BindOk != "" {
				arms[i].bindOk = lo.newReg()
			}
		case ast.SelectSend:
			arms[i].rch = lo.lowerExpr(c.Ch)
			arms[i].rsend = lo.lowerExpr(c.Send)
		}
	}

	lo.emit(line, "SELBEG")
	for i, c := range st.Cases {
		a := arms[i]
		switch c.Kind {
		case ast.SelectRecv:
			args := []string{a.label, a.rch}
			if a.bindV != "" {
				args = append(args, c.Bind+"="+a.bindV)
			}
			if a.bindOk != "" {
				args = append(args, c.BindOk+"="+a.bindOk)
			}
			lo.emit(c.P.Line, "SELRECV", args...)
		case ast.SelectSend:
			lo.emit(c.P.Line, "SELSEND", a.label, a.rch, a.rsend)
		case ast.SelectDefault:
			lo.emit(c.P.Line, "SELDEF", a.label)
		}
	}
	lo.emit(line, "SELEND")

	for i, c := range st.Cases {
		lo.mark(arms[i].label)
		lo.pushScope(false)
		if c.Bind != "" && arms[i].bindV != "" {
			lo.scopes[len(lo.scopes)-1].vars[c.Bind] = arms[i].bindV
		}
		if c.BindOk != "" && arms[i].bindOk != "" {
			lo.scopes[len(lo.scopes)-1].vars[c.BindOk] = arms[i].bindOk
		}
		for _, s := range c.Body {
			lo.lowerStmt(s)
		}
		lo.popScope()
		lo.emit(0, "JUMP", end)
	}
	lo.mark(end)
}

func (lo *lowerer) lowerGroup(st *ast.GroupStmt) {
	line := st.P.Line
	if st.Timeout != nil {
		rt := lo.lowerExpr(st.Timeout)
		lo.emit(line, "GRPBEG", rt)
	} else {
		lo.emit(line, "GRPBEG")
	}
	lo.fins = append(lo.fins, &finRegion{group: true, tryGroup: st.Try})
	lo.lowerBody(st.Body, false)
	lo.fins = lo.fins[:len(lo.fins)-1]
	if st.Try {
		lo.emit(0, "GRPENDT") // try group: failure propagates like TRYP
	} else {
		lo.emit(0, "GRPEND") // plain group: failure rethrows here
	}
}

func (lo *lowerer) lowerParse(st *ast.ParseStmt) {
	line := st.P.Line
	rsrc := lo.lowerExpr(st.Src)
	fold := "NONE"
	if st.CaseFold != "" {
		fold = strings.ToUpper(st.CaseFold)
	}

	// A PARSE term can only name a register, so a target that is NOT a plain
	// local — a package-level global, or a variable captured from an enclosing
	// frame — is parsed into a scratch register and written back afterwards.
	type writeback struct {
		name   string
		reg    string
		global bool
	}
	var backs []writeback

	var terms []string
	var structured []Term
	for _, t := range st.Terms {
		switch t.Kind {
		case ast.ParseVar:
			op, isLocal, found := lo.lookup(t.Name)
			switch {
			case found && isLocal:
				// re-target the existing local
			case found: // an upvalue
				op = lo.newReg()
				backs = append(backs, writeback{name: t.Name, reg: op})
			case lo.globals[t.Name]:
				op = lo.newReg()
				backs = append(backs, writeback{name: t.Name, reg: op, global: true})
			default:
				// Not previously bound: implicitly a mutable str local (§7.4).
				op = lo.declare(t.Name)
			}
			terms = append(terms, t.Name+"="+op)
			structured = append(structured, Term{Kind: TermVar, Name: t.Name, Reg: op})
		case ast.ParseDiscard:
			terms = append(terms, ".")
			structured = append(structured, Term{Kind: TermDiscard})
		case ast.ParseLit:
			terms = append(terms, "'"+t.Lit+"'")
			structured = append(structured, Term{Kind: TermLit, Lit: t.Lit})
		case ast.ParseCol:
			terms = append(terms, fmt.Sprintf("%d", t.Col))
			structured = append(structured, Term{Kind: TermCol, Col: t.Col})
		}
	}
	lo.emit(line, "PARSE", rsrc, fold, "("+strings.Join(terms, " ")+")").Terms = structured

	for _, b := range backs {
		if b.global {
			lo.emit(line, "SETGLB", b.name, b.reg)
		} else {
			lo.emit(line, "UPSET", b.name, b.reg)
		}
	}
}

// ---------------------------------------------------------------- try/catch

func (lo *lowerer) lowerTryCatch(st *ast.TryCatchStmt) {
	line := st.P.Line
	catchL := lo.newLabel()
	endL := lo.newLabel()

	region := &finRegion{finally: st.Finally, ehActive: true, loopSeen: len(lo.loops)}
	lo.fins = append(lo.fins, region)

	lo.emit(line, "EHPUSH", catchL)
	lo.lowerBody(st.Body, false)
	lo.emit(0, "EHPOP")
	region.ehActive = false
	lo.runFinally(st.Finally, "normal")
	lo.emit(0, "JUMP", endL)

	// Handler: the unwinder pops the region and materializes the exception.
	lo.mark(catchL)
	re := lo.newReg()
	lo.emit(line, "CATCH", re)
	lo.catchRegs = append(lo.catchRegs, re)

	matched := false
	for _, c := range st.Catches {
		nextC := lo.newLabel()
		if len(c.Types) > 0 {
			var names []string
			for _, te := range c.Types {
				names = append(names, shortType(te))
			}
			rt := lo.newReg()
			lo.emit(c.P.Line, "ISTYPE", rt, re, "'"+strings.Join(names, ",")+"'")
			lo.emit(c.P.Line, "JMPF", rt, nextC)
		} else {
			matched = true // catch-all
		}
		lo.pushScope(false)
		if c.Name != "" {
			lo.scopes[len(lo.scopes)-1].vars[c.Name] = re
		}
		lo.lowerBody(c.Body, false)
		lo.popScope()
		lo.runFinally(st.Finally, "catch")
		lo.emit(0, "JUMP", endL)
		lo.mark(nextC)
		if len(c.Types) == 0 {
			break
		}
	}
	if !matched {
		// No clause matched: finally still runs, then the exception
		// continues unwinding.
		lo.runFinally(st.Finally, "unwind")
		lo.emit(0, "THROW", re)
	}
	lo.catchRegs = lo.catchRegs[:len(lo.catchRegs)-1]
	lo.fins = lo.fins[:len(lo.fins)-1]
	lo.mark(endL)
}

// runFinally lowers one duplicated copy of a finally block (frozen decision:
// duplication per exit edge, each copy commented).
func (lo *lowerer) runFinally(fin *ast.Block, edge string) {
	if fin == nil {
		return
	}
	lo.note("finally (dup: " + edge + ")")
	lo.lowerBody(fin, false)
}

// unwindFinallies emits the EHPOP + finally copies required for a
// return/break/continue leaving try regions. downTo is the fins depth of
// the jump target (0 for return).
func (lo *lowerer) unwindFinallies(downTo int, edge string) {
	for i := len(lo.fins) - 1; i >= downTo; i-- {
		region := lo.fins[i]
		if region.group {
			// A group must be joined before control leaves it (§9.1): no task
			// may outlive the block that spawned it, whatever the exit edge.
			lo.note("group joined on " + edge)
			if region.tryGroup {
				lo.emit(0, "GRPENDT")
			} else {
				lo.emit(0, "GRPEND")
			}
			continue
		}
		if region.ehActive {
			lo.emit(0, "EHPOP")
		}
		lo.runFinally(region.finally, edge)
	}
}

func posOnLine(line int) diag.Pos { return diag.Pos{Line: line} }
