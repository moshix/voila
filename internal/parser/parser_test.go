package parser

import (
	"testing"

	"voila/internal/ast"
	"voila/internal/diag"
)

func parse(t *testing.T, src string) *ast.File {
	t.Helper()
	s := diag.NewSource("test.voi", src)
	bag := diag.NewBag(s)
	f := Parse(s, bag)
	if bag.HasErrors() {
		t.Fatalf("parse errors:\n%s", bag.Render())
	}
	return f
}

func TestHelloWorld(t *testing.T) {
	f := parse(t, `
package main

func main() {
    say "Hello, world!"
}
`)
	if f.Package != "main" || len(f.Decls) != 1 {
		t.Fatalf("bad file: %+v", f)
	}
}

func TestScriptForm(t *testing.T) {
	f := parse(t, "#!/usr/bin/env voila\nsay \"Hello, world!\"\nsay \"2 + 2 =\", 2 + 2\n")
	if len(f.ScriptStmts) != 2 {
		t.Fatalf("want 2 script stmts, got %d", len(f.ScriptStmts))
	}
}

// Every worked example from the specification must parse.

func TestSpecStructsAndEnums(t *testing.T) {
	parse(t, `
struct Point {
    x float
    y float
}

struct Server {
    host   str
    port   int = 8080
    routes map[str]int
}

enum Shape {
    Circle(radius float)
    Rect(w float, h float)
    Empty
}

func area(s Shape) float {
    match s {
        Circle(r)  => return 3.14159 * r * r
        Rect(w, h) => return w * h
        Empty      => return 0.0
    }
}

func classify(n int) {
    match n {
        x if x < 0  => say "negative"
        0           => say "zero"
        _           => say "positive"
    }
}

func demo() {
    let p = Point{x: 1.0, y: 2.0}
    let q = Point{1.0, 2.0}
    let s = Server{host: "localhost"}
    say p, q, s
}
`)
}

func TestSpecTraitsGenerics(t *testing.T) {
	parse(t, `
trait Show {
    func show(self) str
}

trait Reader {
    func read(mut self, buf mut []byte) int!
}

impl Show for Point {
    func show(self) str {
        return "({self.x}, {self.y})"
    }
}

func print_all(items []Show) {
    each it in items { say it.show() }
}

func max[T: Ord](a T, b T) T {
    if a > b { return a }
    return b
}

struct Stack[T] {
    items []T
}

impl[T] Stack[T] {
    func push(mut self, own v T) { self.items.append(v) }
    func pop(mut self) ?T        { return self.items.pop() }
}

type UserID = int
type Handler fn(int) str
`)
}

func TestSpecErrorsAndExceptions(t *testing.T) {
	parse(t, `
exception NotFound  { path str }
exception ConvError { from str, to str, value str }

exception ParseError {
    line int
    col  int
    func message(self) str {
        return "parse error at {self.line}:{self.col}"
    }
}

func parse_port(s str) int! {
    let n = try to_int(s) else return err("not a number: {s}")
    if n < 1 or n > 65535 {
        return err("port out of range: {n}")
    }
    return n
}

func consume(s str) {
    let p = try parse_port(s)
    let q = parse_port(s) else 8080
    match parse_port(s) {
        Ok(v)  => say "port", v
        Err(e) => say "bad:", e.message()
    }
    say p, q
}

func main() {
    try {
        say "load"
    } catch e: NotFound {
        say "no config at", e.path
    } catch e: Timeout, NetError {
        say "network trouble:", e.message()
    } catch e {
        say "fatal:", e.message()
        each frame in e.trace() { say "   ", frame }
    } finally {
        say "flush"
    }
    let n = must parse_port("80")
    let r = attempt load(path)
    say n, r
}
`)
}

func TestSpecConcurrency(t *testing.T) {
	parse(t, `
func main() {
    group {
        spawn worker(1)
        spawn worker(2)
    }
    say "all workers done"

    let ch = chan[int](0)
    let ch2 = chan[str](100)
    ch <- 42
    let job = <-ch
    let job2, ok = <-ch
    close(ch)
    each j in ch { say j }

    select {
    case job := <-jobs:
        process(job)
    case results <- value:
        say "sent"
    case <-after:
        say "timeout"
    default:
        say "nothing ready"
    }

    let counter = cell[int](0)
    group {
        for i in 0..1000 {
            spawn { counter.update(fn(n) { return n + 1 }) }
        }
    }
    say counter.get()

    group timeout 5*t {
        let a = spawn compute_a()
        let b = spawn compute_b()
        say a.await() + b.await()
    }
}

func fetch_all(urls []str) []str! {
    var results = make([]str, len(urls))
    try group {
        each i, u in urls {
            spawn {
                results[i] = try http.get(u)
            }
        }
    }
    return results
}
`)
}

func TestSpecParseStmt(t *testing.T) {
	parse(t, `
func main() {
    let line = "moshix,55,Milan"
    parse line "," name "," age "," city
    parse csv_row "," id "," rest .
    parse text word1 word2 rest .
    parse record 1 recid 9 acct 21 amount .
    parse upper cmd verb args .
    parse var s . . third .
    say name, age, city
}
`)
}

