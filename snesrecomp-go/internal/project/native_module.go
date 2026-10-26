package project

// A module owns its runner and generated symbols. Its sole default-visible
// entry point is declared in the game's C API; fatal native paths return to
// its owner through sr_native_module_abort rather than aborting the application.
var privateModuleCompileArgs = []string{"-fPIC", "-fvisibility=hidden", "-Dabort=sr_native_module_abort"}

func nativeModuleName(name, targetOS string) string {
	switch targetOS {
	case "windows":
		return name + ".dll"
	case "darwin":
		return name + ".dylib"
	default:
		return name + ".so"
	}
}
