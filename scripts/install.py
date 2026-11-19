#!/usr/bin/env python3
"""Install this dotfiles snapshot with pinned builds and reversible file changes."""
import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import stat
import subprocess
import sys
import tempfile
import uuid

REPO = Path(__file__).resolve().parents[1]
COMPONENTS = ("shell", "foot", "fuzzel", "nvim", "scripts", "everbar", "dwl", "dwlb", "docs")
BUILD_PACKAGES = {
    "everbar": ("dbus-1", "libpipewire-0.3"),
    "dwl": ("wayland-server", "xkbcommon", "libinput", "wlroots-0.19", "wayland-protocols", "wayland-scanner"),
    "dwlb": ("wayland-client", "wayland-cursor", "fcft", "pixman-1", "wayland-protocols", "wayland-scanner"),
}


class InstallError(Exception):
    pass


@dataclass(frozen=True)
class Item:
    source: Path
    target: Path
    mode: int = 0o644


def absolute(value):
    path = Path(value).expanduser()
    if not path.is_absolute():
        raise InstallError("Paths must be absolute: " + str(path))
    return path


class Layout:
    def __init__(self, home=None):
        # An explicit --home is a sandbox: ignore the caller's XDG paths.
        self.home = absolute(home or str(Path.home())).resolve()
        def xdg(name, fallback):
            return absolute(str(self.home / fallback) if home else
                            os.environ.get(name) or str(self.home / fallback))
        self.config = xdg("XDG_CONFIG_HOME", ".config")
        self.data = xdg("XDG_DATA_HOME", ".local/share")
        self.state = xdg("XDG_STATE_HOME", ".local/state")
        self.cache = xdg("XDG_CACHE_HOME", ".cache")
        self.bin = self.home / ".local/bin"
        self.share = self.data / "everforest-dotfiles"
        self.backups = self.state / "everforest-dotfiles/backups"
        self.roots = (self.home, self.config, self.data, self.state, self.cache)

    def allowed(self, path):
        return any(path != root and path.is_relative_to(root) for root in self.roots)


def source_files():
    """Explicit source-only inventory: never package .git, binaries or caches."""
    roots = ("README.md", "AUTHORS.md", "LICENSE", "SOURCE-BASES.txt", "sources.lock.json",
             "install.sh", "Makefile", ".gitignore", ".gitattributes", ".editorconfig")
    paths = [REPO / name for name in roots]
    for directory in ("configs", "docs", "screenshots"):
        paths.extend(p for p in (REPO / directory).rglob("*") if p.is_file())
    paths.extend(REPO / "scripts" / name for name in ("everbar-start", "install.py"))
    for pattern in ("src/everbar/*.c", "src/everbar/*.h", "src/everbar/Makefile",
                    "src/dwl/*.c", "src/dwl/*.h", "src/dwl/Makefile", "src/dwl/LICENSE*",
                    "src/dwl/protocols/*.xml", "src/dwlb/*.c", "src/dwlb/*.h", "src/dwlb/LICENSE*",
                    "tests/*.py", "tests/*.c", "tests/*.conf"):
        paths.extend(REPO.glob(pattern))
    result = sorted(set(paths))
    for path in result:
        if not path.is_file() or path.is_symlink():
            raise InstallError("Missing/non-regular repository source: " + str(path))
    return result


def config_items(layout, selected):
    mappings = {
        "shell": (("configs/.bashrc", layout.home / ".bashrc"),
                  ("configs/.bash_profile", layout.home / ".bash_profile")),
        "foot": (("configs/foot.ini", layout.config / "foot/foot.ini"),),
        "fuzzel": (("configs/fuzzel.ini", layout.config / "fuzzel/fuzzel.ini"),),
        "nvim": (("configs/nvim/init.lua", layout.config / "nvim/init.lua"),
                 ("docs/nvim-shortcuts.md", layout.config / "nvim/shortcuts.md")),
        "scripts": (("scripts/everbar-start", layout.bin / "everbar-start"),),
    }
    items = []
    for component, pairs in mappings.items():
        if component in selected:
            items.extend(Item(REPO / source, target, 0o755 if component == "scripts" else 0o644)
                         for source, target in pairs)
    if "docs" in selected:
        for source in source_files():
            mode = 0o755 if source.name in ("install.sh", "everbar-start") else 0o644
            items.append(Item(source, layout.share / source.relative_to(REPO), mode))
    return items


