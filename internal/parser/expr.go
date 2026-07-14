package parser

import (
	"strings"

	"voila/internal/ast"
	"voila/internal/diag"
	"voila/internal/lexer"
)

// Precedence levels (§5.1, scaled ×10; `else` and ranges are wedged in per
// the frozen design decisions).
const (
	precElse   = 10
	precOr     = 20
	precAnd    = 30
	precCmp    = 40
	precRange  = 45
	precConcat = 50
	precBits   = 60
	precAdd    = 70
	precMul    = 80
	precPow    = 90
)

func binPrec(k lexer.Kind) (prec int, rightAssoc bool) {
	switch k {
	case lexer.OR:
		return precOr, false
	case lexer.AND:
		return precAnd, false
	case lexer.LT, lexer.LE, lexer.GT, lexer.GE, lexer.EQ, lexer.NE, lexer.IN:
		return precCmp, false
	case lexer.CONCAT:
		return precConcat, false
	case lexer.SHL, lexer.SHR, lexer.AMP, lexer.PIPE, lexer.CARET:
		return precBits, false
	case lexer.PLUS, lexer.MINUS:
		return precAdd, false
	case lexer.STAR, lexer.SLASH, lexer.FLOORDIV, lexer.PERCENT:
		return precMul, false
	case lexer.POWER:
		return precPow, true
	}
	return 0, false
}

func (p *Parser) parseExpr() ast.Expr {
	return p.parseBinary(precElse)
}

func (p *Parser) parseBinary(min int) ast.Expr {
	x := p.parseUnary()
	for {
		k := p.cur().Kind
		pos := p.cur().Pos

		if (k == lexer.RANGE || k == lexer.RANGEEQ) && precRange >= min {
			p.next()
			r := &ast.RangeExpr{Lo: x, Inclusive: k == lexer.RANGEEQ, P: pos}
			r.Hi = p.parseBinary(precRange + 1)
			if p.at(lexer.IDENT) && p.cur().Lit == "by" {
				p.next()
				r.By = p.parseBinary(precRange + 1)
			}
			x = r
			continue
		}

		if k == lexer.ELSE && precElse >= min {
			p.next()
			e := &ast.ElseExpr{X: x, P: pos}
			switch p.cur().Kind {
			case lexer.RETURN, lexer.THROW, lexer.BREAK, lexer.CONTINUE:
				e.AltStmt = p.parseDivergingStmt()
			default:
				e.Alt = p.parseBinary(precElse) // right-assoc
			}
			x = e
			continue
		}

		prec, right := binPrec(k)
		if prec == 0 || prec < min {
			return x
		}
		p.next()
		nextMin := prec + 1
		if right {
			nextMin = prec
		}
		y := p.parseBinary(nextMin)
		x = &ast.BinaryExpr{Op: k, X: x, Y: y, P: pos}
	}
}

// parseDivergingStmt parses the statement form allowed on the RHS of the
// `else` operator (§8.1: `try to_int(s) else return err(...)`) without
// consuming the statement terminator.
func (p *Parser) parseDivergingStmt() ast.Stmt {
	pos := p.cur().Pos
	switch p.cur().Kind {
	case lexer.RETURN:
		p.next()
		s := &ast.ReturnStmt{P: pos}
		if !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) && !p.at(lexer.EOF) && !p.at(lexer.COMMA) && !p.at(lexer.RPAREN) {
			s.Values = append(s.Values, p.parseExpr())
			for p.accept(lexer.COMMA) {
				s.Values = append(s.Values, p.parseExpr())
			}
		}
		return s
	case lexer.THROW:
		p.next()
		s := &ast.ThrowStmt{P: pos}
		if !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) && !p.at(lexer.EOF) {
			s.Value = p.parseExpr()
			if p.at(lexer.IDENT) && p.cur().Lit == "from" {
				p.next()
				s.From = p.parseExpr()
			}
		}
		return s
	case lexer.BREAK:
		p.next()
		s := &ast.BreakStmt{P: pos}
		if p.at(lexer.IDENT) {
			s.Label = p.next().Lit
		}
		return s
	case lexer.CONTINUE:
		p.next()
		s := &ast.ContinueStmt{P: pos}
		if p.at(lexer.IDENT) {
			s.Label = p.next().Lit
		}
		return s
	}
	p.errorf(pos, "expected diverging statement after `else`")
	return &ast.ReturnStmt{P: pos}
}

