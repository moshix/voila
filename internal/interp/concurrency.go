package interp

import (
	"context"
	"reflect"
	"sync"
	"time"

	"voila/internal/ast"
	"voila/internal/diag"
)

// group implements structured concurrency (§9.1): every spawn belongs to a
// group; the group joins ALL children before exiting; the first failure
// cancels siblings and is delivered at the boundary, later failures attach
// as suppressed (§8.7).
type group struct {
	ctx      context.Context
	cancel   context.CancelFunc
	wg       sync.WaitGroup
	mu       sync.Mutex
	failures []*taskFailure
}

type taskFailure struct {
	err        *ErrVal
	isErrValue bool // came from `try` propagation, not a throw
	task       *Task
}

func (g *group) fail(f *taskFailure) {
	g.mu.Lock()
	first := len(g.failures) == 0
	g.failures = append(g.failures, f)
	g.mu.Unlock()
	if first {
		g.cancel() // first failure cancels the siblings (§9.1)
	}
}

// deliver picks the failure to raise at the group boundary: the first
// unconsumed one, with the rest attached as suppressed.
func (g *group) deliver() *taskFailure {
	g.mu.Lock()
	defer g.mu.Unlock()
	var chosen *taskFailure
	for _, f := range g.failures {
		if f.task != nil && f.task.isConsumed() {
			continue
		}
		if chosen == nil {
			chosen = f
		} else {
			chosen.err.Suppressed = append(chosen.err.Suppressed, f.err)
		}
	}
	return chosen
}

func (tk *Task) isConsumed() bool {
	tk.mu.Lock()
	defer tk.mu.Unlock()
	return tk.consumed
}

func (t *T) execGroup(env *Env, st *ast.GroupStmt) {
	parent := t.ctx
	var g *group
	if st.Timeout != nil {
		d := t.toDuration(t.eval(env, st.Timeout), st.P)
		ctx, cancel := context.WithTimeout(parent, d)
		g = &group{ctx: ctx, cancel: cancel}
	} else {
		ctx, cancel := context.WithCancel(parent)
		g = &group{ctx: ctx, cancel: cancel}
	}
	defer g.cancel()

	savedGroup, savedCtx := t.group, t.ctx
	t.group, t.ctx = g, g.ctx

	// Run the body; even if it panics, join all children first (decision g).
	bodyPanic := func() (r any) {
		defer func() { r = recover() }()
		t.execBlock(env, st.Body)
		return nil
	}()

	if bodyPanic != nil {
		g.cancel()
	}
	g.wg.Wait()
	t.group, t.ctx = savedGroup, savedCtx

	if bodyPanic != nil {
		panic(bodyPanic) // parent-frame control flow wins after join
	}

	// Deadline exceeded → Timeout exception. Cancelled failures from the
	// reaped tasks are a consequence of the timeout, not its cause, so the
	// Timeout wins and they attach as suppressed.
	if st.Timeout != nil && g.ctx.Err() == context.DeadlineExceeded {
		onlyCancelled := true
		for _, f := range g.failures {
			if f.err.TypeName != "Cancelled" {
				onlyCancelled = false
			}
		}
		if onlyCancelled {
			timeout := &taskFailure{err: &ErrVal{TypeName: "Timeout", Msg: "group timed out", Trace: t.captureTrace()}}
			for _, f := range g.failures {
				timeout.err.Suppressed = append(timeout.err.Suppressed, f.err)
			}
			g.failures = []*taskFailure{timeout}
		}
	}

	if f := g.deliver(); f != nil {
		if st.Try {
			panic(ctlPropagate{err: f.err}) // try group → error value (§9.1)
		}
		panic(ctlThrow{f.err}) // plain group rethrows (task error values are must-ed)
	}
}