def run(command, **kwargs):
    print("+ " + shlex.join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def check_build_dependencies(selected):
    projects = set(selected).intersection(BUILD_PACKAGES)
    commands = set()
    if projects:
        commands.update(("make", "pkg-config"))
        compiler = shlex.split(os.environ.get("CC", "cc"))
        if not compiler:
            raise InstallError("CC must not be empty")
        commands.add(compiler[0])
    if set(selected).intersection(("dwl", "dwlb", "nvim")):
        commands.add("git")
    missing = sorted(command for command in commands if not shutil.which(command))
    if missing:
        raise InstallError("Missing commands: " + ", ".join(missing))
    packages = sorted({p for project in projects for p in BUILD_PACKAGES[project]})
    if packages:
        result = subprocess.run(["pkg-config", "--print-errors", "--exists", *packages])
        if result.returncode:
            raise InstallError("Missing build dependencies (pkg-config names): " + " ".join(packages) +
                               ". See docs/install.md; do not force incompatible library versions.")


def runtime_warnings(selected):
    commands = {
        "shell": ("dbus-run-session", "pipewire", "gnome-keyring-daemon"),
        "foot": ("foot",), "fuzzel": ("fuzzel",),
        "nvim": ("nvim", "clangd", "wl-copy", "wl-paste", "g++"),
        "everbar": ("fuzzel", "loginctl"),
        "dwl": ("foot", "fuzzel", "wpctl", "brightnessctl", "shot-copy", "lock-screen"),
    }
    missing = sorted({cmd for component in selected for cmd in commands.get(component, ())
                      if not shutil.which(cmd)})
    if missing:
        print("WARNING: runtime commands not found: " + ", ".join(missing))
    if "dwl" in selected:
        print("NOTE: lock-screen and shot-copy are referenced but are NOT supplied by this repository.")
        print("NOTE: brightness uses acpi_video0; keyboard layout is Norwegian; XWayland is disabled.")
    if "shell" in selected:
        print("WARNING: replacing .bash_profile enables automatic dwl startup on tty1 login.")
    if "nvim" in selected:
        print("NOTE: the Neovim configuration requires Neovim 0.11+ and installs the pinned Everforest theme.")
    if "scripts" in selected:
        print("NOTE: wallpaper is optional and is NOT supplied; existing wallpaper is left untouched.")


def load_locks():
    data = json.loads((REPO / "sources.lock.json").read_text())
    if data.get("schema") != 1:
        raise InstallError("Unsupported sources.lock.json schema")
    for name in ("dwl", "dwlb", "everforest"):
        entry = data[name]
        if not re.fullmatch(r"[0-9a-f]{40}", entry["commit"]) or not entry["url"].startswith("https://"):
            raise InstallError("Invalid pinned upstream: " + name)
        for overlay in entry["overlays"]:
            path = Path(overlay)
            if path.is_absolute() or ".." in path.parts:
                raise InstallError("Unsafe overlay path: " + overlay)
    return data


def fetch_source(name, lock, work):
    target = work / name
    # This is a NEW, isolated checkout, never an existing user repository.
    git = ["git", "-c", "core.hooksPath=/dev/null", "-c", "init.templateDir="]
    run([*git, "init", "--quiet", target])
    run([*git, "-C", target, "fetch", "--quiet", "--depth=1", lock["url"], lock["commit"]])
    actual = subprocess.check_output([*git, "-C", str(target), "rev-parse", "FETCH_HEAD"], text=True).strip()
    if actual != lock["commit"]:
        raise InstallError("Upstream commit mismatch for " + name)
    run([*git, "-C", target, "-c", "advice.detachedHead=false", "checkout", "--quiet", "--detach", actual])
    for relative in lock["overlays"]:
        source = REPO / "src" / name / relative
        if not source.is_file() or source.is_symlink():
            raise InstallError("Missing overlay: " + str(source))
        destination = target / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, destination)
    return target


