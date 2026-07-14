package interp

import (
	"math"
	"strings"

	"voila/internal/dec"
	"voila/internal/diag"
	"voila/internal/lexer"
)

// binaryOp implements the operator table (§5.1) over the numeric tower:
// implicit widening only when lossless (§3.2), overflow traps (OverflowError),
// dec arithmetic honours the current `numeric digits`.
func (t *T) binaryOp(op lexer.Kind, l, r Value, pos diag.Pos) Value {
	switch op {
	case lexer.CONCAT:
		return StrV(t.Str(l) + t.Str(r))
	case lexer.EQ:
		return BoolV(equalValues(l, r))
	case lexer.NE:
		return BoolV(!equalValues(l, r))
	case lexer.IN:
		return t.membership(l, r, pos)
	}

	// Rune arithmetic behaves as int (lossless).
	if lr, ok := l.(RuneV); ok {
		l = IntV(lr)
	}
	if rr, ok := r.(RuneV); ok {
		r = IntV(rr)
	}

	// Duration arithmetic: 5 * time.Second, d1 + d2, d1 < d2 (§9.3).
	if v, handled := t.durOp(op, l, r, pos); handled {
		return v
	}

	switch lv := l.(type) {
	case IntV:
		switch rv := r.(type) {
		case IntV:
			return t.intOp(op, int64(lv), int64(rv), pos)
		case FloatV:
			return t.floatOp(op, t.intToFloat(int64(lv), pos), float64(rv), pos)
		case DecV:
			return t.decOp(op, dec.FromInt64(int64(lv)), rv.D, pos)
		}
	case FloatV:
		switch rv := r.(type) {
		case IntV:
			return t.floatOp(op, float64(lv), t.intToFloat(int64(rv), pos), pos)
		case FloatV:
			return t.floatOp(op, float64(lv), float64(rv), pos)
		case DecV:
			t.throwf("ConvError", "implicit float→dec conversion is lossy; use dec(x) or a d-suffixed literal")
		}
	case DecV:
		switch rv := r.(type) {
		case IntV:
			return t.decOp(op, lv.D, dec.FromInt64(int64(rv)), pos)
		case DecV:
			return t.decOp(op, lv.D, rv.D, pos)
		case FloatV:
			t.throwf("ConvError", "implicit float→dec conversion is lossy; use dec(x) or a d-suffixed literal")
		}
	case StrV:
		if rv, ok := r.(StrV); ok {
			switch op {
			case lexer.LT:
				return BoolV(lv < rv)
			case lexer.LE:
				return BoolV(lv <= rv)
			case lexer.GT:
				return BoolV(lv > rv)
			case lexer.GE:
				return BoolV(lv >= rv)
			case lexer.PLUS:
				t.rtErr(pos, "`+` never concatenates; use `||` (§5.1)")
			}
		}
	}
	t.rtErr(pos, "invalid operands for %s: %s and %s", op, typeName(l), typeName(r))
	return NilV{}
}

// intToFloat converts int→float only when exactly representable (§3.2).
func (t *T) intToFloat(n int64, pos diag.Pos) float64 {
	f := float64(n)
	if int64(f) != n || n > (1<<53) || n < -(1<<53) {
		t.throwf("ConvError", "int value %d is not exactly representable as float; use float(x)", n)
	}
	return f
}

func (t *T) intOp(op lexer.Kind, a, b int64, pos diag.Pos) Value {
	switch op {
	case lexer.PLUS:
		c := a + b
		if (a > 0 && b > 0 && c < 0) || (a < 0 && b < 0 && c >= 0) {
			t.throwf("OverflowError", "integer overflow: %d + %d", a, b)
		}
		return IntV(c)
	case lexer.MINUS:
		c := a - b
		if (a >= 0 && b < 0 && c < 0) || (a < 0 && b > 0 && c >= 0) {
			t.throwf("OverflowError", "integer overflow: %d - %d", a, b)
		}
		return IntV(c)
	case lexer.STAR:
		if a != 0 && b != 0 {
			c := a * b
			if c/a != b {
				t.throwf("OverflowError", "integer overflow: %d * %d", a, b)
			}
			return IntV(c)
		}
		return IntV(0)
	case lexer.SLASH:
		// Exact division producing float (§5.1).
		if b == 0 {
			t.throwf("RangeError", "division by zero")
		}
		return FloatV(t.intToFloat(a, pos) / t.intToFloat(b, pos))
	case lexer.FLOORDIV:
		if b == 0 {
			t.throwf("RangeError", "division by zero")
		}
		if a == math.MinInt64 && b == -1 {
			t.throwf("OverflowError", "integer overflow: %d // -1", a)
		}
		q := a / b
		if (a%b != 0) && ((a < 0) != (b < 0)) {
			q--
		}
		return IntV(q)
	case lexer.PERCENT:
		if b == 0 {
			t.throwf("RangeError", "division by zero")
		}
		if b == -1 {
			return IntV(0)
		}
		m := a % b
		if m != 0 && ((a < 0) != (b < 0)) {
			m += b
		}
		return IntV(m) // floor-mod, pairs with `//`
	case lexer.POWER:
		return t.intPow(a, b, pos)
	case lexer.SHL:
		if b < 0 || b > 63 {
			t.throwf("RangeError", "shift count %d out of range", b)
		}
		return IntV(a << uint(b))
	case lexer.SHR:
		if b < 0 || b > 63 {
			t.throwf("RangeError", "shift count %d out of range", b)
		}
		return IntV(a >> uint(b))
	case lexer.AMP:
		return IntV(a & b)
	case lexer.PIPE:
		return IntV(a | b)
	case lexer.CARET:
		return IntV(a ^ b)
	case lexer.LT:
		return BoolV(a < b)
	case lexer.LE:
		return BoolV(a <= b)
	case lexer.GT:
		return BoolV(a > b)
	case lexer.GE:
		return BoolV(a >= b)
	}
	t.rtErr(pos, "invalid int operator %s", op)
	return NilV{}
}

