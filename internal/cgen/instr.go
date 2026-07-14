package cgen

import (
	"fmt"
	"strconv"
	"strings"

	"voila/internal/ir"
)

// knownNatives is the set of prelude and package names the C runtime
// provides. A program using anything else fails at build time with a clear
// message rather than at run time.
var knownNatives = map[string]bool{}

func init() {
	prelude := []string{
		"len", "append", "close", "err", "errf", "assert", "min", "max", "abs",
		"sum", "avg", "round", "floor", "ceil", "trunc", "round_to", "range",
		"zip", "enumerate", "keys", "values", "shared", "cell", "weak", "set",
		"printf", "sprintf",
		"to_int", "to_i8", "to_i16", "to_i32", "to_i64", "to_u8", "to_u16",
		"to_u32", "to_u64", "to_byte", "to_float", "to_dec", "to_rune",
		"wrap_i8", "wrap_i16", "wrap_i32", "wrap_i64", "wrap_u8", "wrap_u16",
		"wrap_u32", "wrap_u64", "sat_i8", "sat_i16", "sat_i32", "sat_i64",
		"sat_u8", "sat_u16", "sat_u32", "sat_u64", "wrap_add", "wrap_mul",
	}
	strFns := []string{
		"upper", "lower", "title", "trim", "trim_left", "trim_right", "strip",
		"pad", "pad_left", "center", "contains", "index", "last_index",
		"starts_with", "ends_with", "count", "equal_fold", "substr", "left",
		"right", "split", "split_n", "lines", "fields", "word", "words",
		"subword", "delword", "join", "replace", "replace_n", "repeat",
		"reverse", "translate", "overlay", "insert", "delstr", "bytes",
		"from_bytes", "runes", "from_runes", "rune_len", "is_digit",
		"is_alpha", "is_space", "is_upper", "hex", "unhex", "b64", "unb64",
	}
	pkg := []string{
		"fmt.printf", "fmt.sprintf", "fmt.eprintf", "fmt.fprintf", "fmt.print",
		"fmt.println", "fmt.comma", "fmt.bytes", "fmt.pad", "fmt.pad_left",
		"fmt.center",
		"os.read", "os.write", "os.open", "os.create", "os.exists", "os.remove",
		"os.args", "os.env", "os.exit", "os.on_abort", "os.stderr", "os.stdout",
		"os.isdir", "os.listdir", "os.run", "os.cwd", "os.pid",
		"math.sqrt", "math.sin", "math.cos", "math.tan", "math.asin",
		"math.acos", "math.atan", "math.ln", "math.log2", "math.log10",
		"math.exp", "math.pow", "math.hypot", "math.mod", "math.pi", "math.e",
		"time.now", "time.since", "time.sleep", "time.after", "time.millis",
		"time.seconds", "time.Nanosecond", "time.Microsecond",
		"time.Millisecond", "time.Second", "time.Minute", "time.Hour",
		"log.info", "log.warn", "log.error", "log.debug", "log.flush",
		"sort.sort", "sort.sort_by", "sort.reversed",
		"json.encode", "json.pretty", "json.decode",
		"rand.int", "rand.float", "rand.seed", "uuid.new",
		"conv.to_int", "conv.to_float", "conv.to_dec",
	}
	for _, n := range prelude {
		knownNatives[n] = true
	}
	for _, n := range strFns {
		knownNatives[n] = true
		knownNatives["str."+n] = true
	}
	for _, n := range pkg {
		knownNatives[n] = true
	}
}

// isConstOperand reports whether a name is a package constant rather than a
// function (LOADFN of these yields a value, not a callable).
var pkgConstants = map[string]bool{
	"time.Nanosecond": true, "time.Microsecond": true, "time.Millisecond": true,
	"time.Second": true, "time.Minute": true, "time.Hour": true,
	"math.pi": true, "math.e": true, "os.stderr": true, "os.stdout": true,
}

