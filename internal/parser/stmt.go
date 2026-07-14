package parser

import (
	"strconv"

	"voila/internal/ast"
	"voila/internal/lexer"
)

// ---------------------------------------------------------------- blocks

func (p *Parser) parseBlock() *ast.Block {
	pos := p.expect(lexer.LBRACE).Pos
	b := &ast.Block{P: pos}
	savedLev := p.exprLev
	p.exprLev = 0
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		b.Stmts = append(b.Stmts, p.parseStmt())
	}
	p.expect(lexer.RBRACE)
	p.exprLev = savedLev
	return b
}

// ---------------------------------------------------------------- statements

func (p *Parser) parseStmt() ast.Stmt {
	pos := p.cur().Pos
	switch p.cur().Kind {
	case lexer.LET, lexer.CONST:
		return p.parseLet()
	case lexer.VAR:
		return p.parseVar()
	case lexer.IF:
		return p.parseIf()
	case lexer.FOR, lexer.EACH, lexer.WHILE:
		return p.parseLoop("")
	case lexer.IDENT:
		// Label: `outer: for …`.
		if p.peek(1).Kind == lexer.COLON {
			switch p.peek(2).Kind {
			case lexer.FOR, lexer.EACH, lexer.WHILE:
				label := p.next().Lit
				p.next() // ':'
				return p.parseLoop(label)
			}
		}
		if p.cur().Lit == "fallthrough" && (p.peek(1).Kind == lexer.SEMI || p.peek(1).Kind == lexer.RBRACE) {
			p.next()
			p.expectSemi()
			return &ast.FallthroughStmt{P: pos}
		}
		return p.parseSimpleStmt()
	case lexer.SWITCH:
		return p.parseSwitch()
	case lexer.MATCH:
		return p.parseMatch()
	case lexer.SELECT:
		return p.parseSelect()
	case lexer.GROUP:
		return p.parseGroup(false)
	case lexer.PARSE:
		return p.parseParse()
	case lexer.SAY:
		return p.parseSay()
	case lexer.DEFER:
		return p.parseDefer()
	case lexer.DROP:
		p.next()
		name := p.expect(lexer.IDENT).Lit
		p.expectSemi()
		return &ast.DropStmt{Name: name, P: pos}
	case lexer.RETURN:
		p.next()
		s := &ast.ReturnStmt{P: pos}
		if !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) && !p.at(lexer.EOF) {
			s.Values = append(s.Values, p.parseExpr())
			for p.accept(lexer.COMMA) {
				s.Values = append(s.Values, p.parseExpr())
			}
		}
		p.expectSemi()
		return s
	case lexer.BREAK:
		p.next()
		s := &ast.BreakStmt{P: pos}
		if p.at(lexer.IDENT) {
			s.Label = p.next().Lit
		}
		p.expectSemi()
		return s
	case lexer.CONTINUE:
		p.next()
		s := &ast.ContinueStmt{P: pos}
		if p.at(lexer.IDENT) {
			s.Label = p.next().Lit
		}
		p.expectSemi()
		return s
	case lexer.NUMERIC:
		p.next()
		w := p.expect(lexer.IDENT)
		if w.Lit != "digits" {
			p.errorf(w.Pos, "expected `digits` after `numeric`")
		}
		n := p.expect(lexer.INT)
		digits, _ := strconv.Atoi(n.Lit)
		p.expectSemi()
		return &ast.NumericStmt{Digits: digits, P: pos}
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
		p.expectSemi()
		return s
	case lexer.TRY:
		// Three-way: `try {` → try/catch, `try group` → try-group, else expr.
		switch p.peek(1).Kind {
		case lexer.LBRACE:
			return p.parseTryCatch()
		case lexer.GROUP:
			p.next()
			return p.parseGroup(true)
		}
		return p.parseSimpleStmt()
	case lexer.LBRACE:
		return p.parseBlock()
	}
	return p.parseSimpleStmt()
}

func (p *Parser) parseLet() ast.Stmt {
	pos := p.cur().Pos
	isConst := p.cur().Kind == lexer.CONST
	p.next()
	s := &ast.LetStmt{Const: isConst, P: pos}
	s.Names = append(s.Names, p.expect(lexer.IDENT).Lit)
	for p.accept(lexer.COMMA) {
		s.Names = append(s.Names, p.expect(lexer.IDENT).Lit)
	}
	if !p.at(lexer.ASSIGN) {
		s.Type = p.parseType()
	}
	p.expect(lexer.ASSIGN)
	s.Value = p.parseExpr()
	p.expectSemi()
	return s
}

