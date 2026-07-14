// Package ir defines Voilà's register-based linear IR — the spec §12
// "register bytecode" — and the lowering from the checked AST. Stage 0 does
// not execute this IR; `voila build -S` renders it as an HLASM-style
// assembly listing. The future bytecode VM grows from it.
package ir

import (
	"fmt"

	"voila/internal/ast"
)

// ConstKind classifies constant-pool entries.
type ConstKind int

const (
	CInt ConstKind = iota
	CFloat
	CDec
	CStr
	CRune
	CBool
	CNil
	CUnit
)

var constKindNames = [...]string{"INT", "FLOAT", "DEC", "STR", "RUNE", "BOOL", "NIL", "UNIT"}

func (k ConstKind) String() string { return constKindNames[k] }

// Const is one deduplicated constant-pool entry. Text is the canonical
// rendering (ints in decimal, floats via strconv 'g', dec as written).
type Const struct {
	Kind ConstKind
	Text string
}

// PatKind classifies a match pattern.
type PatKind int

const (
	PatWildcard PatKind = iota
	PatNil
	PatLiteral // Text holds the literal's source form
	PatBind    // Name binds; Reg is the register it binds into
	PatVariant // Name is the variant; Elems are sub-patterns
)

// Pattern is the structured form of a PMATCH operand. The listing renders it
// as text; a code generator walks it.
type Pattern struct {
	Kind  PatKind
	Name  string
	Text  string // literal source form, for the listing (PatLiteral)
	Reg   string // destination register (PatBind)
	Elems []*Pattern

	// The literal a PatLiteral compares against, carried as data. Re-parsing
	// the rendered Text is lossy (escapes, `0xd`), so we never do it.
	LitKind ConstKind
	LitText string

	CName string // set by the C backend: the static VlPat symbol
}

// TermKind classifies a parse-template term (§7.4).
type TermKind int

const (
	TermVar TermKind = iota
	TermDiscard
	TermLit
	TermCol
)

// Term is one structured parse-template term.
type Term struct {
	Kind TermKind
	Name string // TermVar: source name
	Reg  string // TermVar: target register
	Lit  string // TermLit: separator text
	Col  int    // TermCol: 1-based column
}

// UpRef locates an upvalue in the lexical parent chain: Depth frames up
// (1 = the immediately enclosing frame), slot Slot.
type UpRef struct {
	Name  string
	Depth int
	Slot  int

	// Scope is the lexical scope index the name was found in; the lowerer
	// uses it to tell a loop-local capture from an outer one.
	Scope int
}

// Instr is one IR instruction. Simple operands are symbolic text (registers
// "r3", constants "K7", labels "L2", names) — the IR is an assembly language.
// The two genuinely structured operands, match patterns and parse templates,
// are carried as data so a code generator does not have to re-parse them.
type Instr struct {
	Labels []string // labels attached to this instruction
	Op     string
	Args   []string
	Pat    *Pattern // PMATCH
	Terms  []Term   // PARSE
	Src    int      // 1-based source line; 0 = none
}

// Func is one lowered function (CSECT).
type Func struct {
	Name    string
	Sig     string // rendered signature for the CSECT comment
	NRegs   int
	Instrs  []Instr
	SrcLine int

	// Ups resolves the `^name` operands appearing in this function to their
	// position in the lexical parent chain. Closures capture the enclosing
	// frame by reference, so an upvalue is (depth, slot), not a register.
	Ups map[string]UpRef

	// UpOrder lists the upvalue names in the order they were first referenced.
	// Anything that must be REPRODUCIBLE iterates this, never the map: Go
	// randomises map iteration, and a compiler whose output depends on it is
	// not a compiler you can diff.
	UpOrder []string

	// LoopCapture is set when this closure captures a variable declared
	// inside an enclosing loop. The interpreter gives each iteration its own
	// binding; a frame-capturing backend cannot, so it must refuse rather
	// than answer differently.
	LoopCapture string
}

// TypeDecl carries a declaration rendered as DSECT commentary.
type TypeDecl struct {
	Kind string // "STRUCT", "ENUM", "EXCEPTION"
	Name string
	// Lines are pre-rendered member descriptions, e.g. "FLOAT    x".
	Lines []string
}

// Module is one lowered compilation unit.
type Module struct {
	Source string
	Consts []Const
	Types  []TypeDecl
	Funcs  []*Func

	constIdx map[string]int
}

func newModule(source string) *Module {
	return &Module{Source: source, constIdx: map[string]int{}}
}

// constant interns a constant and returns its pool name ("K3"). The pool is
// insertion-ordered; the dedup key is (kind, canonical text) — never a Go
// map iteration.
func (m *Module) constant(kind ConstKind, text string) string {
	key := fmt.Sprintf("%d:%s", kind, text)
	if i, ok := m.constIdx[key]; ok {
		return fmt.Sprintf("K%d", i)
	}
	i := len(m.Consts)
	m.Consts = append(m.Consts, Const{Kind: kind, Text: text})
	m.constIdx[key] = i
	return fmt.Sprintf("K%d", i)
}

// LowerError is a lowering failure anchored to a source position.
type LowerError struct {
	Line, Col int
	Msg       string
}

func (e *LowerError) Error() string {
	if e.Line > 0 {
		return fmt.Sprintf("%d:%d: %s", e.Line, e.Col, e.Msg)
	}
	return e.Msg
}

// Lower lowers a checked file to an IR module. Every language construct
// must lower; an unsupported node is a hard error, never a skip.
func Lower(file *ast.File, sourceName string) (*Module, error) {
	lo := &lowerer{
		mod:  newModule(sourceName),
		file: file,
	}
	lo.collect()
	if err := lo.lowerModule(); err != nil {
		return nil, err
	}
	return lo.mod, nil
}