var binOpFn = map[string]string{
	"ADD": "vl_add", "SUB": "vl_sub", "MUL": "vl_mul", "DIV": "vl_div",
	"IDIV": "vl_idiv", "MOD": "vl_mod", "POW": "vl_pow", "CAT": "vl_cat",
	"SHL": "vl_shl", "SHR": "vl_shr", "BAND": "vl_band", "BOR": "vl_bor",
	"BXOR": "vl_bxor", "CMPEQ": "vl_cmpeq", "CMPNE": "vl_cmpne",
	"CMPLT": "vl_cmplt", "CMPLE": "vl_cmple", "CMPGT": "vl_cmpgt",
	"CMPGE": "vl_cmpge", "CMPIN": "vl_cmpin",
}

var unOpFn = map[string]string{
	"NEG": "vl_neg", "NOT": "vl_not", "BNOT": "vl_bnot",
}

// emitInstr writes one instruction as C.
func (g *gen) emitInstr(f *ir.Func, in *ir.Instr, seq int) {
	o := func(s string) string { return g.operand(f, s) }
	dst := func(s string) string {
		n := regSlot(s)
		if n < 0 {
			g.failf("cgen: bad destination %q in %s", s, f.Name)
			return "F->r[0]"
		}
		return fmt.Sprintf("F->r[%d]", n)
	}
	a := in.Args

	if fn, ok := binOpFn[in.Op]; ok {
		g.p("  vl_set(&%s, %s(%s, %s));", dst(a[0]), fn, o(a[1]), o(a[2]))
		return
	}
	if fn, ok := unOpFn[in.Op]; ok {
		g.p("  vl_set(&%s, %s(%s));", dst(a[0]), fn, o(a[1]))
		return
	}

	switch in.Op {
	case "*": // listing-only comment
		return

	case "LOADK", "MOVE":
		g.p("  vl_set(&%s, vl_retain(%s));", dst(a[0]), o(a[1]))

	case "ZERO":
		g.p("  vl_set(&%s, vl_zero(%s));", dst(a[0]), cQuote(a[1]))

	case "GETGLB":
		g.p("  vl_set(&%s, vl_retain(G[%d]));", dst(a[0]), g.globals[a[1]])
	case "SETGLB":
		g.p("  vl_set(&G[%d], vl_retain(%s));", g.globals[a[0]], o(a[1]))

	case "UPSET":
		up, ok := f.Ups[a[0]]
		if !ok {
			g.failf("cgen: unresolved upvalue `%s` in %s", a[0], f.Name)
			return
		}
		g.p("  vl_upset(F->up, %d, %d, vl_retain(%s));", up.Depth-1, up.Slot, o(a[1]))

	case "LOADFN":
		name := a[1]
		if idx, ok := g.fnIdx[name]; ok {
			g.p("  vl_set(&%s, vl_closure(%s, NULL));", dst(a[0]), cFuncName(idx))
			return
		}
		if strings.Contains(name, ".") && !pkgConstants[name] {
			// A variant constructor used as a value, or a package function.
			if parts := strings.SplitN(name, ".", 2); g.isVariant(parts[0], parts[1]) {
				g.failf("cgen: enum constructors as first-class values are not supported in native builds yet (%s)", name)
				return
			}
		}
		if !pkgConstants[name] {
			g.native(name) // a function reference: resolve it in NAT too
		} else if !knownNatives[name] {
			g.failf("cgen: `%s` is not available in native builds", name)
		}
		g.p("  vl_set(&%s, vl_lookup_value(%s));", dst(a[0]), cQuote(name))

	case "GETCTX":
		g.failf("cgen: `ctx` is not available in native builds yet")

	case "JUMP":
		g.p("  goto %s;", a[0])
	case "JMPF":
		g.p("  if (!vl_truthy(%s)) goto %s;", o(a[0]), a[1])
	case "JMPT":
		g.p("  if (vl_truthy(%s)) goto %s;", o(a[0]), a[1])

	case "RET":
		if len(a) == 0 {
			g.p("  __ret = vl_unit(); goto __exit;")
		} else {
			g.p("  __ret = vl_retain(%s); goto __exit;", o(a[0]))
		}

	case "ABORT":
		g.p("  vl_abort(\"%%s\", %s);", cQuote(strings.Trim(a[0], "'")))

	case "CKCANC":
		g.p("  vl_check_cancelled();")

	case "CALL":
		name := a[1]
		argv, n := g.emitArgv(f, seq, a[2])
		if idx, ok := g.fnIdx[name]; ok {
			g.p("  vl_set(&%s, %s(%s, %s, NULL));", dst(a[0]), cFuncName(idx), argv, n)
			g.releaseArgv(seq, a[2])
			return
		}
		if v := g.variantCall(name); v != nil {
			g.p("  vl_set(&%s, vl_enum_new(%s, %s, %s, %s));", dst(a[0]),
				cQuote(v[0]), cQuote(v[1]), argv, n)
			g.releaseArgv(seq, a[2])
			return
		}
		if strings.HasSuffix(name, "]") {
			base := name[:strings.Index(name, "[")]
			typ := strings.TrimSuffix(name[strings.Index(name, "[")+1:], "]")
			if base != "json.decode" {
				// Generics are type-erased everywhere else (§3.7): cell[int](0)
				// is simply cell(0).
				if idx, ok := g.fnIdx[base]; ok {
					g.p("  vl_set(&%s, %s(%s, %s, NULL));", dst(a[0]), cFuncName(idx), argv, n)
					return
				}
				bidx := g.native(base)
				g.p("  vl_set(&%s, NAT[%d](%s, %s, NULL));", dst(a[0]), bidx, argv, n)
				return
			}
			idx := g.native(base)
			g.p("  Value ty%d = vl_str(%s);", seq, cQuote(typ))
			g.p("  Value ja%d[8]; ja%d[0] = ty%d;", seq, seq, seq)
			g.p("  for (int i = 0; i < %s && i < 7; i++) ja%d[i+1] = %s[i];", n, seq, argv)
			g.p("  vl_set(&%s, NAT[%d](ja%d, %s + 1, NULL));", dst(a[0]), idx, seq, n)
			g.p("  vl_release(ty%d);", seq)
			return
		}
		idx := g.native(name)
		g.p("  vl_set(&%s, NAT[%d](%s, %s, NULL));", dst(a[0]), idx, argv, n)
		g.releaseArgv(seq, a[2])

	case "CALLR":
		argv, n := g.emitArgv(f, seq, a[2])
		g.p("  vl_set(&%s, vl_call(%s, %s, %s));", dst(a[0]), o(a[1]), argv, n)
		g.releaseArgv(seq, a[2])

	case "CALLM":
		argv, n := g.emitArgv(f, seq, a[3])
		g.p("  vl_set(&%s, vl_callm(%s, %s, %s, %s));", dst(a[0]), o(a[1]),
			cQuote(a[2]), argv, n)
		g.releaseArgv(seq, a[3])

	case "CONV":
		if len(a) > 3 {
			g.p("  Value cb%d = %s;", seq, o(a[3]))
			g.p("  vl_set(&%s, vl_conv(%s, %s, &cb%d));", dst(a[0]), cQuote(a[1]), o(a[2]), seq)
		} else {
			g.p("  vl_set(&%s, vl_conv(%s, %s, NULL));", dst(a[0]), cQuote(a[1]), o(a[2]))
		}

	case "NEWCHAN":
		if len(a) > 1 {
			g.p("  vl_set(&%s, vl_chan_new(vl_int_of(%s)));", dst(a[0]), o(a[1]))
		} else {
			g.p("  vl_set(&%s, vl_chan_new(0));", dst(a[0]))
		}

	case "NEWSLICE":
		spec := a[1]
		if strings.HasPrefix(spec, "[") { // make([]T, n) — sized, zero-filled
			reg := strings.Trim(spec, "[]")
			g.p("  vl_set(&%s, vl_slice_new(vl_int_of(%s)));", dst(a[0]), o(reg))
			return
		}
		g.p("  vl_set(&%s, vl_slice_new(0));", dst(a[0]))
		for _, arg := range parseArgs(spec) {
			g.p("  vl_slice_append(%s, vl_retain(%s));", dst(a[0]), o(arg.reg))
		}

	case "NEWMAP":
		g.p("  vl_set(&%s, vl_map_new());", dst(a[0]))
	case "NEWSET":
		g.p("  vl_set(&%s, vl_set_new());", dst(a[0]))

	case "NEWRANGE":
		by := "1"
		if a[3] != "1" {
			by = "vl_int_of(" + o(a[3]) + ")"
		}
		g.p("  vl_set(&%s, vl_range_new(vl_int_of(%s), vl_int_of(%s), %s, %v));",
			dst(a[0]), o(a[1]), o(a[2]), by, a[4] == "incl")

	case "NEWSTRUCT", "NEWEXC":
		g.emitComposite(f, in, seq, in.Op == "NEWEXC")

	case "NEWENUM":
		typ, variant := splitVariant(a[1])
		argv, n := g.emitArgv(f, seq, a[2])
		g.p("  vl_set(&%s, vl_enum_new(%s, %s, %s, %s));", dst(a[0]),
			cQuote(typ), cQuote(variant), argv, n)
		g.releaseArgv(seq, a[2])

	case "TUPLE":
		argv, n := g.emitArgv(f, seq, a[1])
		g.p("  vl_set(&%s, vl_tuple_new(%s, %s));", dst(a[0]), argv, n)
		g.releaseArgv(seq, a[1])

	case "UNTUPLE":
		regs := parseArgs(a[0])
		for i, r := range regs {
			if r.reg == "_" {
				continue
			}
			g.p("  vl_set(&%s, vl_index(%s, vl_int(%d)));", dst(r.reg), o(a[1]), i)
		}

	case "INDEX":
		g.p("  vl_set(&%s, vl_index(%s, %s));", dst(a[0]), o(a[1]), o(a[2]))
	case "SETIDX":
		g.p("  vl_setidx(%s, %s, vl_retain(%s));", o(a[0]), o(a[1]), o(a[2]))
	case "SLICE":
		g.p("  vl_set(&%s, vl_slice_range(%s, %s));", dst(a[0]), o(a[1]), o(a[2]))
	case "FIELD":
		g.p("  vl_set(&%s, vl_field(%s, %s));", dst(a[0]), o(a[1]), cQuote(a[2]))
	case "SETFLD":
		g.p("  vl_setfld(%s, %s, vl_retain(%s));", o(a[0]), cQuote(a[1]), o(a[2]))

	case "INTERP":
		argv, n := g.emitArgv(f, seq, a[1])
		g.p("  vl_set(&%s, vl_interp(%s, %s));", dst(a[0]), argv, n)
		g.releaseArgv(seq, a[1])

	case "SAY":
		argv, n := g.emitArgv(f, seq, a[0])
		g.p("  vl_say(%s, %s);", argv, n)
		g.releaseArgv(seq, a[0])

	case "TOSTR":
		g.p("  vl_set(&%s, vl_tostr(%s));", dst(a[0]), o(a[1]))

	case "ISFAIL":
		g.p("  vl_set(&%s, vl_bool(vl_isfail(%s)));", dst(a[0]), o(a[1]))

	case "TRYP":
		// `try expr`: a failure becomes this function's result (§8.1).
		g.p("  if (vl_isfail(%s)) { __ret = vl_retain(%s); goto __exit; }", o(a[1]), o(a[1]))
		g.p("  vl_set(&%s, vl_retain(%s));", dst(a[0]), o(a[1]))

	case "MUST":
		g.p("  vl_set(&%s, vl_must(%s));", dst(a[0]), o(a[1]))

	case "THROW":
		if len(a) > 1 {
			g.p("  vl_throw_from(%s, %s);", o(a[0]), o(a[1]))
		} else {
			g.p("  vl_throw(%s);", o(a[0]))
		}
	case "RETHROW":
		g.p("  vl_rethrow(%s);", o(a[0]))

	case "EHPUSH":
		g.p("  VlHandler h%d;", seq)
		g.p("  if (VL_TRY(h%d)) goto %s;", seq, a[0])
	case "EHPOP":
		g.p("  vl_eh_pop();")
	case "CATCH":
		g.p("  vl_set(&%s, vl_eh_current());", dst(a[0]))
	case "ISTYPE":
		g.p("  vl_set(&%s, vl_bool(vl_istype(%s, %s)));", dst(a[0]), o(a[1]),
			cQuote(strings.Trim(a[2], "'")))

	case "NUMDIG":
		g.p("  vl_numeric_digits(%s);", a[0])
	case "NDSAVE":
		g.p("  vl_digits_save();")
	case "NDREST":
		g.p("  vl_digits_restore();")

	case "DROP":
		// The static checker already rejects use-after-move; nothing to do.

	case "DEFER":
		g.p("  vl_defer(F, %s);", o(a[0]))

	case "MKCLOS":
		idx, ok := g.fnIdx[a[1]]
		if !ok {
			g.failf("cgen: unknown closure %s", a[1])
			return
		}
		g.p("  vl_set(&%s, vl_closure(%s, F));", dst(a[0]), cFuncName(idx))

	case "GRPBEG":
		if len(a) > 0 {
			g.p("  vl_group_begin(%s);", o(a[0]))
		} else {
			g.p("  vl_group_begin(vl_nil());")
		}
	case "GRPEND":
		g.p("  vl_release(vl_group_end(false));")
	case "GRPENDT":
		g.p("  { Value ge%d = vl_group_end(true);", seq)
		g.p("    if (ge%d.t != VL_NIL) { __ret = ge%d; goto __exit; }", seq, seq)
		g.p("    vl_release(ge%d); }", seq)

	case "SPAWN":
		name := a[1]
		argv, n := g.emitArgv(f, seq, a[2])
		if idx, ok := g.fnIdx[name]; ok {
			g.p("  vl_set(&%s, vl_spawn_fn(%s, %s, %s));", dst(a[0]), cFuncName(idx), argv, n)
			g.releaseArgv(seq, a[2])
			return
		}
		idx := g.native(name)
		g.p("  vl_set(&%s, vl_spawn_fn(NAT[%d], %s, %s));", dst(a[0]), idx, argv, n)
		g.releaseArgv(seq, a[2])

	case "SPAWNR":
		g.p("  vl_set(&%s, vl_spawn_closure(%s));", dst(a[0]), o(a[1]))

	case "SEND":
		g.p("  vl_chan_send(%s, vl_retain(%s));", o(a[0]), o(a[1]))
	case "RECV":
		g.p("  vl_set(&%s, vl_chan_recv(%s));", dst(a[0]), o(a[1]))
	case "RECVOK":
		g.p("  { Value ok%d; vl_set(&%s, vl_chan_recv_ok(%s, &ok%d));", seq,
			dst(a[0]), o(a[2]), seq)
		g.p("    vl_set(&%s, ok%d); }", dst(a[1]), seq)

	case "ITER":
		g.p("  vl_set(&%s, vl_iter(%s));", dst(a[0]), o(a[1]))
	case "ITNEXT":
		g.p("  { Value ik%d, iv%d;", seq, seq)
		g.p("    if (!vl_iter_next(%s, &ik%d, &iv%d)) goto %s;", o(a[2]), seq, seq, a[3])
		g.p("    vl_set(&%s, ik%d); vl_set(&%s, iv%d); }", dst(a[0]), seq, dst(a[1]), seq)

	case "PMATCH":
		if in.Pat == nil {
			g.failf("cgen: PMATCH without a structured pattern")
			return
		}
		g.p("  vl_set(&%s, vl_bool(vl_match(F, %s, &%s, K)));", dst(a[0]), o(a[1]),
			in.Pat.CName)

	case "PARSE":
		g.emitParse(f, in, seq)

	case "ARGDEF":
		g.p("  if (argc > %d) goto %s;", regSlot(a[0]), a[1])
	case "VARARG":
		slot := regSlot(a[0])
		g.p("  { Value va%d = vl_slice_new(0);", seq)
		g.p("    for (int i = %d; i < argc; i++) vl_slice_append(va%d, vl_retain(argv[i]));", slot, seq)
		g.p("    vl_set(&F->r[%d], va%d); }", slot, seq)

	case "SELBEG":
		g.selCases = nil
	case "SELRECV", "SELSEND", "SELDEF":
		g.selCases = append(g.selCases, *in)
	case "SELEND":
		g.emitSelect(f, seq)

	default:
		g.failf("cgen: no C lowering for %s", in.Op)
	}
}

