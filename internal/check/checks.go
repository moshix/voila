package check

import (
	"fmt"
	"strconv"
	"strings"

	"voila/internal/ast"
	"voila/internal/diag"
)

// ---------------------------------------------------------------- lattice

var intRanges = map[string][2]int64{
	"i8": {-128, 127}, "i16": {-32768, 32767}, "i32": {-2147483648, 2147483647},
	"i64": {-9223372036854775808, 9223372036854775807},
	"int": {-9223372036854775808, 9223372036854775807},
	"u8":  {0, 255}, "u16": {0, 65535}, "u32": {0, 4294967295},
	"u64": {0, 9223372036854775807}, "byte": {0, 255},
}

// widensTo reports whether type `from` implicitly converts to `to` (§3.2).
func widensTo(from, to string) bool {
	if from == to {
		return true
	}
	order := map[string]int{"i8": 1, "i16": 2, "i32": 3, "i64": 4, "int": 4}
	uorder := map[string]int{"u8": 1, "byte": 1, "u16": 2, "u32": 3, "u64": 4}
	if fo, ok := order[from]; ok {
		if to2, ok := order[to]; ok {
			return fo <= to2
		}
		if to == "dec" {
			return true
		}
		if to == "float" {
			return fo <= 3 // i8..i32 fit the 53-bit mantissa; i64 does NOT
		}
		return false
	}
	if fo, ok := uorder[from]; ok {
		if to2, ok := uorder[to]; ok {
			return fo <= to2
		}
		if to2, ok := order[to]; ok {
			return fo < to2 // u8→i16, u16→i32, u32→i64
		}
		if to == "dec" {
			return true
		}
		if to == "float" {
			return fo <= 3
		}
		return false
	}
	switch from {
	case "f32":
		return to == "float"
	case "float":
		return false // float→dec and float→int are explicit
	case "dec":
		return false
	}
	return false
}

// checkLattice walks let/var declarations with explicit primitive types and
// verifies literal and known-binding initializers against §3.2.
func (c *checker) checkLattice() {
	for _, d := range c.file.Decls {
		if fd, ok := d.(*ast.FuncDecl); ok && fd.Body != nil {
			c.latticeBlock(fd.Body, map[string]string{})
		}
		if g, ok := d.(*ast.GlobalDecl); ok {
			c.latticeStmt(g.Stmt, map[string]string{})
		}
	}
	for _, s := range c.file.ScriptStmts {
		c.latticeStmt(s, map[string]string{})
	}
}

func (c *checker) latticeBlock(b *ast.Block, scope map[string]string) {
	inner := map[string]string{}
	for k, v := range scope {
		inner[k] = v
	}
	for _, s := range b.Stmts {
		c.latticeStmt(s, inner)
	}
}

func (c *checker) latticeStmt(s ast.Stmt, scope map[string]string) {
	switch st := s.(type) {
	case *ast.LetStmt:
		c.latticeBinding(st.Type, st.Value, scope, st.Names[0])
	case *ast.VarStmt:
		if st.Value != nil {
			c.latticeBinding(st.Type, st.Value, scope, st.Name)
		} else if nt, ok := st.Type.(*ast.NamedType); ok {
			scope[st.Name] = nt.Name
		}
	case *ast.Block:
		c.latticeBlock(st, scope)
	case *ast.IfStmt:
		c.latticeBlock(st.Then, scope)
		if st.Else != nil {
			c.latticeStmt(st.Else, scope)
		}
	case *ast.ForStmt:
		c.latticeBlock(st.Body, scope)
	case *ast.EachStmt:
		c.latticeBlock(st.Body, scope)
	case *ast.WhileStmt:
		c.latticeBlock(st.Body, scope)
	case *ast.TryCatchStmt:
		c.latticeBlock(st.Body, scope)
		for _, cc := range st.Catches {
			c.latticeBlock(cc.Body, scope)
		}
		if st.Finally != nil {
			c.latticeBlock(st.Finally, scope)
		}
	case *ast.GroupStmt:
		c.latticeBlock(st.Body, scope)
	}
}

