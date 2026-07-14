package main

import (
	"bytes"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"voila/internal/check"
	"voila/internal/diag"
	"voila/internal/interp"
	"voila/internal/parser"
)

// runInProcess executes a .voi file with the interpreter, capturing output.
func runInProcess(t *testing.T, path string, args []string) (string, string, int) {
	t.Helper()
	src, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	source := diag.NewSource(filepath.Base(path), string(src))
	bag := diag.NewBag(source)
	file := parser.Parse(source, bag)
	if !bag.HasErrors() {
		check.Check(file, source, bag)
	}
	if bag.HasErrors() {
		return "", bag.Render(), 2
	}
	in := interp.New(file, source)
	var out, errOut bytes.Buffer
	in.Stdout = &out
	in.Stderr = &errOut
	exit := in.Run(args)
	return out.String(), errOut.String(), exit
}

// TestSamplesGolden runs every sample and compares against golden files.
// Regenerate with UPDATE_GOLDEN=1 go test ./cmd/voila/.
func TestSamplesGolden(t *testing.T) {
	samplesDir, err := filepath.Abs("../../samples")
	if err != nil {
		t.Fatal(err)
	}
	voiFiles, err := filepath.Glob(filepath.Join(samplesDir, "*.voi"))
	if err != nil || len(voiFiles) == 0 {
		t.Fatalf("no samples found: %v", err)
	}
	sort.Strings(voiFiles)

	goldenDir := filepath.Join(samplesDir, "golden")
	if os.Getenv("UPDATE_GOLDEN") != "" {
		os.MkdirAll(goldenDir, 0o755)
	}

	// Samples read their data files relative to the samples directory.
	oldWD, _ := os.Getwd()
	if err := os.Chdir(samplesDir); err != nil {
		t.Fatal(err)
	}
	defer os.Chdir(oldWD)

	for _, voi := range voiFiles {
		name := strings.TrimSuffix(filepath.Base(voi), ".voi")
		t.Run(name, func(t *testing.T) {
			out, errOut, exit := runInProcess(t, voi, nil)
			if exit != 0 {
				t.Fatalf("exit %d\nstderr:\n%s", exit, errOut)
			}
			checkGolden(t, filepath.Join(goldenDir, name+".stdout"), out)
			checkGolden(t, filepath.Join(goldenDir, name+".stderr"), errOut)
		})
	}
}

func checkGolden(t *testing.T, goldenPath, got string) {
	t.Helper()
	if os.Getenv("UPDATE_GOLDEN") != "" {
		if got == "" {
			os.Remove(goldenPath)
			return
		}
		if err := os.WriteFile(goldenPath, []byte(got), 0o644); err != nil {
			t.Fatal(err)
		}
		return
	}
	want := ""
	if data, err := os.ReadFile(goldenPath); err == nil {
		want = string(data)
	}
	if got != want {
		t.Errorf("golden mismatch for %s\n--- got ---\n%s--- want ---\n%s", filepath.Base(goldenPath), got, want)
	}
}

// TestNegativeGolden: programs under testdata/negative must FAIL voila check
// with the expected diagnostic fragments (first line of the .expect file).
func TestNegativeGolden(t *testing.T) {
	files, _ := filepath.Glob("testdata/negative/*.voi")
	if len(files) == 0 {
		t.Skip("no negative fixtures")
	}
	for _, voi := range files {
		name := strings.TrimSuffix(filepath.Base(voi), ".voi")
		t.Run(name, func(t *testing.T) {
			src, err := os.ReadFile(voi)
			if err != nil {
				t.Fatal(err)
			}
			source := diag.NewSource(filepath.Base(voi), string(src))
			bag := diag.NewBag(source)
			file := parser.Parse(source, bag)
			if !bag.HasErrors() {
				check.Check(file, source, bag)
			}
			if !bag.HasErrors() {
				t.Fatalf("%s: expected check errors, got none", name)
			}
			expectPath := strings.TrimSuffix(voi, ".voi") + ".expect"
			expect, err := os.ReadFile(expectPath)
			if err != nil {
				t.Fatalf("missing %s", expectPath)
			}
			rendered := bag.Render()
			for _, frag := range strings.Split(strings.TrimSpace(string(expect)), "\n") {
				if !strings.Contains(rendered, frag) {
					t.Errorf("diagnostics missing %q:\n%s", frag, rendered)
				}
			}
		})
	}
}

// TestBuildEquivalence verifies the Equivalence Guarantee (§12): a sample
// built into a self-contained binary produces byte-identical output to run
// mode. Gated behind VOILA_TEST_BUILD=1 (builds the toolchain and codesigns).
func TestBuildEquivalence(t *testing.T) {
	if os.Getenv("VOILA_TEST_BUILD") == "" {
		t.Skip("set VOILA_TEST_BUILD=1 to exercise build mode")
	}
	tmp := t.TempDir()
	toolchain := filepath.Join(tmp, "voila")
	cmd := exec.Command("go", "build", "-o", toolchain, "voila/cmd/voila")
	cmd.Dir = "../.."
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("go build: %v\n%s", err, out)
	}

	samplesDir, _ := filepath.Abs("../../samples")
	for _, name := range []string{"02_accounts", "04_calculator", "10_life"} {
		t.Run(name, func(t *testing.T) {
			voi := filepath.Join(samplesDir, name+".voi")
			bin := filepath.Join(tmp, name)
			buildCmd := exec.Command(toolchain, "build", voi, "-o", bin)
			if out, err := buildCmd.CombinedOutput(); err != nil {
				t.Fatalf("voila build: %v\n%s", err, out)
			}

			runCmd := exec.Command(toolchain, "run", voi)
			runCmd.Dir = samplesDir
			runOut, _ := runCmd.CombinedOutput()

			binCmd := exec.Command(bin)
			binCmd.Dir = samplesDir
			binOut, _ := binCmd.CombinedOutput()

			if !bytes.Equal(runOut, binOut) {
				t.Errorf("run and build outputs differ\n--- run ---\n%s--- build ---\n%s", runOut, binOut)
			}
		})
	}
}
