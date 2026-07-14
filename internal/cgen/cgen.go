// Package cgen compiles the Voilà IR to C. Generated code calls libvoila
// (runtime/), so the produced executable contains no Go whatsoever.
//
// Model: every function's registers live in a heap frame (VlFrame) so that
// closures can capture them by reference, exactly as the interpreter's
// environments do. Helpers borrow their arguments and return owned values;
// frame slots own their contents. One instruction is one C statement.
package cgen

import (
	"fmt"
	"sort"
	"strconv"
	"strings"

	"voila/internal/ast"
	"voila/internal/ir"
)

// fieldDefault is a struct field whose declaration carries a literal default.
type fieldDefault struct {
	name     string
	constIdx int
}

type gen struct {
	mod *ir.Module
	b   strings.Builder

	fnIdx    map[string]int // Voilà function name → C index
	natIdx   map[string]int // builtin/package name → natives table index
	nats     []string       // in index order
	globals  map[string]int // global name → G[] slot
	gnames   []string
	defaults map[string][]fieldDefault
	selCases []ir.Instr
	patSeq   int
	errs     []string
}

// Compile lowers a checked file to a self-contained C translation unit.
func Compile(file *ast.File, mod *ir.Module) (string, error) {
	g := &gen{
		mod:      mod,
		fnIdx:    map[string]int{},
		natIdx:   map[string]int{},
		globals:  map[string]int{},
		defaults: map[string][]fieldDefault{},
	}
	for i, f := range mod.Funcs {
		g.fnIdx[f.Name] = i
	}
	g.collectGlobals()
	g.collectDefaults(file)

	// Patterns and function bodies intern constants, so they are generated
	// into buffers FIRST and the constant pool is emitted once it is final.
	// (Sizing K[] before the last intern is an out-of-bounds read.)
	patterns := g.capture(func() { g.emitPatterns() })
	bodies := g.capture(func() {
		for i, f := range mod.Funcs {
			g.emitFunc(i, f)
		}
	})

	g.emitHeader(file)
	g.emitConsts()
	g.emitTypes(file)
	g.emitProtos()
	g.b.WriteString(patterns)
	g.b.WriteString(bodies)
	g.emitMethodTable(file)
	g.emitMain()

	if len(g.errs) > 0 {
		return "", fmt.Errorf("%s", strings.Join(g.errs, "\n"))
	}
	return g.b.String(), nil
}

// capture runs an emitter into a separate buffer and returns its text, so
// passes that intern constants can run before the pool is written out.
func (g *gen) capture(emit func()) string {
	saved := g.b
	g.b = strings.Builder{}
	emit()
	out := g.b.String()
	g.b = saved
	return out
}

func (g *gen) failf(format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	for _, e := range g.errs {
		if e == msg {
			return
		}
	}
	g.errs = append(g.errs, msg)
}

func (g *gen) p(format string, args ...any) {
	fmt.Fprintf(&g.b, format, args...)
	g.b.WriteByte('\n')
}

// collectGlobals scans SETGLB/GETGLB for the module's global slots.
func (g *gen) collectGlobals() {
	for _, f := range g.mod.Funcs {
		for _, in := range f.Instrs {
			var name string
			switch in.Op {
			case "SETGLB":
				name = in.Args[0]
			case "GETGLB":
				name = in.Args[1]
			default:
				continue
			}
			if _, ok := g.globals[name]; !ok {
				g.globals[name] = len(g.gnames)
				g.gnames = append(g.gnames, name)
			}
		}
	}
}

