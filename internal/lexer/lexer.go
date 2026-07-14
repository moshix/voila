// Package lexer turns Voilà source text into tokens, applying the
// semicolon-insertion rule of §2.4 and lexing string interpolation into parts.
package lexer

import (
	"strings"
	"unicode"
	"unicode/utf8"

	"voila/internal/diag"
)

type Lexer struct {
	src  *diag.Source
	bag  *diag.Bag
	text string
	pos  int // byte offset
	line int
	col  int // 1-based byte column

	lastKind Kind // last emitted non-SEMI token kind, for insertion + `//` disambiguation
	emitted  bool // any token emitted on the current line
}

func New(src *diag.Source, bag *diag.Bag) *Lexer {
	l := &Lexer{src: src, bag: bag, text: src.Text, line: 1, col: 1}
	// Shebang line (§1 script form).
	if strings.HasPrefix(l.text, "#!") {
		for l.pos < len(l.text) && l.text[l.pos] != '\n' {
			l.pos++
		}
	}
	return l
}

// Tokens lexes the entire input.
func Tokens(src *diag.Source, bag *diag.Bag) []Token {
	l := New(src, bag)
	var toks []Token
	for {
		t := l.Next()
		toks = append(toks, t)
		if t.Kind == EOF {
			return toks
		}
	}
}

func (l *Lexer) here() diag.Pos {
	return diag.Pos{File: l.src.Name, Line: l.line, Col: l.col}
}

func (l *Lexer) errorf(pos diag.Pos, width int, format string, args ...any) {
	l.bag.Errorf(diag.SpanOf(pos, width), "", format, args...)
}

func (l *Lexer) peekByte() byte {
	if l.pos < len(l.text) {
		return l.text[l.pos]
	}
	return 0
}

func (l *Lexer) peekAt(off int) byte {
	if l.pos+off < len(l.text) {
		return l.text[l.pos+off]
	}
	return 0
}

func (l *Lexer) advance() byte {
	c := l.text[l.pos]
	l.pos++
	if c == '\n' {
		l.line++
		l.col = 1
	} else {
		l.col++
	}
	return c
}

// canEndExpr reports whether a token kind can be the last token of an
// expression — the predicate for both semicolon insertion (§2.4) and the
// `//` comment-vs-floor-division disambiguation. DOT is included so a
// `parse` template ending in the discard term `.` terminates at the newline
// (trailing-dot method chains are not legal Voilà anyway).
func canEndExpr(k Kind) bool {
	switch k {
	case IDENT, INT, FLOAT, DEC, STR, ISTR, RUNE, TRUE, FALSE, NIL, SELF,
		RPAREN, RBRACKET, RBRACE, RETURN, BREAK, CONTINUE, DOT, SAY:
		// SAY: a bare `say` prints a blank line (§7.5) and must terminate.
		return true
	}
	return false
}

// Next returns the next token, inserting virtual semicolons at newlines.
func (l *Lexer) Next() Token {
	for {
		// Skip whitespace, handling newline insertion.
		for l.pos < len(l.text) {
			c := l.text[l.pos]
			if c == '\n' {
				if canEndExpr(l.lastKind) && l.emitted {
					tok := Token{Kind: SEMI, Lit: "\n", Pos: l.here(), Virtual: true}
					l.advance()
					l.lastKind = SEMI
					l.emitted = false
					return tok
				}
				l.advance()
				l.emitted = false
				continue
			}
			if c == ' ' || c == '\t' || c == '\r' {
				l.advance()
				continue
			}
			break
		}
		if l.pos >= len(l.text) {
			// EOF acts like a newline for insertion purposes.
			if canEndExpr(l.lastKind) && l.emitted {
				l.lastKind = SEMI
				return Token{Kind: SEMI, Lit: "", Pos: l.here(), Virtual: true}
			}
			return Token{Kind: EOF, Pos: l.here()}
		}

		c := l.peekByte()

		// Comments.
		if c == '/' && l.peekAt(1) == '/' {
			if l.isFloorDiv() {
				pos := l.here()
				l.advance()
				l.advance()
				return l.emit(Token{Kind: FLOORDIV, Pos: pos})
			}
			for l.pos < len(l.text) && l.text[l.pos] != '\n' {
				l.advance()
			}
			continue
		}
		if c == '/' && l.peekAt(1) == '*' {
			l.skipBlockComment()
			continue
		}

		tok := l.scanToken()
		return l.emit(tok)
	}
}