func (p *Parser) parseVar() ast.Stmt {
	pos := p.expect(lexer.VAR).Pos
	s := &ast.VarStmt{P: pos}
	s.Name = p.expect(lexer.IDENT).Lit
	if !p.at(lexer.ASSIGN) && !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) && !p.at(lexer.EOF) {
		s.Type = p.parseType()
	}
	if p.accept(lexer.ASSIGN) {
		s.Value = p.parseExpr()
	}
	p.expectSemi()
	return s
}

// parseSimpleStmt parses expression statements, assignments and channel sends.
func (p *Parser) parseSimpleStmt() ast.Stmt {
	pos := p.cur().Pos
	x := p.parseExpr()
	switch p.cur().Kind {
	case lexer.ASSIGN, lexer.PLUS_ASSIGN, lexer.MINUS_ASSIGN, lexer.STAR_ASSIGN,
		lexer.SLASH_ASSIGN, lexer.PERCENT_ASSIGN, lexer.CONCAT_ASSIGN:
		op := p.next().Kind
		rhs := p.parseExpr()
		p.expectSemi()
		return &ast.AssignStmt{LHS: []ast.Expr{x}, Op: op, RHS: rhs, P: pos}
	case lexer.ARROW:
		// Channel send: `ch <- v`.
		p.next()
		v := p.parseExpr()
		p.expectSemi()
		return &ast.ExprStmt{X: &ast.BinaryExpr{Op: lexer.ARROW, X: x, Y: v, P: pos}, P: pos}
	}
	p.expectSemi()
	return &ast.ExprStmt{X: x, P: pos}
}

func (p *Parser) parseIf() ast.Stmt {
	pos := p.expect(lexer.IF).Pos
	s := &ast.IfStmt{P: pos}
	if p.at(lexer.LET) {
		p.next()
		s.Pattern = p.parsePattern()
		p.expect(lexer.ASSIGN)
		s.Value = p.parseHeaderExpr()
	} else {
		s.Cond = p.parseHeaderExpr()
	}
	s.Then = p.parseBlock()
	if p.skipSemisBefore(lexer.ELSE) {
		p.next() // else
		if p.at(lexer.IF) {
			s.Else = p.parseIf()
		} else {
			s.Else = p.parseBlock()
		}
	}
	return s
}

// parseHeaderExpr parses a condition/iterable in a statement header, where
// composite literals must be parenthesized (§ plan decision b).
func (p *Parser) parseHeaderExpr() ast.Expr {
	saved := p.noLit
	p.noLit = true
	e := p.parseExpr()
	p.noLit = saved
	return e
}

func (p *Parser) parseLoop(label string) ast.Stmt {
	pos := p.cur().Pos
	switch p.cur().Kind {
	case lexer.FOR:
		p.next()
		s := &ast.ForStmt{Label: label, P: pos}
		if !p.at(lexer.LBRACE) {
			s.Var = p.expect(lexer.IDENT).Lit
			p.expect(lexer.IN)
			s.Iter = p.parseHeaderExpr()
		}
		s.Body = p.parseBlock()
		return s
	case lexer.EACH:
		p.next()
		s := &ast.EachStmt{Label: label, P: pos}
		s.A = p.expect(lexer.IDENT).Lit
		if p.accept(lexer.COMMA) {
			s.B = p.expect(lexer.IDENT).Lit
		}
		p.expect(lexer.IN)
		s.Iter = p.parseHeaderExpr()
		s.Body = p.parseBlock()
		return s
	case lexer.WHILE:
		p.next()
		s := &ast.WhileStmt{Label: label, P: pos}
		s.Cond = p.parseHeaderExpr()
		s.Body = p.parseBlock()
		return s
	}
	p.errorf(pos, "expected loop after label")
	return &ast.Block{P: pos}
}

func (p *Parser) parseSwitch() ast.Stmt {
	pos := p.expect(lexer.SWITCH).Pos
	s := &ast.SwitchStmt{P: pos}
	s.Subject = p.parseHeaderExpr()
	p.expect(lexer.LBRACE)
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		c := &ast.SwitchCase{P: p.cur().Pos}
		if p.accept(lexer.CASE) {
			c.Values = append(c.Values, p.parseExpr())
			for p.accept(lexer.COMMA) {
				c.Values = append(c.Values, p.parseExpr())
			}
			p.expect(lexer.COLON)
		} else if p.at(lexer.IDENT) && p.cur().Lit == "default" {
			p.next()
			p.expect(lexer.COLON)
		} else {
			p.errorf(p.cur().Pos, "expected `case` or `default` in switch, found %s", p.cur())
			p.next()
			continue
		}
		for {
			p.skipSemis()
			if p.at(lexer.CASE) || p.at(lexer.RBRACE) || p.at(lexer.EOF) ||
				(p.at(lexer.IDENT) && p.cur().Lit == "default" && p.peek(1).Kind == lexer.COLON) {
				break
			}
			c.Body = append(c.Body, p.parseStmt())
		}
		s.Cases = append(s.Cases, c)
	}
	p.expect(lexer.RBRACE)
	return s
}

