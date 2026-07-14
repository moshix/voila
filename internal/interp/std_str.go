package interp

import (
	"encoding/base64"
	"encoding/hex"
	"strings"
	"unicode"
	"unicode/utf8"

	"voila/internal/diag"
)

// strFuncs implements std/str (§13.2). Each function takes the subject
// string as args[0], enabling the method-call duality s.upper() == str.upper(s).
// Indices are 0-based byte offsets (language convention; REXX's 1-based
// positions survive only in `parse` column templates and word()).
var strFuncs map[string]func(t *T, args []Value, pos diag.Pos) Value

func init() {
	strFuncs = map[string]func(t *T, args []Value, pos diag.Pos) Value{}
	s1 := func(name string, fn func(s string) Value) {
		strFuncs[name] = func(t *T, args []Value, pos diag.Pos) Value {
			t.wantArgs(name, args, 1, pos)
			return fn(string(t.wantStr(args[0], pos)))
		}
	}
	s2 := func(name string, fn func(s, b string) Value) {
		strFuncs[name] = func(t *T, args []Value, pos diag.Pos) Value {
			t.wantArgs(name, args, 2, pos)
			return fn(string(t.wantStr(args[0], pos)), string(t.strOrRune(args[1], pos)))
		}
	}

	// Case and whitespace.
	s1("upper", func(s string) Value { return StrV(strings.ToUpper(s)) })
	s1("lower", func(s string) Value { return StrV(strings.ToLower(s)) })
	s1("title", func(s string) Value {
		return StrV(strings.Join(mapWords(s, func(w string) string {
			r, size := utf8.DecodeRuneInString(w)
			return string(unicode.ToUpper(r)) + strings.ToLower(w[size:])
		}), " "))
	})
	s1("trim", func(s string) Value { return StrV(strings.TrimSpace(s)) })
	s1("trim_left", func(s string) Value { return StrV(strings.TrimLeft(s, " \t\r\n")) })
	s1("trim_right", func(s string) Value { return StrV(strings.TrimRight(s, " \t\r\n")) })
	s2("strip", func(s, cutset string) Value { return StrV(strings.Trim(s, cutset)) })

	strFuncs["pad"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("pad", args, 2, pos)
		s := string(t.wantStr(args[0], pos))
		n := int(t.toInt(args[1], pos))
		if len(s) >= n {
			return StrV(s[:n]) // REXX LEFT truncates to the field
		}
		return StrV(s + strings.Repeat(" ", n-len(s)))
	}
	strFuncs["pad_left"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("pad_left", args, 2, pos)
		s := string(t.wantStr(args[0], pos))
		n := int(t.toInt(args[1], pos))
		if len(s) >= n {
			return StrV(s[len(s)-n:])
		}
		return StrV(strings.Repeat(" ", n-len(s)) + s)
	}
	strFuncs["center"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("center", args, 2, pos)
		s := string(t.wantStr(args[0], pos))
		n := int(t.toInt(args[1], pos))
		if len(s) >= n {
			return StrV(s[:n])
		}
		left := (n - len(s)) / 2
		right := n - len(s) - left
		return StrV(strings.Repeat(" ", left) + s + strings.Repeat(" ", right))
	}

	// Search and test.
	s2("contains", func(s, sub string) Value { return BoolV(strings.Contains(s, sub)) })
	s2("index", func(s, sub string) Value { return IntV(strings.Index(s, sub)) })
	s2("last_index", func(s, sub string) Value { return IntV(strings.LastIndex(s, sub)) })
	s2("starts_with", func(s, p string) Value { return BoolV(strings.HasPrefix(s, p)) })
	s2("ends_with", func(s, p string) Value { return BoolV(strings.HasSuffix(s, p)) })
	s2("count", func(s, sub string) Value { return IntV(strings.Count(s, sub)) })
	s2("equal_fold", func(a, b string) Value { return BoolV(strings.EqualFold(a, b)) })

	// Slice and split.
	strFuncs["substr"] = func(t *T, args []Value, pos diag.Pos) Value {
		// REXX-style forgiving: clamps rather than throwing (§13.2).
		if len(args) < 2 || len(args) > 3 {
			t.rtErr(pos, "substr(s, start[, len])")
		}
		s := string(t.wantStr(args[0], pos))
		start := int(t.toInt(args[1], pos))
		if start < 0 {
			start = 0
		}
		if start > len(s) {
			start = len(s)
		}
		end := len(s)
		if len(args) == 3 {
			n := int(t.toInt(args[2], pos))
			if n < 0 {
				n = 0
			}
			if start+n < end {
				end = start + n
			}
		}
		return StrV(s[start:end])
	}
	strFuncs["left"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("left", args, 2, pos)
		s := string(t.wantStr(args[0], pos))
		n := int(t.toInt(args[1], pos))
		if n > len(s) {
			n = len(s)
		}
		if n < 0 {
			n = 0
		}
		return StrV(s[:n])
	}
	strFuncs["right"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("right", args, 2, pos)
		s := string(t.wantStr(args[0], pos))
		n := int(t.toInt(args[1], pos))
		if n > len(s) {
			n = len(s)
		}
		if n < 0 {
			n = 0
		}
		return StrV(s[len(s)-n:])
	}
	s2("split", func(s, sep string) Value { return strSlice(strings.Split(s, sep)) })
	strFuncs["split_n"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("split_n", args, 3, pos)
		s := string(t.wantStr(args[0], pos))
		sep := string(t.wantStr(args[1], pos))
		n := int(t.toInt(args[2], pos))
		return strSlice(strings.SplitN(s, sep, n))
	}
	s1("lines", func(s string) Value {
		s = strings.TrimSuffix(s, "\n")
		if s == "" {
			return &Slice{}
		}
		return strSlice(strings.Split(s, "\n"))
	})
	s1("fields", func(s string) Value { return strSlice(strings.Fields(s)) })
	strFuncs["word"] = func(t *T, args []Value, pos diag.Pos) Value {
		// REXX: word(s, n) is 1-based.
		t.wantArgs("word", args, 2, pos)
		ws := strings.Fields(string(t.wantStr(args[0], pos)))
		n := int(t.toInt(args[1], pos))
		if n < 1 || n > len(ws) {
			return StrV("")
		}
		return StrV(ws[n-1])
	}
	s1("words", func(s string) Value { return IntV(len(strings.Fields(s))) })
	strFuncs["subword"] = func(t *T, args []Value, pos diag.Pos) Value {
		if len(args) < 2 || len(args) > 3 {
			t.rtErr(pos, "subword(s, n[, len])")
		}
		ws := strings.Fields(string(t.wantStr(args[0], pos)))
		n := int(t.toInt(args[1], pos))
		if n < 1 {
			n = 1
		}
		if n > len(ws) {
			return StrV("")
		}
		end := len(ws)
		if len(args) == 3 {
			l := int(t.toInt(args[2], pos))
			if n-1+l < end {
				end = n - 1 + l
			}
		}
		return StrV(strings.Join(ws[n-1:end], " "))
	}
	strFuncs["delword"] = func(t *T, args []Value, pos diag.Pos) Value {
		if len(args) < 2 || len(args) > 3 {
			t.rtErr(pos, "delword(s, n[, len])")
		}
		ws := strings.Fields(string(t.wantStr(args[0], pos)))
		n := int(t.toInt(args[1], pos))
		count := len(ws)
		if len(args) == 3 {
			count = int(t.toInt(args[2], pos))
		}
		if n < 1 {
			n = 1
		}
		var out []string
		for i, w := range ws {
			if i+1 >= n && i+1 < n+count {
				continue
			}
			out = append(out, w)
		}
		return StrV(strings.Join(out, " "))
	}

	// Build and transform.
	strFuncs["join"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("join", args, 2, pos)
		parts, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(pos, "join(parts, sep) needs a slice")
		}
		sep := string(t.wantStr(args[1], pos))
		var ss []string
		for _, e := range parts.Elems {
			ss = append(ss, t.Str(e))
		}
		return StrV(strings.Join(ss, sep))
	}
	strFuncs["replace"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("replace", args, 3, pos)
		return StrV(strings.ReplaceAll(string(t.wantStr(args[0], pos)),
			string(t.strOrRune(args[1], pos)), string(t.strOrRune(args[2], pos))))
	}
	strFuncs["replace_n"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("replace_n", args, 4, pos)
		return StrV(strings.Replace(string(t.wantStr(args[0], pos)),
			string(t.strOrRune(args[1], pos)), string(t.strOrRune(args[2], pos)),
			int(t.toInt(args[3], pos))))
	}
	strFuncs["repeat"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("repeat", args, 2, pos)
		n := int(t.toInt(args[1], pos))
		if n < 0 {
			n = 0
		}
		return StrV(strings.Repeat(string(t.wantStr(args[0], pos)), n))
	}
	s1("reverse", func(s string) Value {
		rs := []rune(s)
		for i, j := 0, len(rs)-1; i < j; i, j = i+1, j-1 {
			rs[i], rs[j] = rs[j], rs[i]
		}
		return StrV(string(rs))
	})
	strFuncs["translate"] = func(t *T, args []Value, pos diag.Pos) Value {
		// REXX TRANSLATE: map chars of `from` to corresponding chars of `to`.
		t.wantArgs("translate", args, 3, pos)
		s := string(t.wantStr(args[0], pos))
		from := []rune(string(t.wantStr(args[1], pos)))
		to := []rune(string(t.wantStr(args[2], pos)))
		var sb strings.Builder
		for _, c := range s {
			mapped := c
			for i, f := range from {
				if c == f {
					if i < len(to) {
						mapped = to[i]
					} else {
						mapped = -1 // delete
					}
					break
				}
			}
			if mapped >= 0 {
				sb.WriteRune(mapped)
			}
		}
		return StrV(sb.String())
	}
	strFuncs["overlay"] = func(t *T, args []Value, pos diag.Pos) Value {
		// REXX OVERLAY: splice `new` over s at byte offset `at` (0-based).
		t.wantArgs("overlay", args, 3, pos)
		s := string(t.wantStr(args[0], pos))
		newS := string(t.wantStr(args[1], pos))
		at := int(t.toInt(args[2], pos))
		if at < 0 {
			at = 0
		}
		for len(s) < at {
			s += " "
		}
		end := at + len(newS)
		var tail string
		if end < len(s) {
			tail = s[end:]
		}
		return StrV(s[:at] + newS + tail)
	}
	strFuncs["insert"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("insert", args, 3, pos)
		s := string(t.wantStr(args[0], pos))
		newS := string(t.wantStr(args[1], pos))
		at := int(t.toInt(args[2], pos))
		if at < 0 {
			at = 0
		}
		if at > len(s) {
			at = len(s)
		}
		return StrV(s[:at] + newS + s[at:])
	}
	strFuncs["delstr"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("delstr", args, 3, pos)
		s := string(t.wantStr(args[0], pos))
		at := int(t.toInt(args[1], pos))
		n := int(t.toInt(args[2], pos))
		if at < 0 {
			at = 0
		}
		if at > len(s) {
			at = len(s)
		}
		end := at + n
		if end > len(s) {
			end = len(s)
		}
		return StrV(s[:at] + s[end:])
	}

	// Encoding and conversion.
	s1("bytes", func(s string) Value {
		out := &Slice{}
		for _, b := range []byte(s) {
			out.Elems = append(out.Elems, IntV(b))
		}
		return out
	})
	strFuncs["from_bytes"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("from_bytes", args, 1, pos)
		sl, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(pos, "from_bytes needs a []byte")
		}
		var bs []byte
		for _, e := range sl.Elems {
			bs = append(bs, byte(t.toInt(e, pos)))
		}
		if !utf8.Valid(bs) {
			return &ErrVal{Msg: "invalid UTF-8", Trace: t.captureTrace()}
		}
		return StrV(string(bs))
	}
	s1("runes", func(s string) Value {
		out := &Slice{}
		for _, r := range s {
			out.Elems = append(out.Elems, RuneV(r))
		}
		return out
	})
	strFuncs["from_runes"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("from_runes", args, 1, pos)
		sl, ok := args[0].(*Slice)
		if !ok {
			t.rtErr(pos, "from_runes needs a []rune")
		}
		var sb strings.Builder
		for _, e := range sl.Elems {
			if r, ok := e.(RuneV); ok {
				sb.WriteRune(rune(r))
			} else {
				sb.WriteRune(rune(t.toInt(e, pos)))
			}
		}
		return StrV(sb.String())
	}
	s1("rune_len", func(s string) Value { return IntV(utf8.RuneCountInString(s)) })
	s1("is_digit", func(s string) Value {
		return BoolV(s != "" && strings.IndexFunc(s, func(r rune) bool { return !unicode.IsDigit(r) }) < 0)
	})
	s1("is_alpha", func(s string) Value {
		return BoolV(s != "" && strings.IndexFunc(s, func(r rune) bool { return !unicode.IsLetter(r) }) < 0)
	})
	s1("is_space", func(s string) Value { return BoolV(s != "" && strings.TrimSpace(s) == "") })
	s1("is_upper", func(s string) Value { return BoolV(s != "" && s == strings.ToUpper(s) && s != strings.ToLower(s)) })
	strFuncs["hex"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("hex", args, 1, pos)
		switch x := args[0].(type) {
		case StrV:
			return StrV(hex.EncodeToString([]byte(string(x))))
		case *Slice:
			var bs []byte
			for _, e := range x.Elems {
				bs = append(bs, byte(t.toInt(e, pos)))
			}
			return StrV(hex.EncodeToString(bs))
		case IntV:
			return StrV(strings.ToLower(strings.TrimPrefix(hexInt(int64(x)), "0x")))
		}
		t.rtErr(pos, "hex() of %s", typeName(args[0]))
		return NilV{}
	}
	strFuncs["unhex"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("unhex", args, 1, pos)
		bs, err := hex.DecodeString(string(t.wantStr(args[0], pos)))
		if err != nil {
			return &ErrVal{Msg: "bad hex: " + err.Error(), Trace: t.captureTrace()}
		}
		return StrV(string(bs))
	}
	strFuncs["b64"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("b64", args, 1, pos)
		return StrV(base64.StdEncoding.EncodeToString([]byte(string(t.wantStr(args[0], pos)))))
	}
	strFuncs["unb64"] = func(t *T, args []Value, pos diag.Pos) Value {
		t.wantArgs("unb64", args, 1, pos)
		bs, err := base64.StdEncoding.DecodeString(string(t.wantStr(args[0], pos)))
		if err != nil {
			return &ErrVal{Msg: "bad base64: " + err.Error(), Trace: t.captureTrace()}
		}
		return StrV(string(bs))
	}
}

