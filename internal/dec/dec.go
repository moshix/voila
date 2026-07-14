// Package dec implements Voilà's `dec` type: arbitrary-precision decimal
// arithmetic in the REXX tradition, governed by a significant-digits context
// (`numeric digits`, default 28) with round-half-even.
package dec

import (
	"fmt"
	"math"
	"math/big"
	"strings"
)

// DefaultDigits is the default `numeric digits` context (§3.1).
const DefaultDigits = 28

// Dec is an immutable arbitrary-precision decimal: value = sign × coef × 10^exp.
type Dec struct {
	neg  bool
	coef *big.Int // always >= 0
	exp  int
}

var (
	bigZero = big.NewInt(0)
	bigTen  = big.NewInt(10)
)

// New builds a Dec from a coefficient and exponent.
func New(coef int64, exp int) *Dec {
	neg := coef < 0
	if neg {
		coef = -coef
	}
	return &Dec{neg: neg, coef: big.NewInt(coef), exp: exp}
}

// Zero returns 0.
func Zero() *Dec { return &Dec{coef: new(big.Int)} }

// FromInt64 converts an integer exactly.
func FromInt64(n int64) *Dec {
	if n == math.MinInt64 {
		c, _ := new(big.Int).SetString("9223372036854775808", 10)
		return &Dec{neg: true, coef: c}
	}
	return New(n, 0)
}

// Parse reads a decimal literal: [+-]digits[.digits][e[+-]digits].
func Parse(s string) (*Dec, error) {
	orig := s
	s = strings.TrimSpace(s)
	if s == "" {
		return nil, fmt.Errorf("empty decimal literal")
	}
	d := &Dec{coef: new(big.Int)}
	if s[0] == '+' {
		s = s[1:]
	} else if s[0] == '-' {
		d.neg = true
		s = s[1:]
	}
	mant := s
	expPart := 0
	if i := strings.IndexAny(s, "eE"); i >= 0 {
		mant = s[:i]
		var e int
		if _, err := fmt.Sscanf(s[i+1:], "%d", &e); err != nil {
			return nil, fmt.Errorf("bad exponent in decimal %q", orig)
		}
		expPart = e
	}
	intPart := mant
	fracPart := ""
	if i := strings.IndexByte(mant, '.'); i >= 0 {
		intPart = mant[:i]
		fracPart = mant[i+1:]
	}
	digits := intPart + fracPart
	digits = strings.ReplaceAll(digits, "_", "")
	if digits == "" {
		return nil, fmt.Errorf("bad decimal %q", orig)
	}
	if _, ok := d.coef.SetString(digits, 10); !ok {
		return nil, fmt.Errorf("bad decimal %q", orig)
	}
	d.exp = expPart - len(fracPart)
	if d.coef.Sign() == 0 {
		d.neg = false
		d.exp = 0
	}
	return d, nil
}

// MustParse panics on a bad literal (used for literals already lexed).
func MustParse(s string) *Dec {
	d, err := Parse(s)
	if err != nil {
		panic(err)
	}
	return d
}

// FromFloat converts a binary float via its shortest decimal representation.
// The spec makes float→dec explicit (§3.2); this backs the dec(x) conversion.
func FromFloat(f float64) (*Dec, error) {
	if math.IsNaN(f) || math.IsInf(f, 0) {
		return nil, fmt.Errorf("cannot convert %v to dec", f)
	}
	return Parse(fmt.Sprintf("%g", f))
}

func (d *Dec) Sign() int {
	if d.coef.Sign() == 0 {
		return 0
	}
	if d.neg {
		return -1
	}
	return 1
}

func (d *Dec) IsZero() bool { return d.coef.Sign() == 0 }

func (d *Dec) Neg() *Dec {
	if d.IsZero() {
		return d
	}
	return &Dec{neg: !d.neg, coef: d.coef, exp: d.exp}
}

func (d *Dec) Abs() *Dec {
	if !d.neg {
		return d
	}
	return &Dec{coef: d.coef, exp: d.exp}
}

// numDigits returns the number of decimal digits in the coefficient.
func numDigits(x *big.Int) int {
	if x.Sign() == 0 {
		return 1
	}
	return len(x.Text(10))
}

func pow10(n int) *big.Int {
	return new(big.Int).Exp(bigTen, big.NewInt(int64(n)), nil)
}

// align returns the two coefficients scaled to a common (minimum) exponent.
func align(a, b *Dec) (ca, cb *big.Int, exp int) {
	if a.exp == b.exp {
		return a.coef, b.coef, a.exp
	}
	if a.exp > b.exp {
		return new(big.Int).Mul(a.coef, pow10(a.exp-b.exp)), b.coef, b.exp
	}
	return a.coef, new(big.Int).Mul(b.coef, pow10(b.exp-a.exp)), a.exp
}

// roundTo rounds d to at most `digits` significant digits, half-even.
func (d *Dec) roundTo(digits int) *Dec {
	if digits <= 0 {
		digits = DefaultDigits
	}
	nd := numDigits(d.coef)
	if nd <= digits {
		return d
	}
	drop := nd - digits
	return d.reduceExp(d.exp+drop, digits)
}