// collectDefaults records struct/exception field defaults. A default must be
// a literal in native builds; anything else needs an initializer function,
// which the stage-0 backend does not emit.
func (g *gen) collectDefaults(file *ast.File) {
	record := func(name string, fields []*ast.Field) {
		var ds []fieldDefault
		for _, f := range fields {
			if f.Default == nil {
				ds = append(ds, fieldDefault{f.Name, -1})
				continue
			}
			lit, ok := f.Default.(*ast.BasicLit)
			if !ok {
				g.failf("cgen: field default for `%s.%s` must be a literal in native builds", name, f.Name)
				ds = append(ds, fieldDefault{f.Name, -1})
				continue
			}
			kind, norm := litConst(lit)
			ds = append(ds, fieldDefault{f.Name, g.internConst(kind, norm)})
		}
		g.defaults[name] = ds
	}
	for _, d := range file.Decls {
		switch decl := d.(type) {
		case *ast.StructDecl:
			record(decl.Name, decl.Fields)
		case *ast.TypeDecl:
			if decl.Struct != nil {
				record(decl.Name, decl.Struct.Fields)
			}
		case *ast.ExceptionDecl:
			record(decl.Name, decl.Fields)
		}
	}
}

func litConst(lit *ast.BasicLit) (ir.ConstKind, string) {
	switch lit.Kind {
	case ast.IntLit:
		n, _ := strconv.ParseInt(lit.Text, 0, 64)
		return ir.CInt, strconv.FormatInt(n, 10)
	case ast.FloatLit:
		f, _ := strconv.ParseFloat(lit.Text, 64)
		return ir.CFloat, strconv.FormatFloat(f, 'g', -1, 64)
	case ast.DecLit:
		return ir.CDec, lit.Text
	case ast.StrLit:
		return ir.CStr, lit.Text
	case ast.RuneLit:
		return ir.CRune, lit.Text
	case ast.BoolLit:
		return ir.CBool, lit.Text
	}
	return ir.CNil, "nil"
}

func (g *gen) internConst(kind ir.ConstKind, text string) int {
	for i, c := range g.mod.Consts {
		if c.Kind == kind && c.Text == text {
			return i
		}
	}
	g.mod.Consts = append(g.mod.Consts, ir.Const{Kind: kind, Text: text})
	return len(g.mod.Consts) - 1
}

func (g *gen) native(name string) int {
	if i, ok := g.natIdx[name]; ok {
		return i
	}
	if !knownNatives[name] {
		g.failf("cgen: `%s` is not available in native builds (no C implementation yet)", name)
	}
	i := len(g.nats)
	g.natIdx[name] = i
	g.nats = append(g.nats, name)
	return i
}

// ---------------------------------------------------------------- preamble

func (g *gen) emitHeader(file *ast.File) {
	g.p("/* Generated by voila build. Do not edit.")
	g.p(" * source: %s", g.mod.Source)
	g.p(" */")
	g.p("#include \"voila.h\"")
	g.p("#include <stdlib.h>")
	g.p("")
	_ = file
}

func cQuote(s string) string {
	var b strings.Builder
	b.WriteByte('"')
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch c {
		case '"':
			b.WriteString("\\\"")
		case '\\':
			b.WriteString("\\\\")
		case '\n':
			b.WriteString("\\n")
		case '\t':
			b.WriteString("\\t")
		case '\r':
			b.WriteString("\\r")
		default:
			if c < 0x20 {
				fmt.Fprintf(&b, "\\%03o", c)
			} else {
				b.WriteByte(c)
			}
		}
	}
	b.WriteByte('"')
	return b.String()
}

func (g *gen) emitConsts() {
	g.p("static Value K[%d];", max(1, len(g.mod.Consts)))
	g.p("static void init_consts(void) {")
	for i, c := range g.mod.Consts {
		switch c.Kind {
		case ir.CInt:
			g.p("  K[%d] = vl_int(%sLL);", i, c.Text)
		case ir.CFloat:
			g.p("  K[%d] = vl_float(%s);", i, cFloat(c.Text))
		case ir.CDec:
			g.p("  K[%d] = vl_dec_parse(%s);", i, cQuote(c.Text))
		case ir.CStr:
			g.p("  K[%d] = vl_str_n(%s, %d);", i, cQuote(c.Text), len(c.Text))
		case ir.CRune:
			g.p("  K[%d] = vl_rune(%d);", i, []rune(c.Text)[0])
		case ir.CBool:
			g.p("  K[%d] = vl_bool(%s);", i, c.Text)
		case ir.CNil:
			g.p("  K[%d] = vl_nil();", i)
		case ir.CUnit:
			g.p("  K[%d] = vl_unit();", i)
		}
	}
	g.p("}")
	g.p("")
	g.p("static Value G[%d];", max(1, len(g.gnames)))
	g.p("")
}

