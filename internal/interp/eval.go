package interp

import (
	"fmt"
	"strconv"
	"strings"

	"voila/internal/ast"
	"voila/internal/dec"
	"voila/internal/diag"
	"voila/internal/lexer"
)

// ---------------------------------------------------------------- blocks

// execBlock runs a block in a fresh scope. `numeric digits` set inside the
// block is restored on exit, including unwinding (frozen decision i).
func (t *T) execBlock(parent *Env, b *ast.Block) {
	t.execBody(NewEnv(parent), b)
}

// execBody runs statements in the given env and returns the value of a
// trailing expression statement (nil if none) — closures use it.
func (t *T) execBody(env *Env, b *ast.Block) (last Value) {
	savedDigits := t.digits
	defer func() { t.digits = savedDigits }()
	for i, s := range b.Stmts {
		if i == len(b.Stmts)-1 {
			if es, ok := s.(*ast.ExprStmt); ok {
				last = t.eval(env, es.X)
				return last
			}
		}
		t.execStmt(env, s)
	}
	return nil
}

// ---------------------------------------------------------------- statements

func (t *T) execStmt(env *Env, s ast.Stmt) {
	switch st := s.(type) {
	case *ast.Block:
		t.execBlock(env, st)

	case *ast.LetStmt:
		// `let v, ok = <-ch` — the comma-ok receive form (§9.2).
		if len(st.Names) == 2 {
			if ux, ok := st.Value.(*ast.UnaryExpr); ok && ux.Op == lexer.ARROW {
				ch := t.wantChan(t.eval(env, ux.X), ux.P)
				v, recvOK := t.chanRecv(ch, ux.P)
				env.Define(st.Names[0], v, false)
				env.Define(st.Names[1], BoolV(recvOK), false)
				return
			}
		}
		v := t.eval(env, st.Value)
		t.moveIfNeeded(env, st.Value, v)
		if len(st.Names) == 1 {
			env.Define(st.Names[0], t.bindValue(v), false)
			return
		}
		t.destructure(env, st.Names, v, false, st.P)

	case *ast.VarStmt:
		var v Value
		if st.Value != nil {
			v = t.eval(env, st.Value)
			t.moveIfNeeded(env, st.Value, v)
			v = t.bindValue(v)
		} else {
			v = t.zeroValue(st.Type)
		}
		env.Define(st.Name, v, true)

	case *ast.AssignStmt:
		t.execAssign(env, st)

	case *ast.ExprStmt:
		t.eval(env, st.X)

	case *ast.IfStmt:
		t.execIf(env, st)

	case *ast.ForStmt:
		t.execFor(env, st)

	case *ast.EachStmt:
		t.execEach(env, st)

	case *ast.WhileStmt:
		t.execWhile(env, st)

	case *ast.SwitchStmt:
		t.execSwitch(env, st)

	case *ast.MatchStmt:
		t.execMatch(env, st)

	case *ast.SelectStmt:
		t.execSelect(env, st)

	case *ast.GroupStmt:
		t.execGroup(env, st)

	case *ast.ParseStmt:
		t.execParse(env, st)

	case *ast.SayStmt:
		var parts []string
		for _, a := range st.Args {
			parts = append(parts, t.Str(t.eval(env, a)))
		}
		fmt.Fprintln(t.in.Stdout, strings.Join(parts, " "))

	case *ast.DeferStmt:
		fr := t.currentFrame()
		captured := env
		if st.Block != nil {
			blk := st.Block
			fr.defers = append(fr.defers, &deferred{run: func() { t.execBlock(captured, blk) }})
		} else {
			call := st.Call
			fr.defers = append(fr.defers, &deferred{run: func() { t.eval(captured, call) }})
		}

	case *ast.DropStmt:
		b := env.Lookup(st.Name)
		if b == nil {
			t.rtErr(st.P, "cannot drop unknown binding `%s`", st.Name)
		}
		b.Moved = true
		b.MovedPos = st.P

	case *ast.ReturnStmt:
		switch len(st.Values) {
		case 0:
			panic(ctlReturn{UnitV{}})
		case 1:
			panic(ctlReturn{t.eval(env, st.Values[0])})
		default:
			tp := &Tuple{}
			for _, v := range st.Values {
				tp.Elems = append(tp.Elems, t.eval(env, v))
			}
			panic(ctlReturn{tp})
		}

	case *ast.BreakStmt:
		panic(ctlBreak{label: st.Label})

	case *ast.ContinueStmt:
		panic(ctlContinue{label: st.Label})

	case *ast.FallthroughStmt:
		panic(ctlFallthrough{})

	case *ast.NumericStmt:
		t.digits = st.Digits

	case *ast.ThrowStmt:
		t.execThrow(env, st)

	case *ast.TryCatchStmt:
		t.execTryCatch(env, st)

	default:
		t.rtErr(s.Pos(), "unsupported statement %T", s)
	}
}