func (c *checker) latticeBinding(t ast.TypeExpr, value ast.Expr, scope map[string]string, name string) {
	nt, ok := t.(*ast.NamedType)
	if !ok {
		if t == nil {
			// Track inferred primitive types of plain literals for later checks.
			if lit, ok := value.(*ast.BasicLit); ok {
				switch lit.Kind {
				case ast.IntLit:
					scope[name] = "int"
				case ast.FloatLit:
					scope[name] = "float"
				case ast.DecLit:
					scope[name] = "dec"
				}
			}
		}
		return
	}
	target := nt.Name
	scope[name] = target

	switch v := value.(type) {
	case *ast.BasicLit:
		c.latticeLiteral(target, v)
	case *ast.UnaryExpr:
		if lit, ok := v.X.(*ast.BasicLit); ok && lit.Kind == ast.IntLit {
			c.latticeLiteral(target, lit) // sign handled by range symmetry below
		}
	case *ast.Ident:
		if from, ok := scope[v.Name]; ok {
			if _, isPrim := intRanges[from]; isPrim || from == "float" || from == "dec" || from == "f32" {
				if _, isTarget := intRanges[target]; isTarget || target == "float" || target == "dec" || target == "f32" {
					if !widensTo(from, target) {
						c.latticeError(from, target, v.P, len(v.Name))
					}
				}
			}
		}
	}
}

func (c *checker) latticeLiteral(target string, lit *ast.BasicLit) {
	switch lit.Kind {
	case ast.IntLit:
		if rng, ok := intRanges[target]; ok {
			n, err := strconv.ParseInt(lit.Text, 0, 64)
			if err == nil && (n < rng[0] || n > rng[1]) {
				c.bag.Add(diag.Diagnostic{
					Severity: diag.Error,
					Message:  fmt.Sprintf("literal %s does not fit in %s", lit.Text, target),
					Span:     diag.SpanOf(lit.P, len(lit.Text)),
					Label:    fmt.Sprintf("out of range for %s", target),
				})
			}
		}
	case ast.FloatLit:
		switch target {
		case "int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "byte":
			c.bag.Add(diag.Diagnostic{
				Severity: diag.Error,
				Message:  fmt.Sprintf("float → %s truncates", target),
				Span:     diag.SpanOf(lit.P, len(lit.Text)),
				Label:    "lossy conversion",
				Help:     []string{fmt.Sprintf("use int(%s) or round(%s)", lit.Text, lit.Text)},
			})
		case "dec":
			c.bag.Add(diag.Diagnostic{
				Severity: diag.Error,
				Message:  "binary float → dec is lossy",
				Span:     diag.SpanOf(lit.P, len(lit.Text)),
				Label:    "lossy conversion",
				Help:     []string{fmt.Sprintf(`use dec("%s") or %sd`, lit.Text, lit.Text)},
			})
		}
	case ast.DecLit:
		switch target {
		case "float", "f32", "int", "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64":
			c.latticeError("dec", target, lit.P, len(lit.Text)+1)
		}
	case ast.StrLit:
		if _, isNum := intRanges[target]; isNum || target == "float" || target == "dec" {
			c.bag.Add(diag.Diagnostic{
				Severity: diag.Error,
				Message:  fmt.Sprintf("str does not convert implicitly to %s", target),
				Span:     diag.SpanOf(lit.P, len(lit.Text)+2),
				Label:    "explicit conversion required",
				Help:     []string{fmt.Sprintf("use %s(...)", target)},
			})
		}
	}
}

func (c *checker) latticeError(from, to string, pos diag.Pos, width int) {
	help := ""
	switch {
	case from == "i64" && to == "float":
		help = "i64 → float may lose precision. Use float(x)."
	case to == "int":
		help = "use int(x) or round(x)"
	case to == "dec":
		help = `use dec(x) or a d-suffixed literal`
	default:
		help = fmt.Sprintf("use %s(x)", to)
	}
	c.bag.Add(diag.Diagnostic{
		Severity: diag.Error,
		Message:  fmt.Sprintf("%s → %s may lose information; implicit conversion requires exact representability (§3.2)", from, to),
		Span:     diag.SpanOf(pos, width),
		Label:    "lossy conversion",
		Help:     []string{help},
	})
}

// ---------------------------------------------------------------- match

