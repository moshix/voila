package loader

import (
	"unicode"

	"voila/internal/ast"
	"voila/internal/diag"
)

// rewriter qualifies names. Inside package P (self != nil) every reference to
// one of P's own declarations becomes `P.name`; in every file, a reference
// `Q.name` into an imported user package becomes the flat name `Q.name`.
//
// It tracks local scopes, so a variable that happens to be called `parser`
// shadows the package of that name — the same rule the interpreter applies.
type rewriter struct {
	self    *pkg
	imports map[string]*pkg
	bag     *diag.Bag
	scopes  []map[string]bool
}

func (r *rewriter) push() { r.scopes = append(r.scopes, map[string]bool{}) }
func (r *rewriter) pop()  { r.scopes = r.scopes[:len(r.scopes)-1] }
func (r *rewriter) bind(n string) {
	if len(r.scopes) == 0 || n == "" || n == "_" {
		return
	}
	r.scopes[len(r.scopes)-1][n] = true
}

func (r *rewriter) bound(n string) bool {
	for i := len(r.scopes) - 1; i >= 0; i-- {
		if r.scopes[i][n] {
			return true
		}
	}
	return false
}

// qualify returns the flat name for one of self's declarations. A name that a
// local binding (or a generic parameter) shadows is left alone.
func (r *rewriter) qualify(n string) (string, bool) {
	if r.self == nil || !r.self.decls[n] || r.bound(n) {
		return "", false
	}
	return r.self.name + "." + n, true
}

// member resolves `Q.name` into an imported user package.
func (r *rewriter) member(q, name string, pos diag.Pos) (string, bool) {
	p, ok := r.imports[q]
	if !ok || r.bound(q) {
		return "", false
	}
	if !p.decls[name] {
		errorf(r.bag, pos, len(name), "package `%s` has no exported `%s`", q, name)
		return "", false
	}
	if !exported(name) {
		errorf(r.bag, pos, len(name),
			"`%s.%s` is package-private (an exported name starts with a capital)", q, name)
		return "", false
	}
	return p.name + "." + name, true
}

func exported(n string) bool {
	if n == "" {
		return false
	}
	return unicode.IsUpper([]rune(n)[0])
}

// ---------------------------------------------------------------- file

func (r *rewriter) file(f *ast.File) {
	// Declaration names first, so bodies can be rewritten against them.
	for _, d := range f.Decls {
		r.declName(d)
	}
	for _, d := range f.Decls {
		r.decl(d)
	}
	r.push()
	for _, s := range f.ScriptStmts {
		r.stmt(s)
	}
	r.pop()
}

func (r *rewriter) declName(d ast.Decl) {
	if r.self == nil {
		return
	}
	switch decl := d.(type) {
	case *ast.FuncDecl:
		if q, ok := r.qualify(decl.Name); ok {
			decl.Name = q
		}
	case *ast.StructDecl:
		if q, ok := r.qualify(decl.Name); ok {
			decl.Name = q
		}
	case *ast.EnumDecl:
		if q, ok := r.qualify(decl.Name); ok {
			decl.Name = q
		}
	case *ast.ExceptionDecl:
		if q, ok := r.qualify(decl.Name); ok {
			decl.Name = q
		}
	case *ast.TypeDecl:
		if q, ok := r.qualify(decl.Name); ok {
			decl.Name = q
			if decl.Struct != nil {
				decl.Struct.Name = q
			}
		}
	case *ast.GlobalDecl:
		switch st := decl.Stmt.(type) {
		case *ast.LetStmt:
			for i, n := range st.Names {
				if q, ok := r.qualify(n); ok {
					st.Names[i] = q
				}
			}
		case *ast.VarStmt:
			if q, ok := r.qualify(st.Name); ok {
				st.Name = q
			}
		}
	}
}

func (r *rewriter) decl(d ast.Decl) {
	switch decl := d.(type) {
	case *ast.FuncDecl:
		r.fn(decl)
	case *ast.StructDecl:
		r.push()
		r.bindGenerics(decl.Generics)
		for _, f := range decl.Fields {
			r.typ(&f.Type)
			r.expr(&f.Default)
		}
		r.pop()
	case *ast.EnumDecl:
		r.push()
		r.bindGenerics(decl.Generics)
		for _, v := range decl.Variants {
			for _, f := range v.Fields {
				r.typ(&f.Type)
				r.expr(&f.Default)
			}
		}
		r.pop()
	case *ast.ExceptionDecl:
		for _, f := range decl.Fields {
			r.typ(&f.Type)
			r.push()
			r.expr(&f.Default)
			r.pop()
		}
		for _, m := range decl.Methods {
			r.fn(m)
		}
	case *ast.TraitDecl:
		for _, m := range decl.Methods {
			r.fn(m)
		}
	case *ast.ImplDecl:
		r.push()
		r.bindGenerics(decl.Generics)
		r.typ(&decl.Type)
		for _, m := range decl.Methods {
			r.fn(m)
		}
		r.pop()
	case *ast.TypeDecl:
		r.typ(&decl.Target)
		if decl.Struct != nil {
			r.push()
			r.bindGenerics(decl.Struct.Generics)
			for _, f := range decl.Struct.Fields {
				r.typ(&f.Type)
				r.expr(&f.Default)
			}
			r.pop()
		}
	case *ast.GlobalDecl:
		r.push()
		r.stmt(decl.Stmt)
		r.pop()
	}
}

