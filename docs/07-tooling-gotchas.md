[← Back to index](README.md)

# Tooling / Environment Gotchas

Save yourself the debugging time — these are environment traps, not firmware
bugs.

- **`IDF_PATH` mismatch breaks the existing `build/` directory.** After
  sourcing the ESP-IDF PowerShell profile, explicitly re-set `$env:IDF_PATH`
  to the IDF copy the `build/` dir was actually configured against — the
  profile may point at a *different* IDF install than the one used to
  originally configure `build/`, and building with the wrong one produces a
  confusing "bootloader does not match the source" error.
- **Never truncate-then-write a source file with a raw Python
  `open(path, "w")` on Windows without `encoding="utf-8"`.** Python's default
  encoding here is cp1252; if the file contains any non-cp1252 character
  (e.g. a `→` in a comment), `write()` raises *after* `open("w")` has already
  truncated the file to 0 bytes — and if the file was uncommitted at the
  time, that is not recoverable from git. Prefer an actual editing tool for
  source files. If a script must rewrite a file, build the complete new text
  first, write it to a temp file, and `os.replace()` it into place — never
  open the real target in truncating mode before the new content is fully
  computed.
- **Line endings are CRLF** (git `autocrlf`) across most of this tree; a raw
  Python string-replace written with bare `\n` patterns will not match CRLF
  files. A proper diff/edit tool handles this automatically; a hand-rolled
  script needs to account for it explicitly.
- A full ESP-IDF toolchain (`idf.py`/`ninja`) isn't always available in every
  review/CI environment used on this project. A syntax-only check against
  the **real project toolchain** is still possible and meaningfully better
  than nothing: take a compile command from `build/compile_commands.json`,
  strip the output/dependency flags, swap `-c` for `-fsyntax-only`, and run
  it as a single raw command string (not a tokenized argv list — the real
  commands contain escaped-quote defines that a naive shlex/regex split
  mangles).
