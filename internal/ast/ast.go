// Package ast defines the syntax tree for Voilà programs.
package ast

import (
	"voila/internal/diag"
	"voila/internal/lexer"
)

type Node interface{ Pos() diag.Pos }

// ---------------------------------------------------------------- file

type File struct {
	Package     string
	Uses        []*Use
	Decls       []Decl // top-level declarations in source order
	ScriptStmts []Stmt // loose top-level statements (script form, §1)
	P           diag.Pos
}

func (f *File) Pos() diag.Pos { return f.P }

type Use struct {
	Path     string   // "std/fmt"
	Alias    string   // `as t3270`
	Names    []string // selective import { upper, words }
	Wildcard bool     // `use "std/math" *`
	P        diag.Pos
}

func (u *Use) Pos() diag.Pos { return u.P }

// ---------------------------------------------------------------- decls

type Decl interface {
	Node
	declNode()
}

type ParamMode int

const (
	ModeBorrow ParamMode = iota // default
	ModeMut
	ModeOwn
)

type Param struct {
	Mode     ParamMode
	Name     string
	Type     TypeExpr // nil for untyped closure params
	Default  Expr     // default argument value
	Variadic bool     // ...T
	P        diag.Pos
}

func (p *Param) Pos() diag.Pos { return p.P }

type GenericParam struct {
	Name  string
	Bound string // trait name, "" if unconstrained
}

type FuncDecl struct {
	Name     string
	Generics []GenericParam
	Params   []*Param
	SelfMode ParamMode // for methods; SelfKind tells whether self is present
	HasSelf  bool
	Results  []TypeExpr // 0, 1 or more (tuple return)
	Throws   []TypeExpr // checked exception list; nil = unchecked
	NoThrow  bool
	Body     *Block // nil for trait method signatures
	Default  Expr   // trait default impl `= nil`
	Doc      string
	P        diag.Pos
}

func (d *FuncDecl) Pos() diag.Pos { return d.P }
func (d *FuncDecl) declNode()     {}

type Field struct {
	Name    string
	Type    TypeExpr
	Default Expr
	P       diag.Pos
}

type StructDecl struct {
	Name     string
	Generics []GenericParam
	Fields   []*Field
	P        diag.Pos
}

func (d *StructDecl) Pos() diag.Pos { return d.P }
func (d *StructDecl) declNode()     {}

type Variant struct {
	Name   string
	Fields []*Field // payload params, possibly named
	P      diag.Pos
}

type EnumDecl struct {
	Name     string
	Generics []GenericParam
	Variants []*Variant
	P        diag.Pos
}

func (d *EnumDecl) Pos() diag.Pos { return d.P }
func (d *EnumDecl) declNode()     {}

type TraitDecl struct {
	Name    string
	Methods []*FuncDecl
	P       diag.Pos
}

func (d *TraitDecl) Pos() diag.Pos { return d.P }
func (d *TraitDecl) declNode()     {}

type ImplDecl struct {
	Generics []GenericParam
	Trait    string // "" for inherent impl
	Type     TypeExpr
	Methods  []*FuncDecl
	P        diag.Pos
}

func (d *ImplDecl) Pos() diag.Pos { return d.P }
func (d *ImplDecl) declNode()     {}

type ExceptionDecl struct {
	Name    string
	Fields  []*Field
	Methods []*FuncDecl
	P       diag.Pos
}

func (d *ExceptionDecl) Pos() diag.Pos { return d.P }
func (d *ExceptionDecl) declNode()     {}

type TypeDecl struct {
	Name   string
	Alias  bool     // type UserID = int
	Target TypeExpr // alias target or named form
	Struct *StructDecl
	P      diag.Pos
}

func (d *TypeDecl) Pos() diag.Pos { return d.P }
func (d *TypeDecl) declNode()     {}

// GlobalDecl is a top-level let/var/const.
type GlobalDecl struct {
	Stmt Stmt // *LetStmt or *VarStmt
	P    diag.Pos
}