// isVariant reports whether type.variant names an enum variant.
func (g *gen) isVariant(typ, variant string) bool {
	_ = typ
	_ = variant
	return false
}

// splitVariant divides `pkg.Enum.Variant` into its type and variant halves at
// the LAST dot — the type name itself may be package-qualified.
func splitVariant(name string) (string, string) {
	i := strings.LastIndex(name, ".")
	if i < 0 {
		return name, name
	}
	return name[:i], name[i+1:]
}

// variantCall recognizes a CALL whose name is Enum.Variant.
func (g *gen) variantCall(name string) []string {
	if !strings.Contains(name, ".") {
		return nil
	}
	typ, variant := splitVariant(name)
	for _, t := range g.mod.Types {
		if t.Kind == "ENUM" && t.Name == typ {
			return []string{typ, variant}
		}
	}
	return nil
}

// emitComposite builds a struct or exception value.
func (g *gen) emitComposite(f *ir.Func, in *ir.Instr, seq int, isExc bool) {
	a := in.Args
	typ := a[1]
	// Generics are type-erased: Stack[str] is the Stack descriptor (§3.7).
	if i := strings.Index(typ, "["); i > 0 {
		typ = typ[:i]
	}
	args := parseArgs(a[2])
	var pos, named []callArg
	for _, x := range args {
		if x.name != "" {
			named = append(named, x)
		} else {
			pos = append(pos, x)
		}
	}
	posName, posN := "NULL", "0"
	if len(pos) > 0 {
		var vals []string
		for _, x := range pos {
			vals = append(vals, g.operand(f, x.reg))
		}
		g.p("  Value cp%d[] = {%s};", seq, strings.Join(vals, ", "))
		posName, posN = fmt.Sprintf("cp%d", seq), strconv.Itoa(len(pos))
	}
	nmName, nvName, nN := "NULL", "NULL", "0"
	if len(named) > 0 {
		var ks, vs []string
		for _, x := range named {
			ks = append(ks, cQuote(x.name))
			vs = append(vs, g.operand(f, x.reg))
		}
		g.p("  const char *cn%d[] = {%s};", seq, strings.Join(ks, ", "))
		g.p("  Value cv%d[] = {%s};", seq, strings.Join(vs, ", "))
		nmName = fmt.Sprintf("cn%d", seq)
		nvName = fmt.Sprintf("cv%d", seq)
		nN = strconv.Itoa(len(named))
	}
	ctor := "vl_struct_new"
	if isExc {
		ctor = "vl_exc_new"
	}
	dst := fmt.Sprintf("F->r[%d]", regSlot(a[0]))
	g.p("  vl_set(&%s, %s(%s, %s, %s, %s, %s, %s));", dst, ctor, cQuote(typ),
		posName, posN, nmName, nvName, nN)

	// Field defaults for fields the literal omitted (§3.4).
	g.emitFieldDefaults(typ, dst, seq, named, len(pos))
}

