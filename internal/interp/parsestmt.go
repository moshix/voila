package interp

import (
	"strings"

	"voila/internal/ast"
)

// execParse implements the §7.4 `parse` statement with the general REXX
// template algorithm: pattern items (literal separators, absolute column
// positions) fix the boundaries of capture regions; within a region, word
// targets split on blanks with the LAST target taking the raw remainder;
// `.` discards its share.
func (t *T) execParse(env *Env, st *ast.ParseStmt) {
	src := t.Str(t.eval(env, st.Src))
	switch st.CaseFold {
	case "upper":
		src = strings.ToUpper(src)
	case "lower":
		src = strings.ToLower(src)
	}

	assign := func(name, val string) {
		if name == "" {
			return
		}
		if b := env.Lookup(name); b != nil {
			if !b.Mut {
				t.rtErr(st.P, "parse target `%s` is an immutable `let` binding", name)
			}
			b.Val = StrV(val)
			b.Moved = false
			return
		}
		env.Define(name, StrV(val), true) // targets implicitly declared as str vars
	}

	// The spec writes separator-FIRST templates: `parse line "," name "," age`
	// meaning name ends at the first comma (§7.4: name = "moshix"). Rewrite
	// such templates to the canonical REXX var-then-pattern order by shifting
	// each leading literal behind its target.
	terms := st.Terms
	if len(terms) >= 2 && terms[0].Kind == ast.ParseLit &&
		(terms[1].Kind == ast.ParseVar || terms[1].Kind == ast.ParseDiscard) {
		var re []ast.ParseTerm
		for i := 0; i < len(terms); {
			if terms[i].Kind == ast.ParseLit && i+1 < len(terms) &&
				(terms[i+1].Kind == ast.ParseVar || terms[i+1].Kind == ast.ParseDiscard) {
				re = append(re, terms[i+1], terms[i])
				i += 2
			} else {
				re = append(re, terms[i])
				i++
			}
		}
		terms = re
	}

	// Split the template into segments delimited by pattern items (literal
	// separators / column positions). Each segment carries word targets.
	type segment struct {
		targets []ast.ParseTerm // ParseVar / ParseDiscard
		endLit  string          // literal that terminates this segment ("" = none)
		endCol  int             // column that terminates this segment (0 = none)
		hasEnd  bool
	}
	var segs []segment
	cur := segment{}
	startCol := -1 // column position opening the current region (-1 = none)
	var startCols []int
	for _, term := range terms {
		switch term.Kind {
		case ast.ParseVar, ast.ParseDiscard:
			cur.targets = append(cur.targets, term)
		case ast.ParseLit:
			cur.endLit = term.Lit
			cur.hasEnd = true
			segs = append(segs, cur)
			startCols = append(startCols, startCol)
			startCol = -1
			cur = segment{}
		case ast.ParseCol:
			cur.endCol = term.Col
			cur.hasEnd = true
			segs = append(segs, cur)
			startCols = append(startCols, startCol)
			startCol = term.Col
			cur = segment{}
		}
	}
	segs = append(segs, cur)
	startCols = append(startCols, startCol)

	// Walk the source, cutting each segment's region.
	pos := 0
	for _, seg := range segs {
		regionStart := pos
		regionEnd := len(src)
		nextPos := len(src)

		if seg.hasEnd {
			if seg.endLit != "" {
				idx := strings.Index(src[pos:], seg.endLit)
				if idx >= 0 {
					regionEnd = pos + idx
					nextPos = regionEnd + len(seg.endLit)
				} else {
					regionEnd = len(src)
					nextPos = len(src)
				}
			} else {
				// Column positions are 1-based byte offsets, clamped.
				col := seg.endCol - 1
				if col < 0 {
					col = 0
				}
				if col > len(src) {
					col = len(src)
				}
				regionEnd = col
				if regionEnd < regionStart {
					// REXX: a backwards column takes "through end of string".
					regionEnd = len(src)
				}
				nextPos = col
			}
		}

		region := src[min(regionStart, len(src)):min(regionEnd, len(src))]
		t.assignWords(region, seg.targets, assign)
		pos = nextPos
	}
	_ = startCols
}

// assignWords distributes a region across word targets: each non-final
// target takes one blank-delimited word; the final target takes the raw
// remainder (trimmed of the single delimiting space run before it).
func (t *T) assignWords(region string, targets []ast.ParseTerm, assign func(name, val string)) {
	if len(targets) == 0 {
		return
	}
	rest := region
	for i, tgt := range targets {
		last := i == len(targets)-1
		if last {
			val := rest
			if i > 0 || len(targets) > 1 {
				// Multi-target segments consume leading blanks between words.
				val = strings.TrimLeft(val, " \t")
			}
			if tgt.Kind == ast.ParseVar {
				// A single final target takes the raw region; the trailing-`.`
				// idiom (`w1 w2 rest .`) makes earlier ones single words.
				assign(tgt.Name, val)
			}
			return
		}
		// Take one word.
		trimmed := strings.TrimLeft(rest, " \t")
		cut := strings.IndexAny(trimmed, " \t")
		var word string
		if cut < 0 {
			word, rest = trimmed, ""
		} else {
			word, rest = trimmed[:cut], trimmed[cut:]
		}
		if tgt.Kind == ast.ParseVar {
			assign(tgt.Name, word)
		}
	}
}

func min(a, b int) int {
	if a < b {
		return a
	}
	return b
}
