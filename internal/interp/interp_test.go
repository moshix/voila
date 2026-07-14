package interp

import (
	"bytes"
	"strings"
	"testing"

	"voila/internal/diag"
	"voila/internal/parser"
)

// run executes a program and returns (stdout, stderr, exit).
func run(t *testing.T, src string) (string, string, int) {
	t.Helper()
	source := diag.NewSource("test.voi", src)
	bag := diag.NewBag(source)
	file := parser.Parse(source, bag)
	if bag.HasErrors() {
		t.Fatalf("parse errors:\n%s", bag.Render())
	}
	in := New(file, source)
	var out, errOut bytes.Buffer
	in.Stdout = &out
	in.Stderr = &errOut
	exit := in.Run(nil)
	return out.String(), errOut.String(), exit
}

func expect(t *testing.T, src, want string) {
	t.Helper()
	out, errOut, exit := run(t, src)
	if exit != 0 {
		t.Fatalf("exit %d\nstdout:\n%s\nstderr:\n%s", exit, out, errOut)
	}
	if out != want {
		t.Errorf("output mismatch\n--- got ---\n%s--- want ---\n%s", out, want)
	}
}

func TestSayAndArithmetic(t *testing.T) {
	expect(t, `
say "Hello, world!"
say "2 + 2 =", 2 + 2
say 7 / 2
say 17//5, -17//5, 17 % 5, -17 % 5
say 2 ** 10
say "a" || "b" || str(42)
`, "Hello, world!\n2 + 2 = 4\n3.5\n3 -4 2 3\n1024\nab42\n")
}

func TestDecExactness(t *testing.T) {
	expect(t, `
numeric digits 34
let price = 19.99d
let qty   = 3d
say price * qty
say 0.1d + 0.2d
var total = 0d
total += 19.99d
total += 0.01d
say total
`, "59.97\n0.3\n20\n")
}

func TestNumericDigitsDynamicScope(t *testing.T) {
	expect(t, `
func third() dec {
    return 1d / 3d
}
func main() {
    numeric digits 5
    say third()
    numeric digits 10
    say third()
}
`, "0.33333\n0.3333333333\n")
}

func TestInterpolation(t *testing.T) {
	expect(t, `
let x = 42
let name = "moshix"
say "x is {x} and twice is {x * 2}"
say "hello, {name}!"
say "nested: {str(len("abc"))}"
`, "x is 42 and twice is 84\nhello, moshix!\nnested: 3\n")
}

func TestStructsEnumsMatch(t *testing.T) {
	expect(t, `
struct Point { x float, y float }
enum Shape {
    Circle(radius float)
    Rect(w float, h float)
    Empty
}
func area(s Shape) float {
    match s {
        Circle(r)  => return 3.0 * r * r
        Rect(w, h) => return w * h
        Empty      => return 0.0
    }
}
func main() {
    let p = Point{x: 1.0, y: 2.0}
    say p
    say area(Circle(2.0)), area(Rect(3.0, 4.0)), area(Empty)
    match 5 {
        x if x < 0 => say "neg"
        0          => say "zero"
        _          => say "pos"
    }
}
`, "Point{x: 1, y: 2}\n12 12 0\npos\n")
}

func TestTraitShow(t *testing.T) {
	expect(t, `
struct Point { x float, y float }
trait Show { func show(self) str }
impl Show for Point {
    func show(self) str { return "({self.x}, {self.y})" }
}
func main() {
    let p = Point{x: 1.5, y: 2.5}
    say p.show()
    say p
}
`, "(1.5, 2.5)\n(1.5, 2.5)\n")
}

func TestErrorValues(t *testing.T) {
	expect(t, `
func parse_port(s str) int! {
    let n = try to_int(s) else return err("not a number: {s}")
    if n < 1 or n > 65535 {
        return err("port out of range: {n}")
    }
    return n
}
func main() {
    say try parse_port("8080")
    say parse_port("banana") else 1234
    match parse_port("99999") {
        Ok(v)  => say "ok", v
        Err(e) => say "bad:", e.message()
    }
}
`, "8080\n1234\nbad: port out of range: 99999\n")
}

