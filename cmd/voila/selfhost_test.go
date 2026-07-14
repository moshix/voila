package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"voila/internal/diag"
	"voila/internal/lexer"
)

// dumpTokens renders the stage-0 lexer's tokens in the dump format that
// selfhost/lexer.voi prints: "LINE:COL KIND [lit]", one per line.
func dumpTokens(t *testing.T, name, src string) string {
	t.Helper()
	source := diag.NewSource(name, src)
	bag := diag.NewBag(source)
	toks := lexer.Tokens(source, bag)
	if bag.HasErrors() {
		t.Fatalf("go-lexer errors on %s:\n%s", name, bag.Render())
	}
	var sb strings.Builder
	esc := func(s string) string {
		s = strings.ReplaceAll(s, "\\", "\\\\")
		s = strings.ReplaceAll(s, "\n", "\\n")
		s = strings.ReplaceAll(s, "\t", "\\t")
		s = strings.ReplaceAll(s, "\r", "\\r")
		return s
	}
	line := func(tok lexer.Token, kind, lit string) {
		if lit == "" {
			fmt.Fprintf(&sb, "%d:%d %s\n", tok.Pos.Line, tok.Pos.Col, kind)
		} else {
			fmt.Fprintf(&sb, "%d:%d %s %s\n", tok.Pos.Line, tok.Pos.Col, kind, lit)
		}
	}
	for _, tok := range toks {
		switch {
		case tok.Kind.IsKeyword():
			line(tok, "KW", tok.Lit)
		case tok.Kind == lexer.IDENT:
			line(tok, "IDENT", tok.Lit)
		case tok.Kind == lexer.INT:
			line(tok, "INT", tok.Lit)
		case tok.Kind == lexer.FLOAT:
			line(tok, "FLOAT", tok.Lit)
		case tok.Kind == lexer.DEC:
			line(tok, "DEC", tok.Lit)
		case tok.Kind == lexer.STR:
			line(tok, "STR", esc(tok.Lit))
		case tok.Kind == lexer.ISTR:
			line(tok, "ISTR", "")
		case tok.Kind == lexer.RUNE:
			line(tok, "RUNE", esc(tok.Lit))
		case tok.Kind == lexer.SEMI:
			line(tok, "SEMI", "")
		case tok.Kind == lexer.EOF:
			line(tok, "EOF", "")
		default:
			line(tok, "OP", tok.Kind.String())
		}
	}
	return sb.String()
}

// TestSelfhostLexerDifferential runs the Voilà lexer (written in Voilà) and
// the Go lexer over the same fixtures — including the Voilà lexer itself —
// and requires identical token dumps. This is the bootstrap's first canary.
func TestSelfhostLexerDifferential(t *testing.T) {
	selfhostDir, err := filepath.Abs("../../selfhost")
	if err != nil {
		t.Fatal(err)
	}
	lexerVoi := filepath.Join(selfhostDir, "lexer.voi")

	fixtures := []string{
		filepath.Join(selfhostDir, "testdata", "kitchen.voi"),
		lexerVoi, // the lexer lexes itself
	}

	for _, fixture := range fixtures {
		name := filepath.Base(fixture)
		t.Run(name, func(t *testing.T) {
			src, err := os.ReadFile(fixture)
			if err != nil {
				t.Fatal(err)
			}
			want := dumpTokens(t, name, string(src))

			lexSrc, err := os.ReadFile(lexerVoi)
			if err != nil {
				t.Fatal(err)
			}
			got, errOut, exit := func() (string, string, int) {
				return runVoiSource(t, "lexer.voi", string(lexSrc), []string{fixture})
			}()
			if exit != 0 {
				t.Fatalf("selfhost lexer exit %d:\n%s", exit, errOut)
			}
			if got != want {
				reportFirstDiff(t, got, want)
			}
		})
	}
}

func runVoiSource(t *testing.T, name, src string, args []string) (string, string, int) {
	t.Helper()
	tmp := t.TempDir()
	path := filepath.Join(tmp, name)
	if err := os.WriteFile(path, []byte(src), 0o644); err != nil {
		t.Fatal(err)
	}
	return runInProcess(t, path, args)
}

func reportFirstDiff(t *testing.T, got, want string) {
	t.Helper()
	gotLines := strings.Split(got, "\n")
	wantLines := strings.Split(want, "\n")
	n := len(gotLines)
	if len(wantLines) < n {
		n = len(wantLines)
	}
	for i := 0; i < n; i++ {
		if gotLines[i] != wantLines[i] {
			t.Fatalf("first difference at dump line %d:\n  selfhost: %s\n  stage0:   %s",
				i+1, gotLines[i], wantLines[i])
		}
	}
	t.Fatalf("dump length differs: selfhost %d lines, stage0 %d lines",
		len(gotLines), len(wantLines))
}