// bindValue applies copy semantics: Copy values are (deep-)copied on binding
// so structs behave as value types (§3.4, §6.4).
func (t *T) bindValue(v Value) Value {
	if isCopy(v) {
		return copyValue(v)
	}
	return v
}

// moveIfNeeded marks the source binding moved when a non-Copy value is
// re-bound from a plain identifier (§6.4: everything not Copy is moved).
func (t *T) moveIfNeeded(env *Env, src ast.Expr, v Value) {
	if isCopy(v) {
		return
	}
	if id, ok := src.(*ast.Ident); ok {
		if b := env.Lookup(id.Name); b != nil {
			b.Moved = true
			b.MovedPos = id.P
		}
	}
}

func (t *T) destructure(env *Env, names []string, v Value, mut bool, pos diag.Pos) {
	tp, ok := v.(*Tuple)
	if !ok {
		t.rtErr(pos, "cannot destructure %s into %d names", typeName(v), len(names))
		return
	}
	if len(tp.Elems) != len(names) {
		t.rtErr(pos, "destructuring arity mismatch: %d names, %d values", len(names), len(tp.Elems))
	}
	for i, n := range names {
		env.Define(n, t.bindValue(tp.Elems[i]), mut)
	}
}

func (t *T) execAssign(env *Env, st *ast.AssignStmt) {
	rhs := t.eval(env, st.RHS)
	if st.Op == lexer.ASSIGN {
		t.moveIfNeeded(env, st.RHS, rhs)
		t.assignTo(env, st.LHS[0], t.bindValue(rhs))
		return
	}
	cur := t.eval(env, st.LHS[0])
	var op lexer.Kind
	switch st.Op {
	case lexer.PLUS_ASSIGN:
		op = lexer.PLUS
	case lexer.MINUS_ASSIGN:
		op = lexer.MINUS
	case lexer.STAR_ASSIGN:
		op = lexer.STAR
	case lexer.SLASH_ASSIGN:
		op = lexer.SLASH
	case lexer.PERCENT_ASSIGN:
		op = lexer.PERCENT
	case lexer.CONCAT_ASSIGN:
		op = lexer.CONCAT
	}
	t.assignTo(env, st.LHS[0], t.binaryOp(op, cur, rhs, st.P))
}

func (t *T) assignTo(env *Env, lhs ast.Expr, v Value) {
	switch e := lhs.(type) {
	case *ast.Ident:
		b := env.Lookup(e.Name)
		if b == nil {
			t.rtErr(e.P, "assignment to undeclared variable `%s` (use `var`)", e.Name)
			return
		}
		if !b.Mut {
			t.rtErr(e.P, "cannot assign to immutable binding `%s` (declared with `let`)", e.Name)
		}
		b.Val = v
		b.Moved = false
	case *ast.FieldExpr:
		recv := t.evalForMut(env, e.X)
		switch r := unwrapForField(recv).(type) {
		case *Struct:
			if _, ok := r.Fields[e.Name]; !ok {
				t.rtErr(e.P, "type %s has no field `%s`", r.Type, e.Name)
			}
			r.Fields[e.Name] = v
		case *ErrVal:
			if r.Struct != nil {
				r.Struct.Fields[e.Name] = v
				return
			}
			t.rtErr(e.P, "cannot assign field `%s` on %s", e.Name, typeName(recv))
		default:
			t.rtErr(e.P, "cannot assign field `%s` on %s", e.Name, typeName(recv))
		}
	case *ast.IndexExpr:
		coll := t.eval(env, e.X)
		idx := t.eval(env, e.Index)
		switch c := coll.(type) {
		case *Slice:
			i := t.toInt(idx, e.P)
			if i < 0 || i >= int64(len(c.Elems)) {
				t.abort(e.P, "index out of bounds: %d (len %d)", i, len(c.Elems))
			}
			c.Elems[i] = v
		case *Map:
			c.Set(idx, v)
		default:
			t.rtErr(e.P, "cannot index-assign %s", typeName(coll))
		}
	case *ast.SelfExpr:
		t.rtErr(e.P, "cannot assign to self")
	default:
		t.rtErr(lhs.Pos(), "invalid assignment target")
	}
}

// evalForMut evaluates the receiver of a field assignment without copying.
func (t *T) evalForMut(env *Env, e ast.Expr) Value {
	return t.eval(env, e)
}

func unwrapForField(v Value) Value {
	switch x := v.(type) {
	case *SharedBox:
		return x.V
	case *CellBox:
		return x.V
	}
	return v
}

func (t *T) execIf(env *Env, st *ast.IfStmt) {
	if st.Pattern != nil {
		v := t.eval(env, st.Value)
		child := NewEnv(env)
		if t.matchPattern(child, st.Pattern, v) {
			t.execBody(NewEnv(child), st.Then)
		} else if st.Else != nil {
			t.execStmt(env, st.Else)
		}
		return
	}
	if t.truthy(env, st.Cond) {
		t.execBlock(env, st.Then)
	} else if st.Else != nil {
		t.execStmt(env, st.Else)
	}
}

