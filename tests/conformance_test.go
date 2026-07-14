// Package tests holds the conformance suite: every test program is executed
// by BOTH engines — the tree-walking interpreter (`voila run`) and the native
// C build (`voila build` → cc) — and both must produce identical stdout,
// stderr and exit code, and both must match the recorded expectation.
//
// This is the Equivalence Guarantee (spec §12) under continuous test.
//
//	go test ./tests/                    # interpreter only (fast)
//	VOILA_NATIVE=1 go test ./tests/     # both engines, compared
//	UPDATE_EXPECT=1 go test ./tests/    # re-record expectations
package tests

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"testing"
)

var (
	toolchainOnce sync.Once
	toolchainPath string
	toolchainErr  error
)

// toolchain builds the voila binary once per test run.
func toolchain(t *testing.T) string {
	t.Helper()
	toolchainOnce.Do(func() {
		dir, err := os.MkdirTemp("", "voila-tests-*")
		if err != nil {
			toolchainErr = err
			return
		}
		bin := filepath.Join(dir, "voila")
		cmd := exec.Command("go", "build", "-o", bin, "voila/cmd/voila")
		cmd.Dir = ".."
		if out, err := cmd.CombinedOutput(); err != nil {
			toolchainErr = fmt.Errorf("go build: %v\n%s", err, out)
			return
		}
		toolchainPath = bin
	})
	if toolchainErr != nil {
		t.Fatal(toolchainErr)
	}
	return toolchainPath
}

type result struct {
	stdout, stderr string
	exit           int
}

func runCmd(dir string, name string, args ...string) result {
	cmd := exec.Command(name, args...)
	cmd.Dir = dir
	var out, errOut bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &errOut
	err := cmd.Run()
	code := 0
	if ee, ok := err.(*exec.ExitError); ok {
		code = ee.ExitCode()
	} else if err != nil {
		code = -1
		errOut.WriteString(err.Error())
	}
	return result{out.String(), errOut.String(), code}
}

// expectation reads the recorded .out/.err/.exit files for a test.
func expectation(base string) result {
	r := result{}
	if b, err := os.ReadFile(base + ".out"); err == nil {
		r.stdout = string(b)
	}
	if b, err := os.ReadFile(base + ".err"); err == nil {
		r.stderr = string(b)
	}
	if b, err := os.ReadFile(base + ".exit"); err == nil {
		r.exit, _ = strconv.Atoi(strings.TrimSpace(string(b)))
	}
	return r
}

func record(base string, r result) error {
	write := func(path, content string) error {
		if content == "" {
			os.Remove(path)
			return nil
		}
		return os.WriteFile(path, []byte(content), 0o644)
	}
	if err := write(base+".out", r.stdout); err != nil {
		return err
	}
	if err := write(base+".err", r.stderr); err != nil {
		return err
	}
	if r.exit != 0 {
		return os.WriteFile(base+".exit", []byte(strconv.Itoa(r.exit)+"\n"), 0o644)
	}
	os.Remove(base + ".exit")
	return nil
}

// TestConformance is the core suite. Each program runs through the
// interpreter and (with VOILA_NATIVE=1) through a native build; the three
// results — expected, interpreted, native — must all agree.
func TestConformance(t *testing.T) {
	voila := toolchain(t)
	files, err := filepath.Glob("conformance/*.voi")
	if err != nil || len(files) == 0 {
		t.Fatalf("no conformance programs found: %v", err)
	}
	sort.Strings(files)
	native := os.Getenv("VOILA_NATIVE") != ""
	update := os.Getenv("UPDATE_EXPECT") != ""

	for _, prog := range files {
		name := strings.TrimSuffix(filepath.Base(prog), ".voi")
		t.Run(name, func(t *testing.T) {
			t.Parallel()
			base := strings.TrimSuffix(prog, ".voi")

			interp := runCmd(".", voila, "run", prog)
			if update {
				if err := record(base, interp); err != nil {
					t.Fatal(err)
				}
				return
			}
			want := expectation(base)
			compare(t, "interpreter", want, interp)

			if !native {
				return
			}
			bin := filepath.Join(t.TempDir(), name)
			if b := runCmd(".", voila, "build", prog, "-o", bin); b.exit != 0 {
				t.Fatalf("native build failed (exit %d):\n%s%s", b.exit, b.stdout, b.stderr)
			}
			nat := runCmd(".", bin)
			compare(t, "native", want, nat)

			// The Equivalence Guarantee: the two engines must not merely both
			// be right, they must be indistinguishable.
			if interp.stdout != nat.stdout || interp.stderr != nat.stderr || interp.exit != nat.exit {
				t.Errorf("EQUIVALENCE VIOLATION between engines\n"+
					"interpreter: exit=%d\nstdout:\n%s\nstderr:\n%s\n"+
					"native:      exit=%d\nstdout:\n%s\nstderr:\n%s",
					interp.exit, interp.stdout, interp.stderr,
					nat.exit, nat.stdout, nat.stderr)
			}
		})
	}
}

