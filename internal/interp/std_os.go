package interp

import (
	"bufio"
	"errors"
	"io"
	"os"
	"os/exec"
	"sort"
	"strings"

	"voila/internal/diag"
)

// stdOs builds std/os: whole-file I/O returns error VALUES (T!), not
// exceptions — the caller obviously handles a missing file (§8, §13.3).
func stdOs() *Pkg {
	p := &Pkg{Name: "os", Funcs: map[string]Value{}}

	p.Funcs["read"] = bi("read", func(t *T, args []Value) Value {
		t.wantArgs("os.read", args, 1, diag.Pos{})
		path := string(t.wantStr(args[0], diag.Pos{}))
		data, err := os.ReadFile(path)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		return StrV(string(data))
	})

	p.Funcs["write"] = bi("write", func(t *T, args []Value) Value {
		t.wantArgs("os.write", args, 2, diag.Pos{})
		path := string(t.wantStr(args[0], diag.Pos{}))
		if err := os.WriteFile(path, []byte(t.Str(args[1])), 0o644); err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		return UnitV{}
	})

	p.Funcs["open"] = bi("open", func(t *T, args []Value) Value {
		t.wantArgs("os.open", args, 1, diag.Pos{})
		path := string(t.wantStr(args[0], diag.Pos{}))
		f, err := os.Open(path)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		return &File{f: f, name: path, reader: bufio.NewReader(f)}
	})

	p.Funcs["create"] = bi("create", func(t *T, args []Value) Value {
		t.wantArgs("os.create", args, 1, diag.Pos{})
		path := string(t.wantStr(args[0], diag.Pos{}))
		f, err := os.Create(path)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		return &File{f: f, name: path}
	})

	p.Funcs["exists"] = bi("exists", func(t *T, args []Value) Value {
		t.wantArgs("os.exists", args, 1, diag.Pos{})
		_, err := os.Stat(string(t.wantStr(args[0], diag.Pos{})))
		return BoolV(err == nil)
	})

	p.Funcs["remove"] = bi("remove", func(t *T, args []Value) Value {
		t.wantArgs("os.remove", args, 1, diag.Pos{})
		if err := os.Remove(string(t.wantStr(args[0], diag.Pos{}))); err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		return UnitV{}
	})

	// os.isdir, os.listdir and os.run exist for one reason: the Voilà compiler
	// is written in Voilà. It must walk a package directory and it must invoke
	// `cc`. Without these three the toolchain could not build itself.
	p.Funcs["isdir"] = bi("isdir", func(t *T, args []Value) Value {
		t.wantArgs("os.isdir", args, 1, diag.Pos{})
		info, err := os.Stat(string(t.wantStr(args[0], diag.Pos{})))
		return BoolV(err == nil && info.IsDir())
	})

	// listdir returns entry names only (never "." or ".."), sorted, so a
	// package's merge order never depends on the file system.
	p.Funcs["listdir"] = bi("listdir", func(t *T, args []Value) Value {
		t.wantArgs("os.listdir", args, 1, diag.Pos{})
		entries, err := os.ReadDir(string(t.wantStr(args[0], diag.Pos{})))
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}
		}
		names := make([]string, 0, len(entries))
		for _, e := range entries {
			names = append(names, e.Name())
		}
		sort.Strings(names)
		out := &Slice{}
		for _, n := range names {
			out.Elems = append(out.Elems, StrV(n))
		}
		return out
	})

	// run inherits stdio and returns the exit status (128+signal when killed).
	p.Funcs["run"] = bi("run", func(t *T, args []Value) Value {
		t.wantArgs("os.run", args, 2, diag.Pos{})
		prog := string(t.wantStr(args[0], diag.Pos{}))
		var argv []string
		if sl, ok := args[1].(*Slice); ok {
			for _, e := range sl.Elems {
				argv = append(argv, t.Str(e))
			}
		}
		cmd := exec.Command(prog, argv...)
		cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
		if err := cmd.Run(); err != nil {
			var ee *exec.ExitError
			if errors.As(err, &ee) {
				return IntV(ee.ExitCode())
			}
			return IntV(127) // exec failed: the shell's convention
		}
		return IntV(0)
	})

	p.Funcs["args"] = bi("args", func(t *T, args []Value) Value {
		out := &Slice{}
		for _, a := range t.in.Args {
			out.Elems = append(out.Elems, StrV(a))
		}
		return out
	})

	p.Funcs["env"] = bi("env", func(t *T, args []Value) Value {
		t.wantArgs("os.env", args, 1, diag.Pos{})
		v, ok := os.LookupEnv(string(t.wantStr(args[0], diag.Pos{})))
		if !ok {
			return NilV{}
		}
		return StrV(v)
	})

	p.Funcs["exit"] = bi("exit", func(t *T, args []Value) Value {
		code := 0
		if len(args) > 0 {
			code = int(t.toInt(args[0], diag.Pos{}))
		}
		panic(ctlExit{code: code})
	})

	p.Funcs["on_abort"] = bi("on_abort", func(t *T, args []Value) Value {
		t.wantArgs("os.on_abort", args, 1, diag.Pos{})
		t.in.abortMu.Lock()
		t.in.onAbort = args[0]
		t.in.abortMu.Unlock()
		return UnitV{}
	})

	p.Funcs["stderr"] = &File{f: os.Stderr, name: "<stderr>"}
	p.Funcs["stdout"] = &File{f: os.Stdout, name: "<stdout>"}

	p.Funcs["stdin_lines"] = bi("stdin_lines", func(t *T, args []Value) Value {
		sc := bufio.NewScanner(os.Stdin)
		out := &Slice{}
		for sc.Scan() {
			out.Elems = append(out.Elems, StrV(sc.Text()))
		}
		return out
	})

	// pid exists so that a program can name a scratch file nothing else will
	// touch — the compiler needs exactly that for its C output.
	p.Funcs["pid"] = bi("pid", func(t *T, args []Value) Value {
		return IntV(os.Getpid())
	})

	p.Funcs["cwd"] = bi("cwd", func(t *T, args []Value) Value {
		d, err := os.Getwd()
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error()}
		}
		return StrV(d)
	})

	p.Funcs["hostname"] = bi("hostname", func(t *T, args []Value) Value {
		h, err := os.Hostname()
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error()}
		}
		return StrV(h)
	})

	return p
}