func (t *T) truthy(env *Env, e ast.Expr) bool {
	v := t.eval(env, e)
	b, ok := v.(BoolV)
	if !ok {
		t.rtErr(e.Pos(), "condition must be bool, got %s (no implicit truthiness)", typeName(v))
	}
	return bool(b)
}

// loopBody runs one iteration body, handling break/continue with labels.
// Returns false when the loop should stop.
func (t *T) loopBody(env *Env, body *ast.Block, label string) (cont bool) {
	defer func() {
		r := recover()
		if r == nil {
			cont = true
			return
		}
		switch v := r.(type) {
		case ctlBreak:
			if v.label == "" || v.label == label {
				cont = false
				return
			}
			panic(r)
		case ctlContinue:
			if v.label == "" || v.label == label {
				cont = true
				return
			}
			panic(r)
		default:
			panic(r)
		}
	}()
	t.execBlock(env, body)
	return true
}

func (t *T) execFor(env *Env, st *ast.ForStmt) {
	if st.Iter == nil {
		for {
			t.checkCancelled(st.P)
			if !t.loopBody(env, st.Body, st.Label) {
				return
			}
		}
	}
	iter := t.eval(env, st.Iter)
	t.iterate(iter, st.P, func(_, v Value) bool {
		child := NewEnv(env)
		child.Define(st.Var, v, false)
		return t.loopBody(child, st.Body, st.Label)
	})
}

func (t *T) execEach(env *Env, st *ast.EachStmt) {
	iter := t.eval(env, st.Iter)
	twoVars := st.B != ""
	t.iterate(iter, st.P, func(k, v Value) bool {
		child := NewEnv(env)
		if twoVars {
			child.Define(st.A, k, false)
			child.Define(st.B, v, false)
		} else {
			child.Define(st.A, v, false)
		}
		return t.loopBody(child, st.Body, st.Label)
	})
}

func (t *T) execWhile(env *Env, st *ast.WhileStmt) {
	for t.truthy(env, st.Cond) {
		t.checkCancelled(st.P)
		if !t.loopBody(env, st.Body, st.Label) {
			return
		}
	}
}

// iterate drives the iteration protocol: ranges, slices, maps, sets,
// strings (runes), channels, files. fn receives (key-or-index, value).
func (t *T) iterate(v Value, pos diag.Pos, fn func(k, v Value) bool) {
	switch x := v.(type) {
	case *RangeVal:
		step := x.By
		if step == 0 {
			t.rtErr(pos, "range step cannot be 0")
		}
		i := int64(0)
		if step > 0 {
			for n := x.Lo; n < x.Hi || (x.Inclusive && n == x.Hi); n += step {
				if !fn(IntV(i), IntV(n)) {
					return
				}
				i++
			}
		} else {
			for n := x.Lo; n > x.Hi || (x.Inclusive && n == x.Hi); n += step {
				if !fn(IntV(i), IntV(n)) {
					return
				}
				i++
			}
		}
	case *Slice:
		for i, e := range x.Elems {
			if !fn(IntV(i), e) {
				return
			}
		}
	case *Map:
		for _, k := range x.Keys() {
			val, _ := x.Get(k)
			if !fn(k, val) {
				return
			}
		}
	case *Set:
		for i, k := range x.keys {
			if !fn(IntV(i), k) {
				return
			}
		}
	case StrV:
		i := 0
		for bi, r := range string(x) {
			if !fn(IntV(bi), RuneV(r)) {
				return
			}
			i++
		}
	case *Chan:
		for {
			val, ok := t.chanRecv(x, pos)
			if !ok {
				return
			}
			if !fn(NilV{}, val) {
				return
			}
		}
	case *Tuple:
		for i, e := range x.Elems {
			if !fn(IntV(i), e) {
				return
			}
		}
	case NilV:
		// iterating nil = zero iterations (REXX comfort)
	default:
		t.rtErr(pos, "cannot iterate over %s", typeName(v))
	}
}

func (t *T) execSwitch(env *Env, st *ast.SwitchStmt) {
	subject := t.eval(env, st.Subject)
	matched := -1
	for i, c := range st.Cases {
		if c.Values == nil {
			continue // default handled after
		}
		for _, ve := range c.Values {
			if equalValues(subject, t.eval(env, ve)) {
				matched = i
				break
			}
		}
		if matched >= 0 {
			break
		}
	}
	if matched < 0 {
		for i, c := range st.Cases {
			if c.Values == nil {
				matched = i
				break
			}
		}
	}
	if matched < 0 {
		return
	}
	// Execute, honouring explicit fallthrough.
	for i := matched; i < len(st.Cases); i++ {
		fell := t.runCaseBody(env, st.Cases[i].Body)
		if !fell {
			return
		}
	}
}

