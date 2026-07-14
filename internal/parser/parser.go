// Package parser builds the Voilà AST from tokens: recursive descent for
// statements and declarations (§15), Pratt precedence climbing for
// expressions (§5.1).
package parser

import (
	"strconv"

	"voila/internal/ast"
	"voila/internal/diag"
	"voila/internal/lexer"
)

type Parser struct {
	toks []lexer.Token
	i    int
	bag  *diag.Bag
	src  *diag.Source

	exprLev int // >0 inside parens/brackets; composite literals restricted when 0 in header contexts
	noLit   bool
}

// Parse lexes and parses a whole file.
func Parse(src *diag.Source, bag *diag.Bag) *ast.File {
	toks := lexer.Tokens(src, bag)
	p := &Parser{toks: toks, bag: bag, src: src}
	return p.parseFile()
}

// ParseExprString parses a single expression (used for string interpolation).
func ParseExprString(src *diag.Source, bag *diag.Bag) ast.Expr {
	toks := lexer.Tokens(src, bag)
	p := &Parser{toks: toks, bag: bag, src: src}
	e := p.parseExpr()
	p.skipSemis()
	if p.cur().Kind != lexer.EOF {
		p.errorf(p.cur().Pos, "unexpected %s after interpolated expression", p.cur())
	}
	return e
}

// ---------------------------------------------------------------- helpers

func (p *Parser) cur() lexer.Token { return p.toks[p.i] }

func (p *Parser) peek(n int) lexer.Token {
	if p.i+n < len(p.toks) {
		return p.toks[p.i+n]
	}
	return p.toks[len(p.toks)-1]
}

func (p *Parser) next() lexer.Token {
	t := p.toks[p.i]
	if p.i < len(p.toks)-1 {
		p.i++
	}
	return t
}

func (p *Parser) at(k lexer.Kind) bool { return p.cur().Kind == k }

func (p *Parser) accept(k lexer.Kind) bool {
	if p.at(k) {
		p.next()
		return true
	}
	return false
}

func (p *Parser) expect(k lexer.Kind) lexer.Token {
	if p.at(k) {
		return p.next()
	}
	p.errorf(p.cur().Pos, "expected %s, found %s", k, p.cur())
	return lexer.Token{Kind: k, Pos: p.cur().Pos}
}

func (p *Parser) errorf(pos diag.Pos, format string, args ...any) {
	p.bag.Errorf(diag.SpanOf(pos, 1), "", format, args...)
}

// skipSemis consumes any run of (virtual or explicit) semicolons.
func (p *Parser) skipSemis() {
	for p.at(lexer.SEMI) {
		p.next()
	}
}

// skipSemisBefore skips semicolons when the given keyword follows — used for
// `else`, `catch`, `finally` on the line after a closing brace.
func (p *Parser) skipSemisBefore(k lexer.Kind) bool {
	j := p.i
	for p.toks[j].Kind == lexer.SEMI {
		j++
	}
	if p.toks[j].Kind == k {
		p.i = j
		return true
	}
	return false
}

// expectSemi terminates a statement.
func (p *Parser) expectSemi() {
	if p.at(lexer.SEMI) {
		p.skipSemis()
		return
	}
	if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
		return
	}
	p.errorf(p.cur().Pos, "expected end of statement, found %s", p.cur())
	// Recover: skip to next semi or brace.
	for !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) && !p.at(lexer.EOF) {
		p.next()
	}
	p.skipSemis()
}

// ---------------------------------------------------------------- file

func declStart(k lexer.Kind) bool {
	switch k {
	case lexer.FUNC, lexer.STRUCT, lexer.ENUM, lexer.TRAIT, lexer.IMPL,
		lexer.EXCEPTION, lexer.TYPE:
		return true
	}
	return false
}

func (p *Parser) parseFile() *ast.File {
	f := &ast.File{P: p.cur().Pos}
	p.skipSemis()
	if p.at(lexer.PACKAGE) {
		p.next()
		f.Package = p.expect(lexer.IDENT).Lit
		p.expectSemi()
	}
	p.skipSemis()
	for p.at(lexer.USE) {
		f.Uses = append(f.Uses, p.parseUse())
		p.skipSemis()
	}
	for !p.at(lexer.EOF) {
		p.skipSemis()
		if p.at(lexer.EOF) {
			break
		}
		switch {
		case p.at(lexer.USE):
			f.Uses = append(f.Uses, p.parseUse())
		case declStart(p.cur().Kind):
			f.Decls = append(f.Decls, p.parseDecl())
		case p.at(lexer.CONST), p.at(lexer.LET), p.at(lexer.VAR):
			// Top-level binding.
			s := p.parseStmt()
			f.Decls = append(f.Decls, &ast.GlobalDecl{Stmt: s, P: s.Pos()})
		default:
			// Script-form statement (§1).
			f.ScriptStmts = append(f.ScriptStmts, p.parseStmt())
		}
		p.skipSemis()
	}
	return f
}

