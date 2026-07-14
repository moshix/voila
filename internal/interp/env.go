package interp

import "voila/internal/diag"

// Binding is one named slot. Moved bindings poison later use (§6.1/§6.4).
type Binding struct {
	Val      Value
	Mut      bool
	Moved    bool
	MovedPos diag.Pos
}

type Env struct {
	parent *Env
	vars   map[string]*Binding
}

func NewEnv(parent *Env) *Env {
	return &Env{parent: parent, vars: map[string]*Binding{}}
}

func (e *Env) Define(name string, v Value, mut bool) *Binding {
	b := &Binding{Val: v, Mut: mut}
	if name != "_" {
		e.vars[name] = b
	}
	return b
}

func (e *Env) Lookup(name string) *Binding {
	for env := e; env != nil; env = env.parent {
		if b, ok := env.vars[name]; ok {
			return b
		}
	}
	return nil
}
