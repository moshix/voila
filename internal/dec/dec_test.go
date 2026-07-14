package dec

import "testing"

func p(t *testing.T, s string) *Dec {
	t.Helper()
	d, err := Parse(s)
	if err != nil {
		t.Fatalf("parse %q: %v", s, err)
	}
	return d
}

func TestParseAndString(t *testing.T) {
	cases := map[string]string{
		"19.99":   "19.99",
		"3":       "3",
		"0.1":     "0.1",
		"-4.50":   "-4.5",
		"0":       "0",
		"6.02e23": "602000000000000000000000",
		"1.5e-8":  "1.5E-8", // REXX convention: exponential below adj -6
		"0.000":   "0",
		"00042":   "42",
		"9.99e-7": "9.99E-7",
	}
	for in, want := range cases {
		if got := p(t, in).String(); got != want {
			t.Errorf("Parse(%q).String() = %q, want %q", in, got, want)
		}
	}
}

func TestExactArithmetic(t *testing.T) {
	// The spec's marquee example: 19.99 * 3 == 59.97 exactly (§3.1).
	price := p(t, "19.99")
	qty := p(t, "3")
	got := Mul(price, qty, DefaultDigits)
	if got.String() != "59.97" {
		t.Fatalf("19.99 * 3 = %s, want 59.97", got)
	}
	// 0.1 + 0.2 == 0.3 exactly.
	sum := Add(p(t, "0.1"), p(t, "0.2"), DefaultDigits)
	if sum.String() != "0.3" {
		t.Fatalf("0.1 + 0.2 = %s", sum)
	}
	diff := Sub(p(t, "1"), p(t, "0.9"), DefaultDigits)
	if diff.String() != "0.1" {
		t.Fatalf("1 - 0.9 = %s", diff)
	}
}

func TestDivision(t *testing.T) {
	q, err := Div(p(t, "1"), p(t, "3"), 10)
	if err != nil {
		t.Fatal(err)
	}
	if q.String() != "0.3333333333" {
		t.Fatalf("1/3 @10 = %s", q)
	}
	q, _ = Div(p(t, "10"), p(t, "4"), 28)
	if q.String() != "2.5" {
		t.Fatalf("10/4 = %s", q)
	}
	q, _ = Div(p(t, "2"), p(t, "3"), 5)
	if q.String() != "0.66667" {
		t.Fatalf("2/3 @5 = %s", q)
	}
	if _, err := Div(p(t, "1"), Zero(), 28); err == nil {
		t.Fatal("expected division-by-zero error")
	}
}

func TestHalfEven(t *testing.T) {
	// §13.1: %.2f on 19.995d rounds half-even to 20.00.
	if got := p(t, "19.995").FormatFixed(2); got != "20.00" {
		t.Fatalf("19.995 @2 = %q, want 20.00", got)
	}
	if got := p(t, "19.985").FormatFixed(2); got != "19.98" {
		t.Fatalf("19.985 @2 = %q, want 19.98 (half-even)", got)
	}
	if got := p(t, "2.5").FormatFixed(0); got != "2" {
		t.Fatalf("2.5 @0 = %q, want 2", got)
	}
	if got := p(t, "3.5").FormatFixed(0); got != "4" {
		t.Fatalf("3.5 @0 = %q, want 4", got)
	}
	if got := p(t, "-19.995").FormatFixed(2); got != "-20.00" {
		t.Fatalf("-19.995 @2 = %q", got)
	}
	if got := p(t, "7").FormatFixed(3); got != "7.000" {
		t.Fatalf("7 @3 = %q", got)
	}
	if got := p(t, "0.005").FormatFixed(2); got != "0.00" {
		t.Fatalf("0.005 @2 = %q (half-even to even 0)", got)
	}
}

func TestDigitsContext(t *testing.T) {
	// numeric digits 5: results rounded to 5 significant digits.
	got := Mul(p(t, "123.456"), p(t, "1000"), 5)
	if got.String() != "123460" {
		t.Fatalf("123.456*1000 @5 = %s", got)
	}
	// Default 28 keeps far more.
	got = Mul(p(t, "123.456"), p(t, "1000"), 28)
	if got.String() != "123456" {
		t.Fatalf("123.456*1000 @28 = %s", got)
	}
	// Addition rounding at small digits.
	got = Add(p(t, "1.2345"), p(t, "0.00001"), 5)
	if got.String() != "1.2345" {
		t.Fatalf("@5 add = %s", got)
	}
}

func TestCmp(t *testing.T) {
	if Cmp(p(t, "1.5"), p(t, "1.50")) != 0 {
		t.Fatal("1.5 != 1.50")
	}
	if Cmp(p(t, "-2"), p(t, "1")) != -1 {
		t.Fatal("-2 < 1 failed")
	}
	if Cmp(p(t, "0.30"), p(t, "0.3")) != 0 {
		t.Fatal("0.30 != 0.3")
	}
	if Cmp(p(t, "10"), p(t, "9.999999")) != 1 {
		t.Fatal("10 > 9.999999 failed")
	}
}

func TestFloorCeilTrunc(t *testing.T) {
	if got := p(t, "3.7").Floor().String(); got != "3" {
		t.Fatalf("floor 3.7 = %s", got)
	}
	if got := p(t, "-3.7").Floor().String(); got != "-4" {
		t.Fatalf("floor -3.7 = %s", got)
	}
	if got := p(t, "3.2").Ceil().String(); got != "4" {
		t.Fatalf("ceil 3.2 = %s", got)
	}
	if got := p(t, "-3.9").Trunc().String(); got != "-3" {
		t.Fatalf("trunc -3.9 = %s", got)
	}
}

func TestFloorDivMod(t *testing.T) {
	q, _ := FloorDiv(p(t, "17"), p(t, "5"))
	if q.String() != "3" {
		t.Fatalf("17//5 = %s", q)
	}
	q, _ = FloorDiv(p(t, "-17"), p(t, "5"))
	if q.String() != "-4" {
		t.Fatalf("-17//5 = %s", q)
	}
	m, _ := Mod(p(t, "-17"), p(t, "5"), 28)
	if m.String() != "3" {
		t.Fatalf("-17 mod 5 = %s (floor-mod)", m)
	}
}

func TestPow(t *testing.T) {
	got, _ := Pow(p(t, "2"), 10, 28)
	if got.String() != "1024" {
		t.Fatalf("2**10 = %s", got)
	}
	got, _ = Pow(p(t, "1.05"), 3, 28)
	if got.String() != "1.157625" {
		t.Fatalf("1.05**3 = %s", got)
	}
	got, _ = Pow(p(t, "2"), -2, 28)
	if got.String() != "0.25" {
		t.Fatalf("2**-2 = %s", got)
	}
}

func TestInt64(t *testing.T) {
	if v, ok := p(t, "42").Int64(); !ok || v != 42 {
		t.Fatalf("42: %v %v", v, ok)
	}
	if v, ok := p(t, "4.20e2").Int64(); !ok || v != 420 {
		t.Fatalf("420: %v %v", v, ok)
	}
	if _, ok := p(t, "3.5").Int64(); ok {
		t.Fatal("3.5 should not convert")
	}
}

func TestLargePrecision(t *testing.T) {
	// 31-digit exact totals (§14.2 uses numeric digits 31).
	a := p(t, "9999999999999999999999999999.99")
	b := p(t, "0.01")
	got := Add(a, b, 31)
	if got.String() != "10000000000000000000000000000" {
		t.Fatalf("31-digit add = %s", got)
	}
}