// evalSpawn launches a task (green thread → goroutine) and returns Task[T].
func (t *T) evalSpawn(env *Env, x *ast.SpawnExpr) Value {
	g := t.group
	if g == nil {
		t.rtErr(x.P, "spawn outside a group (wrap in `group { … }`)")
	}
	task := &Task{done: make(chan struct{})}
	g.wg.Add(1)

	child := &T{
		in:     t.in,
		ctx:    g.ctx,
		group:  g,
		digits: t.digits, // tasks snapshot the numeric context (decision i)
	}

	// Pre-evaluate the callee and args in the PARENT so argument evaluation
	// is not racy: spawn f(x) evaluates f and x now, calls later.
	var run func()
	if x.Call != nil {
		call, ok := x.Call.(*ast.CallExpr)
		if !ok {
			// spawn <expr> where expr isn't a call: evaluate in the task.
			expr := x.Call
			run = func() { task.setResult(child.eval(NewEnv(env), expr)) }
		} else {
			callee := t.eval(env, call.Fun)
			args, named := t.evalArgs(env, call)
			t.applyOwnMoves(env, callee, call)
			pos := call.P
			run = func() { task.setResult(child.callValue(callee, args, named, pos)) }
		}
	} else {
		blk := x.Block
		run = func() {
			v := child.execBody(NewEnv(env), blk)
			if v == nil {
				task.setResult(UnitV{})
			} else {
				task.setResult(v)
			}
		}
	}

	go func() {
		frame := &Frame{name: "task", callPos: x.P}
		child.frames = []*Frame{frame}
		defer g.wg.Done()
		defer close(task.done)
		defer func() {
			r := recover()
			if r == nil {
				if dp := child.collectDefers(frame); dp != nil {
					r = dp
				}
			} else {
				func() {
					defer func() { recover() }()
					child.collectDefers(frame)
				}()
			}
			switch v := r.(type) {
			case nil:
			case ctlThrow:
				task.mu.Lock()
				task.exc = v.err
				task.mu.Unlock()
				g.fail(&taskFailure{err: v.err, task: task})
			case ctlPropagate:
				e, _ := v.err2().(*ErrVal)
				if e == nil {
					e = &ErrVal{Msg: "nil value"}
				}
				task.mu.Lock()
				task.isErr = true
				task.result = e
				task.mu.Unlock()
				g.fail(&taskFailure{err: e, isErrValue: true, task: task})
			case ctlReturn:
				task.setResult(v.val)
			case ctlExit:
				panic(v)
			case ctlAbort:
				// A bug in a task aborts the whole process (§8.6).
				task.mu.Lock()
				task.exc = &ErrVal{Msg: v.msg, Trace: v.trace}
				task.mu.Unlock()
				g.fail(&taskFailure{err: task.exc, task: task})
			}
		}()
		run()
	}()
	return task
}

func (tk *Task) setResult(v Value) {
	tk.mu.Lock()
	tk.result = v
	tk.mu.Unlock()
}

// awaitTask implements task.await(): joins and yields the result; a task
// exception rethrows in the awaiting task; an error-value result returns it.
func (t *T) awaitTask(tk *Task, pos diag.Pos) Value {
	select {
	case <-tk.done:
	case <-t.ctx.Done():
		t.throwf("Cancelled", "task cancelled while awaiting")
	}
	tk.mu.Lock()
	tk.consumed = true
	exc := tk.exc
	res := tk.result
	tk.mu.Unlock()
	if exc != nil {
		panic(ctlThrow{exc})
	}
	if res == nil {
		return UnitV{}
	}
	return res
}

// ---------------------------------------------------------------- channels

func newChan(capacity int) *Chan {
	return &Chan{ch: make(chan Value, capacity), cap: capacity}
}

// chanSend is cancellation-aware: a cancelled task throws Cancelled instead
// of blocking forever (frozen decision g — this is what makes cancel+join
// deadlock-free).
func (t *T) chanSend(ch *Chan, v Value, pos diag.Pos) {
	ch.mu.Lock()
	closed := ch.closed
	ch.mu.Unlock()
	if closed {
		t.throwf("RuntimeError", "send on closed channel")
	}
	defer func() {
		if r := recover(); r != nil {
			if _, ok := r.(ctlThrow); ok {
				panic(r)
			}
			t.throwf("RuntimeError", "send on closed channel")
		}
	}()
	select {
	case ch.ch <- v:
	case <-t.ctx.Done():
		t.throwf("Cancelled", "task cancelled during channel send")
	}
}