func (t *T) intPow(a, b int64, pos diag.Pos) Value {
	if b < 0 {
		t.throwf("RangeError", "negative integer exponent %d (use float or dec base)", b)
	}
	result := int64(1)
	base := a
	for e := b; e > 0; e >>= 1 {
		if e&1 == 1 {
			r2 := result * base
			if base != 0 && result != 0 && r2/result != base {
				t.throwf("OverflowError", "integer overflow: %d ** %d", a, b)
			}
			result = r2
		}
		if e > 1 {
			b2 := base * base
			if base != 0 && b2/base != base {
				t.throwf("OverflowError", "integer overflow: %d ** %d", a, b)
			}
			base = b2
		}
	}
	return IntV(result)
}

func (t *T) floatOp(op lexer.Kind, a, b float64, pos diag.Pos) Value {
	switch op {
	case lexer.PLUS:
		return FloatV(a + b)
	case lexer.MINUS:
		return FloatV(a - b)
	case lexer.STAR:
		return FloatV(a * b)
	case lexer.SLASH:
		if b == 0 {
			t.throwf("RangeError", "division by zero")
		}
		return FloatV(a / b)
	case lexer.FLOORDIV:
		if b == 0 {
			t.throwf("RangeError", "division by zero")
		}
		q := math.Floor(a / b)
		if q > math.MaxInt64 || q < math.MinInt64 {
			t.throwf("OverflowError", "floor division result out of int range")
		}
		return IntV(int64(q))
	case lexer.PERCENT:
		if b == 0 {
			t.throwf("RangeError", "division by zero")
		}
		m := math.Mod(a, b)
		if m != 0 && (m < 0) != (b < 0) {
			m += b
		}
		return FloatV(m)
	case lexer.POWER:
		return FloatV(math.Pow(a, b))
	case lexer.LT:
		return BoolV(a < b)
	case lexer.LE:
		return BoolV(a <= b)
	case lexer.GT:
		return BoolV(a > b)
	case lexer.GE:
		return BoolV(a >= b)
	}
	t.rtErr(pos, "invalid float operator %s", op)
	return NilV{}
}

func (t *T) decOp(op lexer.Kind, a, b *dec.Dec, pos diag.Pos) Value {
	digits := t.digits
	switch op {
	case lexer.PLUS:
		return DecV{dec.Add(a, b, digits)}
	case lexer.MINUS:
		return DecV{dec.Sub(a, b, digits)}
	case lexer.STAR:
		return DecV{dec.Mul(a, b, digits)}
	case lexer.SLASH:
		q, err := dec.Div(a, b, digits)
		if err != nil {
			t.throwf("RangeError", "division by zero")
		}
		return DecV{q}
	case lexer.FLOORDIV:
		q, err := dec.FloorDiv(a, b)
		if err != nil {
			t.throwf("RangeError", "division by zero")
		}
		return DecV{q}
	case lexer.PERCENT:
		m, err := dec.Mod(a, b, digits)
		if err != nil {
			t.throwf("RangeError", "division by zero")
		}
		return DecV{m}
	case lexer.POWER:
		if n, ok := b.Int64(); ok {
			p, err := dec.Pow(a, n, digits)
			if err != nil {
				t.throwf("RangeError", "%v", err)
			}
			return DecV{p}
		}
		t.throwf("RangeError", "dec exponent must be an integer")
	case lexer.LT:
		return BoolV(dec.Cmp(a, b) < 0)
	case lexer.LE:
		return BoolV(dec.Cmp(a, b) <= 0)
	case lexer.GT:
		return BoolV(dec.Cmp(a, b) > 0)
	case lexer.GE:
		return BoolV(dec.Cmp(a, b) >= 0)
	}
	t.rtErr(pos, "invalid dec operator %s", op)
	return NilV{}
}