func (p *Parser) parseUse() *ast.Use {
	pos := p.expect(lexer.USE).Pos
	path := p.expect(lexer.STR)
	u := &ast.Use{Path: path.Lit, P: pos}
	switch {
	case p.accept(lexer.AS):
		u.Alias = p.expect(lexer.IDENT).Lit
	case p.at(lexer.LBRACE):
		p.next()
		for !p.at(lexer.RBRACE) && !p.at(lexer.EOF) {
			u.Names = append(u.Names, p.expect(lexer.IDENT).Lit)
			if !p.accept(lexer.COMMA) {
				break
			}
		}
		p.expect(lexer.RBRACE)
	case p.at(lexer.STAR):
		p.next()
		u.Wildcard = true
	}
	p.expectSemi()
	return u
}

// ---------------------------------------------------------------- decls

func (p *Parser) parseDecl() ast.Decl {
	switch p.cur().Kind {
	case lexer.FUNC:
		return p.parseFuncDecl()
	case lexer.STRUCT:
		return p.parseStructDecl()
	case lexer.ENUM:
		return p.parseEnumDecl()
	case lexer.TRAIT:
		return p.parseTraitDecl()
	case lexer.IMPL:
		return p.parseImplDecl()
	case lexer.EXCEPTION:
		return p.parseExceptionDecl()
	case lexer.TYPE:
		return p.parseTypeDecl()
	}
	p.errorf(p.cur().Pos, "expected declaration, found %s", p.cur())
	p.next()
	return &ast.TypeDecl{P: p.cur().Pos}
}

func (p *Parser) parseGenerics() []ast.GenericParam {
	if !p.at(lexer.LBRACKET) {
		return nil
	}
	p.next()
	var gs []ast.GenericParam
	for !p.at(lexer.RBRACKET) && !p.at(lexer.EOF) {
		g := ast.GenericParam{Name: p.expect(lexer.IDENT).Lit}
		if p.accept(lexer.COLON) {
			g.Bound = p.expect(lexer.IDENT).Lit
		}
		gs = append(gs, g)
		if !p.accept(lexer.COMMA) {
			break
		}
	}
	p.expect(lexer.RBRACKET)
	return gs
}

func (p *Parser) parseFuncDecl() *ast.FuncDecl {
	pos := p.expect(lexer.FUNC).Pos
	d := &ast.FuncDecl{P: pos}
	d.Name = p.expect(lexer.IDENT).Lit
	d.Generics = p.parseGenerics()
	p.parseParamsInto(d)
	// Result type(s).
	if !p.at(lexer.LBRACE) && !p.at(lexer.SEMI) && !p.at(lexer.THROWS) &&
		!p.at(lexer.NOTHROW) && !p.at(lexer.ASSIGN) && !p.at(lexer.EOF) {
		if p.at(lexer.LPAREN) {
			p.next()
			for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
				d.Results = append(d.Results, p.parseType())
				if !p.accept(lexer.COMMA) {
					break
				}
			}
			p.expect(lexer.RPAREN)
		} else {
			d.Results = append(d.Results, p.parseType())
		}
	}
	if p.accept(lexer.THROWS) {
		for {
			d.Throws = append(d.Throws, p.parseType())
			if !p.accept(lexer.COMMA) {
				break
			}
		}
	} else if p.accept(lexer.NOTHROW) {
		d.NoThrow = true
	}
	switch {
	case p.at(lexer.LBRACE):
		d.Body = p.parseBlock()
	case p.accept(lexer.ASSIGN):
		d.Default = p.parseExpr() // trait default impl, e.g. `= nil`
	}
	return d
}