// reduceExp rounds d so its exponent becomes targetExp (>= d.exp), half-even.
func (d *Dec) reduceExp(targetExp, _ int) *Dec {
	drop := targetExp - d.exp
	if drop <= 0 {
		return d
	}
	div := pow10(drop)
	q, r := new(big.Int).QuoRem(d.coef, div, new(big.Int))
	// Half-even: compare 2r with divisor.
	r2 := new(big.Int).Lsh(r, 1)
	switch r2.Cmp(div) {
	case 1:
		q.Add(q, big.NewInt(1))
	case 0:
		if q.Bit(0) == 1 {
			q.Add(q, big.NewInt(1))
		}
	}
	res := &Dec{neg: d.neg, coef: q, exp: targetExp}
	if res.coef.Sign() == 0 {
		res.neg = false
	}
	return res
}

// Add returns a+b rounded to the digits context.
func Add(a, b *Dec, digits int) *Dec {
	ca, cb, exp := align(a, b)
	sa, sb := ca, cb
	if a.neg {
		sa = new(big.Int).Neg(ca)
	}
	if b.neg {
		sb = new(big.Int).Neg(cb)
	}
	sum := new(big.Int).Add(sa, sb)
	res := &Dec{neg: sum.Sign() < 0, coef: new(big.Int).Abs(sum), exp: exp}
	return res.roundTo(digits)
}

// Sub returns a-b rounded to the digits context.
func Sub(a, b *Dec, digits int) *Dec { return Add(a, b.Neg(), digits) }

// Mul returns a*b rounded to the digits context.
func Mul(a, b *Dec, digits int) *Dec {
	res := &Dec{
		neg:  a.neg != b.neg,
		coef: new(big.Int).Mul(a.coef, b.coef),
		exp:  a.exp + b.exp,
	}
	if res.coef.Sign() == 0 {
		res.neg = false
		res.exp = 0
	}
	return res.roundTo(digits)
}

// Div returns a/b to `digits` significant digits, half-even.
// Division by zero returns an error.
func Div(a, b *Dec, digits int) (*Dec, error) {
	if b.IsZero() {
		return nil, fmt.Errorf("division by zero")
	}
	if a.IsZero() {
		return Zero(), nil
	}
	if digits <= 0 {
		digits = DefaultDigits
	}
	// Scale the dividend so the integer quotient carries digits+1 digits,
	// then round the extra digit away.
	shift := digits + numDigits(b.coef) - numDigits(a.coef) + 1
	num := new(big.Int).Set(a.coef)
	exp := a.exp - b.exp
	if shift > 0 {
		num.Mul(num, pow10(shift))
		exp -= shift
	}
	q, r := new(big.Int).QuoRem(num, b.coef, new(big.Int))
	// Round half-even on the remainder.
	r2 := new(big.Int).Lsh(r, 1)
	switch r2.Cmp(b.coef) {
	case 1:
		q.Add(q, big.NewInt(1))
	case 0:
		if q.Bit(0) == 1 {
			q.Add(q, big.NewInt(1))
		}
	}
	res := &Dec{neg: a.neg != b.neg, coef: q, exp: exp}
	if res.coef.Sign() == 0 {
		res.neg = false
	}
	res = res.roundTo(digits)
	return res.normalizeZeros(), nil
}

// normalizeZeros strips trailing zero digits produced by division scaling
// (keeps exact results tidy: 59.97, not 59.9700000…).
func (d *Dec) normalizeZeros() *Dec {
	if d.coef.Sign() == 0 {
		return &Dec{coef: new(big.Int)}
	}
	coef := new(big.Int).Set(d.coef)
	exp := d.exp
	q, r := new(big.Int), new(big.Int)
	for exp < 0 {
		q.QuoRem(coef, bigTen, r)
		if r.Sign() != 0 {
			break
		}
		coef.Set(q)
		exp++
	}
	return &Dec{neg: d.neg, coef: coef, exp: exp}
}

// FloorDiv returns the floor of a/b as an integral Dec.
func FloorDiv(a, b *Dec) (*Dec, error) {
	if b.IsZero() {
		return nil, fmt.Errorf("division by zero")
	}
	ca, cb, _ := align(a, b)
	sa, sb := new(big.Int).Set(ca), new(big.Int).Set(cb)
	if a.neg {
		sa.Neg(sa)
	}
	if b.neg {
		sb.Neg(sb)
	}
	q := new(big.Int)
	m := new(big.Int)
	q.DivMod(sa, sb, m) // Euclidean; adjust to floor semantics
	if m.Sign() != 0 && sb.Sign() < 0 {
		q.Sub(q, big.NewInt(1))
	}
	res := &Dec{neg: q.Sign() < 0, coef: new(big.Int).Abs(q)}
	return res, nil
}

// Mod returns a - floor(a/b)*b (floor-mod, pairing with FloorDiv).
func Mod(a, b *Dec, digits int) (*Dec, error) {
	q, err := FloorDiv(a, b)
	if err != nil {
		return nil, err
	}
	return Sub(a, Mul(q, b, 0), digits), nil
}