func TestExceptions(t *testing.T) {
	expect(t, `
exception NotFound { path str }
func load(path str) str {
    throw NotFound{path: path}
}
func main() {
    try {
        let c = load("/etc/app.conf")
        say c
    } catch e: NotFound {
        say "no config at", e.path
    } finally {
        say "cleanup"
    }
    say "after"
}
`, "no config at /etc/app.conf\ncleanup\nafter\n")
}

func TestMustAttemptBridges(t *testing.T) {
	expect(t, `
exception Boom { n int }
func fails() int! { return err("nope") }
func boom() int { throw Boom{n: 1} }
func main() {
    try {
        let x = must fails()
        say x
    } catch e {
        say "must bridged:", e.message()
    }
    match attempt boom() {
        Ok(v)  => say "ok", v
        Err(e) => say "attempt bridged:", e.message()
    }
}
`, "must bridged: nope\nattempt bridged: Boom{n: 1}\n")
}

func TestDeferLIFOAndUnwind(t *testing.T) {
	expect(t, `
exception E { }
func f() {
    defer say "defer 1"
    defer say "defer 2"
    say "body"
    throw E{}
}
func main() {
    try { f() } catch e { say "caught" }
}
`, "body\ndefer 2\ndefer 1\ncaught\n")
}

func TestUserMessageImpl(t *testing.T) {
	expect(t, `
exception ParseError {
    line int
    col  int
    func message(self) str {
        return "parse error at {self.line}:{self.col}"
    }
}
func main() {
    try {
        throw ParseError{line: 3, col: 9}
    } catch e {
        say e.message()
    }
}
`, "parse error at 3:9\n")
}

func TestThrowFromChain(t *testing.T) {
	expect(t, `
exception Low { }
exception High { }
func main() {
    try {
        try {
            throw Low{}
        } catch e: Low {
            throw High{} from e
        }
    } catch e: High {
        say "high, caused by:", e.cause().message()
    }
}
`, "high, caused by: Low\n")
}

func TestMovesRuntime(t *testing.T) {
	_, errOut, exit := run(t, `
func main() {
    var a = [1, 2, 3]
    let b = a
    say a
}
`)
	if exit == 0 {
		t.Fatal("expected use-of-moved error")
	}
	if !strings.Contains(errOut, "use of moved value") {
		t.Errorf("stderr: %s", errOut)
	}
}

func TestOwnParamMove(t *testing.T) {
	_, errOut, exit := run(t, `
func consume(own v []int) { }
func main() {
    var data = [1, 2, 3]
    if len(data) > 0 { consume(data) }
    say data
}
`)
	if exit == 0 || !strings.Contains(errOut, "moved") {
		t.Errorf("exit %d, stderr: %s", exit, errOut)
	}
}

func TestClone(t *testing.T) {
	expect(t, `
func main() {
    var a = [1, 2, 3]
    let b = a.clone()
    b.append(4)
    say a, b
}
`, "[1, 2, 3] [1, 2, 3, 4]\n")
}

func TestParseTemplates(t *testing.T) {
	expect(t, `
func main() {
    let line = "moshix,55,Milan"
    parse line "," name "," age "," city
    say name, age, city
    parse "one two three four" w1 w2 rest .
    say w1, "/", w2, "/", rest
    let rec = "AC12345678  0099"
    parse rec 1 code 3 acct 13 amt .
    say trim(code), trim(acct), int(trim(amt))
    parse upper "hello world" a .
    say a
}
`, "moshix 55 Milan\none / two / three\nAC 12345678 99\nHELLO\n")
}

func TestPrintfVerbs(t *testing.T) {
	expect(t, `
use "std/fmt"
func main() {
    fmt.printf("%-8s|%6.2f|%05d|%x|%o|%b|%c|%t|%q|%v\n",
        "ab", 19.995d, 42, 255, 8, 5, 'Z', true, "q", [1, 2])
    fmt.printf("%*d\n", 6, 42)
}
`, "ab      | 20.00|00042|ff|10|101|Z|true|\"q\"|[1, 2]\n    42\n")
}

