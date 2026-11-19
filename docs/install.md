# Installing the complete dotfiles repository

The installer was written by **GPT-6.1 Sol via OpenCode**, at iGotUsername's
request. Attribution for the original setup and upstreams is in `AUTHORS.md`.

## Start with a preview

Run from this checkout, as the desktop user, without sudo:

```sh
./install.sh --dry-run
./install.sh --check
./install.sh
```

`--dry-run` performs no writes, builds, or network requests. `--check` checks
build dependencies and prints runtime warnings, without writing files. Normal
installation shows the plan and requires typing `yes`; use `--yes` for deliberate
noninteractive installation. No command enables system services, changes autologin,
installs OS packages, kills a process, or restarts the compositor.

**Important:** installing the `shell` component replaces `.bashrc` and
`.bash_profile`. The supplied profile automatically starts the dwl session on
tty1 login. Read it and the preview before confirming. It does not merge your
current shell customizations; it backs up the previous files.

## Dependencies

The installer needs Python 3.9+. Builds need make, a C compiler, pkg-config and
the relevant development packages. dwl/dwlb and the Neovim theme also need Git
and HTTPS access to Codeberg/GitHub. No arbitrary latest branch is used: upstream
revisions and overlay paths are pinned in `sources.lock.json`.

On Void Linux, typical build dependencies for the full setup are:

```sh
sudo xbps-install -S base-devel git python3 pkg-config \
    dbus-devel pipewire-devel wlroots0.19-devel wayland-devel \
    wayland-protocols libinput-devel libxkbcommon-devel fcft-devel pixman-devel
```

Already-installed packages need not be installed again. If XBPS reports a version
conflict, update the system consistently with `sudo xbps-install -Su` first;
repeat if instructed after an XBPS self-update. Do not force incompatible
D-Bus or wlroots libraries. This dwl snapshot specifically requires **wlroots
0.19**, not whichever wlroots ABI is newest.

Required pkg-config names are checked before downloads/builds:

| Build | pkg-config packages |
| --- | --- |
| everbar | `dbus-1 libpipewire-0.3` |
| dwl | `wayland-server xkbcommon libinput wlroots-0.19 wayland-protocols wayland-scanner` |
| dwlb | `wayland-client wayland-cursor fcft pixman-1 wayland-protocols wayland-scanner` |

Runtime programs include Foot, Fuzzel, Neovim 0.11+, NetworkManager, BlueZ,
PipeWire, WirePlumber/wpctl, D-Bus, gnome-keyring-daemon, loginctl, brightnessctl,
and wl-clipboard. Neovim's C/C++ features also use clangd, GCC/G++. Install timezone
data, Adwaita cursors and Symbols Nerd Font Mono to reproduce the setup.
The installer reports missing commands but does not configure services or device
permissions. Keep the existing working D-Bus, seat/session and audio setup.

## What “all files” means

All components are selected by default:

| Component | Live destination |
| --- | --- |
| `shell` | `~/.bashrc`, `~/.bash_profile` |
| `foot` | `$XDG_CONFIG_HOME/foot/foot.ini` |
| `fuzzel` | `$XDG_CONFIG_HOME/fuzzel/fuzzel.ini` |
| `nvim` | `$XDG_CONFIG_HOME/nvim/init.lua`, `nvim/shortcuts.md`, pinned theme under `$XDG_DATA_HOME/nvim/site/pack/everforest/opt/everforest` |
| `scripts` | `~/.local/bin/everbar-start` |
| `everbar` | locally built `~/.local/bin/everbar` |
| `dwl` | locally built `~/.local/bin/dwl`, man page, `$XDG_DATA_HOME/wayland-sessions/dwl.desktop` |
| `dwlb` | locally built `~/.local/bin/dwlb` and man page |
| `docs` | full source-only snapshot under `$XDG_DATA_HOME/everforest-dotfiles` |

The snapshot includes every supplied configuration, installer/launcher, C source,
protocol XML, test, document, license and screenshot, plus the repository metadata
files such as `sources.lock.json`, `.gitignore`, and `AUTHORS.md`. It excludes
`.git`, build products, foreign executables, Python bytecode and tool caches.
Tests and reference sources are archived, not launched as desktop components.