func (p *Parser) parseMatch() ast.Stmt {
	pos := p.expect(lexer.MATCH).Pos
	s := &ast.MatchStmt{P: pos}
	s.Subject = p.parseHeaderExpr()
	p.expect(lexer.LBRACE)
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		arm := &ast.MatchArm{P: p.cur().Pos}
		arm.Pattern = p.parsePattern()
		if p.at(lexer.IF) {
			p.next()
			saved := p.noLit
			p.noLit = true
			arm.Guard = p.parseExpr()
			p.noLit = saved
		}
		p.expect(lexer.FATARROW)
		if p.at(lexer.LBRACE) {
			arm.Body = p.parseBlock().Stmts
		} else {
			arm.Body = []ast.Stmt{p.parseStmt()}
		}
		s.Arms = append(s.Arms, arm)
	}
	p.expect(lexer.RBRACE)
	return s
}

func (p *Parser) parsePattern() ast.Pattern {
	pos := p.cur().Pos
	switch p.cur().Kind {
	case lexer.NIL:
		p.next()
		return &ast.NilPat{P: pos}
	case lexer.TRUE, lexer.FALSE, lexer.INT, lexer.FLOAT, lexer.DEC, lexer.STR, lexer.RUNE:
		return &ast.LiteralPat{Value: p.parsePrimary(), P: pos}
	case lexer.MINUS:
		return &ast.LiteralPat{Value: p.parseUnary(), P: pos}
	case lexer.IDENT:
		name := p.next().Lit
		if name == "_" {
			return &ast.WildcardPat{P: pos}
		}
		if p.at(lexer.LPAREN) {
			p.next()
			vp := &ast.VariantPat{Name: name, P: pos}
			for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
				vp.Elems = append(vp.Elems, p.parsePattern())
				if !p.accept(lexer.COMMA) {
					break
				}
			}
			p.expect(lexer.RPAREN)
			return vp
		}
		// Uppercase bare identifier = payload-less variant; lowercase = binding.
		if name != "" && name[0] >= 'A' && name[0] <= 'Z' {
			return &ast.VariantPat{Name: name, P: pos}
		}
		return &ast.BindPat{Name: name, P: pos}
	}
	p.errorf(pos, "expected pattern, found %s", p.cur())
	p.next()
	return &ast.WildcardPat{P: pos}
}

func (p *Parser) parseSelect() ast.Stmt {
	pos := p.expect(lexer.SELECT).Pos
	s := &ast.SelectStmt{P: pos}
	p.expect(lexer.LBRACE)
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		c := &ast.SelectCase{P: p.cur().Pos}
		switch {
		case p.accept(lexer.CASE):
			p.parseSelectComm(c)
			p.expect(lexer.COLON)
		case p.at(lexer.IDENT) && p.cur().Lit == "default":
			p.next()
			p.expect(lexer.COLON)
			c.Kind = ast.SelectDefault
		default:
			p.errorf(p.cur().Pos, "expected `case` or `default` in select, found %s", p.cur())
			p.next()
			continue
		}
		for {
			p.skipSemis()
			if p.at(lexer.CASE) || p.at(lexer.RBRACE) || p.at(lexer.EOF) ||
				(p.at(lexer.IDENT) && p.cur().Lit == "default" && p.peek(1).Kind == lexer.COLON) {
				break
			}
			c.Body = append(c.Body, p.parseStmt())
		}
		s.Cases = append(s.Cases, c)
	}
	p.expect(lexer.RBRACE)
	return s
}

// parseSelectComm parses one communication clause:
//
//	job := <-jobs         recv with bind (the spec's only `:=`, §9.3)
//	job, ok := <-jobs     recv with bind + ok
//	let job = <-jobs      recv with bind
//	<-done                bare recv
//	results <- value      send
func (p *Parser) parseSelectComm(c *ast.SelectCase) {
	switch {
	case p.at(lexer.LET):
		p.next()
		c.Kind = ast.SelectRecv
		c.Bind = p.expect(lexer.IDENT).Lit
		if p.accept(lexer.COMMA) {
			c.BindOk = p.expect(lexer.IDENT).Lit
		}
		p.expect(lexer.ASSIGN)
		p.expect(lexer.ARROW)
		c.Ch = p.parseExpr()
	case p.at(lexer.IDENT) && p.peek(1).Kind == lexer.DEFINE:
		c.Kind = ast.SelectRecv
		c.Bind = p.next().Lit
		p.next() // :=
		p.expect(lexer.ARROW)
		c.Ch = p.parseExpr()
	case p.at(lexer.IDENT) && p.peek(1).Kind == lexer.COMMA && p.peek(2).Kind == lexer.IDENT && p.peek(3).Kind == lexer.DEFINE:
		c.Kind = ast.SelectRecv
		c.Bind = p.next().Lit
		p.next() // ,
		c.BindOk = p.next().Lit
		p.next() // :=
		p.expect(lexer.ARROW)
		c.Ch = p.parseExpr()
	case p.at(lexer.ARROW):
		p.next()
		c.Kind = ast.SelectRecv
		c.Ch = p.parseExpr()
	default:
		// Send: expr <- expr.
		c.Kind = ast.SelectSend
		c.Ch = p.parseExpr()
		p.expect(lexer.ARROW)
		c.Send = p.parseExpr()
	}
}

