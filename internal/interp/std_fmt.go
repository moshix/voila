package interp

import (
	"encoding/hex"
	"fmt"
	"io"
	"strconv"
	"strings"

	"voila/internal/diag"
)

// fmtPrintf implements fmt.printf(fmt, args…).
func (t *T) fmtPrintf(w io.Writer, args []Value) Value {
	if len(args) == 0 {
		t.rtErr(diag.Pos{}, "printf needs a format string")
	}
	f, ok := args[0].(StrV)
	if !ok {
		t.rtErr(diag.Pos{}, "printf format must be str, got %s", typeName(args[0]))
	}
	fmt.Fprint(w, t.sprintfVerbs(string(f), args[1:], diag.Pos{}))
	return UnitV{}
}

func (t *T) fmtSprintfV(args []Value) Value {
	if len(args) == 0 {
		t.rtErr(diag.Pos{}, "sprintf needs a format string")
	}
	f, ok := args[0].(StrV)
	if !ok {
		t.rtErr(diag.Pos{}, "sprintf format must be str")
	}
	return StrV(t.sprintfVerbs(string(f), args[1:], diag.Pos{}))
}

// sprintfVerbs is the §13.1 verb engine. Verbs: d i u f e g s q x X o b c t
// v %%; flags - + 0 space #; width and precision, either numeric or *.
func (t *T) sprintfVerbs(format string, args []Value, pos diag.Pos) string {
	var sb strings.Builder
	ai := 0
	next := func() Value {
		if ai >= len(args) {
			t.throwf("FormatError", "printf: not enough arguments for format %q", format)
		}
		v := args[ai]
		ai++
		return v
	}
	i := 0
	for i < len(format) {
		c := format[i]
		if c != '%' {
			sb.WriteByte(c)
			i++
			continue
		}
		i++
		if i >= len(format) {
			t.throwf("FormatError", "printf: trailing %% in format")
		}
		if format[i] == '%' {
			sb.WriteByte('%')
			i++
			continue
		}
		// Flags.
		flags := ""
		for i < len(format) && strings.IndexByte("-+0 #", format[i]) >= 0 {
			flags += string(format[i])
			i++
		}
		plusV := false
		// Width.
		width := -1
		if i < len(format) && format[i] == '*' {
			width = int(t.toInt(next(), pos))
			i++
		} else {
			w := 0
			hasW := false
			for i < len(format) && format[i] >= '0' && format[i] <= '9' {
				w = w*10 + int(format[i]-'0')
				hasW = true
				i++
			}
			if hasW {
				width = w
			}
		}
		// Precision.
		prec := -1
		if i < len(format) && format[i] == '.' {
			i++
			if i < len(format) && format[i] == '*' {
				prec = int(t.toInt(next(), pos))
				i++
			} else {
				p := 0
				for i < len(format) && format[i] >= '0' && format[i] <= '9' {
					p = p*10 + int(format[i]-'0')
					i++
				}
				prec = p
			}
		}
		if i >= len(format) {
			t.throwf("FormatError", "printf: missing verb at end of format")
		}
		if format[i] == '+' { // %+v
			plusV = true
			i++
		}
		verb := format[i]
		i++
		sb.WriteString(t.formatVerb(verb, flags, width, prec, plusV, next, pos))
	}
	if ai < len(args) {
		t.throwf("FormatError", "printf: %d extra argument(s) for format %q", len(args)-ai, format)
	}
	return sb.String()
}