// runCaseBody executes a case body; returns true if it ended in fallthrough.
func (t *T) runCaseBody(env *Env, body []ast.Stmt) (fell bool) {
	defer func() {
		if r := recover(); r != nil {
			if _, ok := r.(ctlFallthrough); ok {
				fell = true
				return
			}
			panic(r)
		}
	}()
	child := NewEnv(env)
	for _, s := range body {
		t.execStmt(child, s)
	}
	return false
}

func (t *T) execMatch(env *Env, st *ast.MatchStmt) {
	subject := t.eval(env, st.Subject)
	for _, arm := range st.Arms {
		child := NewEnv(env)
		if !t.matchPattern(child, arm.Pattern, subject) {
			continue
		}
		if arm.Guard != nil && !t.truthy(child, arm.Guard) {
			continue
		}
		for _, s := range arm.Body {
			t.execStmt(child, s)
		}
		return
	}
	t.abort(st.P, "no match arm matched %s (match must be exhaustive)", t.Str(subject))
}

// matchPattern tries to match v against pat, binding names into env.
func (t *T) matchPattern(env *Env, pat ast.Pattern, v Value) bool {
	switch p := pat.(type) {
	case *ast.WildcardPat:
		return true
	case *ast.NilPat:
		_, isNil := v.(NilV)
		return isNil
	case *ast.LiteralPat:
		return equalValues(v, t.eval(env, p.Value))
	case *ast.BindPat:
		env.Define(p.Name, v, false)
		return true
	case *ast.VariantPat:
		switch p.Name {
		case "Some":
			if _, isNil := v.(NilV); isNil {
				return false
			}
			if len(p.Elems) == 1 {
				return t.matchPattern(env, p.Elems[0], v)
			}
			return len(p.Elems) == 0
		case "Ok":
			if _, isErr := v.(*ErrVal); isErr {
				return false
			}
			if len(p.Elems) == 1 {
				return t.matchPattern(env, p.Elems[0], v)
			}
			return len(p.Elems) == 0
		case "Err":
			e, isErr := v.(*ErrVal)
			if !isErr {
				return false
			}
			if len(p.Elems) == 1 {
				return t.matchPattern(env, p.Elems[0], e)
			}
			return len(p.Elems) == 0
		}
		ev, ok := v.(*EnumVal)
		if !ok {
			// Exception values match their type name in match too.
			if e, isErr := v.(*ErrVal); isErr && e.TypeName == p.Name {
				return true
			}
			return false
		}
		if ev.Variant != p.Name {
			return false
		}
		if len(p.Elems) == 0 {
			return true
		}
		if len(p.Elems) != len(ev.Fields) {
			return false
		}
		for i, sub := range p.Elems {
			if !t.matchPattern(env, sub, ev.Fields[i]) {
				return false
			}
		}
		return true
	}
	return false
}

func (t *T) execThrow(env *Env, st *ast.ThrowStmt) {
	if st.Value == nil {
		// Bare rethrow: find the exception bound by the innermost catch.
		for i := len(t.frames) - 1; i >= 0; i-- {
			if t.frames[i].exc != nil {
				panic(ctlThrow{t.frames[i].exc})
			}
		}
		t.rtErr(st.P, "bare `throw` outside a catch clause")
	}
	v := t.eval(env, st.Value)
	err := t.toErrVal(v, st.P)
	if st.From != nil {
		if cause, ok := t.eval(env, st.From).(*ErrVal); ok {
			err.Cause = cause
			if err.Trace == nil && cause.Trace != nil {
				err.Trace = cause.Trace
			}
		}
	}
	t.throw(err)
}

// toErrVal coerces a thrown value to an exception.
func (t *T) toErrVal(v Value, pos diag.Pos) *ErrVal {
	switch x := v.(type) {
	case *ErrVal:
		return x
	case *Struct:
		return &ErrVal{TypeName: x.Type, Struct: x}
	case StrV:
		return &ErrVal{Msg: string(x)}
	}
	t.rtErr(pos, "cannot throw %s (throw an exception value)", typeName(v))
	return nil
}

func (t *T) execTryCatch(env *Env, st *ast.TryCatchStmt) {
	pending := t.runProtected(env, st)
	// finally runs on every path except aborts/exit (§8.2).
	if st.Finally != nil {
		fpending := func() (r any) {
			defer func() { r = recover() }()
			t.execBlock(env, st.Finally)
			return nil
		}()
		if fpending != nil {
			panic(fpending) // finally's own control flow wins
		}
	}
	if pending != nil {
		panic(pending)
	}
}

