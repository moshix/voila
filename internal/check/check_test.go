package check

import (
	"strings"
	"testing"

	"voila/internal/diag"
	"voila/internal/parser"
)

// checkSrc runs the checker and returns rendered diagnostics ("" = clean).
func checkSrc(t *testing.T, src string) string {
	t.Helper()
	source := diag.NewSource("test.voi", src)
	bag := diag.NewBag(source)
	file := parser.Parse(source, bag)
	if bag.HasErrors() {
		t.Fatalf("parse errors:\n%s", bag.Render())
	}
	Check(file, source, bag)
	if !bag.HasErrors() {
		return ""
	}
	return bag.Render()
}

func TestAcyclicityRule(t *testing.T) {
	// §14.3: scalar shared back-edge must be rejected with the spec's message.
	out := checkSrc(t, `struct Node {
    value  int
    kids   []shared[Node]
    parent shared[Node]
}
`)
	for _, want := range []string{
		"error: type `Node` forms an ownership cycle through field `parent`",
		"owning edge back to `Node`",
		"owning cycles can leak; Voilà forbids them",
		"use `weak[Node]` for a back-edge",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
	// weak back-edge and tree-shaped kids are fine.
	if out := checkSrc(t, `struct Node {
    value  int
    kids   []shared[Node]
    parent weak[Node]
}
`); out != "" {
		t.Errorf("weak back-edge should pass:\n%s", out)
	}
	// Mutual scalar cycle.
	out = checkSrc(t, `struct A { b B }
struct B { a A }
`)
	if !strings.Contains(out, "ownership cycle") {
		t.Errorf("mutual cycle not caught:\n%s", out)
	}
}

func TestBorrowInStructField(t *testing.T) {
	out := checkSrc(t, `struct Bad { p &Point }
struct Point { x float }
`)
	if !strings.Contains(out, "borrows cannot escape") {
		t.Errorf("borrow field not caught:\n%s", out)
	}
}

func TestLatticeErrors(t *testing.T) {
	// The four §3.2 ERROR fixtures.
	out := checkSrc(t, `func main() {
    let big i64 = 9_007_199_254_740_993
    let f float = big
    let n int   = 3.7
    let d dec   = 0.1
    let b u8    = 300
    say f, n, d, b
}
`)
	for _, want := range []string{
		"i64 → float",
		"float → int truncates",
		"binary float → dec is lossy",
		"literal 300 does not fit in u8",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("missing %q in:\n%s", want, out)
		}
	}
	// Legal widenings stay silent.
	if out := checkSrc(t, `func main() {
    let count u16 = 300
    var total int = 0
    let f float = 3
    let d dec = 42
    say count, total, f, d
}
`); out != "" {
		t.Errorf("legal widening flagged:\n%s", out)
	}
}

func TestExhaustiveness(t *testing.T) {
	out := checkSrc(t, `enum Shape { Circle(r float), Rect(w float, h float), Empty }
func f(s Shape) {
    match s {
        Circle(r) => say r
        Empty     => say "e"
    }
}
`)
	if !strings.Contains(out, "not exhaustive") || !strings.Contains(out, "Rect") {
		t.Errorf("missing exhaustiveness error:\n%s", out)
	}
	if out := checkSrc(t, `enum Shape { Circle(r float), Empty }
func f(s Shape) {
    match s {
        Circle(r) => say r
        _         => say "other"
    }
}
`); out != "" {
		t.Errorf("catch-all should satisfy:\n%s", out)
	}
	if out := checkSrc(t, `func f(v ?int) {
    match v {
        Some(x) => say x
        nil     => say "none"
    }
}
`); out != "" {
		t.Errorf("Some+nil should satisfy:\n%s", out)
	}
}

func TestUseAfterMove(t *testing.T) {
	out := checkSrc(t, `func consume(own v []int) { }
func main() {
    var data = [1, 2, 3]
    consume(data)
    say data
}
`)
	if !strings.Contains(out, "use of moved value `data`") {
		t.Errorf("move not caught:\n%s", out)
	}
	// Reassignment revives.
	if out := checkSrc(t, `func consume(own v []int) { }
func main() {
    var data = [1, 2, 3]
    consume(data)
    data = [4, 5]
    say data
}
`); out != "" {
		t.Errorf("revived binding flagged:\n%s", out)
	}
}

func TestFormatStringCheck(t *testing.T) {
	out := checkSrc(t, `use "std/fmt"
func main() {
    fmt.printf("%d %s\n", 1)
}
`)
	if !strings.Contains(out, "needs 2 argument(s), call passes 1") {
		t.Errorf("arity not caught:\n%s", out)
	}
	out = checkSrc(t, `use "std/fmt"
func main() {
    fmt.printf("%y\n", 1)
}
`)
	if !strings.Contains(out, "unknown format verb") {
		t.Errorf("bad verb not caught:\n%s", out)
	}
	// Star width consumes an arg; fprintf offsets by the writer.
	if out := checkSrc(t, `use "std/fmt"
use "std/os"
func main() {
    fmt.printf("%*d\n", 8, 42)
    fmt.fprintf(os.stderr, "%s\n", "hi")
}
`); out != "" {
		t.Errorf("valid formats flagged:\n%s", out)
	}
}