def build_items(layout, selected, work, jobs):
    items = []
    locks = load_locks()
    for name in ("everbar", "dwl", "dwlb"):
        if name not in selected:
            continue
        if name == "everbar":
            source = work / name
            source.mkdir()
            for path in (REPO / "src/everbar").iterdir():
                if path.suffix in (".c", ".h") or path.name == "Makefile":
                    shutil.copyfile(path, source / path.name)
        else:
            source = fetch_source(name, locks[name], work)
        # Always build from clean source here; never reuse foreign .o/binaries.
        flags = os.environ.get("CFLAGS", "-O2") + " " + shlex.quote("-ffile-prefix-map=" + str(work) + "=.")
        command = ["make", "-C", source, "-j", str(jobs), "CC=" + os.environ.get("CC", "cc")]
        if name == "dwl":
            command.append("VERSION=0.8-dev+everforest." + locks[name]["commit"][:12])
        # Environment CFLAGS let upstream makefiles append their required
        # include/library flags. A command-line CFLAGS assignment suppresses
        # dwlb's target-specific pkg-config CFLAGS and breaks its build.
        run(command, env=dict(os.environ, CFLAGS=flags))
        items.append(Item(source / name, layout.bin / name, 0o755))
        if name in ("dwl", "dwlb"):
            items.append(Item(source / (name + ".1"), layout.home / ".local/share/man/man1" / (name + ".1")))
        if name == "dwl":
            desktop = source / "dwl.desktop"
            # Display managers need not include ~/.local/bin in their PATH.
            executable = str(layout.bin / "dwl")
            if any(c in executable for c in ("\n", "\r", "%")):
                raise InstallError("Home path cannot be represented safely in a desktop Exec entry")
            escaped = executable.replace("\\", "\\\\\\\\").replace('"', '\\"').replace("`", "\\`").replace("$", "\\$")
            text = desktop.read_text()
            text, count = re.subn(r"^Exec=.*$", lambda _: 'Exec="' + escaped + '"', text, flags=re.MULTILINE)
            if count != 1:
                raise InstallError("Unexpected upstream desktop entry")
            desktop.write_text(text)
            items.append(Item(desktop, layout.data / "wayland-sessions/dwl.desktop"))
    if "nvim" in selected:
        source = fetch_source("everforest", locks["everforest"], work)
        for path in sorted(source.rglob("*")):
            relative = path.relative_to(source)
            if ".git" in relative.parts:
                continue
            if path.is_symlink():
                raise InstallError("Unexpected symlink in pinned Neovim theme: " + str(relative))
            if path.is_file():
                items.append(Item(path, layout.data / "nvim/site/pack/everforest/opt/everforest" / relative))
    return items


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(131072), b""):
            h.update(block)
    return h.hexdigest()


def same(item):
    return (item.target.is_file() and not item.target.is_symlink() and
            stat.S_IMODE(item.target.stat().st_mode) == item.mode and
            digest(item.source) == digest(item.target))


def validate_target(path, layout):
    if not path.is_absolute() or ".." in path.parts or not layout.allowed(path):
        raise InstallError("Target outside installation roots: " + str(path))
    if path == REPO or REPO in path.parents:
        raise InstallError("Refusing to install over the source repository: " + str(path))
    for parent in path.parents:
        if parent.is_symlink():
            raise InstallError("Refusing to write through a symlinked parent: " + str(parent))
        if parent.exists() and not parent.is_dir():
            raise InstallError("Parent is not a directory: " + str(parent))
    if path.exists() and not path.is_file() and not path.is_symlink():
        raise InstallError("Refusing to replace a directory/special file: " + str(path))


def save_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd, temporary = tempfile.mkstemp(prefix=".manifest-", dir=path.parent)
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(data, stream, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        Path(temporary).unlink(missing_ok=True)


@contextmanager
def installation_lock(layout):
    path = layout.state / "everforest-dotfiles/install.lock"
    validate_target(path, layout)
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            raise InstallError("Installer lock must be a private regular file owned by you")
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise InstallError("Another install/restore operation is already running") from None
        yield
    finally:
        os.close(fd)


def atomic_copy(source, target, mode):
    fd, temporary = tempfile.mkstemp(prefix=".everforest-", dir=target.parent)
    try:
        with os.fdopen(fd, "wb") as output, source.open("rb") as input_file:
            shutil.copyfileobj(input_file, output)
            os.fchmod(output.fileno(), mode)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary, target)
    finally:
        Path(temporary).unlink(missing_ok=True)


def ensure_parents(path, created):
    missing = []
    parent = path.parent
    while not parent.exists():
        missing.append(parent)
        parent = parent.parent
    for directory in reversed(missing):
        directory.mkdir(mode=0o755)
        created.append(str(directory))


