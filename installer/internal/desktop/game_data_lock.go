package desktop

// LockGameData serializes short publications into the selected game output
// with other processes initializing that same output.
func LockGameData(root string) (func(), error) {
	return lockInitialization(root)
}