func TestStringFunctions(t *testing.T) {
	expect(t, `
use "std/str"
func main() {
    say str.upper("abc"), "abc".upper()
    say str.word("a b c d", 2), str.words("a b c d")
    say str.translate("hello", "el", "ip")
    say str.overlay("1234567890", "XX", 3)
    say str.substr("abc", 10, 5) || "<empty>"
    say str.left("voila", 2), str.right("voila", 2)
    say "héllo".rune_len(), len("héllo")
    say str.is_digit("123"), str.is_digit("12a")
    var sb = str.Builder{}
    sb.write("a")
    sb.write("b")
    say sb.done()
}
`, "ABC ABC\nb 4\nhippo\n123XX67890\n<empty>\nvo la\n5 6\ntrue false\nab\n")
}

func TestCollections(t *testing.T) {
	expect(t, `
func main() {
    let v = [3, 1, 2]
    v.sort()
    say v
    say v.map(fn(x) { x * 10 })
    say v.filter(fn(x) { x > 1 })
    say v.reduce(fn(a, x) { a + x }, 0)
    say v.contains(2), v.index_of(3)
    let m = make(map[str]int)
    m["b"] = 2
    m["a"] = 1
    each k, val in m { say k, val }
    say keys(m), values(m)
    let s = set()
    s.add(1)
    s.add(1)
    s.add(2)
    say len(s)
    say zip([1, 2], ["a", "b"])
}
`, "[1, 2, 3]\n[10, 20, 30]\n[2, 3]\n6\ntrue 2\nb 2\na 1\n[\"b\", \"a\"] [2, 1]\n2\n[(1, \"a\"), (2, \"b\")]\n")
}

func TestConcurrencyGroup(t *testing.T) {
	expect(t, `
func work(n int) int { return n * 2 }
func main() {
    group {
        let a = spawn work(21)
        let b = spawn work(100)
        say a.await() + b.await()
    }
    say "done"
}
`, "242\ndone\n")
}

func TestChannels(t *testing.T) {
	expect(t, `
func main() {
    let ch = chan[int](3)
    ch <- 1
    ch <- 2
    close(ch)
    var total = 0
    each x in ch { total += x }
    say total
    let ch2 = chan[int](1)
    ch2 <- 9
    let v, ok = <-ch2
    say v, ok
    close(ch2)
    let v2, ok2 = <-ch2
    say v2, ok2
}
`, "3\n9 true\nnil false\n")
}

func TestGroupErrorPropagation(t *testing.T) {
	expect(t, `
exception Fail { who str }
func task(n int) {
    if n == 2 { throw Fail{who: "task2"} }
    // Block until cancelled.
    let never = chan[int](0)
    let x = <-never
}
func main() {
    try {
        group {
            spawn task(1)
            spawn task(2)
            spawn task(3)
        }
    } catch e: Fail {
        say "caught", e.who
    }
    say "all reaped"
}
`, "caught task2\nall reaped\n")
}

func TestCellCounter(t *testing.T) {
	expect(t, `
func main() {
    let counter = cell[int](0)
    group {
        for i in 0..50 {
            spawn { counter.update(fn(n) { return n + 1 }) }
        }
    }
    say counter.get()
}
`, "50\n")
}

func TestSelectDefault(t *testing.T) {
	expect(t, `
func main() {
    let ch = chan[int](1)
    select {
    case v := <-ch:
        say "got", v
    default:
        say "nothing ready"
    }
    ch <- 7
    select {
    case v := <-ch:
        say "got", v
    default:
        say "nothing ready"
    }
}
`, "nothing ready\ngot 7\n")
}

func TestSharedWeakTree(t *testing.T) {
	expect(t, `
struct Node {
    name   str
    parent weak[Node]
}
func depth(n shared[Node]) int {
    match n.parent.upgrade() {
        Some(p) => return 1 + depth(p)
        nil     => return 0
    }
}
func main() {
    let root = shared(Node{name: "root"})
    let kid = shared(Node{name: "kid", parent: weak(root)})
    say depth(root), depth(kid)
}
`, "0 1\n")
}