// checkExhaustiveness verifies match statements cover their subject (§3.5).
func (c *checker) checkExhaustiveness() {
	walkStmts(c.file, func(s ast.Stmt) {
		m, ok := s.(*ast.MatchStmt)
		if !ok {
			return
		}
		var variants []string
		sawCatchAll := false
		sawNil, sawSome, sawOk, sawErr := false, false, false, false
		for _, arm := range m.Arms {
			if arm.Guard != nil {
				continue // guarded arms don't count toward exhaustiveness
			}
			switch p := arm.Pattern.(type) {
			case *ast.WildcardPat, *ast.BindPat:
				sawCatchAll = true
			case *ast.NilPat:
				sawNil = true
			case *ast.VariantPat:
				switch p.Name {
				case "Some":
					sawSome = true
				case "Ok":
					sawOk = true
				case "Err":
					sawErr = true
				default:
					variants = append(variants, p.Name)
				}
			}
		}
		if sawCatchAll || (sawNil && sawSome) || (sawOk && sawErr) {
			return
		}
		// All named variants of one enum?
		if len(variants) > 0 {
			if enum := c.enumOf(variants[0]); enum != nil {
				covered := map[string]bool{}
				for _, v := range variants {
					covered[v] = true
				}
				var missing []string
				for _, v := range enum.Variants {
					if !covered[v.Name] {
						missing = append(missing, v.Name)
					}
				}
				if sawNil || sawSome {
					// matching ?Enum: Some/nil combos handled above
					return
				}
				if len(missing) == 0 {
					return
				}
				c.bag.Add(diag.Diagnostic{
					Severity: diag.Error,
					Message:  fmt.Sprintf("match on `%s` is not exhaustive", enum.Name),
					Span:     diag.SpanOf(m.P, len("match")),
					Label:    "missing cases: " + strings.Join(missing, ", "),
					Help:     []string{"add the missing variants or a `_` arm"},
				})
				return
			}
		}
		c.bag.Add(diag.Diagnostic{
			Severity: diag.Error,
			Message:  "match is not exhaustive",
			Span:     diag.SpanOf(m.P, len("match")),
			Label:    "no catch-all arm",
			Help:     []string{"add a `_` arm"},
		})
	})
}

func (c *checker) enumOf(variant string) *ast.EnumDecl {
	for _, e := range c.enums {
		for _, v := range e.Variants {
			if v.Name == variant {
				return e
			}
		}
	}
	return nil
}

// ---------------------------------------------------------------- moves

// checkMoves is the straight-line use-after-move analysis (§6.1): within one
// block, sequentially, a binding passed to an `own` parameter (or dropped)
// must not be used again. Branches reset conservatively.
func (c *checker) checkMoves() {
	for _, d := range c.file.Decls {
		if fd, ok := d.(*ast.FuncDecl); ok && fd.Body != nil {
			c.movesBlock(fd.Body, map[string]diag.Pos{})
		}
	}
	if len(c.file.ScriptStmts) > 0 {
		b := &ast.Block{Stmts: c.file.ScriptStmts}
		c.movesBlock(b, map[string]diag.Pos{})
	}
}

func (c *checker) movesBlock(b *ast.Block, moved map[string]diag.Pos) {
	for _, s := range b.Stmts {
		c.movesStmt(s, moved)
	}
}

func (c *checker) movesStmt(s ast.Stmt, moved map[string]diag.Pos) {
	check := func(e ast.Expr) { c.checkMovedUse(e, moved) }
	switch st := s.(type) {
	case *ast.ExprStmt:
		check(st.X)
		c.recordMoves(st.X, moved)
	case *ast.LetStmt:
		check(st.Value)
		c.recordMoves(st.Value, moved)
	case *ast.VarStmt:
		if st.Value != nil {
			check(st.Value)
			c.recordMoves(st.Value, moved)
		}
	case *ast.AssignStmt:
		check(st.RHS)
		c.recordMoves(st.RHS, moved)
		// Re-assignment revives the binding.
		if id, ok := st.LHS[0].(*ast.Ident); ok {
			delete(moved, id.Name)
		}
	case *ast.SayStmt:
		for _, a := range st.Args {
			check(a)
		}
	case *ast.ReturnStmt:
		for _, v := range st.Values {
			check(v)
		}
	case *ast.DropStmt:
		moved[st.Name] = st.P
	case *ast.Block:
		c.movesBlock(st, moved)
	case *ast.IfStmt, *ast.ForStmt, *ast.EachStmt, *ast.WhileStmt,
		*ast.MatchStmt, *ast.SwitchStmt, *ast.TryCatchStmt, *ast.GroupStmt,
		*ast.SelectStmt:
		// Branch/loop bodies: analyzed with a fresh view; conservative.
	}
}