// parseParamsInto parses "(" params ")" handling self, modes, grouped names,
// defaults and variadics.
func (p *Parser) parseParamsInto(d *ast.FuncDecl) {
	p.expect(lexer.LPAREN)
	p.exprLev++
	defer func() { p.exprLev-- }()
	type entry struct {
		param   *ast.Param
		untyped bool
	}
	var entries []entry
	for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
		mode := ast.ModeBorrow
		if p.accept(lexer.OWN) {
			mode = ast.ModeOwn
		} else if p.accept(lexer.MUT) {
			mode = ast.ModeMut
		}
		if p.at(lexer.SELF) {
			pos := p.next().Pos
			d.HasSelf = true
			d.SelfMode = mode
			_ = pos
			if !p.accept(lexer.COMMA) {
				break
			}
			continue
		}
		if p.at(lexer.ELLIPSIS) { // bare `...` (ffi-style); treat as variadic any
			pos := p.next().Pos
			entries = append(entries, entry{&ast.Param{Name: "args", Variadic: true, P: pos}, false})
			break
		}
		nameTok := p.expect(lexer.IDENT)
		prm := &ast.Param{Mode: mode, Name: nameTok.Lit, P: nameTok.Pos}
		// Postfix mode: the spec also writes `buf mut []byte` (§3.6).
		if p.accept(lexer.MUT) {
			prm.Mode = ast.ModeMut
		} else if p.accept(lexer.OWN) {
			prm.Mode = ast.ModeOwn
		}
		if p.accept(lexer.ELLIPSIS) { // nums ...int
			prm.Variadic = true
			prm.Type = p.parseType()
			entries = append(entries, entry{prm, false})
		} else if p.at(lexer.COMMA) || p.at(lexer.RPAREN) {
			entries = append(entries, entry{prm, true}) // type comes from a later group member
		} else if p.at(lexer.ASSIGN) {
			p.next()
			prm.Default = p.parseExpr()
			entries = append(entries, entry{prm, true})
		} else {
			prm.Type = p.parseType()
			if p.accept(lexer.ASSIGN) {
				prm.Default = p.parseExpr()
			}
			entries = append(entries, entry{prm, false})
		}
		if !p.accept(lexer.COMMA) {
			break
		}
	}
	p.expect(lexer.RPAREN)
	// Propagate shared types backward: `a, b int` gives both int.
	var lastType ast.TypeExpr
	for i := len(entries) - 1; i >= 0; i-- {
		if !entries[i].untyped {
			lastType = entries[i].param.Type
		} else if entries[i].param.Type == nil {
			entries[i].param.Type = lastType
		}
	}
	for _, e := range entries {
		d.Params = append(d.Params, e.param)
	}
}

func (p *Parser) parseFieldList(term lexer.Kind) []*ast.Field {
	var fields []*ast.Field
	for !p.at(term) && !p.at(lexer.EOF) {
		p.skipSemis()
		if p.at(term) {
			break
		}
		nameTok := p.expect(lexer.IDENT)
		f := &ast.Field{Name: nameTok.Lit, P: nameTok.Pos}
		f.Type = p.parseType()
		if p.accept(lexer.ASSIGN) {
			f.Default = p.parseExpr()
		}
		fields = append(fields, f)
		// Separators: newline (virtual semi), `;`, or `,` (§14 uses all three).
		if !p.accept(lexer.COMMA) {
			if !p.at(term) {
				p.expectSemi()
			}
		}
		p.skipSemis()
	}
	return fields
}

func (p *Parser) parseStructDecl() *ast.StructDecl {
	pos := p.expect(lexer.STRUCT).Pos
	d := &ast.StructDecl{P: pos}
	d.Name = p.expect(lexer.IDENT).Lit
	d.Generics = p.parseGenerics()
	p.expect(lexer.LBRACE)
	d.Fields = p.parseFieldList(lexer.RBRACE)
	p.expect(lexer.RBRACE)
	return d
}

