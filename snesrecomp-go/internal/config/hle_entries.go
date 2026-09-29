package config

// HLEEntryPoints are function/prefix interceptions, including conditional
// native fallbacks. Incoming edges must enter their wrapper, not absorb the
// ROM body into another region. HLEDispatch remains an instruction-site hook
// and is handled separately by the decoder. Return a fresh map for callers.
func HLEEntryPoints(cfg *Config) map[uint16]struct{} {
	entries := make(map[uint16]struct{})
	if cfg == nil {
		return entries
	}
	for pc := range cfg.HLEFunctions {
		entries[pc] = struct{}{}
	}
	for pc := range cfg.HLEFunctionsIf {
		entries[pc] = struct{}{}
	}
	for _, pc := range cfg.HLESPCUpload {
		entries[pc] = struct{}{}
	}
	return entries
}
