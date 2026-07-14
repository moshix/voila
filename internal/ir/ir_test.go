package ir

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"voila/internal/diag"
	"voila/internal/parser"
)

func lowerSrc(t *testing.T, src string) (*Module, *diag.Source) {
	t.Helper()
	source := diag.NewSource("test.voi", src)
	bag := diag.NewBag(source)
	file := parser.Parse(source, bag)
	if bag.HasErrors() {
		t.Fatalf("parse errors:\n%s", bag.Render())
	}
	mod, err := Lower(file, "test.voi")
	if err != nil {
		t.Fatalf("lowering failed: %v", err)
	}
	return mod, source
}

func fnByName(t *testing.T, m *Module, name string) *Func {
	t.Helper()
	for _, f := range m.Funcs {
		if f.Name == name {
			return f
		}
	}
	t.Fatalf("no function %q in module (have %v)", name, funcNames(m))
	return nil
}

func funcNames(m *Module) []string {
	var names []string
	for _, f := range m.Funcs {
		names = append(names, f.Name)
	}
	return names
}

func ops(f *Func) []string {
	var out []string
	for _, in := range f.Instrs {
		if in.Op != "*" {
			out = append(out, in.Op)
		}
	}
	return out
}

func hasOpSeq(f *Func, seq ...string) bool {
	all := ops(f)
	i := 0
	for _, op := range all {
		if op == seq[i] {
			i++
			if i == len(seq) {
				return true
			}
		}
	}
	return false
}

func countComments(f *Func, substr string) int {
	n := 0
	for _, in := range f.Instrs {
		if in.Op == "*" && strings.Contains(strings.Join(in.Args, " "), substr) {
			n++
		}
	}
	return n
}

// ---------------------------------------------------------------- sweep

// TestSweepAllPrograms: every sample and the selfhost lexer must lower with
// zero errors, and every listing line must fit in 79 columns (runes).
func TestSweepAllPrograms(t *testing.T) {
	samples, err := filepath.Glob("../../samples/*.voi")
	if err != nil || len(samples) == 0 {
		t.Fatalf("no samples: %v", err)
	}
	programs := append(samples,
		"../../selfhost/lexer.voi",
		"../../selfhost/testdata/kitchen.voi")

	for _, path := range programs {
		name := filepath.Base(path)
		t.Run(name, func(t *testing.T) {
			text, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			source := diag.NewSource(name, string(text))
			bag := diag.NewBag(source)
			file := parser.Parse(source, bag)
			if bag.HasErrors() {
				t.Fatalf("parse errors:\n%s", bag.Render())
			}
			mod, err := Lower(file, name)
			if err != nil {
				t.Fatalf("lowering must cover the whole language: %v", err)
			}
			listing := Listing(mod, source)
			for i, line := range strings.Split(listing, "\n") {
				if n := len([]rune(line)); n > 79 {
					t.Errorf("line %d is %d runes (max 79): %s", i+1, n, line)
				}
			}
		})
	}
}

// ---------------------------------------------------------------- goldens

// TestGoldenListings locks the full listings of two representative samples.
// Unlike the sample-output goldens, these FAIL when missing — a silent
// bless would hide bad output. Regenerate with UPDATE_IR_GOLDEN=1.
func TestGoldenListings(t *testing.T) {
	for _, name := range []string{"04_calculator", "05_pipeline"} {
		t.Run(name, func(t *testing.T) {
			path := "../../samples/" + name + ".voi"
			text, err := os.ReadFile(path)
			if err != nil {
				t.Fatal(err)
			}
			source := diag.NewSource(name+".voi", string(text))
			bag := diag.NewBag(source)
			file := parser.Parse(source, bag)
			if bag.HasErrors() {
				t.Fatalf("parse:\n%s", bag.Render())
			}
			mod, err := Lower(file, name+".voi")
			if err != nil {
				t.Fatal(err)
			}
			got := Listing(mod, source)

			golden := filepath.Join("testdata", name+".s")
			if os.Getenv("UPDATE_IR_GOLDEN") != "" {
				os.MkdirAll("testdata", 0o755)
				if err := os.WriteFile(golden, []byte(got), 0o644); err != nil {
					t.Fatal(err)
				}
				return
			}
			want, err := os.ReadFile(golden)
			if err != nil {
				t.Fatalf("missing golden %s (run UPDATE_IR_GOLDEN=1 go test ./internal/ir)", golden)
			}
			if got != string(want) {
				t.Errorf("listing changed; diff against %s (regen with UPDATE_IR_GOLDEN=1 if intended)", golden)
			}
		})
	}
}

