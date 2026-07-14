package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestBuildAsmFlag exercises `voila build -S`: the listing lands in the
// default <input>.s (or -o target), and the binary path is untouched.
func TestBuildAsmFlag(t *testing.T) {
	tmp := t.TempDir()
	src := filepath.Join(tmp, "prog.voi")
	program := "func main() {\n    say \"hi\"\n}\n"
	if err := os.WriteFile(src, []byte(program), 0o644); err != nil {
		t.Fatal(err)
	}

	out := filepath.Join(tmp, "prog.s")
	if exit := cmdBuild([]string{"-S", src, "-o", out}); exit != 0 {
		t.Fatalf("voila build -S exit %d", exit)
	}
	listing, err := os.ReadFile(out)
	if err != nil {
		t.Fatal(err)
	}
	text := string(listing)
	for _, want := range []string{"VOILA VM ASSEMBLY LISTING", "CONSTANT POOL", "CSECT", "SAY"} {
		if !strings.Contains(text, want) {
			t.Errorf("listing missing %q", want)
		}
	}

	// Default output name: input minus .voi plus .s.
	if exit := cmdBuild([]string{"-S", src}); exit != 0 {
		t.Fatalf("default-output build -S exit %d", exit)
	}
	if _, err := os.Stat(filepath.Join(tmp, "prog.s")); err != nil {
		t.Errorf("default .s output missing: %v", err)
	}

	// A front-end error keeps exit code 2.
	bad := filepath.Join(tmp, "bad.voi")
	os.WriteFile(bad, []byte("func main() { let x int = 3.7\nsay x }\n"), 0o644)
	if exit := cmdBuild([]string{"-S", bad}); exit != 2 {
		t.Errorf("front-end failure should exit 2, got %d", exit)
	}
}
