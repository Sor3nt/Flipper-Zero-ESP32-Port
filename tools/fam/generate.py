#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from appmanifest import (  # noqa: E402
    AppBuildset,
    AppManager,
    FlipperAppType,
    FlipperApplication,
    FlipperManifestException,
)


INTERNALIZED_EXTERNAL_TYPES = (
    FlipperAppType.EXTERNAL,
    FlipperAppType.MENUEXTERNAL,
    FlipperAppType.EXTSETTINGS,
)


@dataclass
class SimpleDirNode:
    abspath: str

    @property
    def name(self) -> str:
        return os.path.basename(self.abspath)


@dataclass(frozen=True)
class ManifestCandidate:
    priority: int
    root: str
    manifest_path: Path


def load_config(config_path: Path) -> dict:
    namespace: dict = {}
    with config_path.open("rt", encoding="utf-8") as config_file:
        exec(config_file.read(), namespace)
    return namespace


def parse_manifest(manifest_path: Path) -> list[FlipperApplication]:
    apps: list[FlipperApplication] = []
    app_dir = SimpleDirNode(str(manifest_path.parent))

    def App(*args, **kwargs):
        apps.append(
            FlipperApplication(
                *args,
                **kwargs,
                _appdir=app_dir,
                _apppath=str(manifest_path.parent),
            )
        )

    def ExtFile(*args, **kwargs):
        return FlipperApplication.ExternallyBuiltFile(*args, **kwargs)

    def Lib(*args, **kwargs):
        return FlipperApplication.Library(*args, **kwargs)

    namespace = {
        "App": App,
        "ExtFile": ExtFile,
        "Lib": Lib,
        "FlipperAppType": FlipperAppType,
    }
    try:
        with manifest_path.open("rt", encoding="utf-8") as manifest_file:
            exec(manifest_file.read(), namespace)
    except Exception as exc:
        raise FlipperManifestException(f"Failed parsing manifest '{manifest_path}': {exc}") from exc

    if not apps:
        raise FlipperManifestException(f"Manifest '{manifest_path}' did not declare any apps")

    for app in apps:
        app._manifest_path = str(manifest_path)
    return apps


def discover_manifests(project_dir: Path, roots: list[str]) -> list[ManifestCandidate]:
    manifests: list[ManifestCandidate] = []
    for priority, root in enumerate(roots):
        root_path = project_dir / root
        if not root_path.exists():
            continue
        for manifest_path in sorted(root_path.rglob(FlipperApplication.APP_MANIFEST_DEFAULT_NAME)):
            manifests.append(
                ManifestCandidate(priority=priority, root=root, manifest_path=manifest_path)
            )
    return manifests


def load_manifests(
    project_dir: Path, roots: list[str], app_source_overrides: dict[str, str] | None = None
) -> AppManager:
    appmgr = AppManager()
    app_sources: dict[str, tuple[int, str, str]] = {}
    app_source_overrides = app_source_overrides or {}

    for candidate in discover_manifests(project_dir, roots):
        for app in parse_manifest(candidate.manifest_path):
            existing = appmgr.known_apps.get(app.appid)
            if existing is None:
                appmgr.known_apps[app.appid] = app
                app_sources[app.appid] = (
                    candidate.priority,
                    candidate.root,
                    str(candidate.manifest_path),
                )
                continue

            existing_priority, existing_root, existing_manifest = app_sources[app.appid]
            override_root = app_source_overrides.get(app.appid)

            if override_root:
                current_matches_override = candidate.root == override_root
                existing_matches_override = existing_root == override_root
                if current_matches_override and not existing_matches_override:
                    appmgr.known_apps[app.appid] = app
                    app_sources[app.appid] = (
                        candidate.priority,
                        candidate.root,
                        str(candidate.manifest_path),
                    )
                    continue
                if existing_matches_override and not current_matches_override:
                    continue

            if candidate.priority < existing_priority:
                appmgr.known_apps[app.appid] = app
                app_sources[app.appid] = (
                    candidate.priority,
                    candidate.root,
                    str(candidate.manifest_path),
                )
                continue

            if candidate.priority == existing_priority:
                raise FlipperManifestException(
                    f"Duplicate app declaration for '{app.appid}' in '{existing_manifest}' and '{candidate.manifest_path}'"
                )

    return appmgr


