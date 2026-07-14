package ir

import (
	"fmt"
	"strings"

	"voila/internal/diag"
)

// Listing renders a module as an HLASM-style assembly listing. Every output
// line is at most maxCols columns, counted in RUNES (the 3270 rule).
const maxCols = 79

// Column layout (0-based):
//
//	0        8        16       25
//	LOC      label    opcode   operands            * line| source
const (
	colLabel   = 8
	colOpcode  = 17
	colOperand = 26
	colComment = 48
)

// Listing renders the module. src provides source-line echo text (nil OK).
func Listing(m *Module, src *diag.Source) string {
	var b strings.Builder
	w := func(line string) {
		b.WriteString(clipLine(line))
		b.WriteByte('\n')
	}

	// ---- header
	w("*" + strings.Repeat("-", maxCols-1))
	w("*        VOILA VM ASSEMBLY LISTING")
	w("*        SOURCE: " + m.Source)
	w("*")
	w("*        REGISTER-BASED LINEAR IR (SPEC #12). THIS IS THE FORM")
	w("*        THE C BACKEND CONSUMES; IT IS NOT EXECUTED DIRECTLY.")
	w("*        ENTRY ORDER: $INIT, $SCRIPT, MAIN. ^NAME DENOTES AN")
	w("*        UPVALUE OF THE ENCLOSING FRAME: A CLOSURE CAPTURES THAT")
	w("*        FRAME BY REFERENCE, NOT ITS VALUES BY COPY.")
	w("*" + strings.Repeat("-", maxCols-1))

	// ---- constant pool
	if len(m.Consts) > 0 {
		w("*")
		w("*        CONSTANT POOL")
		for i, c := range m.Consts {
			name := fmt.Sprintf("K%d", i)
			text := c.Text
			if c.Kind == CStr || c.Kind == CRune {
				text = "'" + escapeConst(text) + "'"
			}
			line := pad(name, colLabel) + pad("DC", colOpcode-colLabel) + pad(c.Kind.String(), 6) + " " + text
			writeWrapped(&b, line, colOperand)
		}
	}

	// ---- type commentary (DSECT-flavored)
	for _, t := range m.Types {
		w("*")
		w("*        " + t.Kind + " " + t.Name)
		w(pad(dsectName(t.Name), colLabel) + "DSECT")
		for _, l := range t.Lines {
			w(strings.Repeat(" ", colLabel) + pad("DS", colOpcode-colLabel) + l)
		}
	}

	// ---- functions
	for _, fn := range m.Funcs {
		w("*")
		head := pad(fn.Name, colLabel)
		if len(fn.Name) >= colLabel {
			// Long names get their own line; CSECT goes on the next.
			w("*        " + fn.Name)
			head = pad("", colLabel)
		}
		csect := head + pad("CSECT", colOpcode-colLabel)
		note := fmt.Sprintf("* %s  regs=%d", fn.Sig, fn.NRegs)
		w(fitComment(csect, note))

		lastSrc := 0
		loc := 0
		for _, in := range fn.Instrs {
			if in.Op == "*" { // pure comment line
				w("*        " + strings.Join(in.Args, " "))
				continue
			}
			label := ""
			if len(in.Labels) > 0 {
				label = strings.Join(in.Labels, ",")
			}
			line := fmt.Sprintf("%06X  ", loc)
			line += pad(label, colOpcode-colLabel)
			line += pad(in.Op, colOperand-colOpcode)
			line += strings.Join(in.Args, ",")

			// Source echo when the line number changes.
			if in.Src > 0 && in.Src != lastSrc && src != nil {
				echo := fmt.Sprintf("* %d| %s", in.Src, strings.TrimSpace(src.Line(in.Src)))
				line = fitComment(line, echo)
				lastSrc = in.Src
			}
			writeWrapped(&b, line, colOperand)
			loc += 4
		}
	}

	w("*")
	w(strings.Repeat(" ", colLabel) + "END")
	return b.String()
}

func dsectName(name string) string {
	if len(name) >= colLabel {
		return name[:colLabel-1]
	}
	return name
}

func pad(s string, width int) string {
	if runeLen(s) >= width {
		return s + " "
	}
	return s + strings.Repeat(" ", width-runeLen(s))
}

func runeLen(s string) int { return len([]rune(s)) }

// fitComment appends a comment at colComment when it fits within maxCols;
// otherwise the comment is truncated with an ellipsis (source echo is
// decoration — operands are never truncated).
func fitComment(line, comment string) string {
	if runeLen(line) >= colComment {
		line += "  "
	} else {
		line += strings.Repeat(" ", colComment-runeLen(line))
	}
	room := maxCols - runeLen(line)
	if room < 4 {
		return strings.TrimRight(line, " ")
	}
	if runeLen(comment) > room {
		comment = string([]rune(comment)[:room-1]) + "…"
	}
	return strings.TrimRight(line+comment, " ")
}

// clipLine hard-truncates pathological lines (comments only) at maxCols.
func clipLine(s string) string {
	r := []rune(s)
	if len(r) <= maxCols {
		return s
	}
	return string(r[:maxCols-1]) + "…"
}

// writeWrapped emits a line, splitting overlong operand text at commas onto
// continuation lines: the continued line ends with `+` and the continuation
// resumes at the operand column (frozen listing decision).
func writeWrapped(b *strings.Builder, line string, contCol int) {
	for runeLen(line) > maxCols {
		runes := []rune(line)
		// Find the last comma or blank that lets the `+` fit.
		cut := -1
		for i := maxCols - 2; i > contCol; i-- {
			if runes[i] == ',' || runes[i] == ' ' {
				cut = i + 1
				break
			}
		}
		if cut <= contCol {
			// No safe split point: hard cut before the budget.
			cut = maxCols - 2
		}
		b.WriteString(string(runes[:cut]) + "+\n")
		line = strings.Repeat(" ", contCol) + strings.TrimLeft(string(runes[cut:]), " ")
	}
	b.WriteString(line)
	b.WriteByte('\n')
}

// escapeConst renders string/rune constant text with Go-style escapes,
// keeping the pool single-line.
func escapeConst(s string) string {
	var sb strings.Builder
	for _, r := range s {
		switch r {
		case '\n':
			sb.WriteString(`\n`)
		case '\t':
			sb.WriteString(`\t`)
		case '\r':
			sb.WriteString(`\r`)
		case '\\':
			sb.WriteString(`\\`)
		case '\'':
			sb.WriteString(`\'`)
		case 0:
			sb.WriteString(`\0`)
		default:
			sb.WriteRune(r)
		}
	}
	return sb.String()
}
