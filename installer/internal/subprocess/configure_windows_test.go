package subprocess

import (
	"os/exec"
	"syscall"
	"testing"
)

func TestConsolePolicyPreservesOtherAttributes(t *testing.T) {
	for _, hasConsole := range []bool{false, true} {
		cmd := exec.Command("not-executed")
		cmd.SysProcAttr = &syscall.SysProcAttr{CreationFlags: 0x200}
		configure(cmd, hasConsole)
		expected := uint32(0x200)
		if !hasConsole {
			expected |= 0x08000000
		}
		if cmd.SysProcAttr.CreationFlags != expected || cmd.SysProcAttr.HideWindow {
			t.Fatalf("console=%v: %+v", hasConsole, cmd.SysProcAttr)
		}
	}
	cmd := exec.Command("not-executed")
	configure(cmd, false)
	if cmd.SysProcAttr == nil || cmd.SysProcAttr.CreationFlags != 0x08000000 {
		t.Fatal("background child can allocate a console")
	}
}
