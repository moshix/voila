package interp

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"

	"voila/internal/dec"
	"voila/internal/diag"
)

// stdJSON: json.encode(x) / json.decode(s) / json.decode[T](s) (§13.3 #25).
func stdJSON() *Pkg {
	p := &Pkg{Name: "json", Funcs: map[string]Value{}}

	p.Funcs["encode"] = bi("encode", func(t *T, args []Value) Value {
		_, rest := stripTypeArg(args)
		t.wantArgs("json.encode", rest, 1, diag.Pos{})
		var sb strings.Builder
		t.encodeJSON(&sb, rest[0], "")
		return StrV(sb.String())
	})

	p.Funcs["pretty"] = bi("pretty", func(t *T, args []Value) Value {
		_, rest := stripTypeArg(args)
		t.wantArgs("json.pretty", args, 1, diag.Pos{})
		var sb strings.Builder
		t.encodeJSON(&sb, rest[0], "  ")
		return StrV(sb.String())
	})

	p.Funcs["decode"] = bi("decode", func(t *T, args []Value) Value {
		tv, rest := stripTypeArg(args)
		t.wantArgs("json.decode", rest, 1, diag.Pos{})
		s, ok := rest[0].(StrV)
		if !ok {
			t.rtErr(diag.Pos{}, "json.decode needs a string")
		}
		var raw any
		d := json.NewDecoder(strings.NewReader(string(s)))
		d.UseNumber()
		if err := d.Decode(&raw); err != nil {
			return &ErrVal{Msg: "bad JSON: " + err.Error(), Trace: t.captureTrace()}
		}
		v := t.jsonToValue(raw)
		if tv != nil {
			return t.shapeAs(v, tv.Name)
		}
		return v
	})

	return p
}

// encodeJSON renders a Voilà value as JSON. Struct fields keep declaration
// order; map keys keep insertion order (deterministic output).
func (t *T) encodeJSON(sb *strings.Builder, v Value, indent string) {
	t.encodeJSONIndent(sb, v, indent, "")
}

func (t *T) encodeJSONIndent(sb *strings.Builder, v Value, indent, cur string) {
	nl, pad := "", ""
	if indent != "" {
		nl = "\n"
		pad = cur + indent
	}
	switch x := v.(type) {
	case NilV:
		sb.WriteString("null")
	case BoolV:
		sb.WriteString(Str(x))
	case IntV:
		sb.WriteString(Str(x))
	case FloatV:
		sb.WriteString(Str(x))
	case DecV:
		sb.WriteString(x.D.String())
	case StrV:
		b, _ := json.Marshal(string(x))
		sb.Write(b)
	case RuneV:
		b, _ := json.Marshal(string(rune(x)))
		sb.Write(b)
	case *Slice:
		if len(x.Elems) == 0 {
			sb.WriteString("[]")
			return
		}
		sb.WriteString("[" + nl)
		for i, e := range x.Elems {
			sb.WriteString(pad)
			t.encodeJSONIndent(sb, e, indent, pad)
			if i < len(x.Elems)-1 {
				sb.WriteString(",")
			}
			sb.WriteString(nl)
		}
		sb.WriteString(cur + "]")
	case *Map:
		keys := x.Keys()
		if len(keys) == 0 {
			sb.WriteString("{}")
			return
		}
		sb.WriteString("{" + nl)
		for i, k := range keys {
			val, _ := x.Get(k)
			kb, _ := json.Marshal(t.Str(k))
			sb.WriteString(pad)
			sb.Write(kb)
			sb.WriteString(kvSep(indent))
			t.encodeJSONIndent(sb, val, indent, pad)
			if i < len(keys)-1 {
				sb.WriteString(",")
			}
			sb.WriteString(nl)
		}
		sb.WriteString(cur + "}")
	case *Struct:
		sb.WriteString("{" + nl)
		for i, name := range x.Order {
			kb, _ := json.Marshal(name)
			sb.WriteString(pad)
			sb.Write(kb)
			sb.WriteString(kvSep(indent))
			t.encodeJSONIndent(sb, x.Fields[name], indent, pad)
			if i < len(x.Order)-1 {
				sb.WriteString(",")
			}
			sb.WriteString(nl)
		}
		sb.WriteString(cur + "}")
	case *Set:
		sl := &Slice{Elems: x.keys}
		t.encodeJSONIndent(sb, sl, indent, cur)
	case *SharedBox:
		t.encodeJSONIndent(sb, x.V, indent, cur)
	default:
		b, _ := json.Marshal(t.Str(v))
		sb.Write(b)
	}
}

