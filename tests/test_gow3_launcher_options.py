from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from paths import ROOT
import sys
sys.path.insert(0, str(ROOT))

from launcher import RESOLUTIONS, launch_options, load_settings
from patches import (EBOOT_BASE, DEFAULT_XML, FPS_120, NO_VIDEO_PATCH, TEXTURE_FIX,
                     VIDEO_PROFILES, catalog, compile_patches, runtime_options)


class LauncherOptionsTests(unittest.TestCase):
    def test_video_and_patch_controls_map_to_runtime_environment(self):
        with patch.dict(os.environ, {}, clear=True):
            env = launch_options('1280x720', True, True, False)
        self.assertEqual(env, {
            'GOW3R_INTERNAL_WIDTH': '1280',
            'GOW3R_INTERNAL_HEIGHT': '720',
            'GOW3R_FULLSCREEN': '1',
            'GOW3R_PATCH_SKIP_INTRO': '1',
            'GOW3R_PATCH_SKIP_VIDEO': '0',
            'GOW3R_PATCHES': TEXTURE_FIX,
            'GOW3R_DMEM_MB': '7104',
            'GOW3R_VBLANK_HZ': '60',
            'GOW3R_FPS_LIMIT': '60',
            'GOW3R_READBACKS': '2',
            'GOW3R_READBACK_LINEAR': '1',
            'GOW3R_DIRECT_MEMORY_ACCESS': '0',
        })
        self.assertEqual(RESOLUTIONS, ('1280x720', '1920x1080'))

    def test_precise_readbacks_default_for_all_video_profiles(self):
        with patch.dict(os.environ, {}, clear=True):
            for profile in (*VIDEO_PROFILES, NO_VIDEO_PATCH):
                for fps_120 in (False, True):
                    with self.subTest(profile=profile, fps_120=fps_120):
                        env = launch_options('1280x720', False, False, False, profile, fps_120)
                        self.assertEqual(env['GOW3R_READBACKS'], '2')
                        self.assertEqual(env['GOW3R_READBACK_LINEAR'], '1')
                        self.assertEqual(env['GOW3R_DIRECT_MEMORY_ACCESS'], '0')

    def test_explicit_readback_environment_overrides_are_preserved(self):
        overrides = {'GOW3R_READBACKS': '0', 'GOW3R_READBACK_LINEAR': '0',
                     'GOW3R_DIRECT_MEMORY_ACCESS': '1'}
        with patch.dict(os.environ, overrides, clear=True):
            env = launch_options('1920x1080', False, False, False)
        for name, value in overrides.items():
            self.assertEqual(env[name], value)

    def test_unknown_resolution_falls_back_to_1080p(self):
        env = launch_options('3840x2160', False, False, False)
        self.assertEqual((env['GOW3R_INTERNAL_WIDTH'], env['GOW3R_INTERNAL_HEIGHT']),
                         ('1920', '1080'))

    def test_invalid_saved_settings_are_ignored(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'settings.json'
            path.write_text('{invalid', encoding='utf-8')
            self.assertEqual(load_settings(path), {})

    def test_no_graphics_patch_and_fps_toggle_do_not_inherit_old_selection(self):
        env = launch_options('1920x1080', False, False, False, NO_VIDEO_PATCH, False)
        self.assertEqual(env['GOW3R_PATCHES'], 'none')
        self.assertEqual(env['GOW3R_VBLANK_HZ'], '60')
        self.assertEqual(env['GOW3R_FPS_LIMIT'], '60')

    def test_resolution_patch_controls_output_memory_and_120hz(self):
        env = launch_options('1280x720', False, False, False, 'Resolution Patch - 4K', True)
        self.assertEqual((env['GOW3R_INTERNAL_WIDTH'], env['GOW3R_INTERNAL_HEIGHT']),
                         ('3840', '2160'))
        self.assertEqual(env['GOW3R_DMEM_MB'], '11200')
        self.assertEqual(env['GOW3R_VBLANK_HZ'], '120')
        self.assertEqual(env['GOW3R_FPS_LIMIT'], '120')
        self.assertEqual(env['GOW3R_PATCHES'], 'Resolution Patch - 4K;' + FPS_120)


class Gow3PatchTests(unittest.TestCase):
    def test_cli_replaces_patch_binary_when_graphics_patch_is_disabled(self):
        with tempfile.TemporaryDirectory() as directory:
            out = Path(directory)
            elf = bytearray(0x40 + 56)
            struct.pack_into('<Q', elf, 0x20, 0x40)
            struct.pack_into('<HH', elf, 0x36, 56, 1)
            struct.pack_into('<IIQQQQQQ', elf, 0x40, 1, 7, 0, 0, 0, 0, 0x3000000, 0x1000)
            (out / 'eboot.elf').write_bytes(elf)
            env = dict(os.environ, GOW3R_DMEM_MB='7104')
            command = [sys.executable, str(ROOT / 'scripts/patches.py'), '--out', str(out),
                       '--runtime-env', str(out / 'patch-runtime.bat')]
            subprocess.run(command + ['--extra', TEXTURE_FIX], env=env, check=True,
                           capture_output=True, text=True)
            self.assertEqual((out / 'patches.bin').read_bytes(),
                             struct.pack('<8sQQQQ', b'BBPATCH2', EBOOT_BASE, 1, 0x3cd918, 4)
                             + bytes.fromhex('00000090'))
            self.assertIn('set "GOW3R_DMEM_MB=7104"',
                          (out / 'patch-runtime.bat').read_text(encoding='ascii'))
            subprocess.run(command + ['--extra', 'none'], env=env, check=True,
                           capture_output=True, text=True)
            self.assertEqual((out / 'patches.bin').read_bytes(),
                             struct.pack('<8sQQ', b'BBPATCH2', EBOOT_BASE, 0))

    def test_catalog_exposes_all_nine_102_patches(self):
        self.assertEqual(set(catalog()), set(VIDEO_PROFILES) | {
            FPS_120, 'Skip Intro', 'Skip Any Video With X Button'})
        self.assertEqual(catalog(app_version='01.00'), {})

    def test_texture_fix_targets_the_xml_virtual_address_not_the_old_bss_offset(self):
        writes = compile_patches(DEFAULT_XML, [TEXTURE_FIX], '01.02', [(0, 0x3000000)])
        self.assertEqual(writes, [(0x3cd918, bytes.fromhex('00000090'))])
        self.assertEqual(writes[0][0] + EBOOT_BASE, 0x007cd918)

    def test_mutually_exclusive_video_patches_are_rejected(self):
        for selected in ([TEXTURE_FIX, 'Resolution Patch - 720p'],
                         ['Resolution Patch - 720p', 'Resolution Patch - 4K']):
            with self.assertRaisesRegex(ValueError, 'choose only one'):
                compile_patches(DEFAULT_XML, selected, '01.02', [(0, 0x3000000)])

    def test_dmem_requirements_translate_additional_memory_to_total_pool(self):
        expected = {'Resolution Patch - 480p': '7104', 'Resolution Patch - 720p': '7104',
                    'Resolution Patch - 1440p': '8128', 'Resolution Patch - 1800p': '9664',
                    'Resolution Patch - 4K': '11200', TEXTURE_FIX: '7104'}
        for name, budget in expected.items():
            self.assertEqual(runtime_options([name], 5056)['GOW3R_DMEM_MB'], budget)
        self.assertEqual(runtime_options([TEXTURE_FIX], 12000)['GOW3R_DMEM_MB'], '12000')

    def test_wrong_version_and_out_of_bounds_writes_fail_before_applying(self):
        with self.assertRaisesRegex(ValueError, 'patches not found'):
            compile_patches(DEFAULT_XML, [TEXTURE_FIX], '01.00', [(0, 0x3000000)])
        with self.assertRaisesRegex(ValueError, 'outside the eboot'):
            compile_patches(DEFAULT_XML, [TEXTURE_FIX], '01.02', [(0, 0x10000)])

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