// runProtected executes the try body and catch clauses, returning any control
// panic that must continue after finally.
func (t *T) runProtected(env *Env, st *ast.TryCatchStmt) (pending any) {
	bodyPanic := func() (r any) {
		defer func() { r = recover() }()
		t.execBlock(env, st.Body)
		return nil
	}()
	if bodyPanic == nil {
		return nil
	}
	thr, isThrow := bodyPanic.(ctlThrow)
	if !isThrow {
		if _, isAbort := bodyPanic.(ctlAbort); isAbort {
			panic(bodyPanic) // aborts skip catch AND finally
		}
		if _, isExit := bodyPanic.(ctlExit); isExit {
			panic(bodyPanic)
		}
		return bodyPanic // return/break/continue/propagate: run finally, then continue
	}
	for _, c := range st.Catches {
		if !t.catchMatches(c, thr.err) {
			continue
		}
		child := NewEnv(env)
		if c.Name != "" {
			child.Define(c.Name, thr.err, false)
		}
		fr := t.currentFrame()
		savedExc := fr.exc
		fr.exc = thr.err
		catchPanic := func() (r any) {
			defer func() { r = recover() }()
			t.execBody(NewEnv(child), c.Body)
			return nil
		}()
		fr.exc = savedExc
		return catchPanic // nil if the catch completed normally
	}
	return bodyPanic // no clause matched: rethrow after finally
}

func (t *T) catchMatches(c *ast.CatchClause, err *ErrVal) bool {
	if len(c.Types) == 0 {
		return true // catch-all
	}
	for _, te := range c.Types {
		if nt, ok := te.(*ast.NamedType); ok {
			if nt.Name == err.TypeName {
				return true
			}
			if nt.Name == "Error" {
				return true
			}
		}
	}
	return false
}

// ---------------------------------------------------------------- expressions

func (t *T) eval(env *Env, e ast.Expr) Value {
	switch x := e.(type) {
	case *ast.BasicLit:
		return t.evalLit(x)

	case *ast.InterpStr:
		var sb strings.Builder
		for _, part := range x.Parts {
			if part.Expr != nil {
				sb.WriteString(t.Str(t.eval(env, part.Expr)))
			} else {
				sb.WriteString(part.Lit)
			}
		}
		return StrV(sb.String())

	case *ast.Ident:
		return t.evalIdent(env, x)

	case *ast.SelfExpr:
		if b := env.Lookup("self"); b != nil {
			return b.Val
		}
		t.rtErr(x.P, "`self` used outside a method")
		return NilV{}

	case *ast.UnaryExpr:
		return t.evalUnary(env, x)

	case *ast.BinaryExpr:
		return t.evalBinary(env, x)

	case *ast.RangeExpr:
		lo := t.toInt(t.eval(env, x.Lo), x.P)
		hi := t.toInt(t.eval(env, x.Hi), x.P)
		by := int64(1)
		if x.By != nil {
			by = t.toInt(t.eval(env, x.By), x.P)
		}
		return &RangeVal{Lo: lo, Hi: hi, Inclusive: x.Inclusive, By: by}

	case *ast.CallExpr:
		return t.evalCall(env, x)

	case *ast.IndexExpr:
		return t.evalIndex(env, x)

	case *ast.FieldExpr:
		return t.evalField(env, x)

	case *ast.CompositeLit:
		return t.evalComposite(env, x)

	case *ast.SliceLit:
		s := &Slice{}
		for _, el := range x.Elems {
			s.Elems = append(s.Elems, t.eval(env, el))
		}
		return s

	case *ast.TupleExpr:
		if len(x.Elems) == 0 {
			return UnitV{}
		}
		tp := &Tuple{}
		for _, el := range x.Elems {
			tp.Elems = append(tp.Elems, t.eval(env, el))
		}
		return tp

	case *ast.ClosureExpr:
		return t.evalClosure(env, x)

	case *ast.TryExpr:
		v := t.eval(env, x.X)
		switch ev := v.(type) {
		case *ErrVal:
			panic(ctlPropagate{err: ev})
		case NilV:
			panic(ctlPropagate{err: nil})
		}
		return v

	case *ast.MustExpr:
		v := t.eval(env, x.X)
		switch ev := v.(type) {
		case *ErrVal:
			t.throw(ev)
		case NilV:
			t.throwf("ValueError", "must: value is nil")
		}
		return v

	case *ast.AttemptExpr:
		return t.evalAttempt(env, x)

	case *ast.ElseExpr:
		return t.evalElse(env, x)

	case *ast.SpawnExpr:
		return t.evalSpawn(env, x)
	}
	t.rtErr(e.Pos(), "unsupported expression %T", e)
	return NilV{}
}

func (t *T) evalLit(x *ast.BasicLit) Value {
	switch x.Kind {
	case ast.IntLit:
		n, err := strconv.ParseInt(x.Text, 0, 64)
		if err != nil {
			t.rtErr(x.P, "bad integer literal %q", x.Text)
		}
		return IntV(n)
	case ast.FloatLit:
		f, err := strconv.ParseFloat(x.Text, 64)
		if err != nil {
			t.rtErr(x.P, "bad float literal %q", x.Text)
		}
		return FloatV(f)
	case ast.DecLit:
		return DecV{dec.MustParse(x.Text)}
	case ast.StrLit:
		return StrV(x.Text)
	case ast.RuneLit:
		return RuneV([]rune(x.Text)[0])
	case ast.BoolLit:
		return BoolV(x.Text == "true")
	case ast.NilLit:
		return NilV{}
	}
	return NilV{}
}