func (d *GlobalDecl) Pos() diag.Pos { return d.P }
func (d *GlobalDecl) declNode()     {}

// ---------------------------------------------------------------- types

type TypeExpr interface {
	Node
	typeNode()
}

type NamedType struct {
	Pkg  string // package qualifier, e.g. str.Builder
	Name string
	Args []TypeExpr // generic arguments Stack[int]
	P    diag.Pos
}

func (t *NamedType) Pos() diag.Pos { return t.P }
func (t *NamedType) typeNode()     {}

type OptionType struct { // ?T
	Elem TypeExpr
	P    diag.Pos
}

func (t *OptionType) Pos() diag.Pos { return t.P }
func (t *OptionType) typeNode()     {}

type ResultType struct { // T!
	Elem TypeExpr
	P    diag.Pos
}

func (t *ResultType) Pos() diag.Pos { return t.P }
func (t *ResultType) typeNode()     {}

type SliceType struct { // []T or [n]T
	Elem TypeExpr
	Len  Expr // nil for slice
	P    diag.Pos
}

func (t *SliceType) Pos() diag.Pos { return t.P }
func (t *SliceType) typeNode()     {}

type MapType struct {
	Key, Value TypeExpr
	P          diag.Pos
}

func (t *MapType) Pos() diag.Pos { return t.P }
func (t *MapType) typeNode()     {}

type SetType struct {
	Elem TypeExpr
	P    diag.Pos
}

func (t *SetType) Pos() diag.Pos { return t.P }
func (t *SetType) typeNode()     {}

type ChanType struct {
	Elem TypeExpr
	P    diag.Pos
}

func (t *ChanType) Pos() diag.Pos { return t.P }
func (t *ChanType) typeNode()     {}

// BoxKind distinguishes shared[T], cell[T], weak[T].
type BoxKind int

const (
	SharedBox BoxKind = iota
	CellBox
	WeakBox
)

type BoxType struct {
	Kind BoxKind
	Elem TypeExpr
	P    diag.Pos
}

func (t *BoxType) Pos() diag.Pos { return t.P }
func (t *BoxType) typeNode()     {}

type FuncType struct {
	Params []TypeExpr
	Result TypeExpr // nil = unit
	P      diag.Pos
}

func (t *FuncType) Pos() diag.Pos { return t.P }
func (t *FuncType) typeNode()     {}

type TupleType struct {
	Elems []TypeExpr // empty = unit ()
	P     diag.Pos
}

func (t *TupleType) Pos() diag.Pos { return t.P }
func (t *TupleType) typeNode()     {}

// BorrowType is `&T` in type position — always illegal in field position
// (§6.2); parsed so the checker can produce the dedicated error.
type BorrowType struct {
	Elem TypeExpr
	P    diag.Pos
}

func (t *BorrowType) Pos() diag.Pos { return t.P }
func (t *BorrowType) typeNode()     {}

// ---------------------------------------------------------------- statements

type Stmt interface {
	Node
	stmtNode()
}

type Block struct {
	Stmts []Stmt
	P     diag.Pos
}

func (s *Block) Pos() diag.Pos { return s.P }
func (s *Block) stmtNode()     {}

type LetStmt struct {
	Names []string // >1 = tuple destructure; "_" allowed
	Type  TypeExpr
	Value Expr
	Const bool
	P     diag.Pos
}

func (s *LetStmt) Pos() diag.Pos { return s.P }
func (s *LetStmt) stmtNode()     {}

type VarStmt struct {
	Name  string
	Type  TypeExpr
	Value Expr // nil = zero value
	P     diag.Pos
}

func (s *VarStmt) Pos() diag.Pos { return s.P }
func (s *VarStmt) stmtNode()     {}

type AssignStmt struct {
	LHS []Expr // multiple only for plain `=`
	Op  lexer.Kind
	RHS Expr
	P   diag.Pos
}

func (s *AssignStmt) Pos() diag.Pos { return s.P }
func (s *AssignStmt) stmtNode()     {}

type ExprStmt struct {
	X Expr
	P diag.Pos
}