func cFloat(s string) string {
	if _, err := strconv.ParseFloat(s, 64); err != nil {
		return "0"
	}
	if !strings.ContainsAny(s, ".eE") {
		return s + ".0"
	}
	return s
}

// emitTypes writes the struct/enum/exception descriptors the runtime needs.
func (g *gen) emitTypes(file *ast.File) {
	type tdecl struct {
		name   string
		fields []string
		ftypes []string
	}
	var types []tdecl
	type vdecl struct {
		typ, variant string
		fields       []string
	}
	var variants []vdecl

	addStruct := func(name string, fields []*ast.Field) {
		t := tdecl{name: name}
		for _, f := range fields {
			t.fields = append(t.fields, f.Name)
			t.ftypes = append(t.ftypes, typeText(f.Type))
		}
		types = append(types, t)
	}
	for _, d := range file.Decls {
		switch decl := d.(type) {
		case *ast.StructDecl:
			addStruct(decl.Name, decl.Fields)
		case *ast.TypeDecl:
			if decl.Struct != nil {
				addStruct(decl.Name, decl.Struct.Fields)
			}
		case *ast.ExceptionDecl:
			addStruct(decl.Name, decl.Fields)
		case *ast.EnumDecl:
			for _, v := range decl.Variants {
				vd := vdecl{typ: decl.Name, variant: v.Name}
				for _, f := range v.Fields {
					vd.fields = append(vd.fields, f.Name)
				}
				variants = append(variants, vd)
			}
		}
	}

	for i, t := range types {
		if len(t.fields) == 0 {
			continue
		}
		var qs, ts []string
		for k, f := range t.fields {
			qs = append(qs, cQuote(f))
			ts = append(ts, cQuote(t.ftypes[k]))
		}
		g.p("static const char *const tf%d[] = {%s};", i, strings.Join(qs, ", "))
		g.p("static const char *const tt%d[] = {%s};", i, strings.Join(ts, ", "))
	}
	g.p("static const VlType g_types[] = {")
	for i, t := range types {
		fp, tp := "NULL", "NULL"
		if len(t.fields) > 0 {
			fp = fmt.Sprintf("tf%d", i)
			tp = fmt.Sprintf("tt%d", i)
		}
		g.p("  {%s, %d, %s, %s},", cQuote(t.name), len(t.fields), fp, tp)
	}
	g.p("  {NULL, 0, NULL, NULL}")
	g.p("};")

	for i, v := range variants {
		if len(v.fields) == 0 {
			continue
		}
		var qs []string
		for _, f := range v.fields {
			qs = append(qs, cQuote(f))
		}
		g.p("static const char *const vf%d[] = {%s};", i, strings.Join(qs, ", "))
	}
	g.p("static const VlVariant g_variants[] = {")
	for i, v := range variants {
		fp := "NULL"
		if len(v.fields) > 0 {
			fp = fmt.Sprintf("vf%d", i)
		}
		g.p("  {%s, %s, %d, %s},", cQuote(v.typ), cQuote(v.variant), len(v.fields), fp)
	}
	g.p("  {NULL, NULL, 0, NULL}")
	g.p("};")
	g.p("static const int g_ntypes = %d;", len(types))
	g.p("static const int g_nvariants = %d;", len(variants))
	g.p("")
}

// ---------------------------------------------------------------- patterns