Unset XDG variables use the usual `~/.config`, `~/.local/share`, `~/.local/state`
and `~/.cache` defaults. Man pages go to `~/.local/share/man/man1`. Binaries stay
in `~/.local/bin`, as expected by the existing configuration. A user-local Wayland
session file may not be discovered by every display manager; the supplied setup
starts dwl from tty1 and does not depend on display-manager discovery.

The wallpaper, `shot-copy`, and `lock-screen` are **not present in this repo** and
cannot be installed from it. They are left untouched if already installed.
The optional wallpaper launcher checks for `~/.local/share/backgrounds/wallpaper`
and `swaybg`. Brightness uses `acpi_video0`, temperature uses `x86_pkg_temp`,
keyboard layout is Norwegian, clock timezone is Oslo, and XWayland is disabled.

## Selection and build-only operation

```sh
./install.sh --only everbar,scripts --yes
./install.sh --only shell,foot,fuzzel,nvim
./install.sh --build-only --only everbar,dwl,dwlb
./install.sh --jobs 2
```

Dependencies between selected components are not silently added: choosing
`scripts` alone assumes everbar/dwlb already exist. Choosing `nvim` includes its
theme because the configuration explicitly requires it. Choose `docs` to keep a
source/documentation snapshot; all components, including `docs`, are the default.

Build workspaces are unique private directories under
`$XDG_CACHE_HOME/everforest-dotfiles/builds`. Builds never reuse the repository's
`.o` files or binaries and never modify its dwl/dwlb source fragments. Failed
builds leave installed configuration and binaries unchanged. Build workspaces
are retained for troubleshooting; the installer does not recursively delete an
existing user cache or source checkout.

`CC` and `CFLAGS` may be set for the native builds. Do not point them at a foreign
toolchain when installing on the actual desktop. Defaults use the native `cc`
and `-O2`. The installer remaps temporary source paths in compiler output to
avoid unnecessary build-directory-dependent binaries.

For an isolated target, `--home /absolute/test/home` ignores the caller's XDG
paths and keeps destinations, cache and backups under that home. This installs
files for testing; it is not a chroot and does not sandbox the compiler or Git.

## Backups, rollback and restore

Changed existing files are copied to a private transaction directory under
`$XDG_STATE_HOME/everforest-dotfiles/backups/<BACKUP_ID>`. Symlinks at the target
itself are backed up as symlinks, then replaced without modifying their referents.
Directory/special-file conflicts and symlinked parent directories are refused.
The installer does not merge configuration directories or follow directory
symlinks into another dotfiles checkout.

Each replacement is staged beside its destination and atomically renamed. This
also allows replacing an executable without writing into its running inode.
All builds finish before live file replacement starts. Only one install/restore
may run at a time. Existing unchanged files are skipped. If a copy fails, the
installer attempts to roll back files it changed and retains the journal/backups.
This is a recoverable sequence of atomic file changes, **not** a filesystem-wide
atomic transaction. Power loss, storage failures or concurrent manual edits can
still require manual recovery from the journal.

The backup ID is printed at the end. Preview a restore, then apply it:

```sh
./install.sh --restore BACKUP_ID --dry-run
./install.sh --restore BACKUP_ID
```

Use the actual printed ID, not the literal `BACKUP_ID`. Restore verifies all
targets first and refuses to overwrite content or permission changes made after
installation. Previous files/symlinks are restored; newly installed files are
removed only when they still match the installed copy. Only empty directories
created by that transaction are removed. Backups and build workspaces are kept.
With multiple installations, restore the newest relevant transaction first.

After installation, log out and back in when ready. Existing processes keep
running the old executable until restarted. Do not start `everbar-start` again
inside an existing session: that would start a duplicate dwlb/wallpaper process.

## Repository hygiene and tests

Only source/configuration/documentation should be committed. `.gitignore` excludes
native build products and Python bytecode. `make clean` removes only local
everbar build artifacts; it never runs `git clean`, resets history or deletes
dotfiles. User-specific credentials and installation backups do not belong here.

```sh
make test-installer  # temporary homes; no network or real desktop changes
make test           # installer tests, then C/Python everbar tests
```

Installer tests cover backups/restores, file permissions, existing and broken
symlinks, directory/FIFO conflicts, no-op reinstalls, dry runs, simulated write
failures, interruption rollback, later-edit protection, single-installer locking,
source inventory and commit-pin validation. Full native source builds require
the development libraries listed above; desktop permissions and hardware behavior
still need validation on the actual machine.
