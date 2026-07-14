package lexer

import (
	"strings"
	"testing"

	"voila/internal/diag"
)

func lex(t *testing.T, src string) []Token {
	t.Helper()
	s := diag.NewSource("test.voi", src)
	bag := diag.NewBag(s)
	toks := Tokens(s, bag)
	if bag.HasErrors() {
		t.Fatalf("lex errors:\n%s", bag.Render())
	}
	return toks
}

func kinds(toks []Token) []Kind {
	var ks []Kind
	for _, t := range toks {
		ks = append(ks, t.Kind)
	}
	return ks
}

func eq(t *testing.T, got []Kind, want ...Kind) {
	t.Helper()
	if len(got) != len(want) {
		t.Fatalf("got %v want %v", got, want)
	}
	for i := range got {
		if got[i] != want[i] {
			t.Fatalf("token %d: got %v want %v", i, got[i], want[i])
		}
	}
}

func TestSemicolonInsertion(t *testing.T) {
	cases := []struct {
		src  string
		want []Kind
	}{
		// Inserted after identifiers and literals at newline.
		{"x\ny", []Kind{IDENT, SEMI, IDENT, SEMI, EOF}},
		{"42\n", []Kind{INT, SEMI, EOF}},
		{"say \"hi\"\n", []Kind{SAY, STR, SEMI, EOF}},
		// Not inserted after operators or commas (continuation).
		{"a +\nb", []Kind{IDENT, PLUS, IDENT, SEMI, EOF}},
		{"f(a,\nb)", []Kind{IDENT, LPAREN, IDENT, COMMA, IDENT, RPAREN, SEMI, EOF}},
		{"a and\nb", []Kind{IDENT, AND, IDENT, SEMI, EOF}},
		// Inserted after ) ] } and keywords return/break/continue.
		{"f()\ng()", []Kind{IDENT, LPAREN, RPAREN, SEMI, IDENT, LPAREN, RPAREN, SEMI, EOF}},
		{"return\nx", []Kind{RETURN, SEMI, IDENT, SEMI, EOF}},
		// Not after { or in an empty line run.
		{"if x {\n}\n", []Kind{IF, IDENT, LBRACE, RBRACE, SEMI, EOF}},
		// Explicit semicolons still work.
		{"a; b", []Kind{IDENT, SEMI, IDENT, SEMI, EOF}},
	}
	for _, c := range cases {
		got := kinds(lex(t, c.src))
		if len(got) != len(c.want) {
			t.Errorf("%q: got %v want %v", c.src, got, c.want)
			continue
		}
		for i := range got {
			if got[i] != c.want[i] {
				t.Errorf("%q: token %d got %v want %v", c.src, i, got[i], c.want[i])
			}
		}
	}
}

func TestNestedBlockComments(t *testing.T) {
	toks := lex(t, "a /* outer /* inner */ still outer */ b")
	eq(t, kinds(toks), IDENT, IDENT, SEMI, EOF)
}

func TestFloorDivVsComment(t *testing.T) {
	// Tight `//` in operator position = floor division.
	toks := lex(t, "x = a//b")
	eq(t, kinds(toks), IDENT, ASSIGN, IDENT, FLOORDIV, IDENT, SEMI, EOF)
	// One-side tight also operator.
	toks = lex(t, "x = a// b")
	eq(t, kinds(toks), IDENT, ASSIGN, IDENT, FLOORDIV, IDENT, SEMI, EOF)
	// Spaced on both sides = trailing comment.
	toks = lex(t, "x = a // a comment here")
	eq(t, kinds(toks), IDENT, ASSIGN, IDENT, SEMI, EOF)
	// Line-start = comment.
	toks = lex(t, "// just a comment\nx")
	eq(t, kinds(toks), IDENT, SEMI, EOF)
	// After an operator = comment (no expression to divide).
	toks = lex(t, "x = // comment\n5")
	eq(t, kinds(toks), IDENT, ASSIGN, INT, SEMI, EOF)
	// Doc comment never divides.
	toks = lex(t, "x = a/// doc\n")
	eq(t, kinds(toks), IDENT, ASSIGN, IDENT, SEMI, EOF)
}

func TestNumbers(t *testing.T) {
	toks := lex(t, "42 0x1F 0o755 0b1010 1_000_000 3.14 6.02e23 19.99d 3d 0.1d")
	want := []struct {
		k   Kind
		lit string
	}{
		{INT, "42"}, {INT, "0x1F"}, {INT, "0o755"}, {INT, "0b1010"}, {INT, "1000000"},
		{FLOAT, "3.14"}, {FLOAT, "6.02e23"}, {DEC, "19.99"}, {DEC, "3"}, {DEC, "0.1"},
	}
	for i, w := range want {
		if toks[i].Kind != w.k || toks[i].Lit != w.lit {
			t.Errorf("token %d: got %v(%q) want %v(%q)", i, toks[i].Kind, toks[i].Lit, w.k, w.lit)
		}
	}
}

