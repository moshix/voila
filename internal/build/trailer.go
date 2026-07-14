// Package build implements `voila build`: the program is packed into a copy
// of the running voila executable. Instead of appending a trailer (which
// breaks Mach-O strict validation on macOS), the toolchain binary carries a
// 2 MiB zero-filled payload SLOT via go:embed; `voila build` patches the
// program into the slot of the copied binary — the file structure is
// untouched, so the copy re-signs cleanly — and the built binary finds its
// program in memory at startup with zero I/O. Run and build share one
// engine, so the Equivalence Guarantee (§12) holds by construction.
package build

import (
	"bytes"
	_ "embed"
	"encoding/binary"
	"encoding/gob"
	"fmt"
	"io"
	"os"
	"os/exec"
	"runtime"
)

//go:embed slotdata/slot.bin
var slot []byte

// slotMarker is decoded at runtime (XOR-masked) so the plaintext marker
// appears in the binary exactly once — inside the embedded slot. A folded
// string constant would create a second match and the patcher would write
// into the wrong place.
func slotMarker() []byte {
	masked := []byte{
		0x0c, 0x15, 0x13, 0x16, 0x1b, 0x77, 0x0a, 0x1b, 0x03, 0x16, 0x15,
		0x1b, 0x1e, 0x77, 0x09, 0x16, 0x15, 0x0e, 0x77, 0x63, 0x3c, 0x69,
		0x3e, 0x68, 0x39, 0x6b, 0x38,
	}
	out := make([]byte, len(masked))
	for i, b := range masked {
		out[i] = b ^ 0x5a
	}
	return out
}

// Program is the embedded payload.
type Program struct {
	Name   string // original source file name (for diagnostics)
	Source string // full source text
}

// WriteEmbedded copies the current executable to outPath and patches the
// gob-encoded program into the payload slot.
func WriteEmbedded(selfPath, outPath string, prog Program) error {
	self, err := os.Open(selfPath)
	if err != nil {
		return fmt.Errorf("open self: %w", err)
	}
	data, err := io.ReadAll(self)
	self.Close()
	if err != nil {
		return fmt.Errorf("read runtime: %w", err)
	}

	marker := slotMarker()
	idx := bytes.Index(data, marker)
	if idx < 0 {
		return fmt.Errorf("payload slot not found in %s (is this the voila toolchain binary?)", selfPath)
	}

	var payload bytes.Buffer
	if err := gob.NewEncoder(&payload).Encode(prog); err != nil {
		return fmt.Errorf("encode program: %w", err)
	}
	capacity := len(slot) - len(marker) - 8
	if payload.Len() > capacity {
		return fmt.Errorf("program too large: %d bytes (slot capacity %d)", payload.Len(), capacity)
	}

	at := idx + len(marker)
	binary.LittleEndian.PutUint64(data[at:at+8], uint64(payload.Len()))
	copy(data[at+8:], payload.Bytes())

	if err := os.WriteFile(outPath, data, 0o755); err != nil {
		return fmt.Errorf("write %s: %w", outPath, err)
	}

	// The patch invalidated the original signature; the structure is intact,
	// so an ad-hoc re-sign succeeds (required on arm64 macOS).
	if runtime.GOOS == "darwin" {
		cmd := exec.Command("codesign", "-s", "-", "-f", outPath)
		if outp, err := cmd.CombinedOutput(); err != nil {
			return fmt.Errorf("codesign failed: %v\n%s", err, outp)
		}
	}
	return nil
}

// ReadEmbedded reports the program embedded in THIS process's slot, if any.
// No file access: the patched bytes are already in memory via go:embed.
func ReadEmbedded(_ string) (Program, bool) {
	marker := slotMarker()
	if len(slot) < len(marker)+8 {
		return Program{}, false
	}
	at := len(marker)
	plen := binary.LittleEndian.Uint64(slot[at : at+8])
	if plen == 0 || int(plen) > len(slot)-at-8 {
		return Program{}, false
	}
	var prog Program
	if err := gob.NewDecoder(bytes.NewReader(slot[at+8 : at+8+int(plen)])).Decode(&prog); err != nil {
		return Program{}, false
	}
	return prog, true
}