// isFloorDiv decides `//` in context: it is the floor-division operator only
// when the previous token can end an expression AND the `//` touches at least
// one operand (no whitespace immediately before, or none immediately after).
// `a // b` with space on both sides is a comment. Documented language rule
// (spec erratum: §2.2 and §5.1 conflict).
func (l *Lexer) isFloorDiv() bool {
	if !canEndExpr(l.lastKind) || !l.emitted {
		return false
	}
	if l.peekAt(2) == '/' { // `///` doc comment
		return false
	}
	tightBefore := l.pos > 0 && !isSpaceByte(l.text[l.pos-1])
	after := l.peekAt(2)
	tightAfter := after != 0 && !isSpaceByte(after) && after != '\n'
	return tightBefore || tightAfter
}

func isSpaceByte(c byte) bool { return c == ' ' || c == '\t' || c == '\r' || c == '\n' }

func (l *Lexer) skipBlockComment() {
	pos := l.here()
	l.advance() // '/'
	l.advance() // '*'
	depth := 1
	for l.pos < len(l.text) {
		if l.peekByte() == '/' && l.peekAt(1) == '*' {
			l.advance()
			l.advance()
			depth++
		} else if l.peekByte() == '*' && l.peekAt(1) == '/' {
			l.advance()
			l.advance()
			depth--
			if depth == 0 {
				return
			}
		} else {
			l.advance()
		}
	}
	l.errorf(pos, 2, "unterminated block comment")
}

func (l *Lexer) emit(t Token) Token {
	l.lastKind = t.Kind
	l.emitted = true
	return t
}

func (l *Lexer) scanToken() Token {
	pos := l.here()
	c := l.peekByte()

	switch {
	case c >= '0' && c <= '9':
		return l.scanNumber(pos)
	case c == '"':
		return l.scanString(pos)
	case c == '`':
		return l.scanRawString(pos)
	case c == '\'':
		return l.scanRune(pos)
	}

	r, size := utf8.DecodeRuneInString(l.text[l.pos:])
	if isLetter(r) {
		start := l.pos
		for l.pos < len(l.text) {
			r2, s2 := utf8.DecodeRuneInString(l.text[l.pos:])
			if !isLetter(r2) && !unicode.IsDigit(r2) {
				break
			}
			for i := 0; i < s2; i++ {
				l.advance()
			}
		}
		word := l.text[start:l.pos]
		if k, ok := keywords[word]; ok {
			return Token{Kind: k, Lit: word, Pos: pos}
		}
		return Token{Kind: IDENT, Lit: word, Pos: pos}
	}
	_ = size

	// Operators, longest match first.
	two := ""
	if l.pos+1 < len(l.text) {
		two = l.text[l.pos : l.pos+2]
	}
	three := ""
	if l.pos+2 < len(l.text) {
		three = l.text[l.pos : l.pos+3]
	}

	mk := func(k Kind, n int) Token {
		for i := 0; i < n; i++ {
			l.advance()
		}
		return Token{Kind: k, Lit: kindNames[k], Pos: pos}
	}

	switch three {
	case "...":
		return mk(ELLIPSIS, 3)
	case "..=":
		return mk(RANGEEQ, 3)
	case "||=":
		return mk(CONCAT_ASSIGN, 3)
	}
	switch two {
	case "..":
		return mk(RANGE, 2)
	case "**":
		return mk(POWER, 2)
	case "||":
		return mk(CONCAT, 2)
	case "<<":
		return mk(SHL, 2)
	case ">>":
		return mk(SHR, 2)
	case "<=":
		return mk(LE, 2)
	case ">=":
		return mk(GE, 2)
	case "==":
		return mk(EQ, 2)
	case "!=":
		return mk(NE, 2)
	case "+=":
		return mk(PLUS_ASSIGN, 2)
	case "-=":
		return mk(MINUS_ASSIGN, 2)
	case "*=":
		return mk(STAR_ASSIGN, 2)
	case "/=":
		return mk(SLASH_ASSIGN, 2)
	case "%=":
		return mk(PERCENT_ASSIGN, 2)
	case "<-":
		return mk(ARROW, 2)
	case "=>":
		return mk(FATARROW, 2)
	case ":=":
		return mk(DEFINE, 2)
	}
	switch c {
	case '+':
		return mk(PLUS, 1)
	case '-':
		return mk(MINUS, 1)
	case '*':
		return mk(STAR, 1)
	case '/':
		return mk(SLASH, 1)
	case '%':
		return mk(PERCENT, 1)
	case '&':
		return mk(AMP, 1)
	case '|':
		return mk(PIPE, 1)
	case '^':
		return mk(CARET, 1)
	case '~':
		return mk(TILDE, 1)
	case '<':
		return mk(LT, 1)
	case '>':
		return mk(GT, 1)
	case '=':
		return mk(ASSIGN, 1)
	case '(':
		return mk(LPAREN, 1)
	case ')':
		return mk(RPAREN, 1)
	case '[':
		return mk(LBRACKET, 1)
	case ']':
		return mk(RBRACKET, 1)
	case '{':
		return mk(LBRACE, 1)
	case '}':
		return mk(RBRACE, 1)
	case ',':
		return mk(COMMA, 1)
	case '.':
		return mk(DOT, 1)
	case ':':
		return mk(COLON, 1)
	case ';':
		return mk(SEMI, 1)
	case '!':
		return mk(BANG, 1)
	case '?':
		return mk(QUESTION, 1)
	}

	l.advance()
	l.errorf(pos, 1, "unexpected character %q", string(rune(c)))
	return Token{Kind: ILLEGAL, Lit: string(rune(c)), Pos: pos}
}