def render_icon(icon_name: str | None) -> str:
    return f"&{icon_name}" if icon_name else "NULL"


def render_flags(flags: Iterable[str]) -> str:
    rendered = [f"FlipperInternalApplicationFlag{flag}" for flag in flags]
    return "|".join(rendered) if rendered else "FlipperInternalApplicationFlagDefault"


# ESP32 needs larger stacks than STM32 due to deeper call chains (SPI mutex, FATFS LFN, etc.)
ESP32_MIN_STACK_SIZE = 4096


def render_internal_entry(app: FlipperApplication) -> str:
    stack = max(app.stack_size, ESP32_MIN_STACK_SIZE)
    return (
        "    {"
        f'.app = {app.entry_point}, '
        f'.name = "{app.name}", '
        f'.appid = "{app.appid}", '
        f".stack_size = {stack}, "
        f".icon = {render_icon(app.icon)}, "
        f".flags = {render_flags(app.flags)}"
        "}"
    )


def render_external_entry(app: FlipperApplication) -> str:
    return (
        "    {"
        f'.name = "{app.name}", '
        f".icon = {render_icon(app.icon)}, "
        f'.path = "{app.appid}"'
        "}"
    )


def sorted_unique_apps(apps: Iterable[FlipperApplication]) -> list[FlipperApplication]:
    by_appid: dict[str, FlipperApplication] = {}
    for app in apps:
        if app.appid not in by_appid:
            by_appid[app.appid] = app
    return sorted(by_appid.values(), key=lambda app: (app.order, app.appid))


def apps_of_type(apps: Iterable[FlipperApplication], app_type: FlipperAppType) -> list[FlipperApplication]:
    return sorted_unique_apps(app for app in apps if app.apptype == app_type)


def generate_registry(buildset: AppBuildset, autorun_app: str) -> str:
    services = buildset.get_apps_of_type(FlipperAppType.SERVICE)
    startup_hooks = buildset.get_apps_of_type(FlipperAppType.STARTUP)
    internal_external_apps = buildset.get_apps_of_type(FlipperAppType.EXTERNAL)
    internalized_external = sorted_unique_apps(
        app
        for app_type in INTERNALIZED_EXTERNAL_TYPES
        for app in buildset.get_apps_of_type(app_type)
    )
    system_apps = sorted_unique_apps(
        list(buildset.get_apps_of_type(FlipperAppType.SYSTEM)) + internalized_external
    )
    main_apps = buildset.get_apps_of_type(FlipperAppType.APP)
    settings_apps = buildset.get_apps_of_type(FlipperAppType.SETTINGS)
    debug_apps = buildset.get_apps_of_type(FlipperAppType.DEBUG)
    archive_apps = buildset.get_apps_of_type(FlipperAppType.ARCHIVE)
    menu_external_apps = buildset.get_apps_of_type(FlipperAppType.MENUEXTERNAL)
    settings_external_apps = buildset.get_apps_of_type(FlipperAppType.EXTSETTINGS)

    contents = [
        '#include "applications.h"',
        "#include <assets_icons.h>",
        "",
        f'const char* FLIPPER_AUTORUN_APP_NAME = "{autorun_app}";',
        "",
    ]

    internal_forwarded = sorted_unique_apps(
        services
        + main_apps
        + system_apps
        + settings_apps
        + debug_apps
        + archive_apps
    )
    for app in internal_forwarded:
        contents.append(f"extern int32_t {app.entry_point}(void* p);")
    for app in startup_hooks:
        contents.append(f"extern void {app.entry_point}(void);")
    contents.append("")

    registry_sections = (
        ("FlipperInternalApplication", "FLIPPER_SERVICES", services),
        ("FlipperInternalApplication", "FLIPPER_APPS", main_apps),
        ("FlipperInternalOnStartHook", "FLIPPER_ON_SYSTEM_START", startup_hooks),
        ("FlipperInternalApplication", "FLIPPER_SYSTEM_APPS", system_apps),
        ("FlipperInternalApplication", "FLIPPER_DEBUG_APPS", debug_apps),
        ("FlipperInternalApplication", "FLIPPER_SETTINGS_APPS", settings_apps),
    )

    for entry_type, entry_name, apps in registry_sections:
        contents.append(f"const {entry_type} {entry_name}[] = {{")
        if entry_type == "FlipperInternalOnStartHook":
            contents.extend(f"    {app.entry_point}," for app in apps)
        else:
            contents.extend(render_internal_entry(app) + "," for app in apps)
        contents.append("};")
        contents.append(f"const size_t {entry_name}_COUNT = COUNT_OF({entry_name});")
        contents.append("")

    if archive_apps:
        contents.append(
            f"const FlipperInternalApplication FLIPPER_ARCHIVE = {render_internal_entry(archive_apps[0])};"
        )
    else:
        contents.append("const FlipperInternalApplication FLIPPER_ARCHIVE = {0};")
    contents.append("")

    for entry_name, apps in (
        ("FLIPPER_EXTSETTINGS_APPS", settings_external_apps),
        ("FLIPPER_EXTERNAL_APPS", menu_external_apps),
        ("FLIPPER_INTERNAL_EXTERNAL_APPS", internal_external_apps),
    ):
        contents.append(f"const FlipperExternalApplication {entry_name}[] = {{")
        contents.extend(render_external_entry(app) + "," for app in apps)
        contents.append("};")
        contents.append(f"const size_t {entry_name}_COUNT = COUNT_OF({entry_name});")
        contents.append("")

    return "\n".join(contents)


