#!/bin/sh
set -eu
cd "$(dirname "$0")/../.."
python3 tests/bw16/run.py
python3 tests/bw16/run_legacy_guards.py