func isLetter(r rune) bool {
	return r == '_' || unicode.IsLetter(r)
}

func (l *Lexer) scanNumber(pos diag.Pos) Token {
	start := l.pos
	// Radix prefixes.
	if l.peekByte() == '0' && (l.peekAt(1) == 'x' || l.peekAt(1) == 'X' ||
		l.peekAt(1) == 'o' || l.peekAt(1) == 'O' ||
		l.peekAt(1) == 'b' || l.peekAt(1) == 'B') {
		l.advance()
		base := l.advance()
		digits := "0123456789abcdefABCDEF"
		switch base {
		case 'o', 'O':
			digits = "01234567"
		case 'b', 'B':
			digits = "01"
		}
		n := 0
		for l.pos < len(l.text) {
			c := l.peekByte()
			if c == '_' {
				l.advance()
				continue
			}
			if strings.IndexByte(digits, c) < 0 {
				break
			}
			l.advance()
			n++
		}
		if n == 0 {
			l.errorf(pos, l.pos-start, "malformed number literal")
		}
		lit := strings.ReplaceAll(l.text[start:l.pos], "_", "")
		return Token{Kind: INT, Lit: lit, Pos: pos}
	}

	isFloat := false
	scanDigits := func() {
		for l.pos < len(l.text) {
			c := l.peekByte()
			if c == '_' {
				l.advance()
				continue
			}
			if c < '0' || c > '9' {
				break
			}
			l.advance()
		}
	}
	scanDigits()
	// Fraction — but `0..10` must leave `..` alone.
	if l.peekByte() == '.' && l.peekAt(1) >= '0' && l.peekAt(1) <= '9' {
		isFloat = true
		l.advance()
		scanDigits()
	}
	// Exponent.
	if c := l.peekByte(); c == 'e' || c == 'E' {
		next := l.peekAt(1)
		next2 := l.peekAt(2)
		if next >= '0' && next <= '9' || ((next == '+' || next == '-') && next2 >= '0' && next2 <= '9') {
			isFloat = true
			l.advance()
			if c := l.peekByte(); c == '+' || c == '-' {
				l.advance()
			}
			scanDigits()
		}
	}
	lit := strings.ReplaceAll(l.text[start:l.pos], "_", "")
	// `d` suffix → dec literal (§2.6).
	if l.peekByte() == 'd' {
		after := l.peekAt(1)
		r, _ := utf8.DecodeRuneInString(string(after))
		if after == 0 || (!isLetter(r) && !(after >= '0' && after <= '9')) {
			l.advance()
			return Token{Kind: DEC, Lit: lit, Pos: pos}
		}
	}
	if isFloat {
		return Token{Kind: FLOAT, Lit: lit, Pos: pos}
	}
	return Token{Kind: INT, Lit: lit, Pos: pos}
}

// scanEscape decodes one escape sequence after the backslash has been peeked.
// Returns the decoded string.
func (l *Lexer) scanEscape(pos diag.Pos) string {
	l.advance() // backslash
	if l.pos >= len(l.text) {
		l.errorf(pos, 1, "unterminated escape sequence")
		return ""
	}
	c := l.advance()
	switch c {
	case 'n':
		return "\n"
	case 't':
		return "\t"
	case 'r':
		return "\r"
	case '0':
		return "\x00"
	case '\\':
		return "\\"
	case '"':
		return "\""
	case '\'':
		return "'"
	case '{':
		return "{"
	case '}':
		return "}"
	case 'u':
		if l.peekByte() != '{' {
			l.errorf(pos, 2, `\u escape must be \u{...}`)
			return ""
		}
		l.advance()
		hex := ""
		for l.pos < len(l.text) && l.peekByte() != '}' {
			hex += string(l.advance())
		}
		if l.pos < len(l.text) {
			l.advance() // '}'
		}
		var v int64
		for _, h := range hex {
			d := hexVal(byte(h))
			if d < 0 {
				l.errorf(pos, len(hex)+4, `bad hex in \u{...} escape`)
				return ""
			}
			v = v*16 + int64(d)
		}
		return string(rune(v))
	}
	l.errorf(pos, 2, "unknown escape sequence \\%c", c)
	return string(c)
}

func hexVal(c byte) int {
	switch {
	case c >= '0' && c <= '9':
		return int(c - '0')
	case c >= 'a' && c <= 'f':
		return int(c-'a') + 10
	case c >= 'A' && c <= 'F':
		return int(c-'A') + 10
	}
	return -1
}

