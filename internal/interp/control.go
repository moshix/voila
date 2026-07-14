package interp

import (
	"fmt"

	"voila/internal/diag"
)

// Non-local control flow travels as panics with these sentinel types. Go
// panics are used ONLY for these — never for ordinary results.

// ctlReturn unwinds to the enclosing function call.
type ctlReturn struct{ val Value }

// ctlBreak / ctlContinue unwind to the matching loop.
type ctlBreak struct{ label string }
type ctlContinue struct{ label string }

// ctlFallthrough transfers to the next switch case.
type ctlFallthrough struct{}

// ctlPropagate is `try expr` observing an error value: the enclosing
// function returns the Err as its result (§8.1).
type ctlPropagate struct{ err *ErrVal }

// ctlThrow is an in-flight exception (§8.2).
type ctlThrow struct{ err *ErrVal }

// ctlExit is os.exit(n): unwinds everything without running defers.
type ctlExit struct{ code int }

// ctlAbort is a §8.6 abort: assert failure, index out of bounds, throw
// escaping nothrow. Not catchable; user defers do not run.
type ctlAbort struct {
	msg   string
	trace []string
}

// throwValue creates and panics a new exception carrying the current trace.
func (t *T) throw(err *ErrVal) {
	if err.Trace == nil {
		err.Trace = t.captureTrace()
	}
	panic(ctlThrow{err})
}

// throwf raises a built-in exception type with a plain message.
func (t *T) throwf(typeName, format string, args ...any) {
	t.throw(&ErrVal{TypeName: typeName, Msg: fmt.Sprintf(format, args...), Trace: t.captureTrace()})
}

// where renders a position as file:line:col, using the file the position
// itself names — a program spans many files once packages are involved.
func (t *T) where(pos diag.Pos) string {
	name := pos.File
	if name == "" {
		name = t.in.SrcName
	}
	return fmt.Sprintf("%s:%d:%d", name, pos.Line, pos.Col)
}

// rtErr raises a generic RuntimeError exception.
func (t *T) rtErr(pos diag.Pos, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	if pos.IsValid() {
		msg = fmt.Sprintf("%s (%s)", msg, t.where(pos))
	}
	t.throw(&ErrVal{TypeName: "RuntimeError", Msg: msg, Trace: t.captureTrace()})
}

// abort raises an uncatchable abort (§8.6).
func (t *T) abort(pos diag.Pos, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	if pos.IsValid() {
		msg = fmt.Sprintf("%s (%s)", msg, t.where(pos))
	}
	panic(ctlAbort{msg: msg, trace: t.captureTrace()})
}