// fileMethod implements File methods: lines(), records(n), read_all(),
// write(s), close() (§13.3 #23, §7.4 fixed-width records).
func (t *T) fileMethod(f *File, name string, args []Value, pos diag.Pos) (Value, bool) {
	switch name {
	case "close":
		if f.closed {
			return UnitV{}, true
		}
		f.closed = true
		if c, ok := f.f.(io.Closer); ok {
			if c == os.Stdout || c == os.Stderr {
				return UnitV{}, true
			}
			c.Close()
		}
		return UnitV{}, true

	case "read_all":
		r, ok := f.reader.(*bufio.Reader)
		if !ok {
			return &ErrVal{TypeName: "IOError", Msg: "file not open for reading"}, true
		}
		data, err := io.ReadAll(r)
		if err != nil {
			return &ErrVal{TypeName: "IOError", Msg: err.Error(), Trace: t.captureTrace()}, true
		}
		return StrV(string(data)), true

	case "lines":
		r, ok := f.reader.(*bufio.Reader)
		if !ok {
			t.throwf("IOError", "file %s not open for reading", f.name)
		}
		data, err := io.ReadAll(r)
		if err != nil {
			t.throwf("IOError", "%v", err)
		}
		s := strings.TrimSuffix(string(data), "\n")
		out := &Slice{}
		if s != "" {
			for _, line := range strings.Split(s, "\n") {
				out.Elems = append(out.Elems, StrV(strings.TrimSuffix(line, "\r")))
			}
		}
		return out, true

	case "records":
		// Fixed-width records (§7.4): f.records(80) — newline-agnostic when
		// the file has line breaks, raw chunks otherwise.
		t.wantArgs("records", args, 1, pos)
		width := int(t.toInt(args[0], pos))
		if width <= 0 {
			t.rtErr(pos, "record width must be positive")
		}
		r, ok := f.reader.(*bufio.Reader)
		if !ok {
			t.throwf("IOError", "file %s not open for reading", f.name)
		}
		data, err := io.ReadAll(r)
		if err != nil {
			t.throwf("IOError", "%v", err)
		}
		out := &Slice{}
		text := string(data)
		if strings.Contains(text, "\n") {
			// Line-oriented dataset: each line is one record, padded/truncated.
			for _, line := range strings.Split(strings.TrimSuffix(text, "\n"), "\n") {
				line = strings.TrimSuffix(line, "\r")
				if len(line) < width {
					line += strings.Repeat(" ", width-len(line))
				} else if len(line) > width {
					line = line[:width]
				}
				out.Elems = append(out.Elems, StrV(line))
			}
		} else {
			for i := 0; i+width <= len(text); i += width {
				out.Elems = append(out.Elems, StrV(text[i:i+width]))
			}
		}
		return out, true

	case "write":
		w, ok := f.f.(io.Writer)
		if !ok {
			return &ErrVal{TypeName: "IOError", Msg: "file not open for writing"}, true
		}
		for _, a := range args {
			io.WriteString(w, t.Str(a))
		}
		return UnitV{}, true

	case "writeln":
		w, ok := f.f.(io.Writer)
		if !ok {
			return &ErrVal{TypeName: "IOError", Msg: "file not open for writing"}, true
		}
		var parts []string
		for _, a := range args {
			parts = append(parts, t.Str(a))
		}
		io.WriteString(w, strings.Join(parts, " ")+"\n")
		return UnitV{}, true

	case "name":
		return StrV(f.name), true
	}
	return nil, false
}