// chanRecv returns (value, ok); ok=false when the channel is closed+drained.
func (t *T) chanRecv(ch *Chan, pos diag.Pos) (Value, bool) {
	select {
	case v, ok := <-ch.ch:
		if !ok {
			return NilV{}, false
		}
		return v, true
	case <-t.ctx.Done():
		// Drain-biased: prefer a ready value over cancellation.
		select {
		case v, ok := <-ch.ch:
			if !ok {
				return NilV{}, false
			}
			return v, true
		default:
		}
		t.throwf("Cancelled", "task cancelled during channel receive")
	}
	return NilV{}, false
}

func (t *T) closeChan(ch *Chan, pos diag.Pos) {
	ch.mu.Lock()
	defer ch.mu.Unlock()
	if ch.closed {
		t.throwf("RuntimeError", "close of already-closed channel")
	}
	ch.closed = true
	close(ch.ch)
}

// checkCancelled polls for cooperative cancellation in unbounded loops.
func (t *T) checkCancelled(pos diag.Pos) {
	select {
	case <-t.ctx.Done():
		t.throwf("Cancelled", "task cancelled")
	default:
	}
}

// ---------------------------------------------------------------- select

func (t *T) execSelect(env *Env, st *ast.SelectStmt) {
	var cases []reflect.SelectCase
	var caseIdx []int // maps reflect case → AST case index
	hasDefault := false
	var sendVals []Value

	for i, c := range st.Cases {
		switch c.Kind {
		case ast.SelectDefault:
			hasDefault = true
			cases = append(cases, reflect.SelectCase{Dir: reflect.SelectDefault})
			caseIdx = append(caseIdx, i)
		case ast.SelectRecv:
			ch := t.wantChan(t.eval(env, c.Ch), c.P)
			cases = append(cases, reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(ch.ch)})
			caseIdx = append(caseIdx, i)
			sendVals = append(sendVals, nil)
		case ast.SelectSend:
			ch := t.wantChan(t.eval(env, c.Ch), c.P)
			v := t.eval(env, c.Send)
			cases = append(cases, reflect.SelectCase{Dir: reflect.SelectSend, Chan: reflect.ValueOf(ch.ch), Send: reflect.ValueOf(&v).Elem()})
			caseIdx = append(caseIdx, i)
			sendVals = append(sendVals, v)
		}
	}
	for len(sendVals) < len(cases) {
		sendVals = append(sendVals, nil)
	}

	// Cancellation awareness: with no default, also wait on ctx.done.
	ctxCase := -1
	if !hasDefault && t.ctx.Done() != nil {
		ctxCase = len(cases)
		cases = append(cases, reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(t.ctx.Done())})
	}

	chosen, recvVal, recvOK := reflect.Select(cases)
	if chosen == ctxCase {
		t.throwf("Cancelled", "task cancelled in select")
	}
	c := st.Cases[caseIdx[chosen]]

	child := NewEnv(env)
	switch c.Kind {
	case ast.SelectRecv:
		var v Value = NilV{}
		if recvOK {
			v = recvVal.Interface().(Value)
		}
		if c.Bind != "" {
			child.Define(c.Bind, v, false)
		}
		if c.BindOk != "" {
			child.Define(c.BindOk, BoolV(recvOK), false)
		}
	case ast.SelectSend:
		// Move-on-send applies only to the case that fired (decision f).
		if id, ok := c.Send.(*ast.Ident); ok {
			if v := sendVals[chosen]; v != nil && !isCopy(v) {
				if b := env.Lookup(id.Name); b != nil {
					b.Moved = true
					b.MovedPos = id.P
				}
			}
		}
	}
	for _, s := range c.Body {
		t.execStmt(child, s)
	}
}

func (t *T) wantChan(v Value, pos diag.Pos) *Chan {
	ch, ok := v.(*Chan)
	if !ok {
		t.rtErr(pos, "expected channel, got %s", typeName(v))
	}
	return ch
}

func (t *T) toDuration(v Value, pos diag.Pos) time.Duration {
	switch x := v.(type) {
	case DurV:
		return time.Duration(x)
	case IntV:
		return time.Duration(x) // raw nanoseconds
	}
	t.rtErr(pos, "expected duration, got %s", typeName(v))
	return 0
}
