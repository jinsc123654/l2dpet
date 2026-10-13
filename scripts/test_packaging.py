#!/usr/bin/env python3
"""Packaging regressions; uses temporary fixtures, never the user's install/data.

Run: python3 scripts/test_packaging.py
Linux release smoke checks also run automatically in linux_build.sh.
"""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="l2dpet-packaging-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.home = self.base / "home space &$`\"'%\\中文"
        self.home.mkdir()
        self.pkg = self.base / "package"
        self.pkg.mkdir()
        for name in ("l2dpet", "install.sh", "uninstall.sh", "desktop-entry.sh"):
            shutil.copyfile(ROOT / "scripts/linux-pack" / name, self.pkg / name)
        for name in ("l2dpet.png", "l2dpet-logo.png", "VERSION", "README.md", "BUILD-INFO"):
            (self.pkg / name).write_text("fixture\n")
        (self.pkg / "assets").mkdir()
        (self.pkg / "assets/Font Awesome 7 Free-Solid-900.otf").write_text("font")
        desktop = self.pkg / "share/applications/l2dpet.desktop"
        desktop.parent.mkdir(parents=True)
        shutil.copyfile(ROOT / "scripts/linux-pack/l2dpet.desktop", desktop)
        model = self.pkg / "models/A/A.model3.json"
        model.parent.mkdir(parents=True)
        model.write_text("{}")
        for name in ("l2dtest", "l2dpet-ui"):
            (self.pkg / name).write_text(
                '#!/usr/bin/env python3\nimport json, os, sys\n'
                'with open(os.environ["CAPTURE"], "w") as f: json.dump(sys.argv[1:], f)\n'
            )
            (self.pkg / name).chmod(0o755)
        self.env = {k: v for k, v in os.environ.items()
                    if not k.startswith(("L2DPET_", "XDG_")) and k not in ("APPIMAGE", "APPDIR")}
        self.env.update(HOME=str(self.home), DISPLAY=":99", WAYLAND_DISPLAY="",
                        L2DPET_FOREGROUND="1", CAPTURE=str(self.base / "args.json"))

    def run_cmd(self, *args, env=None, check=True):
        return subprocess.run(args, env=env or self.env, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=check)

    def install(self, env=None):
        return self.run_cmd("sh", str(self.pkg / "install.sh"), env=env)

    def captured(self):
        return json.loads((self.base / "args.json").read_text())

    def test_default_start_leaves_model_choice_to_player(self):
        self.run_cmd("sh", str(self.pkg / "l2dpet"))
        args = self.captured()
        self.assertEqual(args[0], "0")  # unlimited frames, no explicit model
        self.assertIn("--model-dir", args)
        self.assertFalse(any(a.endswith(".model3.json") for a in args))
        self.assertTrue((self.home / ".local/share/l2dpet/models/A/A.model3.json").is_file())

    def test_explicit_model_and_environment_override(self):
        model = self.pkg / "models/A/A.model3.json"
        env = dict(self.env, L2DPET_MODEL=str(model))
        self.run_cmd("sh", str(self.pkg / "l2dpet"), env=env)
        self.assertEqual(self.captured()[0], str(model))
        second = self.pkg / "B.model3.json"
        second.write_text("{}")
        self.run_cmd("sh", str(self.pkg / "l2dpet"), "start", str(second), env=env)
        self.assertEqual(self.captured()[0], str(second))

    def test_install_resources_and_desktop_special_path(self):
        self.install()
        prefix = self.home / ".local/opt/l2dpet"
        self.assertEqual((prefix / "l2dpet-logo.png").read_bytes(), (self.pkg / "l2dpet-logo.png").read_bytes())
        self.run_cmd(str(prefix / "l2dpet"), "version")
        desktop = self.home / ".local/share/applications/l2dpet.desktop"
        if shutil.which("desktop-file-validate"):
            self.run_cmd("desktop-file-validate", str(desktop))
        if shutil.which("gio"):
            self.run_cmd("gio", "launch", str(desktop))
            for _ in range(50):
                if (self.base / "args.json").exists():
                    break
                time.sleep(0.1)
            self.assertEqual(self.captured()[0], "0")

    def test_autostart_xdg_directory_and_desktop_group(self):
        import configparser
        config = self.base / "custom config"
        env = dict(self.env, XDG_CONFIG_HOME=str(config))
        entry = config / "autostart/l2dpet.desktop"
        self.run_cmd("sh", str(self.pkg / "l2dpet"), "autostart", "on", env=env)
        parser = configparser.ConfigParser(interpolation=None)
        parser.read(entry)
        self.assertEqual(parser["Desktop Entry"]["X-GNOME-Autostart-enabled"], "true")
        self.assertEqual(parser["Desktop Entry"]["Type"], "Application")
        self.assertFalse((self.home / ".config/autostart/l2dpet.desktop").exists())
        self.run_cmd("sh", str(self.pkg / "l2dpet"), "autostart", "off", env=env)
        self.assertFalse(entry.exists())
        env["XDG_CONFIG_HOME"] = "invalid-relative"
        self.run_cmd("sh", str(self.pkg / "l2dpet"), "autostart", "on", env=env)
        self.assertTrue((self.home / ".config/autostart/l2dpet.desktop").exists())

    def test_install_copy_failure_never_reports_success(self):
        fakebin = self.base / "fakebin"
        fakebin.mkdir()
        (fakebin / "cp").write_text("#!/bin/sh\necho simulated-copy-error >&2\nexit 77\n")
        (fakebin / "cp").chmod(0o755)
        env = dict(self.env, PATH=str(fakebin) + os.pathsep + self.env["PATH"])
        result = self.run_cmd("sh", str(self.pkg / "install.sh"), env=env, check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("simulated-copy-error", result.stdout)
        self.assertNotIn("[install] 完成", result.stdout)

    def test_appimage_integration_quotes_shell_and_desktop_paths(self):
        image = self.base / "pet space &$`\"'%\\中文.AppImage"
        image.write_text('#!/bin/sh\nprintf "%s" "$1" > "$CAPTURE"\n')
        image.chmod(0o755)
        env = dict(self.env, APPIMAGE=str(image), APPDIR=str(self.pkg), L2DPET_DRY_RUN="1")
        self.run_cmd("sh", str(self.pkg / "l2dpet"), env=env)
        wrapper = self.home / ".local/bin/l2dpet"
        self.run_cmd("sh", str(wrapper), "literal argument")
        self.assertEqual((self.base / "args.json").read_text(), "literal argument")
        desktop = self.home / ".local/share/applications/l2dpet.desktop"
        if shutil.which("desktop-file-validate"):
            self.run_cmd("desktop-file-validate", str(desktop))
        # Moving the AppImage must update the forwarding script as well as Exec.
        moved = self.base / "moved pet.AppImage"
        image.rename(moved)
        self.run_cmd("sh", str(self.pkg / "l2dpet"), env=dict(env, APPIMAGE=str(moved)))
        self.run_cmd("sh", str(wrapper), "after move")
        self.assertEqual((self.base / "args.json").read_text(), "after move")

    def test_uninstalled_appimage_autostart_uses_actual_file(self):
        import configparser
        image = self.base / "standalone pet.AppImage"
        image.write_text('#!/bin/sh\nprintf "%s" "$1" > "$CAPTURE"\n')
        image.chmod(0o755)
        env = dict(self.env, APPIMAGE=str(image), APPDIR=str(self.pkg))
        self.run_cmd("sh", str(self.pkg / "l2dpet"), "autostart", "on", env=env)
        entry = self.home / ".config/autostart/l2dpet.desktop"
        parser = configparser.ConfigParser(interpolation=None)
        parser.read(entry)
        self.assertIn(str(image), parser["Desktop Entry"]["Exec"])
        self.assertNotIn(".local/opt/l2dpet/l2dpet.AppImage", parser["Desktop Entry"]["Exec"])
        self.assertFalse((self.home / ".local/opt/l2dpet/l2dpet.AppImage").exists())
        if shutil.which("gio"):
            self.run_cmd("gio", "launch", str(entry), env=env)
            for _ in range(50):
                if (self.base / "args.json").exists(): break
                time.sleep(0.1)
            self.assertTrue((self.base / "args.json").exists())

    def test_start_repairs_only_known_broken_appimage_autostart(self):
        image = self.base / "standalone.AppImage"
        image.write_text('#!/bin/sh\nexit 0\n')
        image.chmod(0o755)
        env = dict(self.env, APPIMAGE=str(image), APPDIR=str(self.pkg), L2DPET_DRY_RUN="1")
        self.run_cmd("sh", str(self.pkg / "l2dpet"), "autostart", "on", env=env)
        entry = self.home / ".config/autostart/l2dpet.desktop"
        legacy = self.home / ".local/opt/l2dpet/l2dpet.AppImage"
        old_exec = self.run_cmd("sh", "-c", '. "$1"; desktop_entry_exec "$2"', "sh",
                                str(self.pkg / "desktop-entry.sh"), str(legacy)).stdout.strip()
        old = re.sub(r"^Exec=.*$", lambda _: "Exec=" + old_exec, entry.read_text(), flags=re.M)
        entry.write_text(old)
        self.run_cmd("sh", str(self.pkg / "l2dpet"), env=env)
        self.assertIn(str(image), entry.read_text())
        self.assertNotIn(".local/opt/l2dpet/l2dpet.AppImage", entry.read_text())
        disabled = old + "Hidden=true\n"
        entry.write_text(disabled)
        self.run_cmd("sh", str(self.pkg / "l2dpet"), env=env)
        self.assertEqual(entry.read_text(), disabled)

    def test_installed_appimage_autostart_uses_installed_copy(self):
        image = self.base / "source.AppImage"
        image.write_text('#!/bin/sh\nexit 0\n')
        image.chmod(0o755)
        env = dict(self.env, APPIMAGE=str(image), APPDIR=str(self.pkg))
        self.run_cmd("sh", str(self.pkg / "l2dpet"), "install", "--autostart", env=env)
        entry = self.home / ".config/autostart/l2dpet.desktop"
        self.assertIn(".local/opt/l2dpet/l2dpet.AppImage", entry.read_text())
        self.assertNotIn(str(image), entry.read_text())

    def test_appimage_does_not_overwrite_existing_install(self):
        self.install()
        installed = self.home / ".local/opt/l2dpet/l2dpet"
        before = installed.read_bytes()
        desktop = self.home / ".local/share/applications/l2dpet.desktop"
        entry_before = desktop.read_bytes()
        image = self.base / "another.AppImage"
        image.touch()
        self.run_cmd("sh", str(self.pkg / "l2dpet"), env=dict(
            self.env, APPIMAGE=str(image), APPDIR=str(self.pkg), L2DPET_DRY_RUN="1"))
        self.assertTrue((self.home / ".local/bin/l2dpet").is_symlink())
        self.assertEqual(installed.read_bytes(), before)
        self.assertEqual(desktop.read_bytes(), entry_before)

    def test_appimage_dependency_check_rejects_prefix_collision_omissions(self):
        appdir = self.base / "AppDir"
        libs = appdir / "usr/lib"
        bins = appdir / "usr/bin"
        libs.mkdir(parents=True)
        bins.mkdir(parents=True)
        crypto = self.base / "crypto.c"
        crypto.write_text("int crypto_fixture(void) { return 42; }\n")
        ssl = self.base / "ssl.c"
        ssl.write_text("extern int crypto_fixture(void); int ssl_fixture(void) { return crypto_fixture(); }\n")
        main = self.base / "main.c"
        main.write_text("int main(void) { return 0; }\n")
        self.run_cmd("cc", "-shared", "-fPIC", "-Wl,-soname,libcrypto.so.3", str(crypto),
                     "-o", str(libs / "libcrypto.so.3"))
        self.run_cmd("cc", "-shared", "-fPIC", "-Wl,-soname,libssl.so.3", str(ssl),
                     "-L" + str(libs), "-l:libcrypto.so.3", "-o", str(libs / "libssl.so.3"))
        self.run_cmd("cc", str(main), "-o", str(bins / "l2dtest"))
        shutil.copyfile(bins / "l2dtest", bins / "l2dpet-ui")
        rtmp = self.base / "rtmp.c"
        rtmp.write_text("int rtmp_fixture(void) { return 1; }\n")
        curl = self.base / "curl.c"
        curl.write_text("extern int rtmp_fixture(void); int curl_fixture(void) { return rtmp_fixture(); }\n")
        self.run_cmd("cc", "-shared", "-fPIC", "-Wl,-soname,librtmp.so.1", str(rtmp),
                     "-o", str(libs / "librtmp.so.1"))
        self.run_cmd("cc", "-shared", "-fPIC", "-Wl,-soname,libcurl.so.4", str(curl),
                     "-L" + str(libs), "-l:librtmp.so.1", "-o", str(libs / "libcurl.so.4"))
        validator = ROOT / "scripts/check_appimage_deps.py"
        self.run_cmd("python3", str(validator), str(appdir))
        (libs / "libcrypto.so.3").unlink()
        (libs / "librtmp.so.1").unlink()
        result = self.run_cmd("python3", str(validator), str(appdir), check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("libssl.so.3 requires bundled libcrypto.so.3", result.stdout)
        self.assertIn("libcurl.so.4 requires bundled librtmp.so.1", result.stdout)

    def test_windows_payload_key_tracks_same_size_changes_and_names(self):
        payload = self.base / "payload"
        payload.mkdir()
        data = payload / "resource.txt"
        data.write_text("aaaa")
        build = self.base / "portable-build"
        def key():
            self.run_cmd("cmake", "-S", str(ROOT / "live2d/tools/portable"), "-B", str(build),
                         "-DPORTABLE_PAYLOAD_DIR=" + str(payload), "-DPORTABLE_APP_VERSION=1.0")
            header = (build / "payload_files.h").read_text()
            return re.search(r'kPayloadKey\[\] = "([^"]+)"', header).group(1)
        first = key()
        self.assertEqual(first, key())
        data.write_text("bbbb")
        changed = key()
        self.assertNotEqual(first, changed)
        data.rename(payload / "renamed.txt")
        self.assertNotEqual(changed, key())

    def test_linux_sdk_subdirectory_and_unpacked_stb(self):
        work = self.base / "work"
        sdk = work / "live2d-sdk/CubismSdkForNative-fixture"
        (sdk / "Framework/src/Rendering").mkdir(parents=True)
        (sdk / "Core").mkdir()
        stb = sdk / "Samples/OpenGL/thirdParty/stb/stb_image.h"
        stb.parent.mkdir(parents=True)
        stb.write_text("// SDK header fixture\n")
        fakebin = self.base / "fake-cmake"
        fakebin.mkdir()
        (fakebin / "cmake").write_text("#!/bin/sh\nexit 0\n")
        (fakebin / "cmake").chmod(0o755)
        env = dict(self.env, L2D_WORK=str(work), PATH=str(fakebin) + os.pathsep + self.env["PATH"])
        env.pop("CSM_SDK", None)
        self.run_cmd("bash", str(ROOT / "live2d/build-player.sh"), env=env)
        self.assertEqual((work / "sdk-tree/third_party/stb_image.h").read_text(), stb.read_text())
        self.assertTrue((work / "sdk-tree/Framework/src/Rendering/Software/SoftTexture.cpp").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)