// scanString lexes "..." with escapes; interpolation `{expr}` produces ISTR.
func (l *Lexer) scanString(pos diag.Pos) Token {
	l.advance() // opening quote
	var parts []StrPart
	var lit strings.Builder
	flush := func(p diag.Pos) {
		if lit.Len() > 0 {
			parts = append(parts, StrPart{Lit: lit.String(), Pos: p})
			lit.Reset()
		}
	}
	for {
		if l.pos >= len(l.text) || l.peekByte() == '\n' {
			l.errorf(pos, 1, "unterminated string literal")
			break
		}
		c := l.peekByte()
		if c == '"' {
			l.advance()
			break
		}
		if c == '\\' {
			epos := l.here()
			lit.WriteString(l.scanEscape(epos))
			continue
		}
		if c == '{' {
			exprPos := l.here()
			l.advance()
			src := l.scanInterpExpr(exprPos)
			flush(pos)
			parts = append(parts, StrPart{ExprSrc: src, IsExpr: true, Pos: diag.Pos{Line: exprPos.Line, Col: exprPos.Col + 1}})
			continue
		}
		if c == '}' {
			// A lone `}` in a literal is allowed; `\}` also works.
			lit.WriteByte(l.advance())
			continue
		}
		lit.WriteByte(l.advance())
	}
	hasExpr := false
	for _, p := range parts {
		if p.IsExpr {
			hasExpr = true
		}
	}
	if !hasExpr {
		return Token{Kind: STR, Lit: lit.String(), Pos: pos}
	}
	flush(pos)
	return Token{Kind: ISTR, Parts: parts, Pos: pos}
}

// scanInterpExpr scans the source of an embedded {expr}, handling nested
// braces, strings (which may themselves interpolate), runes and raw strings.
func (l *Lexer) scanInterpExpr(pos diag.Pos) string {
	start := l.pos
	depth := 1
	for l.pos < len(l.text) {
		c := l.peekByte()
		switch c {
		case '{':
			depth++
			l.advance()
		case '}':
			depth--
			if depth == 0 {
				src := l.text[start:l.pos]
				l.advance()
				return src
			}
			l.advance()
		case '"':
			// Nested string: skip its contents (its own {…} do not affect depth).
			l.advance()
			for l.pos < len(l.text) && l.peekByte() != '"' && l.peekByte() != '\n' {
				if l.peekByte() == '\\' {
					l.advance()
					if l.pos < len(l.text) {
						l.advance()
					}
					continue
				}
				l.advance()
			}
			if l.pos < len(l.text) && l.peekByte() == '"' {
				l.advance()
			}
		case '\'':
			l.advance()
			for l.pos < len(l.text) && l.peekByte() != '\'' && l.peekByte() != '\n' {
				if l.peekByte() == '\\' {
					l.advance()
					if l.pos < len(l.text) {
						l.advance()
					}
					continue
				}
				l.advance()
			}
			if l.pos < len(l.text) && l.peekByte() == '\'' {
				l.advance()
			}
		case '`':
			l.advance()
			for l.pos < len(l.text) && l.peekByte() != '`' {
				l.advance()
			}
			if l.pos < len(l.text) {
				l.advance()
			}
		default:
			l.advance()
		}
	}
	l.errorf(pos, 1, "unterminated interpolation in string literal")
	return l.text[start:l.pos]
}

func (l *Lexer) scanRawString(pos diag.Pos) Token {
	l.advance() // opening backtick
	start := l.pos
	for l.pos < len(l.text) && l.peekByte() != '`' {
		l.advance()
	}
	if l.pos >= len(l.text) {
		l.errorf(pos, 1, "unterminated raw string literal")
		return Token{Kind: STR, Lit: l.text[start:l.pos], Pos: pos}
	}
	lit := l.text[start:l.pos]
	l.advance() // closing backtick
	return Token{Kind: STR, Lit: lit, Pos: pos}
}

func (l *Lexer) scanRune(pos diag.Pos) Token {
	l.advance() // opening quote
	var val string
	if l.peekByte() == '\\' {
		val = l.scanEscape(l.here())
	} else if l.pos < len(l.text) {
		r, size := utf8.DecodeRuneInString(l.text[l.pos:])
		for i := 0; i < size; i++ {
			l.advance()
		}
		val = string(r)
	}
	if l.peekByte() != '\'' {
		l.errorf(pos, 1, "unterminated rune literal")
	} else {
		l.advance()
	}
	if utf8.RuneCountInString(val) != 1 {
		l.errorf(pos, 2, "rune literal must contain exactly one codepoint")
		if val == "" {
			val = "\x00"
		}
	}
	return Token{Kind: RUNE, Lit: val, Pos: pos}
}