func (t *T) jsonToValue(raw any) Value {
	switch x := raw.(type) {
	case nil:
		return NilV{}
	case bool:
		return BoolV(x)
	case json.Number:
		if n, err := x.Int64(); err == nil {
			return IntV(n)
		}
		if d, err := dec.Parse(x.String()); err == nil {
			return DecV{d}
		}
		f, _ := x.Float64()
		return FloatV(f)
	case string:
		return StrV(x)
	case []any:
		s := &Slice{}
		for _, e := range x {
			s.Elems = append(s.Elems, t.jsonToValue(e))
		}
		return s
	case map[string]any:
		m := NewMap()
		keys := make([]string, 0, len(x))
		for k := range x {
			keys = append(keys, k)
		}
		sort.Strings(keys)
		for _, k := range keys {
			m.Set(StrV(k), t.jsonToValue(x[k]))
		}
		return m
	}
	return NilV{}
}

// shapeAs converts a decoded map into a declared struct type (json.decode[T]).
func (t *T) shapeAs(v Value, typeName string) Value {
	sd, ok := t.in.Structs[typeName]
	if !ok {
		return v
	}
	m, ok := v.(*Map)
	if !ok {
		return &ErrVal{Msg: fmt.Sprintf("cannot decode %s into %s", Str(v), typeName)}
	}
	named := map[string]Value{}
	for _, f := range sd.Fields {
		if fv, ok := m.Get(StrV(f.Name)); ok {
			named[f.Name] = fv
		}
	}
	return t.newStruct(sd, nil, named)
}

// stdHTTP: thin wrappers over net/http (§13.3 #26, §14.1).
func stdHTTP() *Pkg {
	p := &Pkg{Name: "http", Funcs: map[string]Value{}}
	client := &http.Client{Timeout: 30 * time.Second}

	respToValue := func(t *T, resp *http.Response) Value {
		defer resp.Body.Close()
		body, err := io.ReadAll(resp.Body)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		headers := NewMap()
		var names []string
		for k := range resp.Header {
			names = append(names, k)
		}
		sort.Strings(names)
		for _, k := range names {
			headers.Set(StrV(k), StrV(resp.Header.Get(k)))
		}
		return &Struct{
			Type:  "Response",
			Order: []string{"status", "body", "headers"},
			Fields: map[string]Value{
				"status":  IntV(resp.StatusCode),
				"body":    StrV(string(body)),
				"headers": headers,
			},
		}
	}

	p.Funcs["get"] = bi("get", func(t *T, args []Value) Value {
		t.wantArgs("http.get", args, 1, diag.Pos{})
		url := string(t.wantStr(args[0], diag.Pos{}))
		req, err := http.NewRequestWithContext(t.ctx, "GET", url, nil)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		resp, err := client.Do(req)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		return respToValue(t, resp)
	})

	p.Funcs["post"] = bi("post", func(t *T, args []Value) Value {
		t.wantArgs("http.post", args, 2, diag.Pos{})
		url := string(t.wantStr(args[0], diag.Pos{}))
		body := t.Str(args[1])
		req, err := http.NewRequestWithContext(t.ctx, "POST", url, strings.NewReader(body))
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		req.Header.Set("Content-Type", "application/json")
		resp, err := client.Do(req)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		return respToValue(t, resp)
	})

	return p
}

// kvSep is ":" in compact mode, ": " when pretty-printing.
func kvSep(indent string) string {
	if indent == "" {
		return ":"
	}
	return ": "
}