// bindGenerics makes a generic parameter shadow a package type of the same
// name: in `func F[T](x T)`, `T` is the parameter, not the package's `T`.
func (r *rewriter) bindGenerics(gs []ast.GenericParam) {
	for _, g := range gs {
		r.bind(g.Name)
	}
}

func (r *rewriter) fn(d *ast.FuncDecl) {
	r.push()
	defer r.pop()
	r.bindGenerics(d.Generics)
	if d.HasSelf {
		r.bind("self")
	}
	for _, p := range d.Params {
		r.typ(&p.Type)
		r.expr(&p.Default)
		r.bind(p.Name)
	}
	for i := range d.Results {
		r.typ(&d.Results[i])
	}
	for i := range d.Throws {
		r.typ(&d.Throws[i])
	}
	if d.Body != nil {
		r.block(d.Body)
	}
	r.expr(&d.Default)
}

// ---------------------------------------------------------------- types

func (r *rewriter) typ(t *ast.TypeExpr) {
	if t == nil || *t == nil {
		return
	}
	switch ty := (*t).(type) {
	case *ast.NamedType:
		if ty.Pkg != "" {
			if flat, ok := r.member(ty.Pkg, ty.Name, ty.P); ok {
				ty.Pkg = ""
				ty.Name = flat
			}
		} else if q, ok := r.qualify(ty.Name); ok {
			ty.Name = q
		}
		for i := range ty.Args {
			r.typ(&ty.Args[i])
		}
	case *ast.OptionType:
		r.typ(&ty.Elem)
	case *ast.ResultType:
		r.typ(&ty.Elem)
	case *ast.SliceType:
		r.typ(&ty.Elem)
		r.expr(&ty.Len)
	case *ast.MapType:
		r.typ(&ty.Key)
		r.typ(&ty.Value)
	case *ast.SetType:
		r.typ(&ty.Elem)
	case *ast.ChanType:
		r.typ(&ty.Elem)
	case *ast.BoxType:
		r.typ(&ty.Elem)
	case *ast.BorrowType:
		r.typ(&ty.Elem)
	case *ast.FuncType:
		for i := range ty.Params {
			r.typ(&ty.Params[i])
		}
		r.typ(&ty.Result)
	case *ast.TupleType:
		for i := range ty.Elems {
			r.typ(&ty.Elems[i])
		}
	}
}

// ---------------------------------------------------------------- statements

func (r *rewriter) block(b *ast.Block) {
	if b == nil {
		return
	}
	r.push()
	defer r.pop()
	for _, s := range b.Stmts {
		r.stmt(s)
	}
}

func (r *rewriter) stmt(s ast.Stmt) {
	switch st := s.(type) {
	case nil:
		return
	case *ast.Block:
		r.block(st)
	case *ast.LetStmt:
		r.typ(&st.Type)
		r.expr(&st.Value)
		for _, n := range st.Names {
			r.bind(n)
		}
	case *ast.VarStmt:
		r.typ(&st.Type)
		r.expr(&st.Value)
		r.bind(st.Name)
	case *ast.AssignStmt:
		for i := range st.LHS {
			r.expr(&st.LHS[i])
		}
		r.expr(&st.RHS)
	case *ast.ExprStmt:
		r.expr(&st.X)
	case *ast.DropStmt:
		if !r.bound(st.Name) {
			if q, ok := r.qualify(st.Name); ok {
				st.Name = q
			}
		}
	case *ast.IfStmt:
		r.expr(&st.Cond)
		r.expr(&st.Value)
		r.push()
		r.pattern(st.Pattern)
		r.block(st.Then)
		r.pop()
		r.stmt(st.Else)
	case *ast.ForStmt:
		r.expr(&st.Iter)
		r.push()
		r.bind(st.Var)
		r.block(st.Body)
		r.pop()
	case *ast.EachStmt:
		r.expr(&st.Iter)
		r.push()
		r.bind(st.A)
		r.bind(st.B)
		r.block(st.Body)
		r.pop()
	case *ast.WhileStmt:
		r.expr(&st.Cond)
		r.block(st.Body)
	case *ast.SwitchStmt:
		r.expr(&st.Subject)
		for _, c := range st.Cases {
			for i := range c.Values {
				r.expr(&c.Values[i])
			}
			r.push()
			for _, b := range c.Body {
				r.stmt(b)
			}
			r.pop()
		}
	case *ast.MatchStmt:
		r.expr(&st.Subject)
		for _, arm := range st.Arms {
			r.push()
			r.pattern(arm.Pattern)
			r.expr(&arm.Guard)
			for _, b := range arm.Body {
				r.stmt(b)
			}
			r.pop()
		}
	case *ast.SelectStmt:
		for _, c := range st.Cases {
			r.expr(&c.Ch)
			r.expr(&c.Send)
			r.push()
			r.bind(c.Bind)
			r.bind(c.BindOk)
			for _, b := range c.Body {
				r.stmt(b)
			}
			r.pop()
		}
	case *ast.GroupStmt:
		r.expr(&st.Timeout)
		r.block(st.Body)
	case *ast.ParseStmt:
		r.expr(&st.Src)
		for i := range st.Terms {
			t := &st.Terms[i]
			if t.Kind != ast.ParseVar {
				continue
			}
			// A target that names a package-level global must be QUALIFIED, or
			// the parse would silently write a fresh local instead.
			if !r.bound(t.Name) {
				if q, ok := r.qualify(t.Name); ok {
					t.Name = q
					continue
				}
			}
			r.bind(t.Name)
		}
	case *ast.SayStmt:
		for i := range st.Args {
			r.expr(&st.Args[i])
		}
	case *ast.DeferStmt:
		r.expr(&st.Call)
		r.block(st.Block)
	case *ast.ReturnStmt:
		for i := range st.Values {
			r.expr(&st.Values[i])
		}
	case *ast.ThrowStmt:
		r.expr(&st.Value)
		r.expr(&st.From)
	case *ast.TryCatchStmt:
		r.block(st.Body)
		for _, c := range st.Catches {
			for i := range c.Types {
				r.typ(&c.Types[i])
			}
			r.push()
			r.bind(c.Name)
			r.block(c.Body)
			r.pop()
		}
		r.block(st.Finally)
	}
}