// emitFieldDefaults fills declared defaults the composite literal omitted.
func (g *gen) emitFieldDefaults(typ, dst string, seq int, named []callArg, npos int) {
	def, ok := g.defaults[typ]
	if !ok {
		return
	}
	given := map[string]bool{}
	for _, n := range named {
		given[n.name] = true
	}
	for i, fd := range def {
		if given[fd.name] || i < npos || fd.constIdx < 0 {
			continue
		}
		g.p("  vl_setfld(%s, %s, vl_retain(K[%d]));", dst, cQuote(fd.name), fd.constIdx)
	}
}

// emitParse renders the PARSE template.
func (g *gen) emitParse(f *ir.Func, in *ir.Instr, seq int) {
	a := in.Args
	g.p("  { static const VlTerm t%d[] = {", seq)
	for _, t := range in.Terms {
		switch t.Kind {
		case ir.TermVar:
			slot := regSlot(t.Reg)
			if slot < 0 {
				g.failf("cgen: `parse` into the captured variable `%s` is not supported in native builds", t.Name)
				slot = 0
			}
			g.p("    {VL_T_VAR, %d, NULL, 0},", slot)
		case ir.TermDiscard:
			g.p("    {VL_T_DISCARD, 0, NULL, 0},")
		case ir.TermLit:
			g.p("    {VL_T_LIT, 0, %s, 0},", cQuote(t.Lit))
		case ir.TermCol:
			g.p("    {VL_T_COL, 0, NULL, %d},", t.Col)
		}
	}
	g.p("    {VL_T_DISCARD, 0, NULL, 0}};")
	g.p("    vl_parse(F, %s, %s, t%d, %d); }", g.operand(f, a[0]), cQuote(a[1]),
		seq, len(in.Terms))
}