func (p *Parser) parseUnary() ast.Expr {
	pos := p.cur().Pos
	switch p.cur().Kind {
	case lexer.MINUS, lexer.NOT, lexer.TILDE:
		op := p.next().Kind
		return &ast.UnaryExpr{Op: op, X: p.parseUnary(), P: pos}
	case lexer.ARROW: // channel receive
		p.next()
		return &ast.UnaryExpr{Op: lexer.ARROW, X: p.parseUnary(), P: pos}
	case lexer.TRY:
		p.next()
		return &ast.TryExpr{X: p.parseUnary(), P: pos}
	case lexer.MUST:
		p.next()
		return &ast.MustExpr{X: p.parseUnary(), P: pos}
	case lexer.ATTEMPT:
		p.next()
		return &ast.AttemptExpr{X: p.parseUnary(), P: pos}
	}
	return p.parsePostfix(p.parsePrimary())
}

func (p *Parser) parsePostfix(x ast.Expr) ast.Expr {
	for {
		switch p.cur().Kind {
		case lexer.LPAREN:
			x = p.parseCallTail(x, nil)
		case lexer.LBRACKET:
			x = p.parseIndexTail(x)
		case lexer.DOT:
			pos := p.next().Pos
			name := p.expect(lexer.IDENT).Lit
			x = &ast.FieldExpr{X: x, Name: name, P: pos}
		case lexer.LBRACE:
			if p.noLit && p.exprLev == 0 {
				return x
			}
			nt, ok := exprToNamedType(x)
			if !ok {
				return x
			}
			x = p.parseCompositeTail(nt)
		case lexer.BANG, lexer.QUESTION:
			// §5.1 lists postfix `!`/`?` but the spec defines no expression
			// semantics for them; they are type forms only.
			p.errorf(p.cur().Pos, "postfix `%s` is a type form (`int%s`); it has no expression meaning", p.cur().Kind, p.cur().Kind)
			p.next()
		default:
			return x
		}
	}
}

// exprToNamedType converts an already-parsed expression into the type of a
// composite literal: `Point{…}`, `pkg.Point{…}`, `Stack[int]{…}`.
func exprToNamedType(x ast.Expr) (*ast.NamedType, bool) {
	switch e := x.(type) {
	case *ast.Ident:
		return &ast.NamedType{Name: e.Name, P: e.P}, true
	case *ast.FieldExpr:
		if base, ok := e.X.(*ast.Ident); ok {
			return &ast.NamedType{Pkg: base.Name, Name: e.Name, P: e.P}, true
		}
	case *ast.IndexExpr:
		base, ok := e.X.(*ast.Ident)
		if !ok {
			return nil, false
		}
		arg, ok := exprToType(e.Index)
		if !ok {
			return nil, false
		}
		return &ast.NamedType{Name: base.Name, Args: []ast.TypeExpr{arg}, P: e.P}, true
	}
	return nil, false
}

func exprToType(x ast.Expr) (ast.TypeExpr, bool) {
	switch e := x.(type) {
	case *ast.Ident:
		return &ast.NamedType{Name: e.Name, P: e.P}, true
	case *ast.FieldExpr:
		if base, ok := e.X.(*ast.Ident); ok {
			return &ast.NamedType{Pkg: base.Name, Name: e.Name, P: e.P}, true
		}
	}
	return nil, false
}