def has_wildcards(pattern: str) -> bool:
    return any(char in pattern for char in "*?[]")


def recursive_glob(base_dir: Path, pattern: str) -> list[Path]:
    matches: set[Path] = set()
    for current_root, _, _ in os.walk(base_dir):
        current_dir = Path(current_root)
        matches.update(path for path in current_dir.glob(pattern) if path.is_file())
    return sorted(matches)


def gather_sources(base_dir: Path, patterns: Iterable[str]) -> list[Path]:
    include_patterns = [pattern for pattern in patterns if not pattern.startswith("!")]
    exclude_patterns = [pattern[1:] for pattern in patterns if pattern.startswith("!")]
    resolved: list[Path] = []

    def expand(pattern: str) -> list[Path]:
        path = base_dir / pattern
        if has_wildcards(pattern):
            return recursive_glob(base_dir, pattern)
        if path.is_dir():
            return [match for match in sorted(path.rglob("*")) if match.is_file()]
        if path.is_file():
            return [path]
        return []

    for pattern in include_patterns:
        resolved.extend(expand(pattern))

    excluded: set[Path] = set()
    for pattern in exclude_patterns:
        excluded.update(expand(pattern))

    unique = sorted({path for path in resolved if path.is_file() and path not in excluded})
    return [path for path in unique if "/lib/" not in str(path)]


def cmake_quote(path: Path | str) -> str:
<<<<<<< HEAD
    """Quote a path for generated CMake. Use forward slashes so Windows paths are not
    parsed as escape sequences (e.g. \\U in C:\\Users\\U_... breaks CMake)."""
    s = str(path).replace("\\", "/")
    return '"' + s.replace('"', '\\"') + '"'
=======
    return '"' + str(path).replace("\\", "\\\\").replace('"', '\\"') + '"'
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685


def gather_asset_sources(app: FlipperApplication) -> list[Path]:
    asset_roots: list[Path] = []
    for relative_path in (app.resources, app.fap_file_assets, app.fap_icon_assets):
        if not relative_path:
            continue
        candidate = Path(app._apppath) / relative_path
        if candidate.exists():
            asset_roots.append(candidate)
    return asset_roots