func compare(t *testing.T, engine string, want, got result) {
	t.Helper()
	if got.stdout != want.stdout {
		t.Errorf("%s: stdout mismatch\n--- got ---\n%s--- want ---\n%s", engine, got.stdout, want.stdout)
	}
	if got.stderr != want.stderr {
		t.Errorf("%s: stderr mismatch\n--- got ---\n%s--- want ---\n%s", engine, got.stderr, want.stderr)
	}
	if got.exit != want.exit {
		t.Errorf("%s: exit = %d, want %d\nstderr:\n%s", engine, got.exit, want.exit, got.stderr)
	}
}

// TestSamplesEquivalence drives the 10 sample programs through both engines.
func TestSamplesEquivalence(t *testing.T) {
	if os.Getenv("VOILA_NATIVE") == "" {
		t.Skip("set VOILA_NATIVE=1 to compare engines on the samples")
	}
	voila := toolchain(t)
	files, _ := filepath.Glob("../samples/*.voi")
	sort.Strings(files)
	for _, prog := range files {
		name := strings.TrimSuffix(filepath.Base(prog), ".voi")
		t.Run(name, func(t *testing.T) {
			abs, _ := filepath.Abs(prog)
			interp := runCmd("../samples", voila, "run", abs)
			bin := filepath.Join(t.TempDir(), name)
			if b := runCmd("../samples", voila, "build", abs, "-o", bin); b.exit != 0 {
				t.Fatalf("native build failed:\n%s%s", b.stdout, b.stderr)
			}
			nat := runCmd("../samples", bin)
			if interp.stdout != nat.stdout || interp.exit != nat.exit {
				t.Errorf("engines differ on %s\n--- interpreted ---\n%s--- native ---\n%s",
					name, interp.stdout, nat.stdout)
			}
		})
	}
}

// TestMultiPackage: a program split across packages must load, check, run and
// build — and the two engines must still agree. Package privacy and import
// cycles must be rejected.
func TestMultiPackage(t *testing.T) {
	voila := toolchain(t)

	t.Run("program", func(t *testing.T) {
		entry := "multipkg/main.voi"
		want := expectation("multipkg/main")
		interp := runCmd(".", voila, "run", entry)
		compare(t, "interpreter", want, interp)

		if os.Getenv("VOILA_NATIVE") == "" {
			return
		}
		bin := filepath.Join(t.TempDir(), "mp")
		if b := runCmd(".", voila, "build", entry, "-o", bin); b.exit != 0 {
			t.Fatalf("native build failed:\n%s%s", b.stdout, b.stderr)
		}
		nat := runCmd(".", bin)
		compare(t, "native", want, nat)
		if interp.stdout != nat.stdout || interp.exit != nat.exit {
			t.Errorf("EQUIVALENCE VIOLATION across packages\n--- interpreted ---\n%s--- native ---\n%s",
				interp.stdout, nat.stdout)
		}
	})

	// Bad programs: each directory is one rejected build.
	// Each of these is a program the loader must REJECT. Every one of them was
	// a silent miscompile before the review: a wrong variant constructed, a
	// package silently shadowed, a typo'd import waved through as "probably
	// std", a path escaping the module root.
	bad := map[string]string{
		"private":       "is package-private",
		"cycle":         "imports itself",
		"variantclash":  "is declared in both",
		"dupname":       "two packages are both named",
		"unknownimport": "no package `bad/geomm`",
		"selective":     "selective and wildcard imports are not supported",
	}
	for dir, frag := range bad {
		t.Run(dir, func(t *testing.T) {
			r := runCmd(".", voila, "check", filepath.Join("multipkg-bad", dir, "main.voi"))
			if r.exit != 2 {
				t.Errorf("exit = %d, want 2 (rejected)", r.exit)
			}
			if !strings.Contains(r.stderr, frag) {
				t.Errorf("diagnostics missing %q\n%s", frag, r.stderr)
			}
		})
	}
}

