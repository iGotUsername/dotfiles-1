#!/usr/bin/env python3
"""Installer tests: temporary homes, no network, no real desktop changes."""
from contextlib import redirect_stdout, redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("dotfiles_installer", ROOT / "scripts/install.py")
installer = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = installer
spec.loader.exec_module(installer)


class Installer(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dotfiles-install-", dir=os.environ.get("TMPDIR"))
        self.root = Path(self.temp.name)
        self.home = self.root / "home with spaces"
        self.home.mkdir()
        self.layout = installer.Layout(str(self.home))
        self.source = self.root / "new.conf"
        self.source.write_text("new contents\n")
        self.output = io.StringIO()
        self.stdout = redirect_stdout(self.output)
        self.stderr = redirect_stderr(self.output)
        self.stdout.__enter__()
        self.stderr.__enter__()

    def tearDown(self):
        self.stderr.__exit__(None, None, None)
        self.stdout.__exit__(None, None, None)
        self.temp.cleanup()

    def item(self, name=".bashrc", mode=0o644):
        return installer.Item(self.source, self.home / name, mode)

    def install(self, items=None):
        identifier = installer.install_items(items or [self.item()], self.layout)
        return self.layout.backups / identifier if identifier else None

    def test_install_and_restore_new_file(self):
        directory = self.install()
        self.assertEqual((self.home / ".bashrc").read_text(), "new contents\n")
        self.assertFalse((self.home / ".bashrc").is_symlink())
        installer.restore_transaction(directory, self.layout)
        self.assertFalse((self.home / ".bashrc").exists())
        installer.restore_transaction(directory, self.layout)  # idempotent restore

    def test_backup_existing_file_and_permissions(self):
        target = self.home / ".bashrc"
        target.write_text("precious\n")
        target.chmod(0o600)
        directory = self.install()
        self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
        manifest = json.loads((directory / "manifest.json").read_text())
        backup = directory / manifest["files"][0]["backup"]
        self.assertEqual(backup.read_text(), "precious\n")
        self.assertEqual(stat.S_IMODE(backup.stat().st_mode), 0o600)
        installer.restore_transaction(directory, self.layout)
        self.assertEqual(target.read_text(), "precious\n")
        self.assertEqual(stat.S_IMODE(target.stat().st_mode), 0o600)

    def test_existing_and_broken_symlinks_preserved(self):
        for name in ("exists", "missing"):
            target = self.home / name
            original = self.root / (name + "-original")
            if name == "exists":
                original.write_text("do not modify")
            target.symlink_to(original)
            directory = self.install([self.item(name)])
            self.assertFalse(target.is_symlink())
            installer.restore_transaction(directory, self.layout)
            self.assertTrue(target.is_symlink())
            self.assertEqual(os.readlink(target), str(original))
            if original.exists():
                self.assertEqual(original.read_text(), "do not modify")

    def test_directory_conflict_preflights_all_files(self):
        (self.home / "directory").mkdir()
        with self.assertRaises(installer.InstallError):
            self.install([self.item("safe"), self.item("directory")])
        self.assertFalse((self.home / "safe").exists())
        self.assertFalse(self.layout.backups.exists())

    def test_fifo_target_refused(self):
        os.mkfifo(self.home / "fifo")
        with self.assertRaises(installer.InstallError):
            self.install([self.item("fifo")])

    def test_symlinked_parent_refused(self):
        outside = self.root / "outside"
        outside.mkdir()
        (self.home / ".config").symlink_to(outside, target_is_directory=True)
        with self.assertRaises(installer.InstallError):
            self.install([self.item(".config/nvim/init.lua")])
        self.assertEqual(list(outside.iterdir()), [])

    def test_outside_home_refused(self):
        with self.assertRaises(installer.InstallError):
            self.install([installer.Item(self.source, self.root / "outside")])

    def test_repository_target_refused(self):
        layout = installer.Layout(str(ROOT.parent))
        with self.assertRaises(installer.InstallError):
            installer.validate_target(ROOT / "README.md", layout)

    def test_duplicate_targets_refused_before_changes(self):
        with self.assertRaises(installer.InstallError):
            self.install([self.item(), self.item()])
        self.assertFalse((self.home / ".bashrc").exists())

    def test_source_symlink_refused(self):
        link = self.root / "source-link"
        link.symlink_to(self.source)
        with self.assertRaises(installer.InstallError):
            self.install([installer.Item(link, self.home / ".bashrc")])

    def test_unchanged_install_is_noop(self):
        self.install()
        before = list(self.layout.backups.iterdir())
        self.assertIsNone(installer.install_items([self.item()], self.layout))
        self.assertEqual(list(self.layout.backups.iterdir()), before)

    def test_executable_modes(self):
        self.install([self.item(".local/bin/program", 0o755)])
        self.assertEqual(stat.S_IMODE((self.home / ".local/bin/program").stat().st_mode), 0o755)

    def test_restore_refuses_subsequent_edits_before_any_changes(self):
        directory = self.install([self.item("a"), self.item("b")])
        (self.home / "a").write_text("later user edit")
        with self.assertRaises(installer.InstallError):
            installer.restore_transaction(directory, self.layout)
        self.assertEqual((self.home / "a").read_text(), "later user edit")
        self.assertEqual((self.home / "b").read_text(), "new contents\n")

    def test_restore_refuses_mode_edits(self):
        directory = self.install()
        (self.home / ".bashrc").chmod(0o600)
        with self.assertRaises(installer.InstallError):
            installer.restore_transaction(directory, self.layout)

    def test_restore_refuses_other_home(self):
        directory = self.install()
        with self.assertRaises(installer.InstallError):
            installer.restore_transaction(directory, installer.Layout(str(self.root / "other")))

    def test_restore_dry_run_changes_nothing(self):
        directory = self.install()
        before = (directory / "manifest.json").read_bytes()
        installer.restore_transaction(directory, self.layout, dry_run=True)
        self.assertTrue((self.home / ".bashrc").exists())
        self.assertEqual((directory / "manifest.json").read_bytes(), before)

    def test_created_directories_removed_only_when_empty(self):
        directory = self.install([self.item("new/child/config")])
        (self.home / "new/keep").write_text("user data")
        installer.restore_transaction(directory, self.layout)
        self.assertFalse((self.home / "new/child").exists())
        self.assertEqual((self.home / "new/keep").read_text(), "user data")

    def test_failed_second_file_automatically_rolls_back(self):
        (self.home / "a").write_text("original")
        real_copy = installer.atomic_copy
        calls = 0
        def fail_once(source, target, mode):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("simulated write failure")
            real_copy(source, target, mode)
        with patch.object(installer, "atomic_copy", side_effect=fail_once):
            with self.assertRaises(OSError):
                self.install([self.item("a"), self.item("b")])
        self.assertEqual((self.home / "a").read_text(), "original")
        self.assertFalse((self.home / "b").exists())

    def test_failure_after_replace_still_rolls_back(self):
        real_copy = installer.atomic_copy
        def replace_then_interrupt(source, target, mode):
            real_copy(source, target, mode)
            raise KeyboardInterrupt
        with patch.object(installer, "atomic_copy", side_effect=replace_then_interrupt):
            with self.assertRaises(KeyboardInterrupt):
                self.install()
        self.assertFalse((self.home / ".bashrc").exists())

    def test_dry_run_has_no_writes_or_subprocesses(self):
        with patch.object(installer.subprocess, "run") as run:
            result = installer.main(["--home", str(self.home), "--dry-run"])
        self.assertEqual(result, 0)
        run.assert_not_called()
        self.assertEqual(list(self.home.iterdir()), [])

    def test_unknown_component_and_restore_path_traversal(self):
        for args in (["--only", "invalid", "--dry-run"], ["--restore", "../../etc"]):
            self.assertEqual(installer.main(["--home", str(self.home), *args]), 1)
        self.assertEqual(list(self.home.iterdir()), [])

    def test_build_failure_leaves_installed_files_untouched(self):
        target = self.home / ".bashrc"
        target.write_text("original")
        with patch.object(installer, "check_build_dependencies"), \
             patch.object(installer.os, "geteuid", return_value=1000), \
             patch.object(installer, "build_items", side_effect=subprocess.CalledProcessError(1, "make")):
            result = installer.main(["--home", str(self.home), "--only", "shell,everbar", "--yes"])
        self.assertEqual(result, 1)
        self.assertEqual(target.read_text(), "original")
        self.assertFalse(self.layout.backups.exists())

    def test_explicit_home_ignores_host_xdg_paths(self):
        with patch.dict(os.environ, {"XDG_CONFIG_HOME": "/do-not-touch", "XDG_STATE_HOME": "/do-not-touch"}):
            layout = installer.Layout(str(self.home))
        self.assertEqual(layout.config, self.home / ".config")
        self.assertTrue(layout.backups.is_relative_to(self.home))

    def test_single_installer_lock_and_symlink_lock_safety(self):
        with installer.installation_lock(self.layout):
            with self.assertRaises(installer.InstallError):
                with installer.installation_lock(self.layout):
                    self.fail("second installer acquired the lock")
        lock = self.layout.state / "everforest-dotfiles/install.lock"
        lock.unlink()
        lock.symlink_to(self.source)
        with self.assertRaises(OSError):
            with installer.installation_lock(self.layout):
                pass
        self.assertEqual(self.source.read_text(), "new contents\n")

    def test_pins_and_overlay_files_exist(self):
        locks = installer.load_locks()
        for name in ("dwl", "dwlb"):
            for relative in locks[name]["overlays"]:
                self.assertTrue((ROOT / "src" / name / relative).is_file())

    def test_pinned_commit_mismatch_rejected(self):
        with patch.object(installer, "run"), patch.object(installer.subprocess, "check_output", return_value="bad\n"):
            with self.assertRaises(installer.InstallError):
                installer.fetch_source("dwl", installer.load_locks()["dwl"], self.root)

    def test_build_preserves_upstream_target_specific_cflags(self):
        work = self.root / "build with spaces"
        work.mkdir()
        with patch.object(installer, "run") as run:
            installer.build_items(self.layout, {"everbar"}, work, 2)
        command = run.call_args.args[0]
        self.assertFalse(any(str(arg).startswith("CFLAGS=") for arg in command))
        self.assertIn("-ffile-prefix-map=", run.call_args.kwargs["env"]["CFLAGS"])

    def test_snapshot_has_all_project_sources_but_no_build_artifacts(self):
        paths = {p.relative_to(ROOT).as_posix() for p in installer.source_files()}
        self.assertIn("src/everbar/everbar.h", paths)
        self.assertIn("tests/reference_everbar.py", paths)
        self.assertIn("AUTHORS.md", paths)
        self.assertIn("scripts/install.py", paths)
        self.assertIn("screenshots/desktop.png", paths)
        self.assertNotIn("src/everbar/everbar", paths)
        self.assertNotIn("src/everbar/main.o", paths)
        self.assertFalse(any(".git/" in path or "__pycache__" in path for path in paths))

    def test_every_configuration_file_has_a_live_target(self):
        items = installer.config_items(self.layout, set(installer.COMPONENTS))
        live_sources = {i.source.relative_to(ROOT).as_posix() for i in items if not i.target.is_relative_to(self.layout.share)}
        configs = {p.relative_to(ROOT).as_posix() for p in (ROOT / "configs").rglob("*") if p.is_file()}
        self.assertTrue(configs.issubset(live_sources))


if __name__ == "__main__":
    unittest.main(verbosity=2)