func (p *Parser) parseGroup(isTry bool) ast.Stmt {
	pos := p.expect(lexer.GROUP).Pos
	s := &ast.GroupStmt{Try: isTry, P: pos}
	if p.at(lexer.IDENT) && p.cur().Lit == "timeout" {
		p.next()
		s.Timeout = p.parseHeaderExpr()
	}
	s.Body = p.parseBlock()
	return s
}

func (p *Parser) parseParse() ast.Stmt {
	pos := p.expect(lexer.PARSE).Pos
	s := &ast.ParseStmt{P: pos}
	if p.at(lexer.IDENT) && (p.cur().Lit == "upper" || p.cur().Lit == "lower") {
		s.CaseFold = p.next().Lit
	}
	p.accept(lexer.VAR) // `parse var s …` — REXX-style marker, no semantic difference
	// Source: a restricted expression (primary + call/index postfix, no field
	// access — `.` is the discard term).
	s.Src = p.parseParseSource()
	for !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) && !p.at(lexer.EOF) {
		t := ast.ParseTerm{P: p.cur().Pos}
		switch p.cur().Kind {
		case lexer.IDENT:
			t.Kind = ast.ParseVar
			t.Name = p.next().Lit
		case lexer.DOT:
			p.next()
			t.Kind = ast.ParseDiscard
		case lexer.STR:
			t.Kind = ast.ParseLit
			t.Lit = p.next().Lit
		case lexer.INT:
			t.Kind = ast.ParseCol
			t.Col, _ = strconv.Atoi(p.next().Lit)
		default:
			p.errorf(p.cur().Pos, "unexpected %s in parse template", p.cur())
			p.next()
			continue
		}
		s.Terms = append(s.Terms, t)
	}
	p.expectSemi()
	return s
}

// parseParseSource parses the source expression of a `parse` statement:
// postfix chains are limited to calls and indexing so template terms that
// follow are not swallowed.
func (p *Parser) parseParseSource() ast.Expr {
	x := p.parsePrimary()
	for {
		switch p.cur().Kind {
		case lexer.LPAREN:
			x = p.parseCallTail(x, nil)
		case lexer.LBRACKET:
			x = p.parseIndexTail(x)
		default:
			return x
		}
	}
}

func (p *Parser) parseSay() ast.Stmt {
	pos := p.expect(lexer.SAY).Pos
	s := &ast.SayStmt{P: pos}
	if !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) && !p.at(lexer.EOF) {
		s.Args = append(s.Args, p.parseExpr())
		for p.accept(lexer.COMMA) {
			s.Args = append(s.Args, p.parseExpr())
		}
	}
	p.expectSemi()
	return s
}

func (p *Parser) parseDefer() ast.Stmt {
	pos := p.expect(lexer.DEFER).Pos
	s := &ast.DeferStmt{P: pos}
	switch {
	case p.at(lexer.LBRACE):
		s.Block = p.parseBlock()
	case p.at(lexer.SAY):
		// Grammar says call|block, but `defer say "…"` is too natural to reject.
		s.Block = &ast.Block{Stmts: []ast.Stmt{p.parseSay()}, P: p.cur().Pos}
		return s
	default:
		s.Call = p.parseExpr()
	}
	p.expectSemi()
	return s
}

func (p *Parser) parseTryCatch() ast.Stmt {
	pos := p.expect(lexer.TRY).Pos
	s := &ast.TryCatchStmt{P: pos}
	s.Body = p.parseBlock()
	for p.skipSemisBefore(lexer.CATCH) {
		p.next() // catch
		c := &ast.CatchClause{P: p.cur().Pos}
		if p.at(lexer.IDENT) {
			c.Name = p.next().Lit
			if p.accept(lexer.COLON) {
				c.Types = append(c.Types, p.parseType())
				for p.accept(lexer.COMMA) {
					c.Types = append(c.Types, p.parseType())
				}
			}
		}
		c.Body = p.parseBlock()
		s.Catches = append(s.Catches, c)
	}
	if p.skipSemisBefore(lexer.FINALLY) {
		p.next()
		s.Finally = p.parseBlock()
	}
	p.expectSemi()
	return s
}
