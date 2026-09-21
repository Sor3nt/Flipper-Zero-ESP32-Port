#!/usr/bin/env python3
"""Extract fap_private_libs build info from an application.fam.

buildFap.sh compiles every .c it finds under the app dir. That breaks apps
that vendor a large library but only build a subset of it via
`fap_private_libs=[Lib(name=..., sources=[...])]` (e.g. the authenticator app
bundles all of wolfssl but builds only 7 wolfcrypt files). fbt honors each
Lib's `sources`/`cflags`/`cdefines`/`cincludes`; this helper exposes the same
info to the shell builder.

The .fam is plain Python, so we exec it with stub App()/Lib()/ExtFile()/
FlipperAppType definitions and read back the first non-plugin App.

Output (one directive per line, paths relative to the app dir):
    LIBDIR  <name>                  one per private lib (exclude from glob)
    SOURCE  lib/<name>/<file>       resolved source to compile
    CDEFINE <NAME[=val]>            aggregated lib cdefines
    CINCLUDE <path>                 aggregated lib cincludes (app-dir relative)
    CFLAG   <flag>                  aggregated lib cflags
"""
import sys
import glob
import os


class _AnyEnum:
    """Stand-in for FlipperAppType.* — any attribute access returns a marker."""

    def __getattr__(self, name):
        return f"FlipperAppType.{name}"


def main():
    # buildFap.sh reads our output line-by-line with bash's `read` via
    # IFS=$'\t'. On Windows, Python's text-mode stdout translates "\n" to
    # "\r\n"; `read` splits on the LF but leaves the CR attached to the last
    # field, which then silently fails string/path comparisons downstream
    # (e.g. `find -path` never matching). Force LF-only output.
    sys.stdout.reconfigure(newline="\n")
    if len(sys.argv) != 2:
        sys.stderr.write("usage: fap_lib_info.py <app_dir>\n")
        return 2
    app_dir = sys.argv[1]
    fam_path = os.path.join(app_dir, "application.fam")
    if not os.path.isfile(fam_path):
        return 0  # nothing to do; caller falls back to plain glob

    apps = []

    def App(**kw):
        apps.append(kw)

    def Lib(**kw):
        # Mirror appmanifest.py Library defaults.
        kw.setdefault("sources", ["*.c*"])
        kw.setdefault("cflags", [])
        kw.setdefault("cdefines", [])
        kw.setdefault("cincludes", [])
        return kw

    def ExtFile(**kw):
        return kw

    g = {
        "App": App,
        "Lib": Lib,
        "ExtFile": ExtFile,
        "FlipperAppType": _AnyEnum(),
    }
    with open(fam_path, "r", encoding="utf-8") as f:
        exec(compile(f.read(), fam_path, "exec"), g)

    # The main app is the one that carries fap_private_libs (plugins don't).
    main_app = None
    for a in apps:
        if a.get("fap_private_libs"):
            main_app = a
            break
    if main_app is None:
        return 0

    out = []
    for lib in main_app["fap_private_libs"]:
        name = lib["name"]
        lib_root = os.path.join(app_dir, "lib", name)
        out.append(f"LIBDIR\t{name}")
        for pat in lib["sources"]:
            matches = sorted(glob.glob(os.path.join(lib_root, pat)))
            for m in matches:
                # buildFap.sh flattens object filenames by replacing "/" with
                # "_"; os.path.relpath() uses the platform separator, which is
                # "\" on Windows, so normalize to "/" here or those backslash
                # segments survive as literal (nonexistent) subdirectories.
                rel = os.path.relpath(m, app_dir).replace(os.sep, "/")
                out.append(f"SOURCE\t{rel}")
        for d in lib["cdefines"]:
            out.append(f"CDEFINE\t{d}")
        for inc in lib["cincludes"]:
            out.append(f"CINCLUDE\t{inc}")
        for fl in lib["cflags"]:
            out.append(f"CFLAG\t{fl}")

    sys.stdout.write("\n".join(out) + ("\n" if out else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
