#!/usr/bin/env python3
"""Linux startup adapter integration checks; uses only isolated temporary fixtures."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]

class DesktopStartupTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="l2dpet-startup-build-")
        cls.addClassCleanup(cls.build.cleanup)
        source = Path(cls.build.name) / "driver.cpp"
        source.write_text('''#include "DesktopStartup.h"
#include <string>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    bool ok = std::string(argv[1]) == "enabled" ? desktop::AutostartEnabled()
              : desktop::SetAutostart(std::string(argv[1]) == "on");
    std::cout << (ok ? "true" : "false") << "\\n";
    return 0;
}
''')
        cls.binary = Path(cls.build.name) / "driver"
        subprocess.run(["g++", "-std=c++17", "-I", str(ROOT / "live2d/panel"), str(source),
                        str(ROOT / "live2d/panel/DesktopStartup.cpp"),
                        str(ROOT / "live2d/player/src/Platform.cpp"), "-o", str(cls.binary)], check=True)

    def setUp(self):
        tmp = tempfile.TemporaryDirectory(prefix="l2dpet-startup-")
        self.addCleanup(tmp.cleanup)
        self.base = Path(tmp.name)
        self.pkg = self.base / 'pet space &$`"%\\中文'
        self.pkg.mkdir()
        self.ui = self.pkg / "l2dpet-ui"
        shutil.copyfile(self.binary, self.ui)
        self.ui.chmod(0o755)
        self.env = {k:v for k,v in os.environ.items() if not k.startswith(("L2DPET_", "XDG_", "APPIMAGE", "APPDIR"))}
        self.env.update(HOME=str(self.base / "home"), XDG_CONFIG_HOME=str(self.base / "config"),
                        CAPTURE=str(self.base / "capture.json"))
        self.entry = self.base / "config/autostart/l2dpet.desktop"

    def run_adapter(self, mode):
        return subprocess.check_output([str(self.ui), mode], env=self.env, text=True).strip()

    def executable(self, name):
        exe = self.pkg / name
        exe.write_text('#!/usr/bin/python3\nimport json,os,sys\n'
                       'with open(os.environ["CAPTURE"],"w") as f: json.dump({"args":sys.argv[1:],"cwd":os.getcwd()},f)\n')
        exe.chmod(0o755)
        return exe

    def launch_entry(self):
        if not shutil.which("gio"): self.skipTest("gio unavailable")
        subprocess.run(["gio", "launch", str(self.entry)], env=self.env, cwd="/tmp", check=True,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        for _ in range(50):
            if (self.base / "capture.json").exists():
                return json.loads((self.base / "capture.json").read_text())
            time.sleep(.1)
        self.fail("startup entry failed to launch fixture")

    def test_sibling_launcher_and_working_directory(self):
        self.executable("l2dpet")
        self.assertEqual(self.run_adapter("on"), "true")
        capture = self.launch_entry()
        self.assertEqual(capture, {"args": [], "cwd": str(self.pkg)})
        self.assertEqual(self.run_adapter("enabled"), "true")

    def test_bare_player_fallback_enters_pet_mode(self):
        self.executable("l2dtest")
        self.assertEqual(self.run_adapter("on"), "true")
        self.assertEqual(self.launch_entry(), {"args": ["--pet"], "cwd": str(self.pkg)})

    def test_actual_appimage_and_missing_target(self):
        image = self.executable("standalone pet.AppImage")
        self.env["L2DPET_LAUNCHER"] = str(image)
        self.assertEqual(self.run_adapter("on"), "true")
        self.assertEqual(self.launch_entry()["cwd"], str(self.pkg))
        self.assertEqual(self.run_adapter("off"), "true")
        image.unlink()
        self.assertEqual(self.run_adapter("on"), "false")
        self.assertFalse(self.entry.exists())

    def test_disabled_entry_and_repeated_disable(self):
        self.executable("l2dpet")
        self.assertEqual(self.run_adapter("on"), "true")
        with self.entry.open("a") as f: f.write("Hidden=true\n")
        self.assertEqual(self.run_adapter("enabled"), "false")
        self.assertEqual(self.run_adapter("off"), "true")
        self.assertEqual(self.run_adapter("off"), "true")

if __name__ == "__main__": unittest.main(verbosity=2)