// ---------------------------------------------------------------- shapes

func TestShortCircuitLowering(t *testing.T) {
	mod, _ := lowerSrc(t, `
func f(a bool, b bool) bool { return a and b }
func g(a bool, b bool) bool { return a or b }
`)
	if !hasOpSeq(fnByName(t, mod, "f"), "JMPF", "MOVE", "JUMP", "LOADK") {
		t.Errorf("and: want short-circuit shape, got %v", ops(fnByName(t, mod, "f")))
	}
	if !hasOpSeq(fnByName(t, mod, "g"), "JMPT", "MOVE", "JUMP", "LOADK") {
		t.Errorf("or: want short-circuit shape, got %v", ops(fnByName(t, mod, "g")))
	}
}

func TestMatchGuardOrder(t *testing.T) {
	mod, _ := lowerSrc(t, `
enum T { A(n int), B }
func f(v T) int {
    match v {
        A(n) if n > 0 => return n
        A(n)          => return -n
        B             => return 0
    }
}
`)
	f := fnByName(t, mod, "f")
	// Pattern test precedes the guard, which precedes the body.
	if !hasOpSeq(f, "PMATCH", "JMPF", "CMPGT", "JMPF", "RET") {
		t.Errorf("guard must run after binds: %v", ops(f))
	}
	// Exhaustion aborts.
	if !hasOpSeq(f, "ABORT") {
		t.Errorf("match must end in ABORT: %v", ops(f))
	}
}

func TestFinallyExitEdges(t *testing.T) {
	mod, _ := lowerSrc(t, `
exception E { }
func f(n int) int {
    try {
        if n == 1 { return 1 }
        say n
    } catch e: E {
        return 2
    } finally {
        say "fin"
    }
    return 0
}
`)
	f := fnByName(t, mod, "f")
	// Five duplicated copies: return-from-body, normal, return-from-catch,
	// end-of-catch (dead after the diverging return, but emitted by the
	// single-pass lowering), and unwind.
	if got := countComments(f, "finally (dup:"); got != 5 {
		t.Errorf("want 5 finally copies, got %d", got)
	}
	if !hasOpSeq(f, "EHPUSH", "EHPOP") {
		t.Errorf("missing EHPUSH/EHPOP: %v", ops(f))
	}
	if !hasOpSeq(f, "CATCH", "ISTYPE", "JMPF") {
		t.Errorf("missing catch dispatch: %v", ops(f))
	}
}

func TestDeferLowersToClosure(t *testing.T) {
	mod, _ := lowerSrc(t, `
func f() {
    defer say "bye"
    say "hi"
}
`)
	f := fnByName(t, mod, "f")
	if !hasOpSeq(f, "MKCLOS", "DEFER") {
		t.Errorf("defer must be MKCLOS+DEFER: %v", ops(f))
	}
	fnByName(t, mod, "f$fn1") // the synthetic body must exist
}

func TestSpawnForms(t *testing.T) {
	mod, _ := lowerSrc(t, `
func work(n int) int { return n }
func main() {
    group {
        spawn work(1)
        spawn { say "block" }
    }
}
`)
	m := fnByName(t, mod, "main")
	if !hasOpSeq(m, "GRPBEG", "SPAWN") {
		t.Errorf("named spawn must pre-evaluate and SPAWN: %v", ops(m))
	}
	if !hasOpSeq(m, "MKCLOS", "SPAWNR", "GRPEND") {
		t.Errorf("block spawn must MKCLOS+SPAWNR: %v", ops(m))
	}
}