// checkMovedUse reports identifiers used after a straight-line move.
func (c *checker) checkMovedUse(e ast.Expr, moved map[string]diag.Pos) {
	walkExpr(e, func(x ast.Expr) {
		if id, ok := x.(*ast.Ident); ok {
			if pos, was := moved[id.Name]; was {
				c.bag.Add(diag.Diagnostic{
					Severity: diag.Error,
					Message:  fmt.Sprintf("use of moved value `%s`", id.Name),
					Span:     diag.SpanOf(id.P, len(id.Name)),
					Label:    fmt.Sprintf("`%s` was moved on line %d", id.Name, pos.Line),
					Help:     []string{fmt.Sprintf("clone it first (`%s.clone()`) if you need two owners", id.Name)},
				})
			}
		}
	})
}

// recordMoves marks bindings moved by `own`-parameter calls in this expr.
func (c *checker) recordMoves(e ast.Expr, moved map[string]diag.Pos) {
	walkExpr(e, func(x ast.Expr) {
		call, ok := x.(*ast.CallExpr)
		if !ok {
			return
		}
		id, ok := call.Fun.(*ast.Ident)
		if !ok {
			return
		}
		fd, ok := c.funcs[id.Name]
		if !ok {
			return
		}
		for i, prm := range fd.Params {
			if prm.Mode != ast.ModeOwn || i >= len(call.Args) {
				continue
			}
			if argID, ok := call.Args[i].Value.(*ast.Ident); ok {
				moved[argID.Name] = argID.P
			}
		}
	})
}

// ---------------------------------------------------------------- printf

// checkFormatStrings validates literal format strings at compile time (§13.1).
func (c *checker) checkFormatStrings() {
	walkStmts(c.file, func(s ast.Stmt) {
		es, ok := s.(*ast.ExprStmt)
		if !ok {
			return
		}
		walkExpr(es.X, func(x ast.Expr) {
			call, ok := x.(*ast.CallExpr)
			if !ok {
				return
			}
			name, argOffset := printfLike(call)
			if name == "" || len(call.Args) <= argOffset {
				return
			}
			lit, ok := call.Args[argOffset].Value.(*ast.BasicLit)
			if !ok || lit.Kind != ast.StrLit {
				return // computed format: checked at runtime (§13.1)
			}
			want, bad := countVerbs(lit.Text)
			if bad != "" {
				c.bag.Add(diag.Diagnostic{
					Severity: diag.Error,
					Message:  fmt.Sprintf("unknown format verb %%%s in %s format string", bad, name),
					Span:     diag.SpanOf(lit.P, len(lit.Text)+2),
					Label:    "bad verb",
				})
				return
			}
			got := len(call.Args) - argOffset - 1
			if want != got {
				c.bag.Add(diag.Diagnostic{
					Severity: diag.Error,
					Message:  fmt.Sprintf("%s format needs %d argument(s), call passes %d", name, want, got),
					Span:     diag.SpanOf(lit.P, len(lit.Text)+2),
					Label:    fmt.Sprintf("%d verb(s) here", want),
				})
			}
		})
	})
}

// printfLike returns the printf-family name and the index of the format arg.
func printfLike(call *ast.CallExpr) (string, int) {
	switch f := call.Fun.(type) {
	case *ast.Ident:
		if f.Name == "printf" || f.Name == "sprintf" {
			return f.Name, 0
		}
	case *ast.FieldExpr:
		if base, ok := f.X.(*ast.Ident); ok && base.Name == "fmt" {
			switch f.Name {
			case "printf", "sprintf", "eprintf":
				return "fmt." + f.Name, 0
			case "fprintf":
				return "fmt.fprintf", 1
			}
		}
	}
	return "", 0
}

// countVerbs counts arguments a format string consumes; returns a bad verb if any.
func countVerbs(f string) (int, string) {
	n := 0
	i := 0
	for i < len(f) {
		if f[i] != '%' {
			i++
			continue
		}
		i++
		if i < len(f) && f[i] == '%' {
			i++
			continue
		}
		for i < len(f) && strings.IndexByte("-+0 #", f[i]) >= 0 {
			i++
		}
		if i < len(f) && f[i] == '*' {
			n++
			i++
		} else {
			for i < len(f) && f[i] >= '0' && f[i] <= '9' {
				i++
			}
		}
		if i < len(f) && f[i] == '.' {
			i++
			if i < len(f) && f[i] == '*' {
				n++
				i++
			} else {
				for i < len(f) && f[i] >= '0' && f[i] <= '9' {
					i++
				}
			}
		}
		if i < len(f) && f[i] == '+' {
			i++
		}
		if i >= len(f) {
			return n, "<end>"
		}
		verb := f[i]
		i++
		if strings.IndexByte("diufFeEgGsqxXobctv", verb) < 0 {
			return n, string(verb)
		}
		n++
	}
	return n, ""
}