// Pow raises d to an integer power (negative allowed, uses Div).
func Pow(d *Dec, n int64, digits int) (*Dec, error) {
	if n == 0 {
		return New(1, 0), nil
	}
	invert := n < 0
	if invert {
		n = -n
	}
	result := New(1, 0)
	base := d
	for n > 0 {
		if n&1 == 1 {
			result = Mul(result, base, digits)
		}
		base = Mul(base, base, digits)
		n >>= 1
	}
	if invert {
		return Div(New(1, 0), result, digits)
	}
	return result, nil
}

// Cmp compares a and b: -1, 0, +1.
func Cmp(a, b *Dec) int {
	sa, sb := a.Sign(), b.Sign()
	if sa != sb {
		if sa < sb {
			return -1
		}
		return 1
	}
	if sa == 0 {
		return 0
	}
	ca, cb, _ := align(a, b)
	c := ca.Cmp(cb)
	if a.neg {
		c = -c
	}
	return c
}

// RoundPlaces rounds to n decimal places (exp = -n), half-even.
func (d *Dec) RoundPlaces(n int) *Dec {
	if d.exp >= -n {
		return d
	}
	return d.reduceExp(-n, 0)
}

// Floor returns the largest integral Dec <= d.
func (d *Dec) Floor() *Dec {
	if d.exp >= 0 {
		return d
	}
	div := pow10(-d.exp)
	q, r := new(big.Int).QuoRem(d.coef, div, new(big.Int))
	if d.neg && r.Sign() != 0 {
		q.Add(q, big.NewInt(1))
	}
	return &Dec{neg: d.neg, coef: q}
}

// Ceil returns the smallest integral Dec >= d.
func (d *Dec) Ceil() *Dec { return d.Neg().Floor().Neg() }

// Trunc drops the fraction toward zero.
func (d *Dec) Trunc() *Dec {
	if d.exp >= 0 {
		return d
	}
	q := new(big.Int).Quo(d.coef, pow10(-d.exp))
	res := &Dec{neg: d.neg, coef: q}
	if q.Sign() == 0 {
		res.neg = false
	}
	return res
}

// IsInt reports whether d has no fractional part.
func (d *Dec) IsInt() bool {
	if d.exp >= 0 {
		return true
	}
	r := new(big.Int).Rem(d.coef, pow10(-d.exp))
	return r.Sign() == 0
}

// Int64 converts if exact and in range.
func (d *Dec) Int64() (int64, bool) {
	if !d.IsInt() {
		return 0, false
	}
	v := new(big.Int).Set(d.coef)
	if d.exp > 0 {
		v.Mul(v, pow10(d.exp))
	} else if d.exp < 0 {
		v.Quo(v, pow10(-d.exp))
	}
	if d.neg {
		v.Neg(v)
	}
	if !v.IsInt64() {
		return 0, false
	}
	return v.Int64(), true
}

// Float64 converts (possibly losing precision — callers gate this per §3.2).
func (d *Dec) Float64() float64 {
	f, _ := new(big.Float).SetString(d.String())
	v, _ := f.Float64()
	return v
}

// String renders in plain notation, using exponential form only for extreme
// exponents (REXX-flavoured).
func (d *Dec) String() string {
	n := d.normalizeZeros()
	digits := n.coef.Text(10)
	sign := ""
	if n.neg {
		sign = "-"
	}
	adj := n.exp + len(digits) - 1 // adjusted exponent
	if n.exp <= 0 && adj >= -6 {
		// Plain notation.
		point := len(digits) + n.exp
		switch {
		case n.exp == 0:
			return sign + digits
		case point > 0:
			return sign + digits[:point] + "." + digits[point:]
		default:
			return sign + "0." + strings.Repeat("0", -point) + digits
		}
	}
	if n.exp > 0 {
		// Plain notation for anything a digits-34 context can hold exactly.
		if adj <= 33 {
			return sign + digits + strings.Repeat("0", n.exp)
		}
	}
	// Scientific.
	mant := digits[:1]
	if len(digits) > 1 {
		mant += "." + digits[1:]
	}
	return fmt.Sprintf("%s%sE%+d", sign, mant, adj)
}

// FormatFixed renders with exactly `places` digits after the point,
// rounding half-even (backs fmt's %.Nf on dec, §13.1). A negative width is a
// caller error and is clamped rather than indexed out of range.
func (d *Dec) FormatFixed(places int) string {
	if places < 0 {
		places = 0
	}
	r := d.RoundPlaces(places)
	// Scale so exp == -places exactly.
	coef := new(big.Int).Set(r.coef)
	if r.exp > -places {
		coef.Mul(coef, pow10(r.exp+places))
	}
	digits := coef.Text(10)
	sign := ""
	if r.neg && coef.Sign() != 0 {
		sign = "-"
	}
	if places == 0 {
		return sign + digits
	}
	for len(digits) <= places {
		digits = "0" + digits
	}
	point := len(digits) - places
	return sign + digits[:point] + "." + digits[point:]
}