func (p *Parser) parseEnumDecl() *ast.EnumDecl {
	pos := p.expect(lexer.ENUM).Pos
	d := &ast.EnumDecl{P: pos}
	d.Name = p.expect(lexer.IDENT).Lit
	d.Generics = p.parseGenerics()
	p.expect(lexer.LBRACE)
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		vtok := p.expect(lexer.IDENT)
		v := &ast.Variant{Name: vtok.Lit, P: vtok.Pos}
		if p.accept(lexer.LPAREN) {
			p.exprLev++
			n := 0
			for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
				// `radius float` (named) or bare type `Some(T)`.
				if p.at(lexer.IDENT) && (p.peek(1).Kind == lexer.IDENT || typeStart(p.peek(1).Kind)) {
					nameTok := p.next()
					v.Fields = append(v.Fields, &ast.Field{Name: nameTok.Lit, Type: p.parseType(), P: nameTok.Pos})
				} else {
					v.Fields = append(v.Fields, &ast.Field{Name: "_" + strconv.Itoa(n), Type: p.parseType(), P: p.cur().Pos})
				}
				n++
				if !p.accept(lexer.COMMA) {
					break
				}
			}
			p.exprLev--
			p.expect(lexer.RPAREN)
		}
		d.Variants = append(d.Variants, v)
		if !p.accept(lexer.COMMA) {
			if !p.at(lexer.RBRACE) {
				p.expectSemi()
			}
		}
	}
	p.expect(lexer.RBRACE)
	return d
}

func typeStart(k lexer.Kind) bool {
	switch k {
	case lexer.LBRACKET, lexer.QUESTION, lexer.CHAN, lexer.SHARED, lexer.WEAK,
		lexer.LPAREN, lexer.AMP:
		return true
	}
	return false
}

func (p *Parser) parseTraitDecl() *ast.TraitDecl {
	pos := p.expect(lexer.TRAIT).Pos
	d := &ast.TraitDecl{P: pos}
	d.Name = p.expect(lexer.IDENT).Lit
	p.expect(lexer.LBRACE)
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		d.Methods = append(d.Methods, p.parseFuncDecl())
		p.skipSemis()
	}
	p.expect(lexer.RBRACE)
	return d
}

func (p *Parser) parseImplDecl() *ast.ImplDecl {
	pos := p.expect(lexer.IMPL).Pos
	d := &ast.ImplDecl{P: pos}
	d.Generics = p.parseGenerics()
	first := p.parseType()
	if p.accept(lexer.FOR) {
		if nt, ok := first.(*ast.NamedType); ok {
			d.Trait = nt.Name
		} else {
			p.errorf(first.Pos(), "trait name expected before `for`")
		}
		d.Type = p.parseType()
	} else {
		d.Type = first
	}
	p.expect(lexer.LBRACE)
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		d.Methods = append(d.Methods, p.parseFuncDecl())
		p.skipSemis()
	}
	p.expect(lexer.RBRACE)
	return d
}

func (p *Parser) parseExceptionDecl() *ast.ExceptionDecl {
	pos := p.expect(lexer.EXCEPTION).Pos
	d := &ast.ExceptionDecl{P: pos}
	d.Name = p.expect(lexer.IDENT).Lit
	p.expect(lexer.LBRACE)
	for {
		p.skipSemis()
		if p.at(lexer.RBRACE) || p.at(lexer.EOF) {
			break
		}
		if p.at(lexer.FUNC) {
			d.Methods = append(d.Methods, p.parseFuncDecl())
		} else {
			nameTok := p.expect(lexer.IDENT)
			f := &ast.Field{Name: nameTok.Lit, P: nameTok.Pos}
			f.Type = p.parseType()
			if p.accept(lexer.ASSIGN) {
				f.Default = p.parseExpr()
			}
			d.Fields = append(d.Fields, f)
			p.accept(lexer.COMMA)
		}
		p.skipSemis()
	}
	p.expect(lexer.RBRACE)
	return d
}

func (p *Parser) parseTypeDecl() ast.Decl {
	pos := p.expect(lexer.TYPE).Pos
	name := p.expect(lexer.IDENT).Lit
	d := &ast.TypeDecl{Name: name, P: pos}
	switch {
	case p.accept(lexer.ASSIGN):
		d.Alias = true
		d.Target = p.parseType()
	case p.at(lexer.STRUCT):
		p.next()
		sd := &ast.StructDecl{Name: name, P: pos}
		p.expect(lexer.LBRACE)
		sd.Fields = p.parseFieldList(lexer.RBRACE)
		p.expect(lexer.RBRACE)
		d.Struct = sd
	default:
		d.Target = p.parseType()
	}
	p.expectSemi()
	return d
}

// ---------------------------------------------------------------- types

func (p *Parser) parseType() ast.TypeExpr {
	t := p.parseTypeInner()
	// Postfix `!` → fallible.
	for p.at(lexer.BANG) {
		pos := p.next().Pos
		t = &ast.ResultType{Elem: t, P: pos}
	}
	return t
}