func TestSpecMemory(t *testing.T) {
	parse(t, `
struct Node {
    value  int
    kids   []shared[Node]
    parent weak[Node]
}

func depth(n shared[Node]) int {
    match n.parent.upgrade() {
        Some(p) => return 1 + depth(p)
        nil     => return 0
    }
}

func read_config(path str) str! {
    let f = try os.open(path)
    defer f.close()
    return try f.read_all()
}

func demo() {
    var data = [1, 2, 3]
    say peek(data)
    fill(data)
    consume(data)
    drop data
}

func peek(v []int) int { return v[0] }
func fill(v mut []int) { }
func consume(own v []int) { }
`)
}

func TestSpecFunctions(t *testing.T) {
	parse(t, `
func add(a int, b int) int { return a + b }
func add2(a, b int) int    { return a + b }
func divmod(a, b int) (int, int) { return a//b, a % b }

func sum(nums ...int) int {
    var t = 0
    each n in nums { t += n }
    return t
}

func greet(name str, greeting str = "Hello") str {
    return "{greeting}, {name}!"
}

func demo() {
    let q, r = divmod(17, 5)
    say sum(1, 2, 3), sum(v...)
    say greet("moshix")
    say greet("moshix", greeting: "Ciao")
    let double = fn(x int) int { return x * 2 }
    let d2 = fn(x) { return x * 2 }
    nums.map(fn(x) { x * 2 })
    let f = fn own [data] () { say data }
    say q, r, double, d2, f
}

impl Point {
    func dist(self, other Point) float { return 0.0 }
    func shift(mut self, dx float)     { }
    func into_pair(own self) (float, float) { return (0.0, 0.0) }
    func origin() Point { return Point{0.0, 0.0} }
}

func checked() int throws NotFound, Timeout { return 1 }
func safe() int nothrow { return 2 }
`)
}

func TestSpecStatements(t *testing.T) {
	parse(t, `
func main() {
    if x > 10 {
        say "big"
    } else if x > 5 {
        say "medium"
    } else {
        say "small"
    }

    if let Some(v) = maybe {
        say v
    }

    for i in 0..10        { say i }
    each x in v           { say x }
    each i, x in v        { say i, x }
    while x > 0           { x -= 1 }
    for                   { break }

    for i in 0..10 {
        if i == 3 { continue }
        if i == 7 { break }
    }

    outer: for i in 0..3 {
        for j in 0..3 {
            if j == 2 { continue outer }
        }
    }

    switch day {
    case "sat", "sun":
        say "weekend"
    case "mon":
        say "ugh"
    default:
        say "weekday"
    }

    numeric digits 34
    let price = 19.99d
    say price

    throw NotFound{path: p}
    throw ParseError{line: 1, col: 2} from e
}
`)
}

func TestElseOperatorNewlineDoesNotContinue(t *testing.T) {
	// A virtual semicolon after `f()` must terminate the statement; the
	// next line's `else` is then a parse error — operator-else must not
	// survive a newline.
	s := diag.NewSource("test.voi", "func f() int { return 1 }\nfunc main() {\n let a = f()\n else 0\n}\n")
	bag := diag.NewBag(s)
	Parse(s, bag)
	if !bag.HasErrors() {
		t.Fatal("expected parse error for newline before operator-else")
	}
}

func TestUseForms(t *testing.T) {
	f := parse(t, `
use "std/io"
use "std/http"
use "github.com/moshix/go3270" as t3270
use "std/str" { upper, words }
use "std/math" *
`)
	if len(f.Uses) != 5 {
		t.Fatalf("want 5 uses, got %d", len(f.Uses))
	}
	if f.Uses[2].Alias != "t3270" {
		t.Errorf("alias: %+v", f.Uses[2])
	}
	if len(f.Uses[3].Names) != 2 {
		t.Errorf("selective: %+v", f.Uses[3])
	}
	if !f.Uses[4].Wildcard {
		t.Errorf("wildcard: %+v", f.Uses[4])
	}
}

func TestPrecedence(t *testing.T) {
	f := parse(t, "let x = 2 + 3 * 4 ** 2\n")
	let := f.Decls[0].(*ast.GlobalDecl).Stmt.(*ast.LetStmt)
	add, ok := let.Value.(*ast.BinaryExpr)
	if !ok {
		t.Fatalf("top not binary: %T", let.Value)
	}
	if add.Op.String() != "+" {
		t.Fatalf("top op: %v", add.Op)
	}
	mul := add.Y.(*ast.BinaryExpr)
	if mul.Op.String() != "*" {
		t.Fatalf("mul op: %v", mul.Op)
	}
	pow := mul.Y.(*ast.BinaryExpr)
	if pow.Op.String() != "**" {
		t.Fatalf("pow op: %v", pow.Op)
	}
}

func TestConcatBindsTighterThanComparison(t *testing.T) {
	f := parse(t, `let x = a || b == c`)
	let := f.Decls[0].(*ast.GlobalDecl).Stmt.(*ast.LetStmt)
	cmp := let.Value.(*ast.BinaryExpr)
	if cmp.Op.String() != "==" {
		t.Fatalf("top should be ==, got %v", cmp.Op)
	}
}