func TestRangeVsFloat(t *testing.T) {
	toks := lex(t, "0..10")
	eq(t, kinds(toks), INT, RANGE, INT, SEMI, EOF)
	toks = lex(t, "0..=10")
	eq(t, kinds(toks), INT, RANGEEQ, INT, SEMI, EOF)
}

func TestStrings(t *testing.T) {
	toks := lex(t, `"hello\nworld"`)
	if toks[0].Kind != STR || toks[0].Lit != "hello\nworld" {
		t.Errorf("got %v %q", toks[0].Kind, toks[0].Lit)
	}
	toks = lex(t, "`raw\n string`")
	if toks[0].Kind != STR || toks[0].Lit != "raw\n string" {
		t.Errorf("raw: got %v %q", toks[0].Kind, toks[0].Lit)
	}
	toks = lex(t, `'A'`)
	if toks[0].Kind != RUNE || toks[0].Lit != "A" {
		t.Errorf("rune: got %v %q", toks[0].Kind, toks[0].Lit)
	}
}

func TestInterpolation(t *testing.T) {
	toks := lex(t, `"cost: {x}"`)
	if toks[0].Kind != ISTR {
		t.Fatalf("got %v", toks[0].Kind)
	}
	parts := toks[0].Parts
	if len(parts) != 2 || parts[0].Lit != "cost: " || !parts[1].IsExpr || parts[1].ExprSrc != "x" {
		t.Fatalf("parts: %+v", parts)
	}

	// Nested string with its own interpolation inside the expression.
	toks = lex(t, `"a {f("{x}")} b"`)
	if toks[0].Kind != ISTR {
		t.Fatalf("nested: got %v", toks[0].Kind)
	}
	parts = toks[0].Parts
	if len(parts) != 3 {
		t.Fatalf("nested parts: %+v", parts)
	}
	if parts[1].ExprSrc != `f("{x}")` {
		t.Errorf("nested expr src: %q", parts[1].ExprSrc)
	}
	if parts[2].Lit != " b" {
		t.Errorf("tail: %q", parts[2].Lit)
	}

	// Nested braces (composite literal inside interpolation).
	toks = lex(t, `"p = {Point{x: 1}}"`)
	parts = toks[0].Parts
	if parts[1].ExprSrc != "Point{x: 1}" {
		t.Errorf("braces: %q", parts[1].ExprSrc)
	}

	// Escaped brace is literal.
	toks = lex(t, `"lit \{x}"`)
	if toks[0].Kind != STR || toks[0].Lit != "lit {x}" {
		t.Errorf("escaped: %v %q", toks[0].Kind, toks[0].Lit)
	}
}

func TestShebang(t *testing.T) {
	toks := lex(t, "#!/usr/bin/env voila\nsay \"hi\"\n")
	eq(t, kinds(toks), SAY, STR, SEMI, EOF)
}

func TestOperators(t *testing.T) {
	toks := lex(t, "a ** b || c << d ..= e ... <- => := ||=")
	eq(t, kinds(toks), IDENT, POWER, IDENT, CONCAT, IDENT, SHL, IDENT,
		RANGEEQ, IDENT, ELLIPSIS, ARROW, FATARROW, DEFINE, CONCAT_ASSIGN, EOF)
}

func TestUnicodeIdents(t *testing.T) {
	toks := lex(t, "let café = 1")
	if toks[1].Kind != IDENT || toks[1].Lit != "café" {
		t.Errorf("got %v %q", toks[1].Kind, toks[1].Lit)
	}
}

func TestKeywordsAll(t *testing.T) {
	src := strings.Join([]string{
		"and", "as", "attempt", "break", "case", "catch", "chan", "const",
		"continue", "defer", "drop", "each", "else", "enum", "exception", "false",
		"finally", "for", "func", "group", "if", "impl", "import", "in",
		"let", "match", "must", "mut", "nil", "not", "nothrow", "numeric",
		"or", "own", "package", "parse", "return", "say", "select", "self",
		"shared", "spawn", "struct", "switch", "throw", "throws", "trait", "true",
		"try", "type", "use", "var", "weak", "while", "yield",
	}, " ")
	toks := lex(t, src)
	for _, tok := range toks {
		if tok.Kind == IDENT {
			t.Errorf("%q lexed as IDENT, expected keyword", tok.Lit)
		}
	}
}