<<<<<<< HEAD
def is_flipper_resources_ext_mirror(app: FlipperApplication, asset_root: Path) -> bool:
    """True when asset_root is the app's `resources/` tree that mirrors /ext on SD.

    Flipper packs e.g. applications/main/infrared/resources/infrared/assets/*.ir so that
    on the card they appear as /ext/infrared/assets/*.ir (EXT_PATH). Staging those
    under apps_assets/<appid>/ breaks paths — we merge each top-level child into
    ${ESP32_FAM_RUNTIME_EXT_ROOT}/ instead.
    """
    if not app.resources or app.resources != "resources":
        return False
    expected = Path(app._apppath) / app.resources
    try:
        return expected.resolve() == asset_root.resolve()
    except (OSError, RuntimeError):
        return expected == asset_root


=======
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
def gather_asset_dependencies(asset_roots: Iterable[Path]) -> list[Path]:
    dependencies: set[Path] = set()
    for asset_root in asset_roots:
        if asset_root.is_file():
            dependencies.add(asset_root)
            continue
        dependencies.update(path for path in asset_root.rglob("*") if path.is_file())
    return sorted(dependencies)


<<<<<<< HEAD
def _app_manifest_path_posix(app: FlipperApplication) -> str:
    """Normalize manifest path so Windows backslashes match application-dir checks."""
    return getattr(app, "_manifest_path", "").replace("\\", "/")


