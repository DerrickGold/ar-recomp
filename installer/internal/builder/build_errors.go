package builder

import (
	"errors"
	"syscall"
)

// Match the Windows error number through the exec/path wrappers, so guidance
// also works on non-English Windows. Permission and compiler errors have
// different causes and must not suggest changing Application Control settings.
func buildErrorGuidance(err error, goos string) (code, recovery string) {
	const errorSystemIntegrityPolicyViolation syscall.Errno = 4551
	if goos == "windows" && errors.Is(err, errorSystemIntegrityPolicyViolation) {
		return "builder.errors.windows_app_control", "builder.recovery.windows_app_control"
	}
	return "", ""
}