func TestGenericsStack(t *testing.T) {
	expect(t, `
struct Stack[T] { items []T }
impl[T] Stack[T] {
    func push(mut self, own v T) { self.items.append(v) }
    func pop(mut self) ?T        { return self.items.pop() }
}
func main() {
    var s = Stack[int]{items: []}
    s.push(1)
    s.push(2)
    say s.pop(), s.pop(), s.pop()
}
`, "2 1 nil\n")
}

func TestClosuresAndCaptures(t *testing.T) {
	expect(t, `
func main() {
    let double = fn(x int) int { return x * 2 }
    say double(21)
    say [1, 2, 3].map(fn(x) { x * x })
    var counter = 0
    let inc = fn() { counter += 1 }
    inc()
    inc()
    say counter
}
`, "42\n[1, 4, 9]\n2\n")
}

func TestDefaultAndNamedArgs(t *testing.T) {
	expect(t, `
func greet(name str, greeting str = "Hello") str {
    return "{greeting}, {name}!"
}
func main() {
    say greet("moshix")
    say greet("moshix", greeting: "Ciao")
}
`, "Hello, moshix!\nCiao, moshix!\n")
}

func TestVariadicSpread(t *testing.T) {
	expect(t, `
func total(nums ...int) int {
    var t = 0
    each n in nums { t += n }
    return t
}
func main() {
    say total(1, 2, 3)
    let v = [4, 5, 6]
    say total(v...)
}
`, "6\n15\n")
}

func TestTuplesMultiReturn(t *testing.T) {
	expect(t, `
func divmod(a, b int) (int, int) { return a//b, a % b }
func main() {
    let q, r = divmod(17, 5)
    say q, r
}
`, "3 2\n")
}

func TestLabeledLoops(t *testing.T) {
	expect(t, `
func main() {
    var n = 0
    outer: for i in 0..3 {
        for j in 0..3 {
            if j == 2 { continue outer }
            n += 1
        }
    }
    say n
    var m = 0
    o2: for i in 0..10 {
        for j in 0..10 {
            if i * j > 4 { break o2 }
            m += 1
        }
    }
    say m
}
`, "6\n15\n")
}

func TestSwitchFallthrough(t *testing.T) {
	expect(t, `
func main() {
    switch "b" {
    case "a":
        say "a"
    case "b":
        say "b"
        fallthrough
    case "c":
        say "c"
    default:
        say "d"
    }
}
`, "b\nc\n")
}

func TestIfLet(t *testing.T) {
	expect(t, `
func find(x int) ?int {
    if x > 0 { return x * 10 }
    return nil
}
func main() {
    if let Some(v) = find(5) {
        say "found", v
    }
    if let Some(v) = find(-1) {
        say "found", v
    } else {
        say "nothing"
    }
}
`, "found 50\nnothing\n")
}

func TestOverflowTraps(t *testing.T) {
	expect(t, `
func main() {
    try {
        let x = 9_223_372_036_854_775_807 + 1
        say x
    } catch e: OverflowError {
        say "trapped"
    }
    say wrap_add(9_223_372_036_854_775_807, 1)
}
`, "trapped\n-9223372036854775808\n")
}

func TestConversions(t *testing.T) {
	expect(t, `
func main() {
    say int("42"), int("ff", 16), int(3.9), int(-3.9)
    say to_int("banana"), to_u8(300), to_u8(200)
    say wrap_u8(300), sat_u8(300), sat_i8(-999)
    say float("3.14"), dec("19.99"), str(42) || "!"
    try {
        let x = u8(300)
        say x
    } catch e: ConvError {
        say "conv trapped:", e.value, "->", e.to
    }
}
`, "42 255 3 -3\nnil nil 200\n44 255 -128\n3.14 19.99 42!\nconv trapped: 300 -> u8\n")
}