func (t *T) formatVerb(verb byte, flags string, width, prec int, plusV bool, next func() Value, pos diag.Pos) string {
	goSpec := func(verbStr string) string {
		s := "%" + flags
		if width >= 0 {
			s += strconv.Itoa(width)
		}
		if prec >= 0 {
			s += "." + strconv.Itoa(prec)
		}
		return s + verbStr
	}
	pad := func(s string) string {
		if width < 0 || len(s) >= width {
			return s
		}
		fill := strings.Repeat(" ", width-len(s))
		if strings.Contains(flags, "-") {
			return s + fill
		}
		if strings.Contains(flags, "0") && len(s) > 0 && (s[0] == '-' || (s[0] >= '0' && s[0] <= '9')) {
			z := strings.Repeat("0", width-len(s))
			if s[0] == '-' {
				return "-" + z + s[1:]
			}
			return z + s
		}
		return fill + s
	}

	v := next()
	switch verb {
	case 'd', 'i', 'u':
		switch x := v.(type) {
		case IntV:
			return fmt.Sprintf(goSpec("d"), int64(x))
		case RuneV:
			return fmt.Sprintf(goSpec("d"), int64(x))
		case DecV:
			if n, ok := x.D.Int64(); ok {
				return fmt.Sprintf(goSpec("d"), n)
			}
			t.throwf("FormatError", "%%d on dec %s with a fraction", x.D)
		}
		t.throwf("FormatError", "%%d applied to %s", typeName(v))
	case 'f', 'F':
		p := prec
		if p < 0 {
			p = 6
		}
		switch x := v.(type) {
		case FloatV:
			return fmt.Sprintf(goSpec("f"), float64(x))
		case IntV:
			return fmt.Sprintf(goSpec("f"), float64(x))
		case DecV:
			return pad(x.D.FormatFixed(p)) // exact, half-even (§13.1)
		}
		t.throwf("FormatError", "%%f applied to %s", typeName(v))
	case 'e', 'E', 'g', 'G':
		switch x := v.(type) {
		case FloatV:
			return fmt.Sprintf(goSpec(string(verb)), float64(x))
		case IntV:
			return fmt.Sprintf(goSpec(string(verb)), float64(x))
		case DecV:
			return fmt.Sprintf(goSpec(string(verb)), x.D.Float64())
		}
		t.throwf("FormatError", "%%%c applied to %s", verb, typeName(v))
	case 's':
		return fmt.Sprintf(goSpec("s"), t.Str(v))
	case 'q':
		return fmt.Sprintf(goSpec("q"), t.Str(v))
	case 'x', 'X':
		switch x := v.(type) {
		case IntV:
			return fmt.Sprintf(goSpec(string(verb)), int64(x))
		case StrV:
			h := hex.EncodeToString([]byte(string(x)))
			if verb == 'X' {
				h = strings.ToUpper(h)
			}
			return pad(h)
		case *Slice:
			var bytes []byte
			for _, e := range x.Elems {
				bytes = append(bytes, byte(t.toInt(e, pos)))
			}
			h := hex.EncodeToString(bytes)
			if verb == 'X' {
				h = strings.ToUpper(h)
			}
			return pad(h)
		}
		t.throwf("FormatError", "%%%c applied to %s", verb, typeName(v))
	case 'o':
		return fmt.Sprintf(goSpec("o"), t.toInt(v, pos))
	case 'b':
		return fmt.Sprintf(goSpec("b"), t.toInt(v, pos))
	case 'c':
		switch x := v.(type) {
		case RuneV:
			return pad(string(rune(x)))
		case IntV:
			return pad(string(rune(x)))
		}
		t.throwf("FormatError", "%%c applied to %s", typeName(v))
	case 't':
		if b, ok := v.(BoolV); ok {
			return pad(Str(b))
		}
		t.throwf("FormatError", "%%t applied to %s", typeName(v))
	case 'v':
		_ = plusV
		return pad(t.Str(v))
	}
	t.throwf("FormatError", "unknown verb %%%c", verb)
	return ""
}