func (s *ExprStmt) Pos() diag.Pos { return s.P }
func (s *ExprStmt) stmtNode()     {}

type IfStmt struct {
	Cond    Expr    // nil when IfLet form
	Pattern Pattern // if let PATTERN = Value
	Value   Expr
	Then    *Block
	Else    Stmt // *Block or *IfStmt, nil
	P       diag.Pos
}

func (s *IfStmt) Pos() diag.Pos { return s.P }
func (s *IfStmt) stmtNode()     {}

type ForStmt struct {
	Label string
	Var   string // "" for infinite loop
	Iter  Expr
	Body  *Block
	P     diag.Pos
}

func (s *ForStmt) Pos() diag.Pos { return s.P }
func (s *ForStmt) stmtNode()     {}

type EachStmt struct {
	Label string
	A, B  string // one or two loop vars; B=="" for value-only
	Iter  Expr
	Body  *Block
	P     diag.Pos
}

func (s *EachStmt) Pos() diag.Pos { return s.P }
func (s *EachStmt) stmtNode()     {}

type WhileStmt struct {
	Label string
	Cond  Expr
	Body  *Block
	P     diag.Pos
}

func (s *WhileStmt) Pos() diag.Pos { return s.P }
func (s *WhileStmt) stmtNode()     {}

type SwitchCase struct {
	Values []Expr // nil = default
	Body   []Stmt
	P      diag.Pos
}

type SwitchStmt struct {
	Subject Expr
	Cases   []*SwitchCase
	P       diag.Pos
}

func (s *SwitchStmt) Pos() diag.Pos { return s.P }
func (s *SwitchStmt) stmtNode()     {}

type MatchArm struct {
	Pattern Pattern
	Guard   Expr
	Body    []Stmt
	P       diag.Pos
}

type MatchStmt struct {
	Subject Expr
	Arms    []*MatchArm
	P       diag.Pos
}

func (s *MatchStmt) Pos() diag.Pos { return s.P }
func (s *MatchStmt) stmtNode()     {}

type SelectCaseKind int

const (
	SelectRecv SelectCaseKind = iota
	SelectSend
	SelectDefault
)

type SelectCase struct {
	Kind   SelectCaseKind
	Ch     Expr   // channel expr
	Send   Expr   // value for send cases
	Bind   string // x := <-ch  /  let x = <-ch
	BindOk string // x, ok := <-ch
	Body   []Stmt
	P      diag.Pos
}

type SelectStmt struct {
	Cases []*SelectCase
	P     diag.Pos
}

func (s *SelectStmt) Pos() diag.Pos { return s.P }
func (s *SelectStmt) stmtNode()     {}

type GroupStmt struct {
	Try     bool
	Timeout Expr // nil if none
	Body    *Block
	P       diag.Pos
}

func (s *GroupStmt) Pos() diag.Pos { return s.P }
func (s *GroupStmt) stmtNode()     {}

type ParseTermKind int

const (
	ParseVar     ParseTermKind = iota // capture into ident
	ParseDiscard                      // `.`
	ParseLit                          // literal separator ","
	ParseCol                          // absolute column position
)

type ParseTerm struct {
	Kind ParseTermKind
	Name string
	Lit  string
	Col  int
	P    diag.Pos
}

type ParseStmt struct {
	CaseFold string // "", "upper", "lower"
	Src      Expr
	Terms    []ParseTerm
	P        diag.Pos
}

func (s *ParseStmt) Pos() diag.Pos { return s.P }
func (s *ParseStmt) stmtNode()     {}

type SayStmt struct {
	Args []Expr
	P    diag.Pos
}

func (s *SayStmt) Pos() diag.Pos { return s.P }
func (s *SayStmt) stmtNode()     {}

type DeferStmt struct {
	Call  Expr   // call form
	Block *Block // block form
	P     diag.Pos
}

func (s *DeferStmt) Pos() diag.Pos { return s.P }
func (s *DeferStmt) stmtNode()     {}

type DropStmt struct {
	Name string
	P    diag.Pos
}

