// Package diag provides source positions and rustc-style caret diagnostics
// shared by the lexer, parser, checker and runtime.
package diag

import (
	"fmt"
	"sort"
	"strings"
)

// Pos is a position in a source file. Line and Col are 1-based; Col counts
// bytes. File names the source; it is empty for positions the compiler
// synthesises, which render without a caret.
type Pos struct {
	File string
	Line int
	Col  int
}

func (p Pos) IsValid() bool { return p.Line > 0 }

func (p Pos) String() string { return fmt.Sprintf("%d:%d", p.Line, p.Col) }

// Span is a half-open range within one file.
type Span struct {
	Start Pos
	End   Pos // exclusive; End.Col == Start.Col+width for single-line spans
}

func SpanOf(start Pos, width int) Span {
	return Span{Start: start,
		End: Pos{File: start.File, Line: start.Line, Col: start.Col + width}}
}

// Severity of a diagnostic.
type Severity int

const (
	Error Severity = iota
	Warning
)

func (s Severity) String() string {
	if s == Warning {
		return "warning"
	}
	return "error"
}

// Diagnostic is a single message anchored to a source span, with optional
// secondary label, notes and help lines, rendered in the style shown in the
// specification (§14.3).
type Diagnostic struct {
	Severity Severity
	Message  string
	Span     Span
	Label    string   // text under the carets
	Notes    []string // "= note: ..." lines
	Help     []string // "= help: ..." lines
}

// Source holds a file's text and answers line lookups for rendering.
type Source struct {
	Name  string
	Text  string
	lines []int // byte offset of the start of each line, lazily built
}

func NewSource(name, text string) *Source {
	return &Source{Name: name, Text: text}
}

func (s *Source) buildLines() {
	if s.lines != nil {
		return
	}
	s.lines = append(s.lines, 0)
	for i := 0; i < len(s.Text); i++ {
		if s.Text[i] == '\n' {
			s.lines = append(s.lines, i+1)
		}
	}
}

// Line returns the text of the 1-based line n, without its newline.
func (s *Source) Line(n int) string {
	s.buildLines()
	if n < 1 || n > len(s.lines) {
		return ""
	}
	start := s.lines[n-1]
	end := len(s.Text)
	if n < len(s.lines) {
		end = s.lines[n] - 1
	}
	line := s.Text[start:end]
	return strings.TrimSuffix(line, "\r")
}

// Bag collects diagnostics across every source in the program: a build spans
// many files once packages are involved, and a diagnostic must be rendered
// against the file it came from.
type Bag struct {
	Src     *Source // the entry source, kept for single-file callers
	sources map[string]*Source
	Diags   []Diagnostic
}

func NewBag(src *Source) *Bag {
	b := &Bag{Src: src, sources: map[string]*Source{}}
	if src != nil {
		b.sources[src.Name] = src
	}
	return b
}

// AddSource registers another file so its diagnostics can be rendered.
func (b *Bag) AddSource(src *Source) {
	if b.sources == nil {
		b.sources = map[string]*Source{}
	}
	b.sources[src.Name] = src
	if b.Src == nil {
		b.Src = src
	}
}

// sourceFor resolves the file a position belongs to.
func (b *Bag) sourceFor(pos Pos) *Source {
	if s, ok := b.sources[pos.File]; ok {
		return s
	}
	return b.Src
}

func (b *Bag) Add(d Diagnostic) { b.Diags = append(b.Diags, d) }

func (b *Bag) Errorf(span Span, label string, format string, args ...any) {
	b.Add(Diagnostic{Severity: Error, Message: fmt.Sprintf(format, args...), Span: span, Label: label})
}

func (b *Bag) HasErrors() bool {
	for _, d := range b.Diags {
		if d.Severity == Error {
			return true
		}
	}
	return false
}

// Render formats all diagnostics, sorted by position, in the spec's format:
//
//	error: type `Node` forms an ownership cycle through field `parent`
//	  --> tree.voi:4:5
//	   |
//	 4 |     parent shared[Node]
//	   |            ^^^^^^^^^^^^ owning edge back to `Node`
//	   = note: owning cycles can leak; Voilà forbids them
//	   = help: use `weak[Node]` for a back-edge
func (b *Bag) Render() string {
	diags := make([]Diagnostic, len(b.Diags))
	copy(diags, b.Diags)
	sort.SliceStable(diags, func(i, j int) bool {
		a, c := diags[i].Span.Start, diags[j].Span.Start
		if a.File != c.File {
			return a.File < c.File
		}
		if a.Line != c.Line {
			return a.Line < c.Line
		}
		return a.Col < c.Col
	})
	var sb strings.Builder
	for i, d := range diags {
		if i > 0 {
			sb.WriteString("\n")
		}
		sb.WriteString(RenderOne(b.sourceFor(d.Span.Start), d))
	}
	return sb.String()
}

// RenderOne renders a single diagnostic against a source.
func RenderOne(src *Source, d Diagnostic) string {
	var sb strings.Builder
	fmt.Fprintf(&sb, "%s: %s\n", d.Severity, d.Message)
	if !d.Span.Start.IsValid() {
		return sb.String()
	}
	lineNo := d.Span.Start.Line
	lineText := src.Line(lineNo)
	num := fmt.Sprintf("%d", lineNo)
	gutter := strings.Repeat(" ", len(num)+1)
	fmt.Fprintf(&sb, "%s--> %s:%d:%d\n", gutter, src.Name, lineNo, d.Span.Start.Col)
	fmt.Fprintf(&sb, "%s|\n", gutter+" ")
	fmt.Fprintf(&sb, " %s | %s\n", num, lineText)
	width := 1
	if d.Span.End.Line == d.Span.Start.Line && d.Span.End.Col > d.Span.Start.Col {
		width = d.Span.End.Col - d.Span.Start.Col
	}
	carets := strings.Repeat("^", width)
	pad := strings.Repeat(" ", d.Span.Start.Col-1)
	if d.Label != "" {
		fmt.Fprintf(&sb, "%s| %s%s %s\n", gutter+" ", pad, carets, d.Label)
	} else {
		fmt.Fprintf(&sb, "%s| %s%s\n", gutter+" ", pad, carets)
	}
	for _, n := range d.Notes {
		fmt.Fprintf(&sb, "%s= note: %s\n", gutter+" ", n)
	}
	for _, h := range d.Help {
		fmt.Fprintf(&sb, "%s= help: %s\n", gutter+" ", h)
	}
	return sb.String()
}
