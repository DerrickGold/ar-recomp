//go:build !windows

package main

func requireWebview2(string) error { return nil }