func (t *T) evalIdent(env *Env, x *ast.Ident) Value {
	if b := env.Lookup(x.Name); b != nil {
		if b.Moved {
			t.rtErr(x.P, "use of moved value `%s` (moved at %d:%d)", x.Name, b.MovedPos.Line, b.MovedPos.Col)
		}
		return b.Val
	}
	// Enum variant without payload used as a value: `Empty`.
	if enum, ok := t.in.Variants[x.Name]; ok {
		for _, v := range enum.Variants {
			if v.Name == x.Name && len(v.Fields) == 0 {
				return &EnumVal{Type: enum.Name, Variant: x.Name}
			}
		}
		// Payload variant referenced as constructor.
		return t.variantCtor(enum, x.Name)
	}
	if fn, ok := t.in.prelude[x.Name]; ok {
		return fn
	}
	// Type names win over package names: `str(42)` is the conversion;
	// `str.upper(s)` reaches the package through the field-access path.
	if isTypeName(x.Name) || t.in.Structs[x.Name] != nil || t.in.Enums[x.Name] != nil ||
		t.in.Exceptions[x.Name] != nil || t.in.Aliases[x.Name] != nil {
		return &TypeV{Name: x.Name}
	}
	if pkg, ok := t.in.packages[x.Name]; ok {
		return pkg
	}
	if x.Name == "ctx" {
		return &CtxV{t: t}
	}
	t.rtErr(x.P, "unknown identifier `%s`", x.Name)
	return NilV{}
}

func isTypeName(name string) bool {
	switch name {
	case "int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
		"byte", "float", "f32", "dec", "bool", "str", "rune", "unit":
		return true
	}
	return false
}

func (t *T) variantCtor(enum *ast.EnumDecl, name string) Value {
	var variant *ast.Variant
	for _, v := range enum.Variants {
		if v.Name == name {
			variant = v
		}
	}
	return &Builtin{Name: name, Fn: func(t *T, args []Value) Value {
		ev := &EnumVal{Type: enum.Name, Variant: name}
		for i, f := range variant.Fields {
			ev.Names = append(ev.Names, f.Name)
			if i < len(args) {
				ev.Fields = append(ev.Fields, args[i])
			} else {
				ev.Fields = append(ev.Fields, t.zeroValue(f.Type))
			}
		}
		return ev
	}}
}

func (t *T) evalUnary(env *Env, x *ast.UnaryExpr) Value {
	if x.Op == lexer.ARROW {
		ch, ok := t.eval(env, x.X).(*Chan)
		if !ok {
			t.rtErr(x.P, "receive from non-channel")
		}
		v, _ := t.chanRecv(ch, x.P)
		return v
	}
	v := t.eval(env, x.X)
	switch x.Op {
	case lexer.MINUS:
		switch n := v.(type) {
		case IntV:
			if int64(n) == -9223372036854775808 {
				t.throwf("OverflowError", "integer overflow negating %d", int64(n))
			}
			return IntV(-int64(n))
		case FloatV:
			return FloatV(-float64(n))
		case DecV:
			return DecV{n.D.Neg()}
		}
		t.rtErr(x.P, "cannot negate %s", typeName(v))
	case lexer.NOT:
		if b, ok := v.(BoolV); ok {
			return BoolV(!b)
		}
		t.rtErr(x.P, "`not` requires bool, got %s", typeName(v))
	case lexer.TILDE:
		if n, ok := v.(IntV); ok {
			return IntV(^int64(n))
		}
		t.rtErr(x.P, "`~` requires int, got %s", typeName(v))
	}
	return NilV{}
}

func (t *T) evalBinary(env *Env, x *ast.BinaryExpr) Value {
	switch x.Op {
	case lexer.AND:
		l := t.eval(env, x.X)
		lb, ok := l.(BoolV)
		if !ok {
			t.rtErr(x.X.Pos(), "`and` requires bool, got %s", typeName(l))
		}
		if !bool(lb) {
			return BoolV(false)
		}
		r := t.eval(env, x.Y)
		rb, ok := r.(BoolV)
		if !ok {
			t.rtErr(x.Y.Pos(), "`and` requires bool, got %s", typeName(r))
		}
		return rb
	case lexer.OR:
		l := t.eval(env, x.X)
		lb, ok := l.(BoolV)
		if !ok {
			t.rtErr(x.X.Pos(), "`or` requires bool, got %s", typeName(l))
		}
		if bool(lb) {
			return BoolV(true)
		}
		r := t.eval(env, x.Y)
		rb, ok := r.(BoolV)
		if !ok {
			t.rtErr(x.Y.Pos(), "`or` requires bool, got %s", typeName(r))
		}
		return rb
	case lexer.ARROW:
		// Channel send as expression-statement.
		ch, ok := t.eval(env, x.X).(*Chan)
		if !ok {
			t.rtErr(x.P, "send on non-channel")
		}
		v := t.eval(env, x.Y)
		t.moveIfNeeded(env, x.Y, v) // sending moves (§9.2)
		t.chanSend(ch, v, x.P)
		return UnitV{}
	}
	l := t.eval(env, x.X)
	r := t.eval(env, x.Y)
	return t.binaryOp(x.Op, l, r, x.P)
}

