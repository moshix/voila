// voila — the Voilà toolchain: run, build, check.
package main

import (
	"fmt"
	"os"
	"path/filepath"

	"voila/internal/build"
	"voila/internal/cgen"
	"voila/internal/check"
	"voila/internal/diag"
	"voila/internal/interp"
	"voila/internal/ir"
	"voila/internal/loader"
)

const version = "0.2.0"

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}

	switch os.Args[1] {
	case "run":
		if len(os.Args) < 3 {
			fmt.Fprintln(os.Stderr, "usage: voila run <file.voi> [args...]")
			os.Exit(2)
		}
		os.Exit(runSource(os.Args[2], os.Args[3:]))

	case "build":
		os.Exit(cmdBuild(os.Args[2:]))

	case "check":
		exit := 0
		if len(os.Args) < 3 {
			fmt.Fprintln(os.Stderr, "usage: voila check <file.voi>...")
			os.Exit(2)
		}
		for _, path := range os.Args[2:] {
			if !checkSource(path) {
				exit = 2
			}
		}
		if exit == 0 {
			fmt.Println("ok")
		}
		os.Exit(exit)

	case "version", "--version", "-v":
		fmt.Printf("voila %s (stage 0, Go runtime)\n", version)

	case "help", "--help", "-h":
		usage()

	default:
		fmt.Fprintf(os.Stderr, "voila: unknown command %q\n\n", os.Args[1])
		usage()
		os.Exit(2)
	}
}

func usage() {
	fmt.Print(`voila — the Voilà language toolchain

Usage:

    voila run <file.voi> [args...]     interpret a program
    voila build <file.voi> -o <out>    compile to a native executable (via cc)
    voila build --emit=c <file.voi>    emit the generated C
    voila build -S <file.voi>          emit a VM assembly listing (.s)
    voila check <file.voi>...          type + safety check, no execution
    voila version                      print version
    voila help                         this help

`)
}

// frontEnd loads the entry file together with every user package it reaches,
// then type- and safety-checks the flattened program. Every verb goes through
// exactly this path, so `run`, `build` and `check` cannot disagree about
// whether a program is legal.
func frontEnd(path string) (*loader.Program, bool) {
	bag := diag.NewBag(nil)
	prog, err := loader.Load(path, bag)
	if err != nil {
		fmt.Fprintf(os.Stderr, "voila: %v\n", err)
		return nil, false
	}
	if !bag.HasErrors() {
		check.Check(prog.File, prog.Entry, bag)
	}
	if len(bag.Diags) > 0 {
		fmt.Fprint(os.Stderr, bag.Render())
	}
	return prog, !bag.HasErrors()
}

func checkSource(path string) bool {
	_, ok := frontEnd(path)
	return ok
}

func runSource(path string, args []string) int {
	prog, ok := frontEnd(path)
	if !ok {
		return 2
	}
	in := interp.New(prog.File, prog.Entry)
	return in.Run(args)
}

func cmdBuild(args []string) int {
	var input, output, keepDir string
	emitAsm, emitC := false, false
	for i := 0; i < len(args); i++ {
		switch {
		case args[i] == "-o":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "voila build: -o needs a path")
				return 2
			}
			output = args[i+1]
			i++
		case args[i] == "-S" || args[i] == "--emit=asm":
			emitAsm = true
		case args[i] == "--emit=c":
			emitC = true
		case args[i] == "--keep-c":
			if i+1 >= len(args) {
				fmt.Fprintln(os.Stderr, "voila build: --keep-c needs a directory")
				return 2
			}
			keepDir = args[i+1]
			i++
		case args[i] == "--target" || (len(args[i]) > 9 && args[i][:9] == "--target="):
			fmt.Fprintln(os.Stderr, "voila build: cross-compilation (--target) is not available yet")
			return 2
		default:
			input = args[i]
		}
	}
	if input == "" {
		fmt.Fprintln(os.Stderr, "usage: voila build [-S|--emit=c] <file.voi> -o <out>")
		return 2
	}
	if output == "" {
		output = input[:len(input)-len(filepath.Ext(input))]
		switch {
		case emitAsm:
			output += ".s"
		case emitC:
			output += ".c"
		}
	}
	if emitAsm {
		return cmdBuildAsm(input, output)
	}
	return cmdBuildNative(input, output, emitC, keepDir)
}

// cmdBuildNative compiles the program to C and hands it to the system C
// compiler. The resulting executable links only libvoila — no Go runtime.
func cmdBuildNative(input, output string, emitC bool, keepDir string) int {
	prog, ok := frontEnd(input)
	if !ok {
		return 2
	}
	file := prog.File
	mod, err := ir.Lower(file, input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "voila build: %s: %v\n", input, err)
		return 1
	}
	csrc, err := cgen.Compile(file, mod)
	if err != nil {
		fmt.Fprintf(os.Stderr, "voila build: %v\n", err)
		return 1
	}
	if emitC {
		if output == "-" {
			fmt.Print(csrc)
			return 0
		}
		if err := os.WriteFile(output, []byte(csrc), 0o644); err != nil {
			fmt.Fprintf(os.Stderr, "voila build: %v\n", err)
			return 1
		}
		fmt.Printf("voila: wrote %s\n", output)
		return 0
	}
	if err := build.CompileC(csrc, output, keepDir); err != nil {
		fmt.Fprintf(os.Stderr, "voila build: %v\n", err)
		return 1
	}
	fmt.Printf("voila: built %s\n", output)
	return 0
}

// cmdBuildAsm implements `voila build -S`: lower the checked program to the
// register IR (spec §12) and write it as an HLASM-style assembly listing.
func cmdBuildAsm(input, output string) int {
	if output == input {
		fmt.Fprintln(os.Stderr, "voila build -S: output would overwrite the input")
		return 2
	}
	prog, ok := frontEnd(input)
	if !ok {
		return 2
	}
	mod, err := ir.Lower(prog.File, input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "voila build -S: %s: %v\n", input, err)
		return 1
	}
	listing := ir.Listing(mod, prog.Entry)
	if output == "-" {
		fmt.Print(listing)
		return 0
	}
	if err := os.WriteFile(output, []byte(listing), 0o644); err != nil {
		fmt.Fprintf(os.Stderr, "voila build -S: %v\n", err)
		return 1
	}
	fmt.Printf("voila: wrote %s\n", output)
	return 0
}