// stdFmt builds the std/fmt package (§13.1).
func stdFmt() *Pkg {
	p := &Pkg{Name: "fmt", Funcs: map[string]Value{}}
	p.Funcs["printf"] = bi("printf", func(t *T, args []Value) Value { return t.fmtPrintf(t.in.Stdout, args) })
	p.Funcs["sprintf"] = bi("sprintf", func(t *T, args []Value) Value { return t.fmtSprintfV(args) })
	p.Funcs["eprintf"] = bi("eprintf", func(t *T, args []Value) Value { return t.fmtPrintf(t.in.Stderr, args) })
	p.Funcs["fprintf"] = bi("fprintf", func(t *T, args []Value) Value {
		if len(args) < 1 {
			t.rtErr(diag.Pos{}, "fprintf needs a writer")
		}
		w := t.writerOf(args[0])
		return t.fmtPrintf(w, args[1:])
	})
	p.Funcs["print"] = bi("print", func(t *T, args []Value) Value {
		var parts []string
		for _, a := range args {
			parts = append(parts, t.Str(a))
		}
		fmt.Fprint(t.in.Stdout, strings.Join(parts, " "))
		return UnitV{}
	})
	p.Funcs["println"] = bi("println", func(t *T, args []Value) Value {
		var parts []string
		for _, a := range args {
			parts = append(parts, t.Str(a))
		}
		fmt.Fprintln(t.in.Stdout, strings.Join(parts, " "))
		return UnitV{}
	})
	p.Funcs["pad"] = bi("pad", func(t *T, args []Value) Value {
		t.wantArgs("pad", args, 2, diag.Pos{})
		return strFuncs["pad"](t, args, diag.Pos{})
	})
	p.Funcs["pad_left"] = bi("pad_left", func(t *T, args []Value) Value {
		t.wantArgs("pad_left", args, 2, diag.Pos{})
		return strFuncs["pad_left"](t, args, diag.Pos{})
	})
	p.Funcs["center"] = bi("center", func(t *T, args []Value) Value {
		t.wantArgs("center", args, 2, diag.Pos{})
		return strFuncs["center"](t, args, diag.Pos{})
	})
	p.Funcs["comma"] = bi("comma", func(t *T, args []Value) Value {
		t.wantArgs("comma", args, 1, diag.Pos{})
		return StrV(commaFmt(t.Str(args[0])))
	})
	p.Funcs["bytes"] = bi("bytes", func(t *T, args []Value) Value {
		t.wantArgs("bytes", args, 1, diag.Pos{})
		return StrV(humanBytes(t.toInt(args[0], diag.Pos{})))
	})
	p.Funcs["table"] = bi("table", func(t *T, args []Value) Value {
		t.wantArgs("table", args, 1, diag.Pos{})
		rows, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(diag.Pos{}, "fmt.table needs a slice of rows")
		}
		return StrV(t.renderTable(rows))
	})
	return p
}

func (t *T) writerOf(v Value) io.Writer {
	if f, ok := v.(*File); ok {
		if w, ok := f.f.(io.Writer); ok {
			return w
		}
	}
	t.rtErr(diag.Pos{}, "expected a writable file, got %s", typeName(v))
	return nil
}

func commaFmt(s string) string {
	neg := strings.HasPrefix(s, "-")
	if neg {
		s = s[1:]
	}
	intPart, frac := s, ""
	if i := strings.IndexByte(s, '.'); i >= 0 {
		intPart, frac = s[:i], s[i:]
	}
	var out []byte
	for i, c := range []byte(intPart) {
		if i > 0 && (len(intPart)-i)%3 == 0 {
			out = append(out, ',')
		}
		out = append(out, c)
	}
	res := string(out) + frac
	if neg {
		res = "-" + res
	}
	return res
}

func humanBytes(n int64) string {
	const unit = 1024
	if n < unit {
		return fmt.Sprintf("%d B", n)
	}
	div, exp := int64(unit), 0
	for m := n / unit; m >= unit; m /= unit {
		div *= unit
		exp++
	}
	return fmt.Sprintf("%.1f %ciB", float64(n)/float64(div), "KMGTPE"[exp])
}

func (t *T) renderTable(rows *Slice) string {
	var cells [][]string
	var widths []int
	for _, r := range rows.Elems {
		row, ok := r.(*Slice)
		if !ok {
			cells = append(cells, []string{t.Str(r)})
			continue
		}
		var line []string
		for i, c := range row.Elems {
			s := t.Str(c)
			line = append(line, s)
			for len(widths) <= i {
				widths = append(widths, 0)
			}
			if len(s) > widths[i] {
				widths[i] = len(s)
			}
		}
		cells = append(cells, line)
	}
	var sb strings.Builder
	for _, line := range cells {
		for i, c := range line {
			if i > 0 {
				sb.WriteString("  ")
			}
			if i < len(line)-1 {
				sb.WriteString(c + strings.Repeat(" ", widths[i]-len(c)))
			} else {
				sb.WriteString(c)
			}
		}
		sb.WriteString("\n")
	}
	return sb.String()
}
