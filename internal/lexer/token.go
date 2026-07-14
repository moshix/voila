package lexer

import (
	"fmt"

	"voila/internal/diag"
)

// Kind identifies a token class.
type Kind int

const (
	EOF Kind = iota
	ILLEGAL
	SEMI // explicit ';' or virtual (inserted at newline; Token.Virtual set)

	IDENT
	INT   // 42, 0x1F, 0o755, 0b1010, 1_000_000
	FLOAT // 3.14, 6.02e23
	DEC   // 19.99d, 3d
	STR   // "text" without interpolation, or `raw`
	ISTR  // "cost: {x}" — interpolated; Token.Parts carries the pieces
	RUNE  // 'A'

	keywordBeg
	// Keywords (§2.5).
	AND
	AS
	ATTEMPT
	BREAK
	CASE
	CATCH
	CHAN
	CONST
	CONTINUE
	DEFER
	DROP
	EACH
	ELSE
	ENUM
	EXCEPTION
	FALSE
	FINALLY
	FOR
	FUNC
	GROUP
	IF
	IMPL
	IMPORT
	IN
	LET
	MATCH
	MUST
	MUT
	NIL
	NOT
	NOTHROW
	NUMERIC
	OR
	OWN
	PACKAGE
	PARSE
	RETURN
	SAY
	SELECT
	SELF
	SHARED
	SPAWN
	STRUCT
	SWITCH
	THROW
	THROWS
	TRAIT
	TRUE
	TRY
	TYPE
	USE
	VAR
	WEAK
	WHILE
	YIELD
	keywordEnd

	// Operators and punctuation.
	PLUS     // +
	MINUS    // -
	STAR     // *
	SLASH    // /
	FLOORDIV // //
	PERCENT  // %
	POWER    // **
	CONCAT   // ||
	SHL      // <<
	SHR      // >>
	AMP      // &
	PIPE     // |
	CARET    // ^
	TILDE    // ~

	LT // <
	LE // <=
	GT // >
	GE // >=
	EQ // ==
	NE // !=

	ASSIGN         // =
	PLUS_ASSIGN    // +=
	MINUS_ASSIGN   // -=
	STAR_ASSIGN    // *=
	SLASH_ASSIGN   // /=
	PERCENT_ASSIGN // %=
	CONCAT_ASSIGN  // ||=
	DEFINE         // :=  (accepted only in select case headers)

	LPAREN   // (
	RPAREN   // )
	LBRACKET // [
	RBRACKET // ]
	LBRACE   // {
	RBRACE   // }
	COMMA    // ,
	DOT      // .
	COLON    // :
	ARROW    // <-
	FATARROW // =>
	RANGE    // ..
	RANGEEQ  // ..=
	ELLIPSIS // ...
	BANG     // !
	QUESTION // ?
)

var kindNames = map[Kind]string{
	EOF: "EOF", ILLEGAL: "ILLEGAL", SEMI: ";",
	IDENT: "IDENT", INT: "INT", FLOAT: "FLOAT", DEC: "DEC", STR: "STR", ISTR: "ISTR", RUNE: "RUNE",
	AND: "and", AS: "as", ATTEMPT: "attempt", BREAK: "break", CASE: "case", CATCH: "catch",
	CHAN: "chan", CONST: "const", CONTINUE: "continue", DEFER: "defer", DROP: "drop",
	EACH: "each", ELSE: "else", ENUM: "enum", EXCEPTION: "exception", FALSE: "false",
	FINALLY: "finally", FOR: "for", FUNC: "func", GROUP: "group", IF: "if", IMPL: "impl",
	IMPORT: "import", IN: "in", LET: "let", MATCH: "match", MUST: "must", MUT: "mut",
	NIL: "nil", NOT: "not", NOTHROW: "nothrow", NUMERIC: "numeric", OR: "or", OWN: "own",
	PACKAGE: "package", PARSE: "parse", RETURN: "return", SAY: "say", SELECT: "select",
	SELF: "self", SHARED: "shared", SPAWN: "spawn", STRUCT: "struct", SWITCH: "switch",
	THROW: "throw", THROWS: "throws", TRAIT: "trait", TRUE: "true", TRY: "try",
	TYPE: "type", USE: "use", VAR: "var", WEAK: "weak", WHILE: "while", YIELD: "yield",
	PLUS: "+", MINUS: "-", STAR: "*", SLASH: "/", FLOORDIV: "//", PERCENT: "%",
	POWER: "**", CONCAT: "||", SHL: "<<", SHR: ">>", AMP: "&", PIPE: "|", CARET: "^",
	TILDE: "~", LT: "<", LE: "<=", GT: ">", GE: ">=", EQ: "==", NE: "!=",
	ASSIGN: "=", PLUS_ASSIGN: "+=", MINUS_ASSIGN: "-=", STAR_ASSIGN: "*=",
	SLASH_ASSIGN: "/=", PERCENT_ASSIGN: "%=", CONCAT_ASSIGN: "||=", DEFINE: ":=",
	LPAREN: "(", RPAREN: ")", LBRACKET: "[", RBRACKET: "]", LBRACE: "{", RBRACE: "}",
	COMMA: ",", DOT: ".", COLON: ":", ARROW: "<-", FATARROW: "=>",
	RANGE: "..", RANGEEQ: "..=", ELLIPSIS: "...", BANG: "!", QUESTION: "?",
}

func (k Kind) String() string {
	if s, ok := kindNames[k]; ok {
		return s
	}
	return fmt.Sprintf("Kind(%d)", int(k))
}

var keywords = map[string]Kind{}

func init() {
	for k := keywordBeg + 1; k < keywordEnd; k++ {
		keywords[kindNames[k]] = k
	}
}

// IsKeyword reports whether k is a reserved word.
func (k Kind) IsKeyword() bool { return k > keywordBeg && k < keywordEnd }

// StrPart is one piece of an interpolated string: either literal text or the
// source of an embedded expression (parsed later by the parser).
type StrPart struct {
	Lit     string // literal text, valid when IsExpr is false
	ExprSrc string // expression source, valid when IsExpr is true
	IsExpr  bool
	Pos     diag.Pos // position of the part (for expression sub-parsing)
}

// Token is a lexed token.
type Token struct {
	Kind    Kind
	Lit     string // raw literal text (identifiers, numbers, decoded strings)
	Parts   []StrPart
	Pos     diag.Pos
	Virtual bool // true for semicolons inserted at newlines
}

func (t Token) String() string {
	switch t.Kind {
	case IDENT, INT, FLOAT, DEC, RUNE:
		return fmt.Sprintf("%s(%s)", t.Kind, t.Lit)
	case STR:
		return fmt.Sprintf("STR(%q)", t.Lit)
	case ISTR:
		return fmt.Sprintf("ISTR(%d parts)", len(t.Parts))
	case SEMI:
		if t.Virtual {
			return "SEMI(virtual)"
		}
		return "SEMI"
	}
	return t.Kind.String()
}