=======
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
def generate_ported_cmake(buildset: AppBuildset, project_dir: Path) -> str:
    contents = [
        "set(ESP32_FAM_PORTED_OBJECT_TARGETS)",
        "",
        f'set(ESP32_FAM_ASSETS_SCRIPT {cmake_quote(project_dir / "tools/fam/compile_icons.py")})',
        'set(ESP32_FAM_RUNTIME_ROOT "${ESP32_FAM_GENERATED_DIR}/fam_runtime_root")',
        'set(ESP32_FAM_RUNTIME_EXT_ROOT "${ESP32_FAM_RUNTIME_ROOT}/ext")',
        'set(ESP32_FAM_STAGE_ASSETS_STAMP "${ESP32_FAM_RUNTIME_ROOT}/.assets.stamp")',
        "",
    ]

    ported_apps = [
        app
        for app in sorted_unique_apps(buildset.apps)
<<<<<<< HEAD
        if (
            "/applications/" in _app_manifest_path_posix(app)
            or "/applications_user/" in _app_manifest_path_posix(app)
        )
=======
        if "/applications/" in getattr(app, "_manifest_path", "")
        or "/applications_user/" in getattr(app, "_manifest_path", "")
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
        and (app.apptype != FlipperAppType.STARTUP or bool(app.sources))
    ]

    for app in ported_apps:
        app_root = Path(app._apppath)
        app_sources = gather_sources(app_root, app.sources)
        app_target = f"esp32_fam_app_{app.appid}"
        icon_include_dir = None

        if app.fap_icon_assets:
            icon_source_dir = app_root / app.fap_icon_assets
            if icon_source_dir.exists():
                icon_bundle_name = f"{app.fap_icon_assets_symbol or app.appid}_icons"
                icon_build_dir = f'${{ESP32_FAM_GENERATED_DIR}}/icons/{app.appid}'
                icon_source_c = f"{icon_build_dir}/{icon_bundle_name}.c"
                icon_header_h = f"{icon_build_dir}/{icon_bundle_name}.h"
                icon_dependencies = gather_asset_dependencies([icon_source_dir])

                contents.append("add_custom_command(")
                contents.append(f"    OUTPUT {cmake_quote(icon_source_c)} {cmake_quote(icon_header_h)}")
                contents.append(
                    f"    COMMAND ${{CMAKE_COMMAND}} -E make_directory {cmake_quote(icon_build_dir)}"
                )
                contents.append(
                    f"    COMMAND ${{Python3_EXECUTABLE}} ${{ESP32_FAM_ASSETS_SCRIPT}} icons {cmake_quote(icon_source_dir)} {cmake_quote(icon_build_dir)} --filename {cmake_quote(icon_bundle_name)}"
                )
                if icon_dependencies:
                    contents.append("    DEPENDS")
                    contents.append("        ${ESP32_FAM_ASSETS_SCRIPT}")
                    contents.extend(f"        {cmake_quote(path)}" for path in icon_dependencies)
                else:
                    contents.append("    DEPENDS")
                    contents.append("        ${ESP32_FAM_ASSETS_SCRIPT}")
                contents.append("    VERBATIM")
                contents.append(")")
                contents.append("")

                app_sources.append(Path(icon_source_c))
                icon_include_dir = icon_build_dir

        for private_lib in app.fap_private_libs:
            lib_root = app_root / "lib" / private_lib.name
            lib_sources = gather_sources(lib_root, private_lib.sources)
            if not lib_sources:
                continue

            lib_target = f"{app_target}_lib_{private_lib.name}"
            contents.append(f"add_library({lib_target} OBJECT")
            contents.extend(f"    {cmake_quote(source)}" for source in lib_sources)
            contents.append(")")
            contents.append(f"target_include_directories({lib_target} PRIVATE")
            contents.append(f"    {cmake_quote(app_root)}")
            for include_path in private_lib.fap_include_paths:
                contents.append(f"    {cmake_quote(lib_root / include_path)}")
            for include_path in private_lib.cincludes:
                contents.append(f"    {cmake_quote(app_root / include_path)}")
            contents.append(")")
            if private_lib.cdefines:
                contents.append(f"target_compile_definitions({lib_target} PRIVATE {' '.join(private_lib.cdefines)})")
            if private_lib.cflags:
                contents.append(f"target_compile_options({lib_target} PRIVATE {' '.join(private_lib.cflags)})")
            contents.append(f"list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS {lib_target})")
            contents.append("")

        if not app_sources:
            continue

        contents.append(f"add_library({app_target} OBJECT")
        contents.extend(f"    {cmake_quote(source)}" for source in app_sources)
        contents.append(")")
        contents.append(f"target_include_directories({app_target} PRIVATE")
        contents.append(f"    {cmake_quote(app_root)}")
        if icon_include_dir:
            contents.append(f"    {cmake_quote(icon_include_dir)}")
        for private_lib in app.fap_private_libs:
            lib_root = app_root / "lib" / private_lib.name
            for include_path in private_lib.fap_include_paths:
                contents.append(f"    {cmake_quote(lib_root / include_path)}")
        contents.append(")")
        if app.cdefines:
            contents.append(f"target_compile_definitions({app_target} PRIVATE {' '.join(app.cdefines)})")
        # Relax warnings for user apps and js_app modules
        if "/applications_user/" in getattr(app, "_manifest_path", "") or app.appid.startswith("js_"):
            contents.append(f"target_compile_options({app_target} PRIVATE -Wno-error=implicit-function-declaration -Wno-error=incompatible-pointer-types)")
        contents.append(f"list(APPEND ESP32_FAM_PORTED_OBJECT_TARGETS {app_target})")
        contents.append("")

    stage_depends: set[Path] = set()
    stage_commands = [
        '    COMMAND ${CMAKE_COMMAND} -E remove_directory "${ESP32_FAM_RUNTIME_ROOT}"',
        '    COMMAND ${CMAKE_COMMAND} -E make_directory "${ESP32_FAM_RUNTIME_EXT_ROOT}/apps_assets"',
    ]

    for app in ported_apps:
        asset_roots = gather_asset_sources(app)
        if not asset_roots:
            continue

