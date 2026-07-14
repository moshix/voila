// godump prints the Go lexer's token stream in the same format voilac does,
// so the two lexers can be diffed directly.
package main

import (
	"fmt"
	"os"
	"strings"

	"voila/internal/diag"
	"voila/internal/lexer"
	"voila/internal/loader"
	"voila/internal/parser"
)

func main() {
	// `godump FILE` dumps tokens; `godump ast FILE` dumps the parse tree.
	args := os.Args[1:]
	mode := "tokens"
	if len(args) > 1 && (args[0] == "ast" || args[0] == "load") {
		mode, args = args[0], args[1:]
	}

	// `godump load FILE` runs the package loader and dumps the FLATTENED tree,
	// so the Voilà loader can be diffed against this one.
	if mode == "load" {
		bag := diag.NewBag(nil)
		prog, err := loader.Load(args[0], bag)
		if err != nil || bag.HasErrors() {
			if err != nil {
				fmt.Fprintln(os.Stderr, err)
			}
			fmt.Fprint(os.Stderr, bag.Render())
			os.Exit(2)
		}
		var d dumper
		d.file(prog.File)
		fmt.Print(d.b.String())
		return
	}
	data, err := os.ReadFile(args[0])
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	src := diag.NewSource(args[0], string(data))
	bag := diag.NewBag(src)

	if mode == "ast" {
		f := parser.Parse(src, bag)
		if bag.HasErrors() {
			fmt.Fprint(os.Stderr, bag.Render())
			os.Exit(2)
		}
		var d dumper
		d.file(f)
		fmt.Print(d.b.String())
		return
	}
	esc := func(s string) string {
		s = strings.ReplaceAll(s, "\\", "\\\\")
		s = strings.ReplaceAll(s, "\n", "\\n")
		s = strings.ReplaceAll(s, "\t", "\\t")
		s = strings.ReplaceAll(s, "\r", "\\r")
		return s
	}
	for _, t := range lexer.Tokens(src, bag) {
		kind, lit := "", ""
		switch {
		case t.Kind.IsKeyword():
			kind, lit = "KW", t.Lit
		case t.Kind == lexer.IDENT:
			kind, lit = "IDENT", t.Lit
		case t.Kind == lexer.INT:
			kind, lit = "INT", t.Lit
		case t.Kind == lexer.FLOAT:
			kind, lit = "FLOAT", t.Lit
		case t.Kind == lexer.DEC:
			kind, lit = "DEC", t.Lit
		case t.Kind == lexer.STR:
			kind, lit = "STR", esc(t.Lit)
		case t.Kind == lexer.RUNE:
			kind, lit = "RUNE", esc(t.Lit)
		case t.Kind == lexer.ISTR:
			kind = "ISTR"
		case t.Kind == lexer.SEMI:
			kind = "SEMI"
		case t.Kind == lexer.EOF:
			kind = "EOF"
		default:
			kind, lit = "OP", t.Kind.String()
		}
		if lit == "" {
			fmt.Printf("%d:%d %s\n", t.Pos.Line, t.Pos.Col, kind)
		} else {
			fmt.Printf("%d:%d %s %s\n", t.Pos.Line, t.Pos.Col, kind, lit)
		}
	}
}