func TestTryGroupPropagates(t *testing.T) {
	mod, _ := lowerSrc(t, `
func main() {
    try group {
        say "x"
    }
}
`)
	if !hasOpSeq(fnByName(t, mod, "main"), "GRPBEG", "GRPENDT") {
		t.Errorf("try group must end with GRPENDT: %v", ops(fnByName(t, mod, "main")))
	}
}

func TestParseTemplate(t *testing.T) {
	mod, _ := lowerSrc(t, `
func main() {
    let line = "a,b"
    parse line "," x "," y
    say x, y
}
`)
	m := fnByName(t, mod, "main")
	found := false
	for _, in := range m.Instrs {
		if in.Op == "PARSE" {
			found = true
			tmpl := strings.Join(in.Args, " ")
			if !strings.Contains(tmpl, "x=r") || !strings.Contains(tmpl, "','") {
				t.Errorf("template must map targets to registers: %v", in.Args)
			}
		}
	}
	if !found {
		t.Errorf("no PARSE op: %v", ops(m))
	}
}

func TestElseUnwrapsDirectTry(t *testing.T) {
	// `try f() else alt` handles failure locally: no TRYP (decision 4).
	mod, _ := lowerSrc(t, `
func f() int! { return 1 }
func main() {
    let a = try f() else 0
    say a
}
`)
	m := fnByName(t, mod, "main")
	for _, op := range ops(m) {
		if op == "TRYP" {
			t.Errorf("else-guarded try must not propagate: %v", ops(m))
		}
	}
	if !hasOpSeq(m, "ISFAIL", "JMPT") {
		t.Errorf("missing ISFAIL branch: %v", ops(m))
	}
}

func TestBareTryPropagates(t *testing.T) {
	mod, _ := lowerSrc(t, `
func f() int! { return 1 }
func g() int! { return try f() }
`)
	if !hasOpSeq(fnByName(t, mod, "g"), "CALL", "TRYP") {
		t.Errorf("bare try must TRYP: %v", ops(fnByName(t, mod, "g")))
	}
}

func TestSwitchFallthrough(t *testing.T) {
	mod, _ := lowerSrc(t, `
func f(s str) {
    switch s {
    case "a":
        say "a"
        fallthrough
    case "b":
        say "b"
    default:
        say "d"
    }
}
`)
	f := fnByName(t, mod, "f")
	jumps := 0
	for _, in := range f.Instrs {
		if in.Op == "JUMP" {
			jumps++
		}
	}
	if jumps < 4 { // dispatch default-jump + fallthrough + per-body ends
		t.Errorf("fallthrough shape wrong: %v", ops(f))
	}
}

func TestCommaOkReceive(t *testing.T) {
	mod, _ := lowerSrc(t, `
func main() {
    let ch = chan[int](1)
    let v, ok = <-ch
    say v, ok
}
`)
	if !hasOpSeq(fnByName(t, mod, "main"), "NEWCHAN", "RECVOK") {
		t.Errorf("comma-ok must RECVOK: %v", ops(fnByName(t, mod, "main")))
	}
}

func TestGlobalsInit(t *testing.T) {
	mod, _ := lowerSrc(t, `
let G = build()
func build() int { return 42 }
func main() { say G }
`)
	init := fnByName(t, mod, "$INIT")
	if !hasOpSeq(init, "CALL", "SETGLB") {
		t.Errorf("$INIT must call and SETGLB: %v", ops(init))
	}
	if !hasOpSeq(fnByName(t, mod, "main"), "GETGLB", "SAY") {
		t.Errorf("main must GETGLB: %v", ops(fnByName(t, mod, "main")))
	}
}

func TestScriptForm(t *testing.T) {
	// Top-level `let` parses as a GlobalDecl, so its store lands in $INIT;
	// the loose statements land in $SCRIPT and read it back via GETGLB —
	// exactly the interpreter's execution order (globals, then script).
	mod, _ := lowerSrc(t, "say \"hi\"\nlet x = 1\nsay x\n")
	if !hasOpSeq(fnByName(t, mod, "$INIT"), "LOADK", "SETGLB") {
		t.Errorf("$INIT must store the global: %v", ops(fnByName(t, mod, "$INIT")))
	}
	if !hasOpSeq(fnByName(t, mod, "$SCRIPT"), "SAY", "GETGLB", "SAY") {
		t.Errorf("$SCRIPT must read the global: %v", ops(fnByName(t, mod, "$SCRIPT")))
	}
}