<<<<<<< HEAD
        apps_assets_destination = f'${{ESP32_FAM_RUNTIME_EXT_ROOT}}/apps_assets/{app.appid}'
        mirror_roots = [ar for ar in asset_roots if is_flipper_resources_ext_mirror(app, ar)]
        other_roots = [ar for ar in asset_roots if not is_flipper_resources_ext_mirror(app, ar)]

        for asset_root in mirror_roots:
            if not asset_root.is_dir():
                continue
            for child in sorted(asset_root.iterdir(), key=lambda p: p.name):
                ext_destination = f'${{ESP32_FAM_RUNTIME_EXT_ROOT}}/{child.name}'
                stage_commands.append(
                    f"    COMMAND ${{CMAKE_COMMAND}} -E make_directory {cmake_quote(ext_destination)}"
                )
                if child.is_dir():
                    stage_commands.append(
                        f"    COMMAND ${{CMAKE_COMMAND}} -E copy_directory {cmake_quote(child)} {cmake_quote(ext_destination)}"
                    )
                else:
                    dest_file = f'${{ESP32_FAM_RUNTIME_EXT_ROOT}}/{child.name}'
                    stage_commands.append(
                        f"    COMMAND ${{CMAKE_COMMAND}} -E copy_if_different {cmake_quote(child)} {cmake_quote(dest_file)}"
                    )

        if other_roots:
            stage_commands.append(
                f"    COMMAND ${{CMAKE_COMMAND}} -E make_directory {cmake_quote(apps_assets_destination)}"
            )
            for asset_root in other_roots:
                if asset_root.is_dir():
                    stage_commands.append(
                        f"    COMMAND ${{CMAKE_COMMAND}} -E copy_directory {cmake_quote(asset_root)} {cmake_quote(apps_assets_destination)}"
                    )
                elif asset_root.is_file():
                    stage_commands.append(
                        f"    COMMAND ${{CMAKE_COMMAND}} -E copy_if_different {cmake_quote(asset_root)} {cmake_quote(apps_assets_destination)}"
                    )
=======
        destination = f'${{ESP32_FAM_RUNTIME_EXT_ROOT}}/apps_assets/{app.appid}'
        stage_commands.append(f"    COMMAND ${{CMAKE_COMMAND}} -E make_directory {cmake_quote(destination)}")

        for asset_root in asset_roots:
            if asset_root.is_dir():
                stage_commands.append(
                    f"    COMMAND ${{CMAKE_COMMAND}} -E copy_directory {cmake_quote(asset_root)} {cmake_quote(destination)}"
                )
            elif asset_root.is_file():
                stage_commands.append(
                    f"    COMMAND ${{CMAKE_COMMAND}} -E copy_if_different {cmake_quote(asset_root)} {cmake_quote(destination)}"
                )
>>>>>>> 05c91cb486590019377b94b79a37919e1c650685
        stage_depends.update(gather_asset_dependencies(asset_roots))

    stage_commands.append('    COMMAND ${CMAKE_COMMAND} -E touch "${ESP32_FAM_STAGE_ASSETS_STAMP}"')

    contents.append("add_custom_command(")
    contents.append('    OUTPUT "${ESP32_FAM_STAGE_ASSETS_STAMP}"')
    contents.extend(stage_commands)
    if stage_depends:
        contents.append("    DEPENDS")
        contents.extend(f"        {cmake_quote(path)}" for path in sorted(stage_depends))
    contents.append("    VERBATIM")
    contents.append(")")
    contents.append('add_custom_target(esp32_fam_stage_assets DEPENDS "${ESP32_FAM_STAGE_ASSETS_STAMP}")')
    contents.append("")

    return "\n".join(contents)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate ESP32 FAM registry and CMake fragment")
    parser.add_argument("--project-dir", required=True)
    parser.add_argument("--config", required=True)
    parser.add_argument("--registry-out", required=True)
    parser.add_argument("--cmake-out", required=True)
    args = parser.parse_args()

    project_dir = Path(args.project_dir).resolve()
    config = load_config(Path(args.config))

    appmgr = load_manifests(
        project_dir,
        list(config["MANIFEST_ROOTS"]),
        dict(config.get("APP_SOURCE_OVERRIDES", {})),
    )
    buildset = AppBuildset(
        appmgr,
        hw_target=f"f{int(config['TARGET_HW'])}",
        appnames=list(config["APPS"]),
        extra_ext_appnames=list(config["EXTRA_EXT_APPS"]),
    )

    registry_out = Path(args.registry_out)
    registry_out.parent.mkdir(parents=True, exist_ok=True)
    registry_out.write_text(
        generate_registry(buildset, str(config.get("AUTORUN_APP", ""))),
        encoding="utf-8",
    )

    cmake_out = Path(args.cmake_out)
    cmake_out.parent.mkdir(parents=True, exist_ok=True)
    cmake_out.write_text(generate_ported_cmake(buildset, project_dir), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