// emitPatterns writes a static VlPat tree for every PMATCH in the module.
func (g *gen) emitPatterns() {
	for _, f := range g.mod.Funcs {
		for i := range f.Instrs {
			in := &f.Instrs[i]
			if in.Op == "PMATCH" && in.Pat != nil {
				g.emitPattern(in.Pat)
			}
		}
	}
}

// patName assigns each pattern node a stable C symbol.
func (g *gen) emitPattern(p *ir.Pattern) string {
	for _, e := range p.Elems {
		g.emitPattern(e)
	}
	g.patSeq++
	name := fmt.Sprintf("p%d", g.patSeq)
	p.CName = name

	elems := "NULL"
	if len(p.Elems) > 0 {
		var refs []string
		for _, e := range p.Elems {
			refs = append(refs, e.CName)
		}
		g.p("static const VlPat %s_e[] = {%s};", name, strings.Join(refs, ", "))
		elems = name + "_e"
	}

	kind := map[ir.PatKind]string{
		ir.PatWildcard: "VL_P_WILD",
		ir.PatNil:      "VL_P_NIL",
		ir.PatLiteral:  "VL_P_LIT",
		ir.PatBind:     "VL_P_BIND",
		ir.PatVariant:  "VL_P_VARIANT",
	}[p.Kind]

	slot := 0
	if p.Kind == ir.PatBind {
		slot = regSlot(p.Reg)
	}
	lit := 0
	if p.Kind == ir.PatLiteral {
		lit = g.internConst(p.LitKind, p.LitText)
	}
	g.p("static const VlPat %s = {%s, %s, %d, %d, %d, %s};", name, kind,
		cQuote(p.Name), slot, lit, len(p.Elems), elems)
	return name
}

// ---------------------------------------------------------------- functions

func cFuncName(i int) string { return fmt.Sprintf("vf_%d", i) }

func (g *gen) emitProtos() {
	for i, f := range g.mod.Funcs {
		g.p("static Value %s(Value *argv, int argc, VlFrame *up); /* %s */", cFuncName(i), f.Name)
	}
	g.p("")
	g.p("static VlFn NAT[%d]; /* sized exactly; the bodies are generated first */",
		max(1, len(g.nats)))
	g.p("")
}

func regSlot(r string) int {
	n, err := strconv.Atoi(strings.TrimPrefix(r, "r"))
	if err != nil {
		return -1
	}
	return n
}

// operand renders an IR operand as a borrowed C expression.
func (g *gen) operand(f *ir.Func, s string) string {
	switch {
	case strings.HasPrefix(s, "r") && regSlot(s) >= 0:
		return fmt.Sprintf("F->r[%d]", regSlot(s))
	case strings.HasPrefix(s, "K"):
		n, err := strconv.Atoi(s[1:])
		if err == nil {
			return fmt.Sprintf("K[%d]", n)
		}
	case strings.HasPrefix(s, "^"):
		name := s[1:]
		up, ok := f.Ups[name]
		if !ok {
			g.failf("cgen: unresolved upvalue `%s` in %s", name, f.Name)
			return "vl_nil()"
		}
		return fmt.Sprintf("vl_up(F->up, %d, %d)", up.Depth-1, up.Slot)
	}
	g.failf("cgen: bad operand %q in %s", s, f.Name)
	return "vl_nil()"
}

// argList parses a rendered "(r1,name:r2,r3...)" operand into C.
type callArg struct {
	reg    string
	name   string
	spread bool
}

func parseArgs(s string) []callArg {
	s = strings.TrimPrefix(strings.TrimSuffix(s, ")"), "(")
	if s == "" {
		return nil
	}
	var out []callArg
	for _, part := range strings.Split(s, ",") {
		a := callArg{}
		if strings.HasSuffix(part, "...") {
			a.spread = true
			part = strings.TrimSuffix(part, "...")
		}
		if i := strings.Index(part, ":"); i >= 0 {
			a.name = part[:i]
			part = part[i+1:]
		}
		a.reg = part
		out = append(out, a)
	}
	return out
}