func TestUpvalueAccess(t *testing.T) {
	mod, _ := lowerSrc(t, `
func main() {
    var count = 0
    let inc = fn() { count += 1 }
    inc()
    say count
}
`)
	closure := fnByName(t, mod, "main$fn1")
	foundUpset := false
	for _, in := range closure.Instrs {
		if in.Op == "UPSET" {
			foundUpset = true
		}
		for _, a := range in.Args {
			if strings.Contains(a, "^count") {
				return // reads render as ^name
			}
		}
	}
	if !foundUpset {
		t.Errorf("closure mutation must UPSET: %v", ops(closure))
	}
}

func TestDefaultArgPrologue(t *testing.T) {
	mod, _ := lowerSrc(t, `
func greet(name str, greeting str = "Hello") str {
    return "{greeting}, {name}!"
}
`)
	g := fnByName(t, mod, "greet")
	if !hasOpSeq(g, "ARGDEF", "LOADK", "MOVE") {
		t.Errorf("default arg needs ARGDEF prologue: %v", ops(g))
	}
}

func TestVariadicPrologue(t *testing.T) {
	mod, _ := lowerSrc(t, `
func sum(nums ...int) int {
    var t = 0
    each n in nums { t += n }
    return t
}
`)
	if !hasOpSeq(fnByName(t, mod, "sum"), "VARARG") {
		t.Errorf("variadic needs VARARG prologue: %v", ops(fnByName(t, mod, "sum")))
	}
}

func TestConstPoolDedup(t *testing.T) {
	mod, _ := lowerSrc(t, `
func main() {
    say 16, 0x10, 1_6
    say "a", "a"
}
`)
	ints, strs := 0, 0
	for _, c := range mod.Consts {
		switch {
		case c.Kind == CInt && c.Text == "16":
			ints++
		case c.Kind == CStr && c.Text == "a":
			strs++
		}
	}
	if ints != 1 {
		t.Errorf("16/0x10/1_6 must dedup to one INT constant, got %d", ints)
	}
	if strs != 1 {
		t.Errorf(`"a" must dedup, got %d`, strs)
	}
}

func TestSelectComposite(t *testing.T) {
	mod, _ := lowerSrc(t, `
func main() {
    let ch = chan[int](1)
    select {
    case v := <-ch:
        say v
    default:
        say "none"
    }
}
`)
	if !hasOpSeq(fnByName(t, mod, "main"), "SELBEG", "SELRECV", "SELDEF", "SELEND") {
		t.Errorf("select composite shape: %v", ops(fnByName(t, mod, "main")))
	}
}

func TestVariantConstructors(t *testing.T) {
	mod, _ := lowerSrc(t, `
enum Shape { Circle(r float), Empty }
func main() {
    let a = Circle(1.0)
    let b = Empty
    let f = Circle
    say a, b, f
}
`)
	m := fnByName(t, mod, "main")
	var newenums, loadfns int
	for _, in := range m.Instrs {
		switch in.Op {
		case "NEWENUM":
			newenums++
		case "LOADFN":
			if strings.Contains(strings.Join(in.Args, ","), "Shape.Circle") {
				loadfns++
			}
		}
	}
	if newenums != 2 { // Circle(1.0) call + bare Empty value
		t.Errorf("want 2 NEWENUM, got %d: %v", newenums, ops(m))
	}
	if loadfns != 1 { // Circle as a first-class constructor value
		t.Errorf("want variant ctor LOADFN, got %d", loadfns)
	}
}

func TestLoweringErrorsAreHard(t *testing.T) {
	source := diag.NewSource("bad.voi", "func f() { say unknown_name }\n")
	bag := diag.NewBag(source)
	file := parser.Parse(source, bag)
	if bag.HasErrors() {
		t.Fatal("unexpected parse errors")
	}
	if _, err := Lower(file, "bad.voi"); err == nil {
		t.Fatal("unknown identifier must be a hard lowering error")
	}
}