func (t *T) evalAttempt(env *Env, x *ast.AttemptExpr) (result Value) {
	defer func() {
		if r := recover(); r != nil {
			if thr, ok := r.(ctlThrow); ok {
				result = thr.err // exception → error value (§8.5)
				return
			}
			panic(r)
		}
	}()
	return t.eval(env, x.X)
}

func (t *T) evalElse(env *Env, x *ast.ElseExpr) Value {
	// `expr else alt`: if expr yields an error value or nil (unwrapping the
	// try that may guard it), produce alt instead (§8.1).
	v, failed := t.evalMaybe(env, x.X)
	if !failed {
		return v
	}
	if x.AltStmt != nil {
		t.execStmt(env, x.AltStmt) // diverges (return/throw/break/continue)
		return NilV{}
	}
	return t.eval(env, x.Alt)
}

// evalMaybe evaluates e, treating `try` failure as local (for else-defaults).
func (t *T) evalMaybe(env *Env, e ast.Expr) (v Value, failed bool) {
	inner := e
	if te, ok := e.(*ast.TryExpr); ok {
		inner = te.X
	}
	v = t.eval(env, inner)
	switch v.(type) {
	case *ErrVal:
		return v, true
	case NilV:
		return v, true
	}
	return v, false
}

func (t *T) evalClosure(env *Env, x *ast.ClosureExpr) Value {
	closureEnv := env
	if x.OwnCap {
		// fn own [data] () { … }: snapshot listed bindings and move them in.
		closureEnv = NewEnv(env)
		for _, name := range x.Captures {
			if b := env.Lookup(name); b != nil {
				closureEnv.Define(name, b.Val, false)
				if !isCopy(b.Val) {
					b.Moved = true
				}
			}
		}
	}
	return &Func{Params: x.Params, Body: x.Body, Env: closureEnv}
}

func (t *T) evalIndex(env *Env, x *ast.IndexExpr) Value {
	coll := t.eval(env, x.X)

	// Generic instantiation: fn[T] — bind the type argument.
	if id, ok := x.Index.(*ast.Ident); ok {
		switch c := coll.(type) {
		case *Builtin:
			if isTypeName(id.Name) || t.in.Structs[id.Name] != nil || t.in.Enums[id.Name] != nil {
				return &Builtin{Name: c.Name, Fn: c.Fn, TypeArg: &TypeV{Name: id.Name}}
			}
		case *Func:
			if isTypeName(id.Name) || t.in.Structs[id.Name] != nil || t.in.Enums[id.Name] != nil {
				f2 := *c
				f2.TypeArg = id.Name
				return &f2
			}
		}
	}

	idx := t.eval(env, x.Index)
	switch c := coll.(type) {
	case *Slice:
		if r, ok := idx.(*RangeVal); ok {
			lo, hi := t.sliceBounds(r, int64(len(c.Elems)), x.P, true)
			return &Slice{Elems: c.Elems[lo:hi]}
		}
		i := t.toInt(idx, x.P)
		if i < 0 || i >= int64(len(c.Elems)) {
			t.abort(x.P, "index out of bounds: %d (len %d)", i, len(c.Elems))
		}
		return c.Elems[i]
	case StrV:
		s := string(c)
		if r, ok := idx.(*RangeVal); ok {
			lo, hi := t.sliceBounds(r, int64(len(s)), x.P, false)
			return StrV(s[lo:hi])
		}
		i := t.toInt(idx, x.P)
		if i < 0 || i >= int64(len(s)) {
			t.abort(x.P, "string index out of bounds: %d (len %d)", i, len(s))
		}
		return IntV(s[i]) // byte access; runes via runes(s)
	case *Map:
		v, ok := c.Get(idx)
		if !ok {
			return NilV{} // missing key → nil (documented REXX comfort)
		}
		return v
	case *Tuple:
		i := t.toInt(idx, x.P)
		if i < 0 || i >= int64(len(c.Elems)) {
			t.abort(x.P, "tuple index out of bounds: %d", i)
		}
		return c.Elems[i]
	}
	t.rtErr(x.P, "cannot index %s", typeName(coll))
	return NilV{}
}

