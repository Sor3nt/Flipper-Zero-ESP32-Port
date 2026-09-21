#!/usr/bin/env python3
"""Resolve the MAIN app's source list and cdefines from an application.fam.

buildFap.sh normally compiles every .c it finds under the app dir. That breaks
apps whose .fam declares an explicit `sources=[...]` list with fbt glob/exclude
semantics — e.g. protopirate, which builds its protocol decoders as separate
`fal_embedded` PLUGIN apps and EXCLUDES them (and most of protocols/) from the
main app. Blanket-globbing then compiles plugin *template* .c files that #error
without their per-plugin -D defines.

This helper execs the .fam (plain Python) with stub App()/Lib()/ExtFile()/
FlipperAppType and reads back the FIRST non-plugin App, then resolves its
`sources` patterns and emits its `cdefines` so the shell builder can honor them.

fbt `sources` semantics implemented here (order-sensitive):
  "*.c*"              positive glob, basename match anywhere (recursive)
  "dir"               positive: every source under that directory
  "path/to/file.c"    positive: that specific file
  "!<pattern>"        remove everything the pattern matches from the set so far
A later positive entry re-adds files an earlier "!" removed.

Output (paths relative to the app dir, one directive per line):
    APPSOURCE <rel path>            resolved main-app source to compile
    APPCDEFINE <NAME[=val]>         main-app cdefine (add as -D)
    PLUGIN <appid> <entry_point>    one fal_embedded plugin to build as .fal
    PLUGINSRC <appid> <rel path>    a source of that plugin
    PLUGINDEF <appid> <NAME[=val]>  a cdefine of that plugin (as -D)

The main-app APPSOURCE/APPCDEFINE block is emitted only when the manifest has
an explicit (non-"everything") sources list, so callers cleanly fall back to a
plain glob otherwise. PLUGIN blocks are always emitted for fal_embedded plugins.
"""
import sys
import os
import glob
import fnmatch

_SRC_EXTS = (".c", ".cpp", ".cxx", ".cc")
_PRUNE_DIRS = {"tests", "test", ".git"}


class _AnyEnum:
    """Stand-in for FlipperAppType.* — any attribute access returns a marker."""

    def __getattr__(self, name):
        return f"FlipperAppType.{name}"


def _walk_sources(root):
    """Yield source files under `root`, skipping host-only/test/vcs dirs and
    nested app roots (subdirs with their own application.fam — a separate app,
    e.g. a vendored PlatformIO/ESP32 port; mirrors buildFap.sh's glob pruning)."""
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [
            d
            for d in dirnames
            if d not in _PRUNE_DIRS
            and not os.path.isfile(os.path.join(dirpath, d, "application.fam"))
        ]
        for fn in filenames:
            if fn.endswith(_SRC_EXTS):
                yield os.path.join(dirpath, fn)


def _relpath(path, app_dir):
    # buildFap.sh flattens object filenames by replacing "/" with "_";
    # os.path.relpath() uses the platform separator, which is "\" on
    # Windows, so normalize to "/" here or those backslash segments survive
    # as literal (nonexistent) subdirectories.
    return os.path.relpath(path, app_dir).replace(os.sep, "/")


def _match(app_dir, pattern):
    """Return app-dir-relative source files matching one fbt source pattern."""
    full = os.path.join(app_dir, pattern)
    out = []
    if os.path.isdir(full):
        for m in _walk_sources(full):
            out.append(_relpath(m, app_dir))
    elif "/" not in pattern:
        # bare filename (maybe wildcard): match by basename anywhere in the tree
        for m in _walk_sources(app_dir):
            if fnmatch.fnmatch(os.path.basename(m), pattern):
                out.append(_relpath(m, app_dir))
    else:
        for m in glob.glob(full, recursive=True):
            if os.path.isdir(m):
                for f in _walk_sources(m):
                    out.append(_relpath(f, app_dir))
            elif os.path.isfile(m) and m.endswith(_SRC_EXTS):
                out.append(_relpath(m, app_dir))
    return out