func (s *DropStmt) Pos() diag.Pos { return s.P }
func (s *DropStmt) stmtNode()     {}

type ReturnStmt struct {
	Values []Expr
	P      diag.Pos
}

func (s *ReturnStmt) Pos() diag.Pos { return s.P }
func (s *ReturnStmt) stmtNode()     {}

type BreakStmt struct {
	Label string
	P     diag.Pos
}

func (s *BreakStmt) Pos() diag.Pos { return s.P }
func (s *BreakStmt) stmtNode()     {}

type ContinueStmt struct {
	Label string
	P     diag.Pos
}

func (s *ContinueStmt) Pos() diag.Pos { return s.P }
func (s *ContinueStmt) stmtNode()     {}

type FallthroughStmt struct {
	P diag.Pos
}

func (s *FallthroughStmt) Pos() diag.Pos { return s.P }
func (s *FallthroughStmt) stmtNode()     {}

type NumericStmt struct {
	Digits int
	P      diag.Pos
}

func (s *NumericStmt) Pos() diag.Pos { return s.P }
func (s *NumericStmt) stmtNode()     {}

type ThrowStmt struct {
	Value Expr // nil = rethrow
	From  Expr // `throw X from e`
	P     diag.Pos
}

func (s *ThrowStmt) Pos() diag.Pos { return s.P }
func (s *ThrowStmt) stmtNode()     {}

type CatchClause struct {
	Name  string     // bound exception variable, "" if none
	Types []TypeExpr // nil = catch-all
	Body  *Block
	P     diag.Pos
}

type TryCatchStmt struct {
	Body    *Block
	Catches []*CatchClause
	Finally *Block
	P       diag.Pos
}

func (s *TryCatchStmt) Pos() diag.Pos { return s.P }
func (s *TryCatchStmt) stmtNode()     {}

// ---------------------------------------------------------------- patterns

type Pattern interface {
	Node
	patNode()
}

type WildcardPat struct{ P diag.Pos }

func (p *WildcardPat) Pos() diag.Pos { return p.P }
func (p *WildcardPat) patNode()      {}

type NilPat struct{ P diag.Pos }

func (p *NilPat) Pos() diag.Pos { return p.P }
func (p *NilPat) patNode()      {}

type LiteralPat struct {
	Value Expr // literal or negated literal
	P     diag.Pos
}

func (p *LiteralPat) Pos() diag.Pos { return p.P }
func (p *LiteralPat) patNode()      {}

type BindPat struct {
	Name string
	P    diag.Pos
}

func (p *BindPat) Pos() diag.Pos { return p.P }
func (p *BindPat) patNode()      {}

type VariantPat struct {
	Name  string // Circle, Some, Ok, Err…
	Elems []Pattern
	P     diag.Pos
}

func (p *VariantPat) Pos() diag.Pos { return p.P }
func (p *VariantPat) patNode()      {}

// ---------------------------------------------------------------- expressions

type Expr interface {
	Node
	exprNode()
}

type Ident struct {
	Name string
	P    diag.Pos
}

func (e *Ident) Pos() diag.Pos { return e.P }
func (e *Ident) exprNode()     {}

type SelfExpr struct{ P diag.Pos }

func (e *SelfExpr) Pos() diag.Pos { return e.P }
func (e *SelfExpr) exprNode()     {}

type LitKind int

const (
	IntLit LitKind = iota
	FloatLit
	DecLit
	StrLit
	RuneLit
	BoolLit
	NilLit
)

type BasicLit struct {
	Kind LitKind
	Text string // normalized literal text ("42", "3.14", "hello")
	P    diag.Pos
}

func (e *BasicLit) Pos() diag.Pos { return e.P }
func (e *BasicLit) exprNode()     {}

type InterpPart struct {
	Lit  string
	Expr Expr // non-nil for expression parts
}

type InterpStr struct {
	Parts []InterpPart
	P     diag.Pos
}

func (e *InterpStr) Pos() diag.Pos { return e.P }
func (e *InterpStr) exprNode()     {}