def matches_backup(target, backup):
    if backup.is_symlink():
        return target.is_symlink() and os.readlink(target) == os.readlink(backup)
    return (target.is_file() and not target.is_symlink() and
            digest(target) == digest(backup) and
            stat.S_IMODE(target.stat().st_mode) == stat.S_IMODE(backup.stat().st_mode))


def restore_transaction(directory, layout, dry_run=False):
    manifest_path = directory / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if manifest.get("schema") != 1 or manifest.get("home") != str(layout.home):
        raise InstallError("Backup belongs to a different home or has an unsupported format")
    planned = []
    # Validate everything before restoring anything; never overwrite later edits.
    for entry in reversed(manifest["files"]):
        target = Path(entry["target"])
        validate_target(target, layout)
        backup = directory / entry["backup"] if entry["backup"] else None
        if backup and (backup.parent != directory or not (backup.exists() or backup.is_symlink())):
            raise InstallError("Missing or invalid backup for " + str(target))
        if backup and matches_backup(target, backup):
            continue
        if not backup and not target.exists() and not target.is_symlink():
            continue
        if target.is_symlink() or not target.is_file() or digest(target) != entry["sha256"] or stat.S_IMODE(target.stat().st_mode) != entry["mode"]:
            raise InstallError("Changed since installation; refusing to overwrite: " + str(target))
        planned.append((target, backup))
    for target, backup in planned:
        print(("RESTORE " if backup else "REMOVE installed file ") + str(target))
        if dry_run:
            continue
        if not backup:
            target.unlink()
        elif backup.is_symlink():
            temporary = target.parent / (".everforest-" + uuid.uuid4().hex)
            try:
                temporary.symlink_to(os.readlink(backup))
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
        else:
            atomic_copy(backup, target, stat.S_IMODE(backup.stat().st_mode))
    if not dry_run:
        for name in reversed(manifest.get("created_dirs", [])):
            directory_path = Path(name)
            if layout.allowed(directory_path) and not directory_path.is_symlink():
                try:
                    directory_path.rmdir()  # only empty directories created by this transaction
                except OSError:
                    pass
        manifest["status"] = "restored"
        save_json(manifest_path, manifest)
    return len(planned)


def install_items(items, layout):
    targets = set()
    changed = []
    for item in items:
        validate_target(item.target, layout)
        if item.target in targets:
            raise InstallError("Duplicate installation target: " + str(item.target))
        targets.add(item.target)
        if not item.source.is_file() or item.source.is_symlink():
            raise InstallError("Missing/non-regular install source: " + str(item.source))
        if not same(item):
            changed.append(item)
    if not changed:
        print("Already up to date; no files changed and no backup needed.")
        return None
    identifier = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ-") + uuid.uuid4().hex[:8]
    directory = layout.backups / identifier
    validate_target(directory / "manifest.json", layout)
    directory.mkdir(parents=True, mode=0o700)
    os.chmod(directory, 0o700)
    manifest_path = directory / "manifest.json"
    manifest = {"schema": 1, "home": str(layout.home), "status": "installing", "files": [], "created_dirs": []}
    save_json(manifest_path, manifest)
    try:
        for index, item in enumerate(changed):
            backup_name = None
            if item.target.exists() or item.target.is_symlink():
                backup_name = f"{index:05d}.backup"
                shutil.copy2(item.target, directory / backup_name, follow_symlinks=False)
            ensure_parents(item.target, manifest["created_dirs"])
            entry = {"target": str(item.target), "backup": backup_name,
                     "sha256": digest(item.source), "mode": item.mode}
            manifest["files"].append(entry)
            save_json(manifest_path, manifest)  # journal BEFORE the atomic replacement
            atomic_copy(item.source, item.target, item.mode)
            print("INSTALLED " + str(item.target))
        manifest["status"] = "installed"
        save_json(manifest_path, manifest)
    except BaseException:
        print("Installation interrupted; attempting rollback. Backup: " + str(directory), file=sys.stderr)
        try:
            restore_transaction(directory, layout)
        except (OSError, ValueError, InstallError) as error:
            print("Automatic rollback incomplete: " + str(error), file=sys.stderr)
        raise
    print("Backup ID: " + identifier)
    print("Restore with: ./install.sh --restore " + identifier)
    print("No running processes were stopped. Start a new login session to use replaced binaries/configs.")
    return identifier


