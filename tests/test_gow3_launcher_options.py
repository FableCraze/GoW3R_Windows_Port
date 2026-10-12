from pathlib import Path
import tempfile
import unittest

from paths import ROOT
import sys
sys.path.insert(0, str(ROOT))

from launcher import RESOLUTIONS, launch_options, load_settings
from patches import EBOOT_BASE, compile_patches


class LauncherOptionsTests(unittest.TestCase):
    def test_video_and_patch_controls_map_to_runtime_environment(self):
        env = launch_options('1280x720', True, True, False)
        self.assertEqual(env, {
            'GOW3R_INTERNAL_WIDTH': '1280',
            'GOW3R_INTERNAL_HEIGHT': '720',
            'GOW3R_FULLSCREEN': '1',
            'GOW3R_PATCH_SKIP_INTRO': '1',
            'GOW3R_PATCH_SKIP_VIDEO': '0',
        })
        self.assertEqual(RESOLUTIONS, ('1280x720', '1920x1080'))

    def test_unknown_resolution_falls_back_to_1080p(self):
        env = launch_options('3840x2160', False, False, False)
        self.assertEqual((env['GOW3R_INTERNAL_WIDTH'], env['GOW3R_INTERNAL_HEIGHT']),
                         ('1920', '1080'))

    def test_invalid_saved_settings_are_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            path.write_text('{invalid', encoding='utf-8')
            self.assertEqual(load_settings(path), {})


class Gow3PatchTests(unittest.TestCase):
    def test_official_102_patches_compile_to_expected_addresses(self):
        xml = ROOT / 'patches' / 'God_of_War_III_Remastered.xml'
        segments = [(0, 0x3000000)]
        intro = compile_patches(xml, ['Skip Intro'], '01.02', segments)
        video = compile_patches(xml, ['Skip Any Video With X Button'], '01.02', segments)
        self.assertEqual([offset + EBOOT_BASE for offset, _ in intro],
                         [0x00584940, 0x004c8430, 0x0041b36a, 0x00612290])
        self.assertEqual(video, [
            (0x0041b2a3 - EBOOT_BASE, bytes.fromhex('7542')),
            (0x0041b2e7 - EBOOT_BASE, bytes.fromhex('b901000000909090')),
        ])


if __name__ == '__main__':
    unittest.main()