// durOp handles Duration arithmetic; returns handled=false if neither
// operand is a Duration.
func (t *T) durOp(op lexer.Kind, l, r Value, pos diag.Pos) (Value, bool) {
	ld, lIsDur := l.(DurV)
	rd, rIsDur := r.(DurV)
	if !lIsDur && !rIsDur {
		return nil, false
	}
	switch {
	case lIsDur && rIsDur:
		switch op {
		case lexer.PLUS:
			return DurV(ld + rd), true
		case lexer.MINUS:
			return DurV(ld - rd), true
		case lexer.LT:
			return BoolV(ld < rd), true
		case lexer.LE:
			return BoolV(ld <= rd), true
		case lexer.GT:
			return BoolV(ld > rd), true
		case lexer.GE:
			return BoolV(ld >= rd), true
		case lexer.SLASH:
			if rd == 0 {
				t.throwf("RangeError", "division by zero")
			}
			return FloatV(float64(ld) / float64(rd)), true
		}
	case lIsDur:
		if n, ok := r.(IntV); ok {
			switch op {
			case lexer.STAR:
				return DurV(int64(ld) * int64(n)), true
			case lexer.SLASH:
				if n == 0 {
					t.throwf("RangeError", "division by zero")
				}
				return DurV(int64(ld) / int64(n)), true
			}
		}
	case rIsDur:
		if n, ok := l.(IntV); ok && op == lexer.STAR {
			return DurV(int64(n) * int64(rd)), true
		}
	}
	t.rtErr(pos, "invalid duration operation %s between %s and %s", op, typeName(l), typeName(r))
	return NilV{}, true
}

// equalValues is deep structural equality across the numeric tower.
func equalValues(a, b Value) bool {
	// Numeric cross-type equality.
	switch av := a.(type) {
	case IntV:
		switch bv := b.(type) {
		case IntV:
			return av == bv
		case FloatV:
			return float64(av) == float64(bv)
		case DecV:
			return dec.Cmp(dec.FromInt64(int64(av)), bv.D) == 0
		case RuneV:
			return int64(av) == int64(bv)
		}
	case FloatV:
		switch bv := b.(type) {
		case FloatV:
			return av == bv
		case IntV:
			return float64(av) == float64(bv)
		}
	case DecV:
		switch bv := b.(type) {
		case DecV:
			return dec.Cmp(av.D, bv.D) == 0
		case IntV:
			return dec.Cmp(av.D, dec.FromInt64(int64(bv))) == 0
		}
	case RuneV:
		switch bv := b.(type) {
		case RuneV:
			return av == bv
		case IntV:
			return int64(av) == int64(bv)
		case StrV:
			return string(rune(av)) == string(bv)
		}
	case StrV:
		switch bv := b.(type) {
		case StrV:
			return av == bv
		case RuneV:
			return string(av) == string(rune(bv))
		}
		return false
	case BoolV:
		bv, ok := b.(BoolV)
		return ok && av == bv
	case NilV:
		_, ok := b.(NilV)
		return ok
	case UnitV:
		_, ok := b.(UnitV)
		return ok
	case *Slice:
		bv, ok := b.(*Slice)
		if !ok || len(av.Elems) != len(bv.Elems) {
			return false
		}
		for i := range av.Elems {
			if !equalValues(av.Elems[i], bv.Elems[i]) {
				return false
			}
		}
		return true
	case *Tuple:
		bv, ok := b.(*Tuple)
		if !ok || len(av.Elems) != len(bv.Elems) {
			return false
		}
		for i := range av.Elems {
			if !equalValues(av.Elems[i], bv.Elems[i]) {
				return false
			}
		}
		return true
	case *Struct:
		bv, ok := b.(*Struct)
		if !ok || av.Type != bv.Type {
			return false
		}
		for k, v := range av.Fields {
			if !equalValues(v, bv.Fields[k]) {
				return false
			}
		}
		return true
	case *EnumVal:
		bv, ok := b.(*EnumVal)
		if !ok || av.Type != bv.Type || av.Variant != bv.Variant || len(av.Fields) != len(bv.Fields) {
			return false
		}
		for i := range av.Fields {
			if !equalValues(av.Fields[i], bv.Fields[i]) {
				return false
			}
		}
		return true
	case DurV:
		bv, ok := b.(DurV)
		return ok && av == bv
	}
	if _, ok := b.(NilV); ok {
		return false
	}
	return a == b
}

// membership implements `x in coll` (§5.1): slices, map keys, sets, strings
// (substring), ranges.
func (t *T) membership(x, coll Value, pos diag.Pos) Value {
	switch c := coll.(type) {
	case *Slice:
		for _, e := range c.Elems {
			if equalValues(x, e) {
				return BoolV(true)
			}
		}
		return BoolV(false)
	case *Map:
		_, ok := c.Get(x)
		return BoolV(ok)
	case *Set:
		return BoolV(c.Has(x))
	case StrV:
		switch xs := x.(type) {
		case StrV:
			return BoolV(strings.Contains(string(c), string(xs)))
		case RuneV:
			return BoolV(strings.ContainsRune(string(c), rune(xs)))
		}
	case *RangeVal:
		n, ok := x.(IntV)
		if !ok {
			return BoolV(false)
		}
		v := int64(n)
		if c.Inclusive {
			return BoolV(v >= c.Lo && v <= c.Hi)
		}
		return BoolV(v >= c.Lo && v < c.Hi)
	}
	t.rtErr(pos, "`in` not supported on %s", typeName(coll))
	return BoolV(false)
}