// emitArgv writes an argv array for a call, returning its C name and length.
func (g *gen) emitArgv(f *ir.Func, id int, spec string) (string, string) {
	args := parseArgs(spec)
	name := fmt.Sprintf("a%d", id)
	if len(args) == 0 {
		return "NULL", "0"
	}
	hasSpread := false
	for _, a := range args {
		if a.spread {
			hasSpread = true
		}
	}
	if !hasSpread {
		var vals []string
		for _, a := range args {
			vals = append(vals, g.operand(f, a.reg))
		}
		g.p("  Value %s[] = {%s};", name, strings.Join(vals, ", "))
		return name, strconv.Itoa(len(vals))
	}
	// Spread: build the argv dynamically.
	g.p("  Value %s_s = vl_slice_new(0);", name)
	for _, a := range args {
		if a.spread {
			g.p("  vl_spread(%s_s, %s);", name, g.operand(f, a.reg))
		} else {
			g.p("  vl_slice_append(%s_s, vl_retain(%s));", name, g.operand(f, a.reg))
		}
	}
	g.p("  Value *%s = vl_slice_data(%s_s);", name, name)
	return name, fmt.Sprintf("(int)vl_len(%s_s)", name)
}

// releaseArgv frees the temporary slice a spread call built (it is an owned
// value; leaving it unreleased leaked one slice per call).
func (g *gen) releaseArgv(id int, spec string) {
	for _, a := range parseArgs(spec) {
		if a.spread {
			g.p("  vl_release(a%d_s);", id)
			return
		}
	}
}

