// Package vlrt carries the Voilà C runtime (libvoila) inside the toolchain,
// so `voila build` needs nothing on the machine but a C compiler.
//
// The C sources in src/ are the single copy: the Go toolchain embeds them,
// and the self-hosted toolchain compiles against them directly.
package vlrt

import "embed"

//go:embed src/*.c src/*.h
var FS embed.FS