// ---------------------------------------------------------------- walkers

func walkStmts(file *ast.File, fn func(ast.Stmt)) {
	var walkBlock func(b *ast.Block)
	var walkStmt func(s ast.Stmt)
	walkBlock = func(b *ast.Block) {
		if b == nil {
			return
		}
		for _, s := range b.Stmts {
			walkStmt(s)
		}
	}
	walkStmt = func(s ast.Stmt) {
		if s == nil {
			return
		}
		fn(s)
		switch st := s.(type) {
		case *ast.Block:
			walkBlock(st)
		case *ast.IfStmt:
			walkBlock(st.Then)
			walkStmt(st.Else)
		case *ast.ForStmt:
			walkBlock(st.Body)
		case *ast.EachStmt:
			walkBlock(st.Body)
		case *ast.WhileStmt:
			walkBlock(st.Body)
		case *ast.SwitchStmt:
			for _, cs := range st.Cases {
				for _, b := range cs.Body {
					walkStmt(b)
				}
			}
		case *ast.MatchStmt:
			for _, arm := range st.Arms {
				for _, b := range arm.Body {
					walkStmt(b)
				}
			}
		case *ast.SelectStmt:
			for _, cs := range st.Cases {
				for _, b := range cs.Body {
					walkStmt(b)
				}
			}
		case *ast.GroupStmt:
			walkBlock(st.Body)
		case *ast.TryCatchStmt:
			walkBlock(st.Body)
			for _, cc := range st.Catches {
				walkBlock(cc.Body)
			}
			walkBlock(st.Finally)
		}
	}
	for _, d := range file.Decls {
		switch decl := d.(type) {
		case *ast.FuncDecl:
			walkBlock(decl.Body)
		case *ast.ImplDecl:
			for _, m := range decl.Methods {
				walkBlock(m.Body)
			}
		case *ast.ExceptionDecl:
			for _, m := range decl.Methods {
				walkBlock(m.Body)
			}
		case *ast.GlobalDecl:
			walkStmt(decl.Stmt)
		}
	}
	for _, s := range file.ScriptStmts {
		walkStmt(s)
	}
}

func walkExpr(e ast.Expr, fn func(ast.Expr)) {
	if e == nil {
		return
	}
	fn(e)
	switch x := e.(type) {
	case *ast.UnaryExpr:
		walkExpr(x.X, fn)
	case *ast.BinaryExpr:
		walkExpr(x.X, fn)
		walkExpr(x.Y, fn)
	case *ast.RangeExpr:
		walkExpr(x.Lo, fn)
		walkExpr(x.Hi, fn)
		walkExpr(x.By, fn)
	case *ast.CallExpr:
		walkExpr(x.Fun, fn)
		for _, a := range x.Args {
			walkExpr(a.Value, fn)
		}
	case *ast.IndexExpr:
		walkExpr(x.X, fn)
		walkExpr(x.Index, fn)
	case *ast.FieldExpr:
		walkExpr(x.X, fn)
	case *ast.CompositeLit:
		for _, f := range x.Fields {
			walkExpr(f.Value, fn)
		}
	case *ast.SliceLit:
		for _, el := range x.Elems {
			walkExpr(el, fn)
		}
	case *ast.TupleExpr:
		for _, el := range x.Elems {
			walkExpr(el, fn)
		}
	case *ast.InterpStr:
		for _, p := range x.Parts {
			walkExpr(p.Expr, fn)
		}
	case *ast.TryExpr:
		walkExpr(x.X, fn)
	case *ast.MustExpr:
		walkExpr(x.X, fn)
	case *ast.AttemptExpr:
		walkExpr(x.X, fn)
	case *ast.ElseExpr:
		walkExpr(x.X, fn)
		walkExpr(x.Alt, fn)
	case *ast.SpawnExpr:
		walkExpr(x.Call, fn)
	}
}
