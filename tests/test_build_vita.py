"""Host-side tests for Vita cross-build validation; no SDK or ROM required."""
import contextlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

SPEC = importlib.util.spec_from_file_location("build_vita", Path(__file__).resolve().parents[1] / "scripts/build_vita.py")
BUILD = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BUILD)


class BuildVitaTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk with spaces"
        (self.sdk / "share").mkdir(parents=True)
        (self.sdk / "share/vita.toolchain.cmake").touch()
        patcher = patch.object(BUILD, "ROOT", self.root)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.output = io.StringIO()

    def call(self, *args):
        with contextlib.redirect_stdout(self.output), contextlib.redirect_stderr(self.output):
            return BUILD.main(["--vitasdk", str(self.sdk), *args])

    def generated(self, game="daytona93", host="build", *, hook=True):
        host = self.root / host
        for name in BUILD.REQUIRED:
            path = host / "gen" / name.replace("daytona93", game)
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        if hook:
            (host / "gen" / game / "chunk_0.cpp").write_text("rt::hook_draw_list();\n")
        return host

    def rom(self, game, extension="zip"):
        path = self.root / "roms" / f"{game}.{extension}"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()  # discovery only; the script must not read ROM archives
        return path

    def fake_builder(self, *, omit_package=False, omit_self=False, omit_embedded=False,
                     fail_game=None):
        """Model CMake's artifacts, preserving argument and stage visibility."""
        configurations = {}

        def build(command, **kwargs):
            self.assertEqual(command[0], "cmake")
            if "-S" in command:
                directory = Path(command[command.index("-B") + 1])
                flags = dict(arg[2:].split("=", 1) for arg in command if arg.startswith("-D"))
                configurations[directory] = flags
                return
            self.assertEqual(command[1], "--build")
            directory = Path(command[2])
            flags = configurations[directory]
            if flags["DAYTONA_VITA_COMPILE_CHECK"] == "ON":
                return
            if flags["DAYTONA_VITA_ROMSET"] == fail_game:
                raise subprocess.CalledProcessError(1, command)
            directory.mkdir(parents=True, exist_ok=True)
            if not omit_self:
                (directory / "eboot.bin").write_bytes(flags["DAYTONA_VITA_ROMSET"].encode())
            if not omit_package:
                with ZipFile(directory / "daytona_vita.vpk", "w") as package:
                    package.writestr("eboot.bin", flags["DAYTONA_VITA_ROMSET"])
                    if flags["DAYTONA_VITA_REV_A_SELF"] and not omit_embedded:
                        package.write(flags["DAYTONA_VITA_REV_A_SELF"], "daytona.self")

        return build

    @staticmethod
    def configurations(run):
        return [call.args[0] for call in run.call_args_list if "-S" in call.args[0]]

    def test_missing_sdk(self):
        with self.assertRaises(SystemExit):
            self.call("--vitasdk", str(self.root / "missing"))
        self.assertIn("missing VitaSDK toolchain", self.output.getvalue())

    def test_missing_generated_code(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call()
            run.assert_not_called()
        self.assertIn("host-generated code", self.output.getvalue())

    def test_reject_host_directory(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--build-dir", "build")

    def test_reject_generated_directory(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--build-dir", "build/gen/vita")

    def test_reject_nonpositive_jobs(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--jobs", "0")

    def test_compile_check_skips_rom_and_never_executes_importers(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.assertEqual(self.call("--compile-check"), 0)
        self.assertEqual(run.call_count, 2)
        configure, compile_ = run.call_args_list
        self.assertEqual(configure.args[0][0], "cmake")
        self.assertIn("-DDAYTONA_VITA_COMPILE_CHECK=ON", configure.args[0])
        self.assertIn(f"-DCMAKE_TOOLCHAIN_FILE={self.sdk}/share/vita.toolchain.cmake", configure.args[0])
        self.assertEqual(compile_.args[0][0:2], ["cmake", "--build"])
        self.assertEqual(configure.kwargs["env"]["VITASDK"], str(self.sdk))
        self.assertTrue(configure.kwargs["env"]["PATH"].startswith(str(self.sdk / "bin") + os.pathsep))
        self.assertIn("No linked game or VPK", self.output.getvalue())

    def test_default_optimization_enabled(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check")
        self.assertIn("-DDAYTONA_VITA_RENDER_OPT=ON", run.call_args_list[0].args[0])

    def test_reference_renderer(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--reference-renderer")
        self.assertIn("-DDAYTONA_VITA_RENDER_OPT=OFF", run.call_args_list[0].args[0])

    def test_success_requires_package(self):
        for name in BUILD.REQUIRED:
            path = self.root / "build/gen" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()  # presence marker, not generated game code
        with patch.object(BUILD.subprocess, "run"):
            with self.assertRaises(RuntimeError):
                self.call()

    def test_gpu_package_rejects_missing_draw_distance_hook(self):
        for name in BUILD.REQUIRED:
            path = self.root / "build/gen" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.touch()
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--gpu-fast")
            run.assert_not_called()
        self.assertIn("draw-distance hook", self.output.getvalue())

    def test_cores_pinned_by_default(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--gpu-fast")
        self.assertIn("-DDAYTONA_VITA_FREE_CORES=OFF", run.call_args_list[0].args[0])

    def test_free_core(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--gpu-gl", "--free-core")
        configure = run.call_args_list[0].args[0]
        self.assertIn("-DDAYTONA_VITA_FREE_CORES=ON", configure)
        self.assertIn("-DDAYTONA_VITA_GPU_GL=ON", configure)
        self.assertIn("-DDAYTONA_VITA_GPU_FAST=OFF", configure)

    def test_free_core_requires_gpu_build(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--compile-check", "--free-core")
            run.assert_not_called()

    def test_renderers_are_exclusive(self):
        with self.assertRaises(SystemExit):
            self.call("--compile-check", "--gpu-fast", "--gpu-gl")

    def test_custom_host_directory(self):
        with patch.object(BUILD.subprocess, "run") as run:
            self.call("--compile-check", "--host-build-dir", "host build", "--build-dir", "cross build")
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/host build/gen", run.call_args_list[0].args[0])

    def test_auto_builds_both_and_packages_revision_a(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--gpu-gl"), 0)
        self.assertEqual(run.call_count, 4)
        revision_a, launcher = self.configurations(run)
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona", revision_a)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build-daytona/gen", revision_a)
        self.assertEqual(revision_a[revision_a.index("-B") + 1], str(self.root / "build/vita/revision-a"))
        self.assertIn("-DDAYTONA_VITA_REV_A_SELF=", revision_a)
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona93", launcher)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build/gen", launcher)
        self.assertIn(f"-DDAYTONA_VITA_REV_A_SELF={self.root}/build/vita/revision-a/eboot.bin", launcher)
        with ZipFile(self.root / "build/vita/daytona_vita.vpk") as package:
            self.assertEqual(package.read("eboot.bin"), b"daytona93")
            self.assertEqual(package.read("daytona.self"), b"daytona")
        self.assertIn("ux0:data/daytona93/daytona93.zip", self.output.getvalue())
        self.assertIn("ux0:data/daytona/daytona.zip", self.output.getvalue())

    def test_auto_discovers_7z_archives(self):
        self.rom("daytona93", "7z")
        self.rom("daytona", "7z")
        self.generated()
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--gpu-fast"), 0)
        self.assertEqual(len(self.configurations(run)), 2)

    def test_auto_with_only_1993_builds_one_package(self):
        self.rom("daytona93")
        self.generated()
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call(), 0)
        self.assertEqual(run.call_count, 2)
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona93", self.configurations(run)[0])
        self.assertIn("-DDAYTONA_VITA_REV_A_SELF=", self.configurations(run)[0])

    def test_auto_with_only_revision_a_uses_revision_a_host(self):
        self.rom("daytona")
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--gpu-gl"), 0)
        self.assertEqual(run.call_count, 2)
        configure = self.configurations(run)[0]
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona", configure)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build-daytona/gen", configure)
        self.assertEqual(configure[configure.index("-B") + 1], str(self.root / "build/vita"))

    def test_no_rom_archive_retains_existing_generated_1993_workflow(self):
        self.generated()
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call(), 0)
        self.assertEqual(run.call_count, 2)
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona93", self.configurations(run)[0])

    def test_auto_revision_a_requires_gpu_renderer(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call()
            run.assert_not_called()
        self.assertIn("--gpu-fast or --gpu-gl", self.output.getvalue())

    def test_explicit_1993_skips_available_revision_a(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--set", "daytona93", "--gpu-gl"), 0)
        self.assertEqual(run.call_count, 2)
        self.assertIn("-DDAYTONA_VITA_REV_A_SELF=", self.configurations(run)[0])

    def test_explicit_revision_a_uses_supplied_host_unchanged(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated("daytona", "custom host")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--set", "daytona", "--gpu-gl", "--host-build-dir", "custom host"), 0)
        self.assertEqual(run.call_count, 2)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/custom host/gen", self.configurations(run)[0])

    def test_compile_check_ignores_both_roms_and_code_generation(self):
        self.rom("daytona93")
        self.rom("daytona")
        with patch.object(BUILD.subprocess, "run") as run, patch.object(BUILD, "vita_code") as vita_code:
            self.assertEqual(self.call("--compile-check", "--gpu-gl", "--fast-inaccuracy"), 0)
            vita_code.assert_not_called()
        self.assertEqual(run.call_count, 2)
        configure = self.configurations(run)[0]
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona93", configure)
        self.assertIn("-DDAYTONA_VITA_REV_A_SELF=", configure)
        self.assertFalse((self.root / "build/vita/daytona_vita.vpk").exists())

    def test_both_hosts_are_preflighted_before_any_build(self):
        self.rom("daytona93")
        self.rom("daytona")
        # Revision A goes first, but absent launcher sources must stop it too.
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--gpu-gl")
            run.assert_not_called()
        self.assertIn("host-generated code", self.output.getvalue())

    def test_auto_missing_revision_a_sources_fails_before_any_build(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--gpu-gl")
            run.assert_not_called()
        self.assertIn(str(self.root / "build-daytona/gen/daytona"), self.output.getvalue())

    def test_both_draw_distance_hooks_are_preflighted(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated(hook=False)
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--gpu-gl")
            run.assert_not_called()
        self.assertIn("draw-distance hook", self.output.getvalue())

    def test_auto_cannot_overwrite_revision_a_host_or_generated_sources(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")
        for directory in ("build-daytona", "build-daytona/gen", "build-daytona/gen/vita"):
            with self.subTest(directory=directory), patch.object(BUILD.subprocess, "run") as run:
                with self.assertRaises(SystemExit):
                    self.call("--gpu-gl", "--build-dir", directory)
                run.assert_not_called()

    def test_auto_host_alias_preserves_setup_revision_a_directory_name(self):
        self.rom("daytona93")
        self.rom("daytona")
        host = self.generated(host="real host")
        self.generated("daytona", "host alias-daytona")
        (self.root / "host alias").symlink_to(host, target_is_directory=True)
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--gpu-gl", "--host-build-dir", "host alias"), 0)
        revision_a, launcher = self.configurations(run)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/host alias-daytona/gen", revision_a)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/real host/gen", launcher)

    def test_revision_a_output_symlink_cannot_alias_host_or_main_output(self):
        self.rom("daytona93")
        self.rom("daytona")
        host = self.generated()
        revision_a_host = self.generated("daytona", "build-daytona")
        output = self.root / "cross build"
        output.mkdir()
        secondary = output / "revision-a"
        for target in (host, revision_a_host, output):
            with self.subTest(target=target):
                secondary.symlink_to(target, target_is_directory=True)
                try:
                    with patch.object(BUILD.subprocess, "run") as run:
                        with self.assertRaises(SystemExit):
                            self.call("--gpu-gl", "--build-dir", str(output))
                        run.assert_not_called()
                finally:
                    secondary.unlink()

    def test_failed_revision_a_build_does_not_use_stale_self(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")
        stale = self.root / "build/vita/revision-a/eboot.bin"
        stale.parent.mkdir(parents=True)
        stale.write_bytes(b"stale")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder(fail_game="daytona")) as run:
            with self.assertRaises(subprocess.CalledProcessError):
                self.call("--gpu-gl")
        self.assertEqual(len(self.configurations(run)), 1)
        self.assertFalse((self.root / "build/vita/daytona_vita.vpk").exists())

    def test_revision_a_build_must_produce_self_before_launcher(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder(omit_self=True)) as run:
            with self.assertRaises(RuntimeError):
                self.call("--gpu-gl")
        self.assertEqual(len(self.configurations(run)), 1)

    def test_dual_package_must_contain_revision_a(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder(omit_embedded=True)):
            with self.assertRaises(RuntimeError):
                self.call("--gpu-gl")

    def test_dual_package_rejects_stale_embedded_revision_a(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")
        builder = self.fake_builder()

        def stale_archive(command, **kwargs):
            builder(command, **kwargs)
            if "--build" in command and Path(command[2]) == self.root / "build/vita":
                with ZipFile(self.root / "build/vita/daytona_vita.vpk", "w") as package:
                    package.writestr("eboot.bin", "daytona93")
                    package.writestr("daytona.self", "stale revision a")

        with patch.object(BUILD.subprocess, "run", side_effect=stale_archive):
            with self.assertRaisesRegex(RuntimeError, "outdated daytona.self"):
                self.call("--gpu-gl")

    def test_rejects_invalid_vpk_or_missing_main_executable(self):
        self.generated()
        for contents in ("invalid", "missing eboot"):
            with self.subTest(contents=contents):
                builder = self.fake_builder()

                def damaged_archive(command, **kwargs):
                    builder(command, **kwargs)
                    if "--build" in command:
                        path = Path(command[2]) / "daytona_vita.vpk"
                        if contents == "invalid":
                            path.write_bytes(b"not a ZIP archive")
                        else:
                            with ZipFile(path, "w") as package:
                                package.writestr("sce_sys/param.sfo", "metadata only")

                with patch.object(BUILD.subprocess, "run", side_effect=damaged_archive):
                    with self.assertRaises(RuntimeError):
                        self.call()

    def test_compile_check_preserves_explicit_revision_a(self):
        self.rom("daytona93")
        self.rom("daytona")
        with patch.object(BUILD.subprocess, "run") as run, patch.object(BUILD, "vita_code") as vita_code:
            self.assertEqual(self.call("--compile-check", "--set", "daytona", "--gpu-gl", "--fast-inaccuracy"), 0)
            vita_code.assert_not_called()
        self.assertEqual(run.call_count, 2)
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona", self.configurations(run)[0])

    def test_manual_self_preserves_single_launcher_build(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        manual = self.root / "manual build/eboot.bin"
        manual.parent.mkdir()
        manual.write_bytes(b"supplied revision a")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--gpu-gl", "--revision-a-self", str(manual)), 0)
        self.assertEqual(run.call_count, 2)
        configure = self.configurations(run)[0]
        self.assertIn("-DDAYTONA_VITA_ROMSET=daytona93", configure)
        self.assertIn(f"-DDAYTONA_VITA_REV_A_SELF={manual}", configure)

    def test_missing_manual_self_fails_before_build(self):
        self.generated()
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--gpu-gl", "--revision-a-self", "missing/eboot.bin")
            run.assert_not_called()

    def test_manual_self_cannot_be_bundled_with_revision_a_launcher(self):
        with patch.object(BUILD.subprocess, "run") as run:
            with self.assertRaises(SystemExit):
                self.call("--gpu-gl", "--set", "daytona", "--revision-a-self", "manual/eboot.bin")
            run.assert_not_called()

    def test_custom_paths_and_options_apply_to_both_builds(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated(host="host build")
        self.generated("daytona", "host build-daytona")
        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run:
            self.assertEqual(self.call("--gpu-gl", "--release", "--free-core", "--reference-renderer",
                                       "--no-fast-paths", "--jobs", "3", "--host-build-dir", "host build",
                                       "--build-dir", "cross build", "--vitagl-dir", "custom vitaGL"), 0)
        revision_a, launcher = self.configurations(run)
        expected = ["-DDAYTONA_VITA_GPU_GL=ON", "-DDAYTONA_VITA_GPU_FAST=OFF", "-DDAYTONA_VITA_LTO=ON",
                    "-DDAYTONA_VITA_DIAGNOSTICS=OFF", "-DDAYTONA_VITA_FREE_CORES=ON",
                    "-DDAYTONA_VITA_RENDER_OPT=OFF", "-DDAYTONA_VITA_FAST_PATHS=OFF",
                    f"-DDAYTONA_VITAGL_DIR={self.root}/custom vitaGL"]
        for configure in (revision_a, launcher):
            for option in expected:
                self.assertIn(option, configure)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/host build-daytona/gen", revision_a)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/host build/gen", launcher)
        self.assertIn(f"-DDAYTONA_VITA_REV_A_SELF={self.root}/cross build/revision-a/eboot.bin", launcher)
        for invocation in run.call_args_list:
            if "--build" in invocation.args[0]:
                self.assertEqual(invocation.args[0][-2:], ["--parallel", "3"])

    def test_code_generation_runs_for_each_selected_rom(self):
        self.rom("daytona93")
        self.rom("daytona")
        self.generated()
        self.generated("daytona", "build-daytona")

        def code(host, gen, build, mode, game):
            return build / "gen_vita", f"test {game}"

        with patch.object(BUILD.subprocess, "run", side_effect=self.fake_builder()) as run, \
                patch.object(BUILD, "vita_code", side_effect=code) as vita_code:
            self.assertEqual(self.call("--gpu-gl", "--fast-inaccuracy"), 0)
        self.assertEqual([call.args[-1] for call in vita_code.call_args_list], ["daytona", "daytona93"])
        self.assertEqual([call.args[-2] for call in vita_code.call_args_list], ["fast_inaccuracy"] * 2)
        revision_a, launcher = self.configurations(run)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build/vita/revision-a/gen_vita", revision_a)
        self.assertIn(f"-DDAYTONA_GEN_ROOT={self.root}/build/vita/gen_vita", launcher)

    def test_livearea_summary(self):
        self.assertEqual(BUILD.livearea_summary(), "none (Vita defaults)")
        folder = self.root / "platform/vita/logo_livearea"
        folder.mkdir(parents=True)
        (folder / "README.md").touch()
        (folder / "bg.png").touch()
        (folder / "icon0.png").touch()
        self.assertEqual(BUILD.livearea_summary(), "icon0.png, bg.png")


if __name__ == "__main__":
    unittest.main()