func (p *Parser) parseTypeInner() ast.TypeExpr {
	pos := p.cur().Pos
	switch p.cur().Kind {
	case lexer.QUESTION:
		p.next()
		return &ast.OptionType{Elem: p.parseType(), P: pos}
	case lexer.AMP:
		p.next()
		return &ast.BorrowType{Elem: p.parseType(), P: pos}
	case lexer.LBRACKET:
		p.next()
		if p.accept(lexer.RBRACKET) {
			return &ast.SliceType{Elem: p.parseType(), P: pos}
		}
		p.exprLev++
		n := p.parseExpr()
		p.exprLev--
		p.expect(lexer.RBRACKET)
		return &ast.SliceType{Elem: p.parseType(), Len: n, P: pos}
	case lexer.CHAN:
		p.next()
		p.expect(lexer.LBRACKET)
		e := p.parseType()
		p.expect(lexer.RBRACKET)
		return &ast.ChanType{Elem: e, P: pos}
	case lexer.SHARED:
		p.next()
		p.expect(lexer.LBRACKET)
		e := p.parseType()
		p.expect(lexer.RBRACKET)
		return &ast.BoxType{Kind: ast.SharedBox, Elem: e, P: pos}
	case lexer.WEAK:
		p.next()
		p.expect(lexer.LBRACKET)
		e := p.parseType()
		p.expect(lexer.RBRACKET)
		return &ast.BoxType{Kind: ast.WeakBox, Elem: e, P: pos}
	case lexer.FUNC:
		// `fn` is not a keyword; `func` types shouldn't appear, but accept.
		p.next()
		return p.parseFnTypeTail(pos)
	case lexer.LPAREN:
		p.next()
		var elems []ast.TypeExpr
		for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
			elems = append(elems, p.parseType())
			if !p.accept(lexer.COMMA) {
				break
			}
		}
		p.expect(lexer.RPAREN)
		return &ast.TupleType{Elems: elems, P: pos}
	case lexer.IDENT:
		name := p.next().Lit
		switch name {
		case "map":
			p.expect(lexer.LBRACKET)
			k := p.parseType()
			p.expect(lexer.RBRACKET)
			v := p.parseType()
			return &ast.MapType{Key: k, Value: v, P: pos}
		case "set":
			p.expect(lexer.LBRACKET)
			e := p.parseType()
			p.expect(lexer.RBRACKET)
			return &ast.SetType{Elem: e, P: pos}
		case "cell":
			if p.at(lexer.LBRACKET) {
				p.next()
				e := p.parseType()
				p.expect(lexer.RBRACKET)
				return &ast.BoxType{Kind: ast.CellBox, Elem: e, P: pos}
			}
		case "fn":
			return p.parseFnTypeTail(pos)
		}
		nt := &ast.NamedType{Name: name, P: pos}
		if p.accept(lexer.DOT) {
			nt.Pkg = nt.Name
			nt.Name = p.expect(lexer.IDENT).Lit
		}
		if p.at(lexer.LBRACKET) {
			p.next()
			for !p.at(lexer.RBRACKET) && !p.at(lexer.EOF) {
				nt.Args = append(nt.Args, p.parseType())
				if !p.accept(lexer.COMMA) {
					break
				}
			}
			p.expect(lexer.RBRACKET)
		}
		return nt
	}
	p.errorf(pos, "expected type, found %s", p.cur())
	p.next()
	return &ast.NamedType{Name: "unit", P: pos}
}

func (p *Parser) parseFnTypeTail(pos diag.Pos) ast.TypeExpr {
	ft := &ast.FuncType{P: pos}
	p.expect(lexer.LPAREN)
	for !p.at(lexer.RPAREN) && !p.at(lexer.EOF) {
		ft.Params = append(ft.Params, p.parseType())
		if !p.accept(lexer.COMMA) {
			break
		}
	}
	p.expect(lexer.RPAREN)
	if !p.at(lexer.LBRACE) && !p.at(lexer.RPAREN) && !p.at(lexer.RBRACKET) &&
		!p.at(lexer.COMMA) && !p.at(lexer.SEMI) && !p.at(lexer.RBRACE) &&
		!p.at(lexer.EOF) && !p.at(lexer.ASSIGN) {
		ft.Result = p.parseType()
	}
	return ft
}