func hexInt(n int64) string {
	if n < 0 {
		return "-0x" + strings.ToLower(strings.TrimPrefix(hexInt(-n), "0x"))
	}
	const digits = "0123456789abcdef"
	if n == 0 {
		return "0x0"
	}
	var out []byte
	for n > 0 {
		out = append([]byte{digits[n&15]}, out...)
		n >>= 4
	}
	return "0x" + string(out)
}

func strSlice(parts []string) *Slice {
	s := &Slice{}
	for _, p := range parts {
		s.Elems = append(s.Elems, StrV(p))
	}
	return s
}

func mapWords(s string, fn func(string) string) []string {
	ws := strings.Fields(s)
	for i, w := range ws {
		ws[i] = fn(w)
	}
	return ws
}

func (t *T) wantStr(v Value, pos diag.Pos) StrV {
	switch x := v.(type) {
	case StrV:
		return x
	case RuneV:
		return StrV(string(rune(x)))
	}
	t.rtErr(pos, "expected str, got %s", typeName(v))
	return ""
}

func (t *T) strOrRune(v Value, pos diag.Pos) StrV { return t.wantStr(v, pos) }

// stdStr builds the std/str package from strFuncs plus the Builder type.
func stdStr() *Pkg {
	p := &Pkg{Name: "str", Funcs: map[string]Value{}}
	for name, fn := range strFuncs {
		f := fn
		n := name
		p.Funcs[n] = bi(n, func(t *T, args []Value) Value { return f(t, args, diag.Pos{}) })
	}
	return p
}