type UnaryExpr struct {
	Op lexer.Kind // MINUS, NOT, TILDE, ARROW (channel receive)
	X  Expr
	P  diag.Pos
}

func (e *UnaryExpr) Pos() diag.Pos { return e.P }
func (e *UnaryExpr) exprNode()     {}

type BinaryExpr struct {
	Op   lexer.Kind
	X, Y Expr
	P    diag.Pos
}

func (e *BinaryExpr) Pos() diag.Pos { return e.P }
func (e *BinaryExpr) exprNode()     {}

type RangeExpr struct {
	Lo, Hi    Expr
	Inclusive bool
	By        Expr // step, nil if none
	P         diag.Pos
}

func (e *RangeExpr) Pos() diag.Pos { return e.P }
func (e *RangeExpr) exprNode()     {}

type Arg struct {
	Name   string // named argument
	Value  Expr
	Spread bool // v...
}

type CallExpr struct {
	Fun      Expr
	TypeArgs []TypeExpr // json.decode[T](s), make([]T, n) sugar
	Args     []Arg
	P        diag.Pos
}

func (e *CallExpr) Pos() diag.Pos { return e.P }
func (e *CallExpr) exprNode()     {}

type IndexExpr struct {
	X     Expr
	Index Expr // may be *RangeExpr for slicing
	P     diag.Pos
}

func (e *IndexExpr) Pos() diag.Pos { return e.P }
func (e *IndexExpr) exprNode()     {}

type FieldExpr struct {
	X    Expr
	Name string
	P    diag.Pos
}

func (e *FieldExpr) Pos() diag.Pos { return e.P }
func (e *FieldExpr) exprNode()     {}

type CompositeField struct {
	Name  string // "" for positional
	Value Expr
}

type CompositeLit struct {
	Type   *NamedType
	Fields []CompositeField
	P      diag.Pos
}

func (e *CompositeLit) Pos() diag.Pos { return e.P }
func (e *CompositeLit) exprNode()     {}

type SliceLit struct {
	Elems []Expr
	P     diag.Pos
}

func (e *SliceLit) Pos() diag.Pos { return e.P }
func (e *SliceLit) exprNode()     {}

type TupleExpr struct {
	Elems []Expr
	P     diag.Pos
}

func (e *TupleExpr) Pos() diag.Pos { return e.P }
func (e *TupleExpr) exprNode()     {}

type ClosureExpr struct {
	Params   []*Param
	Result   TypeExpr
	Captures []string // fn own [data] () { … }
	OwnCap   bool
	Body     *Block
	P        diag.Pos
}

func (e *ClosureExpr) Pos() diag.Pos { return e.P }
func (e *ClosureExpr) exprNode()     {}

// TryExpr / MustExpr / AttemptExpr are the §8 prefix operators.
type TryExpr struct {
	X Expr
	P diag.Pos
}

func (e *TryExpr) Pos() diag.Pos { return e.P }
func (e *TryExpr) exprNode()     {}

type MustExpr struct {
	X Expr
	P diag.Pos
}

func (e *MustExpr) Pos() diag.Pos { return e.P }
func (e *MustExpr) exprNode()     {}

type AttemptExpr struct {
	X Expr
	P diag.Pos
}

func (e *AttemptExpr) Pos() diag.Pos { return e.P }
func (e *AttemptExpr) exprNode()     {}

// ElseExpr is `expr else alt` — alt is an expression or a diverging statement
// (return/throw/break/continue), §8.1.
type ElseExpr struct {
	X       Expr
	Alt     Expr // nil if AltStmt set
	AltStmt Stmt
	P       diag.Pos
}

func (e *ElseExpr) Pos() diag.Pos { return e.P }
func (e *ElseExpr) exprNode()     {}

type SpawnExpr struct {
	Call  Expr   // spawn f(x)
	Block *Block // spawn { … }
	P     diag.Pos
}

func (e *SpawnExpr) Pos() diag.Pos { return e.P }
func (e *SpawnExpr) exprNode()     {}
