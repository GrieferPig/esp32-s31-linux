#!/usr/bin/env python3
"""Fast, offline tests for configuration boundaries and full board contracts."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("s31_native_configuration", ROOT / "tools/build/configure.py")
configure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(configure)

KERNEL_BASE = """CONFIG_XIP_KERNEL=y
CONFIG_ESP32S31_RADIO_XIP=y
CONFIG_MODULES=y
CONFIG_TRIM_UNUSED_KSYMS=y
CONFIG_EXT4_FS=y
CONFIG_CC_OPTIMIZE_FOR_SIZE=y
CONFIG_MTD_PARTITIONED_MASTER=y
CONFIG_MMC=y
CONFIG_MMC_BLOCK=y
CONFIG_MMC_DW=y
CONFIG_MMC_DW_PLTFM=y
CONFIG_I2C=y
CONFIG_SPI=y
CONFIG_CAN=y
"""


class ConfigurationBoundary(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.source = self.root / "linux"
        configs = self.source / "arch/riscv/configs"
        configs.mkdir(parents=True)
        (configs / "esp32s31_defconfig").write_text(KERNEL_BASE)
        self.compiler = self.root / "toolchain/bin/cross-gcc"
        self.compiler.parent.mkdir(parents=True)
        self.compiler.write_text("compiler identity one\n")
        self.fragment = self.root / "board.config"
        self.fragment.write_text("CONFIG_S31_TEST=y\n")

    def native_args(self):
        return argparse.Namespace(
            source=self.source, output=self.root / "out" / "linux",
            kind="linux", defconfig="esp32s31_defconfig",
            fragment=[self.fragment], compiler=str(self.compiler),
            cmdline="console=ttyS0", cross=str(self.root / "cross-"),
            ksyms_whitelist=None,
        )

    def fake_make(self, argv, **_kwargs):
        """Model only the native configuration outputs; never build/download."""
        output = Path(next(str(x)[2:] for x in argv if str(x).startswith("O=")))
        output.mkdir(parents=True, exist_ok=True)
        if argv[-1] == "esp32s31_defconfig":
            (output / ".config").write_text(KERNEL_BASE)
        elif argv[-1] == "defconfig":
            definition = Path(next(str(x).split("=", 1)[1] for x in argv if str(x).startswith("BR2_DEFCONFIG=")))
            (output / ".config").write_text(definition.read_text())
        elif argv[-1] not in {"olddefconfig", "clean"}:
            self.fail(f"Unexpected native action: {argv}")

    def test_unchanged_native_inputs_skip_make_and_preserve_stamp(self):
        args = self.native_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.native(args)
            self.assertEqual(run.call_count, 2)
            stamp = args.output / ".s31-config.json"
            before = stamp.stat().st_mtime_ns
            run.reset_mock()
            configure.native(args)
            run.assert_not_called()
            self.assertEqual(stamp.stat().st_mtime_ns, before)

    def test_missing_native_config_is_regenerated(self):
        args = self.native_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.native(args)
            (args.output / ".config").unlink()
            run.reset_mock()
            configure.native(args)
            self.assertEqual(run.call_count, 2)
            self.assertTrue((args.output / ".config").is_file())

    def test_fragment_and_compiler_changes_invalidate_native_config(self):
        args = self.native_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.native(args)
            for path in (self.fragment, self.compiler):
                with self.subTest(input=path.name):
                    path.write_text(path.read_text() + "# changed content\n")
                    run.reset_mock()
                    configure.native(args)
                    expected = ["esp32s31_defconfig", "olddefconfig"]
                    if path == self.compiler:
                        expected.insert(0, "clean")
                    self.assertEqual([call.args[0][-1] for call in run.call_args_list], expected)
                    run.reset_mock()
                    configure.native(args)
                    run.assert_not_called()

    def test_full_board_contract_rejects_missing_peripheral(self):
        args = self.native_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make):
            configure.native(args)
        config = args.output / ".config"
        config.write_text(config.read_text().replace("CONFIG_MMC=y", "# CONFIG_MMC is not set"))
        with self.assertRaisesRegex(SystemExit, "CONFIG_MMC: expected y"):
            configure.contracts(args)

    def test_radio_whitelist_is_resolved_and_content_tracked_without_clean(self):
        args = self.native_args()
        args.ksyms_whitelist = self.root / "out/generated/radio-kernel-symbols.txt"
        args.ksyms_whitelist.parent.mkdir(parents=True)
        args.ksyms_whitelist.write_text("radio_import_one\n")
        self.fragment.write_text('CONFIG_UNUSED_KSYMS_WHITELIST=""\n')
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.native(args)
            config = configure.parse_config(args.output / ".config")
            self.assertEqual(config['CONFIG_UNUSED_KSYMS_WHITELIST'],
                             json.dumps(str(args.ksyms_whitelist.resolve())))
            stamp = args.output / ".s31-config.json"
            before = stamp.read_bytes()
            run.reset_mock()
            configure.native(args)
            run.assert_not_called()
            args.ksyms_whitelist.write_text("radio_import_two\n")
            configure.native(args)
            self.assertEqual([call.args[0][-1] for call in run.call_args_list],
                             ["esp32s31_defconfig", "olddefconfig"])
            self.assertNotEqual(before, stamp.read_bytes())
            run.reset_mock()
            configure.native(args)
            run.assert_not_called()
        config = args.output / ".config"
        config.write_text(config.read_text().replace(
            json.dumps(str(args.ksyms_whitelist.resolve())), '""'))
        with self.assertRaisesRegex(SystemExit, "CONFIG_UNUSED_KSYMS_WHITELIST"):
            configure.contracts(args)

    def test_missing_radio_whitelist_fails_before_make(self):
        args = self.native_args()
        args.ksyms_whitelist = self.root / "missing-symbols"
        with mock.patch.object(configure, "run") as run:
            with self.assertRaises(FileNotFoundError):
                configure.native(args)
            run.assert_not_called()

    def test_radio_exports_are_sorted_unique_and_only_written_on_change(self):
        args = argparse.Namespace(nm="cross-nm", payload=self.root / "payload.o",
                                  output=self.root / "generated/symbols.txt")
        with mock.patch.object(configure.subprocess, "check_output",
                               return_value="z_import U         \na_import w         \nz_import U\n") as nm:
            configure.radio_exports(args)
            nm.assert_called_once_with(
                ["cross-nm", "--undefined-only", "--format=posix", str(args.payload)], text=True)
            self.assertEqual(args.output.read_text(), "a_import\nz_import\n")
            before = args.output.stat().st_mtime_ns
            configure.radio_exports(args)
            self.assertEqual(args.output.stat().st_mtime_ns, before)
        with mock.patch.object(configure.subprocess, "check_output",
                               side_effect=subprocess.CalledProcessError(1, "cross-nm")):
            with self.assertRaises(subprocess.CalledProcessError):
                configure.radio_exports(args)
            self.assertEqual(args.output.read_text(), "a_import\nz_import\n")

    def buildroot_args(self):
        config = self.root / "buildroot_defconfig"
        if not config.exists():
            config.write_text('BR2_riscv=y\nBR2_TOOLCHAIN_EXTERNAL_PATH="old"\nBR2_ROOTFS_OVERLAY="old"\n')
        return argparse.Namespace(
            source=self.root / "buildroot", output=self.root / "out" / "buildroot",
            source_config=config, compiler=str(self.compiler), fragment=[],
            toolchain=str(self.root / "toolchain"),
            overlay=str(self.root / "out" / "staging/overlay"),
            static_overlay=str(self.root / "overlay"), external=self.root / "external",
        )

    def test_buildroot_unchanged_configuration_skips_make(self):
        args = self.buildroot_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.buildroot(args)
            self.assertEqual(run.call_count, 1)
            generated = args.output.parent / "generated/buildroot_defconfig"
            self.assertIn(f'BR2_ROOTFS_OVERLAY="{args.static_overlay} {args.overlay}"', generated.read_text())
            run.reset_mock()
            configure.buildroot(args)
            run.assert_not_called()

    def test_buildroot_changed_selection_refuses_populated_output(self):
        args = self.buildroot_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.buildroot(args)
            args.source_config.write_text(args.source_config.read_text() + "BR2_PACKAGE_TEST=y\n")
            run.reset_mock()
            with self.assertRaisesRegex(SystemExit, "buildroot-reconfigure"):
                configure.buildroot(args)
            run.assert_not_called()

    def test_missing_buildroot_config_is_regenerated(self):
        args = self.buildroot_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.buildroot(args)
            (args.output / ".config").unlink()
            run.reset_mock()
            configure.buildroot(args)
            self.assertEqual(run.call_count, 1)

    def test_buildroot_edited_resolved_config_refuses_stale_target(self):
        args = self.buildroot_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.buildroot(args)
            config = args.output / ".config"
            config.write_text(config.read_text() + "BR2_PACKAGE_CHANGED=y\n")
            run.reset_mock()
            with self.assertRaisesRegex(SystemExit, "buildroot-reconfigure"):
                configure.buildroot(args)
            run.assert_not_called()

    def test_buildroot_overlay_path_change_refuses_stale_config(self):
        args = self.buildroot_args()
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.buildroot(args)
            args.static_overlay = str(self.root / "another-overlay")
            run.reset_mock()
            with self.assertRaisesRegex(SystemExit, "buildroot-reconfigure"):
                configure.buildroot(args)
            run.assert_not_called()

    def package_args(self, package="s31-tools", built=True):
        output = self.root / "out/buildroot"
        work = output / "build" / (package + "-1.0")
        work.mkdir(parents=True)
        (output / ".config").write_text("BR2_riscv=y\n")
        (work / ".stamp_rsynced").touch()
        if built:
            (work / ".stamp_built").touch()
        source = self.root / "package-source.c"
        source.write_text("/* initial source */\n")
        return argparse.Namespace(
            source=self.root / "buildroot", output=output,
            external=self.root / "external", package=package,
            stamp=output / ("." + package + "-inputs.json"),
            input=[source], value=[], downloads=self.root / "cache/downloads",
            jobs="2",
        )

    def test_package_rebuild_preserves_offline_cache_arguments(self):
        args = self.package_args()
        with mock.patch.object(configure, "run") as run:
            configure.package_inputs(args)
            run.assert_called_once()
            command = [str(value) for value in run.call_args.args[0]]
            self.assertEqual(command[-1], "s31-tools-rebuild")
            for transport in ("WGET", "CURL", "GIT", "SVN", "HG"):
                self.assertIn("BR2_" + transport + "=false", command)
            self.assertIn("BR2_DL_DIR=" + str(args.downloads), command)
            self.assertIn("BR2_JLEVEL=2", command)
            run.reset_mock()
            configure.package_inputs(args)
            run.assert_not_called()

    def test_btstack_changed_inputs_repatch_clean_local_copy(self):
        args = self.package_args("btstack-s31")
        with mock.patch.object(configure, "run") as run:
            configure.package_inputs(args)
            self.assertEqual([call.args[0][-1] for call in run.call_args_list],
                             ["btstack-s31-dirclean", "btstack-s31"])

    def test_incomplete_package_build_resyncs_changed_source(self):
        args = self.package_args(built=False)
        with mock.patch.object(configure, "run") as run:
            configure.package_inputs(args)
            run.assert_called_once()
            self.assertEqual(run.call_args.args[0][-1], "s31-tools-rebuild")

    def test_failed_package_rebuild_does_not_advance_identity(self):
        args = self.package_args()
        with mock.patch.object(configure, "run"):
            configure.package_inputs(args)
        before = args.stamp.read_bytes()
        args.input[0].write_text("/* changed source */\n")
        with mock.patch.object(configure, "run", side_effect=RuntimeError("build failed")):
            with self.assertRaisesRegex(RuntimeError, "build failed"):
                configure.package_inputs(args)
        self.assertEqual(args.stamp.read_bytes(), before)


    def test_subtool_or_sysroot_change_invalidates_native_objects(self):
        args = self.native_args()
        linker = self.compiler.parent / "cross-ld"
        cc1 = self.compiler.parent.parent / "libexec/gcc/cc1"
        library = self.compiler.parent.parent / "riscv32-esp-linux-musl/sysroot/usr/lib/libc.a"
        cc1.parent.mkdir(parents=True)
        library.parent.mkdir(parents=True)
        linker.write_text("linker one\n")
        cc1.write_text("cc1 one\n")
        library.write_text("library one\n")
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.native(args)
            for path in (cc1, linker, library):
                with self.subTest(input=path.name):
                    path.write_text(path.read_text() + "changed\n")
                    run.reset_mock()
                    configure.native(args)
                    self.assertEqual([call.args[0][-1] for call in run.call_args_list],
                                     ["clean", "esp32s31_defconfig", "olddefconfig"])

    def test_buildroot_sysroot_change_refuses_stale_target(self):
        args = self.buildroot_args()
        library = self.compiler.parent.parent / "lib/libc.a"
        library.parent.mkdir()
        library.write_text("library one\n")
        with mock.patch.object(configure, "run", side_effect=self.fake_make) as run:
            configure.buildroot(args)
            library.write_text("library two\n")
            run.reset_mock()
            with self.assertRaisesRegex(SystemExit, "buildroot-reconfigure"):
                configure.buildroot(args)
            run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