func (p *Parser) parseCompositeTail(nt *ast.NamedType) ast.Expr {
	pos := p.expect(lexer.LBRACE).Pos
	lit := &ast.CompositeLit{Type: nt, P: pos}
	p.exprLev++
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		var f ast.CompositeField
		if p.at(lexer.IDENT) && p.peek(1).Kind == lexer.COLON {
			f.Name = p.next().Lit
			p.next() // ':'
			f.Value = p.parseExpr()
		} else {
			f.Value = p.parseExpr()
		}
		lit.Fields = append(lit.Fields, f)
		if !p.accept(lexer.COMMA) {
			p.skipSemis()
			break
		}
	}
	p.exprLev--
	p.expect(lexer.RBRACE)
	return lit
}

func (p *Parser) parseCallTail(fun ast.Expr, typeArgs []ast.TypeExpr) ast.Expr {
	pos := p.expect(lexer.LPAREN).Pos
	call := &ast.CallExpr{Fun: fun, TypeArgs: typeArgs, P: pos}
	p.exprLev++
	// `make([]str, n)` / `make(map[str]int)`: first argument is a type.
	if id, ok := fun.(*ast.Ident); ok && id.Name == "make" && !p.at(lexer.RPAREN) {
		call.TypeArgs = append(call.TypeArgs, p.parseType())
		if !p.at(lexer.RPAREN) {
			p.expect(lexer.COMMA)
		}
	}
	for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
		var a ast.Arg
		if p.at(lexer.IDENT) && p.peek(1).Kind == lexer.COLON {
			a.Name = p.next().Lit
			p.next() // ':'
			a.Value = p.parseExpr()
		} else {
			a.Value = p.parseExpr()
			if p.accept(lexer.ELLIPSIS) {
				a.Spread = true
			}
		}
		call.Args = append(call.Args, a)
		if !p.accept(lexer.COMMA) {
			break
		}
	}
	p.exprLev--
	p.expect(lexer.RPAREN)
	return call
}

func (p *Parser) parseIndexTail(x ast.Expr) ast.Expr {
	pos := p.expect(lexer.LBRACKET).Pos
	p.exprLev++
	idx := p.parseExpr()
	p.exprLev--
	p.expect(lexer.RBRACKET)
	return &ast.IndexExpr{X: x, Index: idx, P: pos}
}

func (p *Parser) parsePrimary() ast.Expr {
	pos := p.cur().Pos
	switch p.cur().Kind {
	case lexer.INT:
		return &ast.BasicLit{Kind: ast.IntLit, Text: p.next().Lit, P: pos}
	case lexer.FLOAT:
		return &ast.BasicLit{Kind: ast.FloatLit, Text: p.next().Lit, P: pos}
	case lexer.DEC:
		return &ast.BasicLit{Kind: ast.DecLit, Text: p.next().Lit, P: pos}
	case lexer.STR:
		return &ast.BasicLit{Kind: ast.StrLit, Text: p.next().Lit, P: pos}
	case lexer.RUNE:
		return &ast.BasicLit{Kind: ast.RuneLit, Text: p.next().Lit, P: pos}
	case lexer.TRUE:
		p.next()
		return &ast.BasicLit{Kind: ast.BoolLit, Text: "true", P: pos}
	case lexer.FALSE:
		p.next()
		return &ast.BasicLit{Kind: ast.BoolLit, Text: "false", P: pos}
	case lexer.NIL:
		p.next()
		return &ast.BasicLit{Kind: ast.NilLit, Text: "nil", P: pos}
	case lexer.ISTR:
		return p.parseInterpStr(p.next())
	case lexer.SELF:
		p.next()
		return &ast.SelfExpr{P: pos}
	case lexer.SPAWN:
		p.next()
		e := &ast.SpawnExpr{P: pos}
		if p.at(lexer.LBRACE) {
			e.Block = p.parseBlock()
		} else {
			e.Call = p.parseUnary()
		}
		return e
	case lexer.CHAN:
		// chan[T](cap) constructor.
		p.next()
		p.expect(lexer.LBRACKET)
		t := p.parseType()
		p.expect(lexer.RBRACKET)
		call, _ := p.parseChanCtor(t, pos)
		return call
	case lexer.SHARED:
		p.next()
		return &ast.Ident{Name: "shared", P: pos}
	case lexer.WEAK:
		p.next()
		return &ast.Ident{Name: "weak", P: pos}
	case lexer.IDENT:
		if p.cur().Lit == "fn" && (p.peek(1).Kind == lexer.LPAREN || p.peek(1).Kind == lexer.OWN) {
			return p.parseClosure()
		}
		return &ast.Ident{Name: p.next().Lit, P: pos}
	case lexer.LPAREN:
		p.next()
		p.exprLev++
		if p.at(lexer.RPAREN) { // unit ()
			p.exprLev--
			p.next()
			return &ast.TupleExpr{P: pos}
		}
		x := p.parseExpr()
		if p.at(lexer.COMMA) {
			t := &ast.TupleExpr{Elems: []ast.Expr{x}, P: pos}
			for p.accept(lexer.COMMA) {
				if p.at(lexer.RPAREN) {
					break
				}
				t.Elems = append(t.Elems, p.parseExpr())
			}
			p.exprLev--
			p.expect(lexer.RPAREN)
			return t
		}
		p.exprLev--
		p.expect(lexer.RPAREN)
		return x
	case lexer.LBRACKET:
		// Slice literal [1, 2, 3].
		p.next()
		p.exprLev++
		lit := &ast.SliceLit{P: pos}
		for !p.at(lexer.RBRACKET) && !p.at(lexer.EOF) {
			lit.Elems = append(lit.Elems, p.parseExpr())
			if !p.accept(lexer.COMMA) {
				break
			}
		}
		p.exprLev--
		p.expect(lexer.RBRACKET)
		return lit
	}
	p.errorf(pos, "expected expression, found %s", p.cur())
	p.next()
	return &ast.BasicLit{Kind: ast.NilLit, Text: "nil", P: pos}
}