func (g *gen) emitFunc(idx int, f *ir.Func) {
	if f.LoopCapture != "" {
		g.failf("cgen: %s captures `%s`, which is declared inside a loop.\n"+
			"  Each iteration is a separate binding, and native builds cannot yet\n"+
			"  give each one its own frame — the interpreter and the binary would\n"+
			"  disagree, so the build is refused rather than silently differ.\n"+
			"  Workaround: build the closure in a helper function that takes the\n"+
			"  value as a parameter.", f.Name, f.LoopCapture)
	}
	g.p("/* %s — %s */", f.Name, f.Sig)
	g.p("static Value %s(Value *argv, int argc, VlFrame *up) {", cFuncName(idx))
	nregs := f.NRegs
	if nregs < 1 {
		nregs = 1
	}
	g.p("  VlFrame *F = vl_frame_new(%d, up);", nregs)
	g.p("  vl_frame_push(F);")
	g.p("  Value __ret = vl_unit();")
	g.p("  (void)argc;")
	// Parameters land in the lowest registers.
	g.p("  for (int i = 0; i < argc && i < %d; i++) vl_set(&F->r[i], vl_retain(argv[i]));", nregs)

	seq := 0
	for i := range f.Instrs {
		in := &f.Instrs[i]
		for _, l := range in.Labels {
			g.p(" %s: ;", l)
		}
		seq++
		g.emitInstr(f, in, seq)
	}

	g.p(" __exit: ;")
	g.p("  vl_frame_pop_run_defers(F);")
	g.p("  vl_frame_clear(F); /* break frame<->closure cycles before releasing */")
	g.p("  vl_frame_release(F);")
	g.p("  return __ret;")
	g.p("}")
	g.p("")
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

// ---------------------------------------------------------------- table

func (g *gen) emitMethodTable(file *ast.File) {
	type m struct {
		typ, name string
		fn        int
		hasSelf   bool
	}
	var ms []m
	for _, d := range file.Decls {
		switch decl := d.(type) {
		case *ast.ImplDecl:
			tn := ""
			if nt, ok := decl.Type.(*ast.NamedType); ok {
				tn = nt.Name
			}
			for _, meth := range decl.Methods {
				full := tn + "." + meth.Name
				if i, ok := g.fnIdx[full]; ok {
					ms = append(ms, m{tn, meth.Name, i, meth.HasSelf})
				}
			}
		case *ast.ExceptionDecl:
			for _, meth := range decl.Methods {
				full := decl.Name + "." + meth.Name
				if i, ok := g.fnIdx[full]; ok {
					ms = append(ms, m{decl.Name, meth.Name, i, meth.HasSelf})
				}
			}
		}
	}
	sort.SliceStable(ms, func(i, j int) bool {
		if ms[i].typ != ms[j].typ {
			return ms[i].typ < ms[j].typ
		}
		return ms[i].name < ms[j].name
	})
	g.p("static const VlMethod g_methods[] = {")
	for _, x := range ms {
		g.p("  {%s, %s, %s, %s},", cQuote(x.typ), cQuote(x.name), cFuncName(x.fn),
			boolC(x.hasSelf))
	}
	g.p("  {NULL, NULL, NULL, false}")
	g.p("};")
	g.p("static const int g_nmethods = %d;", len(ms))
	g.p("")
}

func boolC(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

func (g *gen) emitMain() {
	g.p("int main(int argc, char **argv) {")
	g.p("  vl_init(argc, argv);")
	g.p("  vl_register_types(g_types, g_ntypes, g_variants, g_nvariants, g_methods, g_nmethods);")
	g.p("  init_consts();")
	for i, name := range g.nats {
		g.p("  NAT[%d] = vl_lookup_fn(%s);", i, cQuote(name))
	}
	g.p("  for (int i = 0; i < %d; i++) if (!NAT[i]) { fprintf(stderr, \"voila: missing runtime function\\n\"); return 1; }",
		len(g.nats))
	if i, ok := g.fnIdx["$INIT"]; ok {
		g.p("  vl_release(%s(NULL, 0, NULL));", cFuncName(i))
	}
	if i, ok := g.fnIdx["$SCRIPT"]; ok {
		g.p("  vl_release(%s(NULL, 0, NULL));", cFuncName(i))
	}
	if i, ok := g.fnIdx["main"]; ok {
		g.p("  Value r = %s(NULL, 0, NULL);", cFuncName(i))
		g.p("  if (vl_isfail(r) && r.t != VL_NIL) {")
		g.p("    Value m = vl_callm(r, \"message\", NULL, 0);")
		g.p("    fprintf(stderr, \"error: %%s\\n\", vl_cstr(m));")
		g.p("    return 1;")
		g.p("  }")
		g.p("  vl_release(r);")
	}
	g.p("  int code = vl_finish();")
	g.p("  for (int i = 0; i < %d; i++) vl_release(G[i]);", max(1, len(g.gnames)))
	g.p("  for (int i = 0; i < %d; i++) vl_release(K[i]);", max(1, len(g.mod.Consts)))
	g.p("  return code;")
	g.p("}")
}

// typeText renders a declared type for the runtime's zero-value constructor.
func typeText(te ast.TypeExpr) string {
	switch ty := te.(type) {
	case nil:
		return ""
	case *ast.NamedType:
		s := ty.Name
		if ty.Pkg != "" {
			s = ty.Pkg + "." + s
		}
		return s
	case *ast.OptionType:
		return "?" + typeText(ty.Elem)
	case *ast.ResultType:
		return typeText(ty.Elem) + "!"
	case *ast.SliceType:
		return "[]" + typeText(ty.Elem)
	case *ast.MapType:
		return "map[" + typeText(ty.Key) + "]" + typeText(ty.Value)
	case *ast.SetType:
		return "set[" + typeText(ty.Elem) + "]"
	case *ast.ChanType:
		return "chan[" + typeText(ty.Elem) + "]"
	case *ast.BoxType:
		kind := map[ast.BoxKind]string{
			ast.SharedBox: "shared", ast.CellBox: "cell", ast.WeakBox: "weak",
		}[ty.Kind]
		return kind + "[" + typeText(ty.Elem) + "]"
	case *ast.TupleType:
		return "tuple"
	case *ast.FuncType:
		return "fn"
	}
	return ""
}
