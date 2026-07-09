#!/usr/bin/env bash
# run.sh — convenience wrapper: default args for a dev run.
#
# The per-run artifact ringfencing (runs/<timestamp>/ with console.log,
# run_info.txt, anomaly captures, dumps, runs/latest symlink) is NATIVE —
# src/run_dir.c does it on every invocation, so running the binary directly
#     ./build/ActRaiserRecomp ar.sfc --config dev-config.ini
# captures everything too. This script just saves typing the default args.
# AR_NO_RUN_DIR=1 opts out (legacy flat saves/ layout).
set -u
cd "$(dirname "$0")/.."
[ -x ./build/ActRaiserRecomp ] || { echo "[run] not built"; exit 1; }
if [ "$#" -gt 0 ]; then
  exec ./build/ActRaiserRecomp "$@"
else
  exec ./build/ActRaiserRecomp ar.sfc --config dev-config.ini
fi
