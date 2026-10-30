package ar2master

import "github.com/DerrickGold/ar-recomp/installer/internal/importstore"

var storage = importstore.Pack{Filename: Filename, MaximumBytes: MaximumBytes, Validate: Validate}

func Status(root string) string                      { return storage.Status(root) }
func Read(root string) ([]byte, error)               { return storage.Read(root) }
func Install(root string, data []byte) (bool, error) { return storage.Install(root, data) }
