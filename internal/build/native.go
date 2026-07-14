package build

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	vlrt "voila/runtime"
)

// FindCC locates a C compiler.
func FindCC() (string, error) {
	if cc := os.Getenv("CC"); cc != "" {
		if p, err := exec.LookPath(cc); err == nil {
			return p, nil
		}
	}
	for _, name := range []string{"cc", "clang", "gcc"} {
		if p, err := exec.LookPath(name); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("no C compiler found (looked for cc, clang, gcc; set CC to override)")
}

// WriteRuntime unpacks the embedded runtime into dir and returns the .c files.
func WriteRuntime(dir string) ([]string, error) {
	entries, err := vlrt.FS.ReadDir("src")
	if err != nil {
		return nil, err
	}
	var sources []string
	for _, e := range entries {
		data, err := vlrt.FS.ReadFile("src/" + e.Name())
		if err != nil {
			return nil, err
		}
		out := filepath.Join(dir, e.Name())
		if err := os.WriteFile(out, data, 0o644); err != nil {
			return nil, err
		}
		if strings.HasSuffix(e.Name(), ".c") {
			sources = append(sources, out)
		}
	}
	return sources, nil
}

// CompileC compiles generated C plus the runtime into a native executable.
func CompileC(cSource, outPath string, keepDir string) error {
	cc, err := FindCC()
	if err != nil {
		return err
	}
	dir := keepDir
	if dir == "" {
		dir, err = os.MkdirTemp("", "voila-build-*")
		if err != nil {
			return err
		}
		defer os.RemoveAll(dir)
	} else if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}

	sources, err := WriteRuntime(dir)
	if err != nil {
		return fmt.Errorf("unpack runtime: %w", err)
	}
	progC := filepath.Join(dir, "program.c")
	if err := os.WriteFile(progC, []byte(cSource), 0o644); err != nil {
		return err
	}

	abs, err := filepath.Abs(outPath)
	if err != nil {
		abs = outPath
	}
	args := []string{"-std=c11", "-O2", "-w", "-I", dir, "-o", abs, progC}
	args = append(args, sources...)
	args = append(args, "-lm", "-lpthread")
	cmd := exec.Command(cc, args...)
	cmd.Dir = dir
	if out, err := cmd.CombinedOutput(); err != nil {
		return fmt.Errorf("%s failed: %v\n%s", filepath.Base(cc), err, out)
	}

	if runtime.GOOS == "darwin" {
		_ = exec.Command("codesign", "-s", "-", "-f", abs).Run()
	}
	return nil
}