// parseChanCtor finishes `chan[T](cap)`.
func (p *Parser) parseChanCtor(t ast.TypeExpr, pos diag.Pos) (ast.Expr, bool) {
	call := &ast.CallExpr{Fun: &ast.Ident{Name: "chan", P: pos}, TypeArgs: []ast.TypeExpr{t}, P: pos}
	if p.at(lexer.LPAREN) {
		p.next()
		p.exprLev++
		for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
			call.Args = append(call.Args, ast.Arg{Value: p.parseExpr()})
			if !p.accept(lexer.COMMA) {
				break
			}
		}
		p.exprLev--
		p.expect(lexer.RPAREN)
	}
	return call, true
}

func (p *Parser) parseClosure() ast.Expr {
	pos := p.cur().Pos
	p.next() // fn
	e := &ast.ClosureExpr{P: pos}
	if p.accept(lexer.OWN) {
		e.OwnCap = true
		if p.accept(lexer.LBRACKET) {
			for !p.at(lexer.RBRACKET) && !p.at(lexer.EOF) {
				e.Captures = append(e.Captures, p.expect(lexer.IDENT).Lit)
				if !p.accept(lexer.COMMA) {
					break
				}
			}
			p.expect(lexer.RBRACKET)
		}
	}
	var d ast.FuncDecl
	p.parseParamsInto(&d)
	e.Params = d.Params
	if !p.at(lexer.LBRACE) {
		e.Result = p.parseType()
	}
	e.Body = p.parseBlock()
	return e
}

// parseInterpStr sub-parses each embedded expression. The snippet source is
// padded with newlines/spaces so positions inside it line up with the file.
func (p *Parser) parseInterpStr(tok lexer.Token) ast.Expr {
	e := &ast.InterpStr{P: tok.Pos}
	for _, part := range tok.Parts {
		if !part.IsExpr {
			e.Parts = append(e.Parts, ast.InterpPart{Lit: part.Lit})
			continue
		}
		padded := strings.Repeat("\n", part.Pos.Line-1) + strings.Repeat(" ", part.Pos.Col-1) + part.ExprSrc
		sub := diag.NewSource(p.src.Name, padded)
		expr := ParseExprString(sub, p.bag)
		e.Parts = append(e.Parts, ast.InterpPart{Expr: expr})
	}
	return e
}