// emitSelect renders a select statement from the buffered case instructions.
func (g *gen) emitSelect(f *ir.Func, seq int) {
	cases := g.selCases
	g.selCases = nil
	g.p("  { VlSelCase sc%d[%d];", seq, max(1, len(cases)))
	for i, c := range cases {
		switch c.Op {
		case "SELRECV":
			g.p("    sc%d[%d].kind = VL_SEL_RECV; sc%d[%d].ch = %s;", seq, i, seq, i,
				g.operand(f, c.Args[1]))
		case "SELSEND":
			g.p("    sc%d[%d].kind = VL_SEL_SEND; sc%d[%d].ch = %s; sc%d[%d].send = %s;",
				seq, i, seq, i, g.operand(f, c.Args[1]), seq, i, g.operand(f, c.Args[2]))
		case "SELDEF":
			g.p("    sc%d[%d].kind = VL_SEL_DEFAULT;", seq, i)
		}
	}
	g.p("    Value rv%d; bool ro%d;", seq, seq)
	g.p("    int ch%d = vl_select(sc%d, %d, &rv%d, &ro%d);", seq, seq, len(cases), seq, seq)
	for i, c := range cases {
		g.p("    if (ch%d == %d) {", seq, i)
		if c.Op == "SELRECV" {
			// Args: label, chan, [value-bind], [ok-bind] — by POSITION.
			binds := c.Args[2:]
			if len(binds) > 0 {
				if parts := strings.SplitN(binds[0], "=", 2); len(parts) == 2 {
					g.p("      vl_set(&F->r[%d], vl_retain(rv%d));", regSlot(parts[1]), seq)
				}
			}
			if len(binds) > 1 {
				if parts := strings.SplitN(binds[1], "=", 2); len(parts) == 2 {
					g.p("      vl_set(&F->r[%d], vl_bool(ro%d));", regSlot(parts[1]), seq)
				}
			}
			g.p("      vl_release(rv%d);", seq)
		}
		g.p("      goto %s;", c.Args[0])
		g.p("    }")
	}
	g.p("  }")
}