// pattern binds a pattern's names; variant names stay global.
func (r *rewriter) pattern(p ast.Pattern) {
	switch pt := p.(type) {
	case *ast.BindPat:
		r.bind(pt.Name)
	case *ast.VariantPat:
		for _, e := range pt.Elems {
			r.pattern(e)
		}
	}
}

// ---------------------------------------------------------------- expressions

func (r *rewriter) expr(e *ast.Expr) {
	if e == nil || *e == nil {
		return
	}
	switch x := (*e).(type) {
	case *ast.Ident:
		if r.bound(x.Name) {
			return
		}
		if q, ok := r.qualify(x.Name); ok {
			x.Name = q
		}
	case *ast.FieldExpr:
		// `Q.name` into an imported user package flattens to one identifier.
		if base, ok := x.X.(*ast.Ident); ok && !r.bound(base.Name) {
			if flat, ok := r.member(base.Name, x.Name, x.P); ok {
				*e = &ast.Ident{Name: flat, P: x.P}
				return
			}
		}
		r.expr(&x.X)
	case *ast.UnaryExpr:
		r.expr(&x.X)
	case *ast.BinaryExpr:
		r.expr(&x.X)
		r.expr(&x.Y)
	case *ast.RangeExpr:
		r.expr(&x.Lo)
		r.expr(&x.Hi)
		r.expr(&x.By)
	case *ast.CallExpr:
		r.expr(&x.Fun)
		for i := range x.TypeArgs {
			r.typ(&x.TypeArgs[i])
		}
		for i := range x.Args {
			r.expr(&x.Args[i].Value)
		}
	case *ast.IndexExpr:
		r.expr(&x.X)
		r.expr(&x.Index)
	case *ast.CompositeLit:
		if x.Type != nil {
			if x.Type.Pkg != "" {
				if flat, ok := r.member(x.Type.Pkg, x.Type.Name, x.Type.P); ok {
					x.Type.Pkg = ""
					x.Type.Name = flat
				}
			} else if q, ok := r.qualify(x.Type.Name); ok {
				x.Type.Name = q
			}
			for i := range x.Type.Args {
				r.typ(&x.Type.Args[i])
			}
		}
		for i := range x.Fields {
			r.expr(&x.Fields[i].Value)
		}
	case *ast.SliceLit:
		for i := range x.Elems {
			r.expr(&x.Elems[i])
		}
	case *ast.TupleExpr:
		for i := range x.Elems {
			r.expr(&x.Elems[i])
		}
	case *ast.InterpStr:
		for i := range x.Parts {
			r.expr(&x.Parts[i].Expr)
		}
	case *ast.ClosureExpr:
		r.push()
		for _, p := range x.Params {
			r.typ(&p.Type)
			r.expr(&p.Default) // before bind: a default cannot see its own param
			r.bind(p.Name)
		}
		r.typ(&x.Result)
		r.block(x.Body)
		r.pop()
	case *ast.TryExpr:
		r.expr(&x.X)
	case *ast.MustExpr:
		r.expr(&x.X)
	case *ast.AttemptExpr:
		r.expr(&x.X)
	case *ast.ElseExpr:
		r.expr(&x.X)
		r.expr(&x.Alt)
		r.stmt(x.AltStmt)
	case *ast.SpawnExpr:
		r.expr(&x.Call)
		r.block(x.Block)
	}
}