def print_plan(layout, selected):
    print("Components: " + ", ".join(component for component in COMPONENTS if component in selected))
    print("Home: " + str(layout.home))
    for item in config_items(layout, selected):
        # The source snapshot can contain many files; summarize it once below.
        if not item.target.is_relative_to(layout.share):
            print("COPY " + str(item.source.relative_to(REPO)) + " -> " + str(item.target))
    if "docs" in selected:
        print("COPY source snapshot, docs, licenses, tests and screenshot -> " + str(layout.share))
    locks = load_locks()
    for name in ("everbar", "dwl", "dwlb"):
        if name in selected:
            base = "local C sources" if name == "everbar" else locks[name]["url"] + " @ " + locks[name]["commit"]
            print("BUILD " + name + " from " + base + " -> " + str(layout.bin / name))
            if name in ("dwl", "dwlb"):
                print("COPY " + name + ".1 -> " + str(layout.home / ".local/share/man/man1"))
            if name == "dwl":
                print("COPY dwl.desktop -> " + str(layout.data / "wayland-sessions/dwl.desktop"))
    if "nvim" in selected:
        print("FETCH Everforest theme @ " + locks["everforest"]["commit"] + " -> " + str(layout.data / "nvim/site/pack/everforest/opt/everforest"))
    print("Backups: " + str(layout.backups))
    runtime_warnings(selected)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", help="comma-separated components (default: all): " + ",".join(COMPONENTS))
    parser.add_argument("--dry-run", action="store_true", help="print the plan; no writes, builds or downloads")
    parser.add_argument("--yes", action="store_true", help="apply the displayed plan without an interactive confirmation")
    parser.add_argument("--check", action="store_true", help="check build tools/dependencies and report runtime warnings; no writes")
    parser.add_argument("--build-only", action="store_true", help="build/fetch in cache without replacing installed files")
    parser.add_argument("--home", help="explicit target home (ignores caller's XDG paths; useful for isolated testing)")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 4))
    parser.add_argument("--restore", metavar="BACKUP_ID", help="restore a backup, refusing to overwrite later edits")
    args = parser.parse_args(argv)
    try:
        if args.jobs < 1:
            raise InstallError("--jobs must be positive")
        if args.restore and (args.only or args.check or args.build_only):
            raise InstallError("--restore cannot be combined with --only, --check or --build-only")
        layout = Layout(args.home)
        selected = set(args.only.split(",")) if args.only else set(COMPONENTS)
        if selected.difference(COMPONENTS):
            raise InstallError("Unknown components: " + ",".join(sorted(selected.difference(COMPONENTS))))
        if args.restore:
            if not re.fullmatch(r"[0-9]{8}T[0-9]{6}Z-[0-9a-f]{8}", args.restore):
                raise InstallError("Invalid backup ID")
            directory = layout.backups / args.restore
            if args.dry_run:
                restore_transaction(directory, layout, dry_run=True)
                return 0
        else:
            print_plan(layout, selected)
            if args.dry_run:
                return 0
            check_build_dependencies(selected)
            if args.check:
                print("Build dependencies OK. No files changed.")
                return 0
        if os.geteuid() == 0:
            raise InstallError("Run as your normal desktop user, not with sudo/root")
        if not args.yes:
            if not sys.stdin.isatty() or input("Apply this plan? Type yes: ").strip() != "yes":
                raise InstallError("Not confirmed. Use --dry-run to inspect, or --yes to apply.")
        with installation_lock(layout):
            if args.restore:
                restore_transaction(directory, layout)
                return 0
            # Builds must finish BEFORE any dotfile or installed executable changes.
            cache = layout.cache / "everforest-dotfiles/builds"
            validate_target(cache / "placeholder", layout)
            cache.mkdir(parents=True, exist_ok=True, mode=0o700)
            work = Path(tempfile.mkdtemp(prefix="build-", dir=cache))
            os.chmod(work, 0o700)
            print("Isolated build workspace: " + str(work))
            items = config_items(layout, selected)
            items.extend(build_items(layout, selected, work, args.jobs))
            if args.build_only:
                print("Build finished; no installed files changed. Build workspace: " + str(work))
                return 0
            install_items(items, layout)
        return 0
    except (InstallError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("install: " + str(error), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("install: interrupted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