// sliceBounds validates a range for slicing. Slicing THROWS RangeError on bad
// bounds (§13.2) — unlike scalar indexing, which aborts (§8.6).
func (t *T) sliceBounds(r *RangeVal, length int64, pos diag.Pos, isSlice bool) (int64, int64) {
	lo, hi := r.Lo, r.Hi
	if r.Inclusive {
		hi++
	}
	if lo < 0 || hi < lo || hi > length {
		t.throwf("RangeError", "slice bounds [%d..%d) out of range (len %d)", lo, hi, length)
	}
	return lo, hi
}

func (t *T) evalField(env *Env, x *ast.FieldExpr) Value {
	// Package member: fmt.printf …
	if id, ok := x.X.(*ast.Ident); ok {
		if env.Lookup(id.Name) == nil {
			if pkg, ok := t.in.packages[id.Name]; ok {
				if v, ok := pkg.Funcs[x.Name]; ok {
					return v
				}
				t.rtErr(x.P, "package %s has no member `%s`", id.Name, x.Name)
			}
		}
	}
	recv := t.eval(env, x.X)
	return t.fieldOf(recv, x.Name, x.P)
}

func (t *T) fieldOf(recv Value, name string, pos diag.Pos) Value {
	switch r := recv.(type) {
	case *Struct:
		if v, ok := r.Fields[name]; ok {
			return v
		}
	case *ErrVal:
		if r.Struct != nil {
			if v, ok := r.Struct.Fields[name]; ok {
				return v
			}
		}
		// Built-in exception pseudo-fields (ConvError etc.).
		switch name {
		case "msg":
			return StrV(r.Msg)
		}
	case *EnumVal:
		for i, n := range r.Names {
			if n == name {
				return r.Fields[i]
			}
		}
	case *SharedBox:
		return t.fieldOf(r.V, name, pos)
	case *CellBox:
		r.mu.Lock()
		defer r.mu.Unlock()
		return t.fieldOf(r.V, name, pos)
	case *Pkg:
		if v, ok := r.Funcs[name]; ok {
			return v
		}
	}
	t.rtErr(pos, "%s has no field `%s`", typeName(recv), name)
	return NilV{}
}

func (t *T) evalComposite(env *Env, x *ast.CompositeLit) Value {
	name := x.Type.Name
	var positional []Value
	named := map[string]Value{}
	for _, f := range x.Fields {
		v := t.eval(env, f.Value)
		t.moveIfNeeded(env, f.Value, v)
		if f.Name != "" {
			named[f.Name] = v
		} else {
			positional = append(positional, v)
		}
	}
	if sd, ok := t.in.Structs[name]; ok {
		return t.newStruct(sd, positional, named)
	}
	if ed, ok := t.in.Exceptions[name]; ok {
		s := t.newStruct(&ast.StructDecl{Name: name, Fields: ed.Fields}, positional, named)
		return &ErrVal{TypeName: name, Struct: s}
	}
	if bd, ok := builtinExceptions[name]; ok {
		s := t.newStruct(bd, positional, named)
		return &ErrVal{TypeName: name, Struct: s}
	}
	// Builder etc.: str.Builder{}
	if x.Type.Pkg == "str" && name == "Builder" {
		return &Builder{}
	}
	t.rtErr(x.P, "unknown type `%s` in composite literal", name)
	return NilV{}
}

// builtinExceptions defines the fields of predeclared exception types so
// user code can construct and catch them by name.
var builtinExceptions = map[string]*ast.StructDecl{
	"ConvError": {Name: "ConvError", Fields: []*ast.Field{
		{Name: "from", Type: &ast.NamedType{Name: "str"}},
		{Name: "to", Type: &ast.NamedType{Name: "str"}},
		{Name: "value", Type: &ast.NamedType{Name: "str"}},
	}},
	"OverflowError": {Name: "OverflowError", Fields: []*ast.Field{{Name: "op", Type: &ast.NamedType{Name: "str"}}}},
	"RangeError":    {Name: "RangeError", Fields: []*ast.Field{{Name: "msg", Type: &ast.NamedType{Name: "str"}}}},
	"FormatError":   {Name: "FormatError", Fields: []*ast.Field{{Name: "msg", Type: &ast.NamedType{Name: "str"}}}},
	"ValueError":    {Name: "ValueError", Fields: []*ast.Field{{Name: "msg", Type: &ast.NamedType{Name: "str"}}}},
	"RuntimeError":  {Name: "RuntimeError", Fields: []*ast.Field{{Name: "msg", Type: &ast.NamedType{Name: "str"}}}},
	"Cancelled":     {Name: "Cancelled", Fields: nil},
	"Timeout":       {Name: "Timeout", Fields: []*ast.Field{{Name: "after", Type: &ast.NamedType{Name: "int"}}}},
	"IOError":       {Name: "IOError", Fields: []*ast.Field{{Name: "msg", Type: &ast.NamedType{Name: "str"}}}},
	"KeyError":      {Name: "KeyError", Fields: []*ast.Field{{Name: "key", Type: &ast.NamedType{Name: "str"}}}},
}