def _resolve(app_dir, patterns):
    ordered = []
    present = set()
    for pat in patterns:
        if isinstance(pat, str) and pat.startswith("!"):
            drop = set(_match(app_dir, pat[1:]))
            if drop:
                ordered = [f for f in ordered if f not in drop]
                present = set(ordered)
        else:
            for f in _match(app_dir, pat):
                if f not in present:
                    present.add(f)
                    ordered.append(f)
    return ordered


def main():
    # buildFap.sh reads our output line-by-line with bash's `read` via
    # IFS=$'\t'. On Windows, Python's text-mode stdout translates "\n" to
    # "\r\n"; `read` splits on the LF but leaves the CR attached to the last
    # field, which then silently fails string/path comparisons downstream
    # (e.g. `find -path` never matching). Force LF-only output.
    sys.stdout.reconfigure(newline="\n")
    if len(sys.argv) != 2:
        sys.stderr.write("usage: fap_app_info.py <app_dir>\n")
        return 2
    app_dir = sys.argv[1]
    fam_path = os.path.join(app_dir, "application.fam")
    if not os.path.isfile(fam_path):
        return 0

    apps = []

    def App(**kw):
        apps.append(kw)

    def Lib(**kw):
        return kw

    def ExtFile(**kw):
        return kw

    g = {
        "App": App,
        "Lib": Lib,
        "ExtFile": ExtFile,
        "FlipperAppType": _AnyEnum(),
        # protopirate (and other fbt manifests) reference this global to locate
        # sibling files like defines.h; fbt injects it per-manifest.
        "app_manifest_path": fam_path,
    }
    try:
        with open(fam_path) as f:
            exec(compile(f.read(), fam_path, "exec"), g)
    except Exception as e:  # noqa: BLE001 — any fam error => fall back to glob
        sys.stderr.write(f"fap_app_info: {e}\n")
        return 0

    # First non-plugin App is the main app.
    main_app = None
    for a in apps:
        if "PLUGIN" not in str(a.get("apptype", "")):
            main_app = a
            break
    if main_app is None:
        return 0

    sources = main_app.get("sources")
    # Only override when the manifest asks for something other than "everything".
    trivial = sources is None or sources == ["*.c*"] or sources == ["*.c"]
    out = []
    if not trivial:
        for rel in _resolve(app_dir, sources):
            out.append(f"APPSOURCE\t{rel}")

    # cdefines are emitted whenever the override triggers, so the main app builds
    # with the same feature flags fbt would pass (e.g. PROTOPIRATE_PROTOCOL_RX_ONLY).
    if out:
        for d in main_app.get("cdefines", []) or []:
            out.append(f"APPCDEFINE\t{d}")

    # fal_embedded plugins: each is compiled to a standalone .fal the main app
    # loads at runtime from APP_ASSETS_PATH("plugins/<appid>.fal"). Emit each
    # plugin's entry point, resolved sources and cdefines so the shell builder
    # can (re)build them. cdefines come through fbt with escaped quotes
    # (PP_TX_PROTOCOL_HEADER=\"../x.h\"); unescape to plain quotes so the shell
    # passes a literal string macro to gcc.
    for a in apps:
        if "PLUGIN" not in str(a.get("apptype", "")):
            continue
        if not a.get("fal_embedded"):
            continue
        appid = a.get("appid")
        ep = a.get("entry_point")
        if not appid or not ep:
            continue
        out.append(f"PLUGIN\t{appid}\t{ep}")
        for rel in _resolve(app_dir, a.get("sources", []) or []):
            out.append(f"PLUGINSRC\t{appid}\t{rel}")
        for d in a.get("cdefines", []) or []:
            out.append(f"PLUGINDEF\t{appid}\t{str(d).replace(chr(92) + chr(34), chr(34))}")

    if out:
        sys.stdout.write("\n".join(out) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