func TestIntToFloatLossless(t *testing.T) {
	expect(t, `
func main() {
    say 1 + 2.5
    try {
        let big = 9_007_199_254_740_993
        say big + 0.5
    } catch e: ConvError {
        say "lossy trapped"
    }
    say float(9_007_199_254_740_993) > 0.0
}
`, "3.5\nlossy trapped\ntrue\n")
}

func TestScriptMode(t *testing.T) {
	expect(t, `#!/usr/bin/env voila
say "script!"
let x = 1 + 1
say x
`, "script!\n2\n")
}

func TestStringSliceIndexing(t *testing.T) {
	expect(t, `
func main() {
    let s = "hello world"
    say s[0..5]
    say s[6..=10]
    try {
        say s[5..99]
    } catch e: RangeError {
        say "range throws"
    }
    let v = [10, 20, 30, 40]
    say v[1..3]
}
`, "hello\nworld\nrange throws\n[20, 30]\n")
}

func TestRangesWithStep(t *testing.T) {
	expect(t, `
func main() {
    var out = []
    for i in 0..10 by 2 { out.append(i) }
    say out
    for i in 0..=6 by 3 { say i }
}
`, "[0, 2, 4, 6, 8]\n0\n3\n6\n")
}

func TestJSONRoundTrip(t *testing.T) {
	expect(t, "struct Cfg { host str, port int }\n"+
		"func main() {\n"+
		"    let cfg = json.decode[Cfg](`{\"host\": \"h\", \"port\": 1}`)\n"+
		"    say cfg\n"+
		"    say json.encode(cfg)\n"+
		"    say json.encode([1, nil, true, \"x\"])\n"+
		"}\n",
		"Cfg{host: \"h\", port: 1}\n{\"host\":\"h\",\"port\":1}\n[1,null,true,\"x\"]\n")
}

func TestUncaughtExceptionExitCode(t *testing.T) {
	_, errOut, exit := run(t, `
exception Boom { }
func main() {
    throw Boom{}
}
`)
	if exit != 1 {
		t.Fatalf("exit = %d, want 1", exit)
	}
	if !strings.Contains(errOut, "unhandled exception") {
		t.Errorf("stderr: %s", errOut)
	}
}

func TestAbortExitCode(t *testing.T) {
	_, errOut, exit := run(t, `assert(false, "boom")`)
	if exit != 1 || !strings.Contains(errOut, "abort: assertion failed: boom") {
		t.Errorf("exit %d stderr %q", exit, errOut)
	}
}

func TestErrorValueEscapingMain(t *testing.T) {
	_, errOut, exit := run(t, `
func fails() int! { return err("bad thing") }
func main() {
    let x = try fails()
    say x
}
`)
	if exit != 1 || !strings.Contains(errOut, "error: bad thing") {
		t.Errorf("exit %d stderr %q", exit, errOut)
	}
}

func TestGroupTimeout(t *testing.T) {
	expect(t, `
use "std/time"
func main() {
    try {
        group timeout 20 * time.Millisecond {
            spawn { time.sleep(10 * time.Second) }
        }
    } catch e: Timeout {
        say "timed out"
    }
}
`, "timed out\n")
}

func TestNothrowViolationAborts(t *testing.T) {
	_, errOut, exit := run(t, `
exception E { }
func bad() nothrow {
    throw E{}
}
func main() { bad() }
`)
	if exit != 1 || !strings.Contains(errOut, "nothrow") {
		t.Errorf("exit %d stderr %q", exit, errOut)
	}
}

func TestEnumEquality(t *testing.T) {
	expect(t, `
enum Color { Red, Green, Blue }
func main() {
    let c = Red
    say c == Red, c == Blue
    let cs = [Red, Green]
    say Green in cs, Blue in cs
}
`, "true false\ntrue false\n")
}

func TestConstAndImmutability(t *testing.T) {
	_, errOut, exit := run(t, `
func main() {
    let x = 1
    x = 2
}
`)
	if exit == 0 || !strings.Contains(errOut, "immutable") {
		t.Errorf("exit %d stderr %q", exit, errOut)
	}
}