// TestNegativeNative: programs the FRONT END accepts but the native backend
// must refuse rather than compile into something that behaves differently
// from the interpreter. Refusing is how the Equivalence Guarantee is kept
// when the backend cannot yet express a semantic.
func TestNegativeNative(t *testing.T) {
	voila := toolchain(t)
	files, _ := filepath.Glob("negative-native/*.voi")
	sort.Strings(files)
	if len(files) == 0 {
		t.Skip("no native-refusal fixtures")
	}
	for _, prog := range files {
		name := strings.TrimSuffix(filepath.Base(prog), ".voi")
		t.Run(name, func(t *testing.T) {
			want, err := os.ReadFile(strings.TrimSuffix(prog, ".voi") + ".expect")
			if err != nil {
				t.Fatalf("missing .expect for %s", name)
			}
			// The interpreter runs it happily...
			if r := runCmd(".", voila, "run", prog); r.exit != 0 {
				t.Errorf("interpreter should accept this program, got exit %d\n%s", r.exit, r.stderr)
			}
			// ...and the native build must refuse it, loudly.
			r := runCmd(".", voila, "build", prog, "-o", filepath.Join(t.TempDir(), "x"))
			if r.exit == 0 {
				t.Fatalf("native build should have refused %s", name)
			}
			for _, frag := range strings.Split(strings.TrimSpace(string(want)), "\n") {
				if !strings.Contains(r.stderr, frag) {
					t.Errorf("refusal missing %q\n%s", frag, r.stderr)
				}
			}
		})
	}
}

// TestNegative: every fixture must be REJECTED by the front end with the
// expected diagnostic — and rejected identically by run, build and check.
func TestNegative(t *testing.T) {
	voila := toolchain(t)
	files, _ := filepath.Glob("negative/*.voi")
	sort.Strings(files)
	if len(files) == 0 {
		t.Skip("no negative fixtures")
	}
	for _, prog := range files {
		name := strings.TrimSuffix(filepath.Base(prog), ".voi")
		t.Run(name, func(t *testing.T) {
			want, err := os.ReadFile(strings.TrimSuffix(prog, ".voi") + ".expect")
			if err != nil {
				t.Fatalf("missing .expect for %s", name)
			}
			for _, verb := range []string{"check", "run", "build"} {
				args := []string{verb, prog}
				if verb == "build" {
					args = append(args, "-o", filepath.Join(t.TempDir(), "x"))
				}
				r := runCmd(".", voila, args...)
				if r.exit != 2 {
					t.Errorf("%s: exit = %d, want 2 (rejected)", verb, r.exit)
				}
				for _, frag := range strings.Split(strings.TrimSpace(string(want)), "\n") {
					if !strings.Contains(r.stderr, frag) {
						t.Errorf("%s: diagnostics missing %q\n%s", verb, frag, r.stderr)
					}
				}
			}
		})
	}
}

// TestAsmListing: every conformance program must also lower to an assembly
// listing (`voila build -S`) with no line over 79 columns. A construct that
// cannot be listed is a hole in the IR.
func TestAsmListing(t *testing.T) {
	voila := toolchain(t)
	files, _ := filepath.Glob("conformance/*.voi")
	files = append(files, "multipkg/main.voi")
	sort.Strings(files)
	for _, prog := range files {
		name := strings.TrimSuffix(filepath.Base(prog), ".voi")
		t.Run(name, func(t *testing.T) {
			out := filepath.Join(t.TempDir(), name+".s")
			if r := runCmd(".", voila, "build", "-S", prog, "-o", out); r.exit != 0 {
				t.Fatalf("listing failed:\n%s%s", r.stdout, r.stderr)
			}
			data, err := os.ReadFile(out)
			if err != nil {
				t.Fatal(err)
			}
			for i, line := range strings.Split(string(data), "\n") {
				if n := len([]rune(line)); n > 79 {
					t.Errorf("line %d is %d runes (max 79): %s", i+1, n, line)
				}
			}
		})
	}
}
