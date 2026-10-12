"""Game-free tests for the GoW3R-specific validation and release safeguards."""
from paths import ROOT
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

import cache_key
import game_check
import import_coverage
import package_release


class ValidationTests(unittest.TestCase):
    def test_exact_title_version_and_image_are_required(self):
        with patch.object(game_check, 'sfo', create=True), \
             patch.object(game_check, 'image_sha256', return_value=game_check.SUPPORTED_IMAGE), \
             patch('prepare.sfo', return_value={'TITLE_ID': 'CUSA01623', 'APP_VER': '01.02'}), \
             patch.object(Path, 'read_bytes', return_value=b'fixture'):
            self.assertTrue(game_check.inspect(Path('fixture'))['ok'])

    def test_cache_key_changes_with_each_link_input(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            for index,name in enumerate(cache_key.INPUTS):
                path=root/name; path.parent.mkdir(parents=True,exist_ok=True)
                path.write_bytes(bytes([index]))
            first=cache_key.cache_key(root)
            (root/cache_key.INPUTS[1]).write_bytes(b'changed')
            self.assertNotEqual(first,cache_key.cache_key(root))

    def test_texture_profile_reserves_required_additional_dmem(self):
        launcher=(ROOT/'run.bat').read_text(encoding='utf-8')
        memory=(ROOT/'src/runtime_memory.c').read_text(encoding='utf-8')
        self.assertIn('GOW3R_DMEM_MB=7104',launcher)
        self.assertIn(': 7104;',memory)


class CoverageTests(unittest.TestCase):
    def test_reference_contract_manifest_is_fully_covered(self):
        report=Path(tempfile.gettempdir())/'gow3r-coverage-fixture.json'
        report.write_text(json.dumps({'format':'GOW3IMG1','bindings':0,
                                      'unresolved_imports':['qkgRiwHyheU#libSceVideodec#libSceVideodec']}))
        result=import_coverage.audit(ROOT,report)
        self.assertEqual(result['covered_host_imports'],1)
        self.assertEqual(result['missing_host_imports'],0)
        report.unlink()

    def test_packager_rejects_proprietary_extensions(self):
        self.assertTrue(package_release.reject(Path('movie.BSF')))
        self.assertTrue(package_release.reject(Path('libc.prx')))
        self.assertTrue(package_release.reject(Path('eboot.bin')))
        self.assertFalse(package_release.reject(Path('gow3r-port.exe')))

    def test_host_resolvers_do_not_compare_self_local_suffixes(self):
        offenders=[]
        pattern=re.compile(r'strcmp\s*\(\s*name\s*,\s*"[^"]+#')
        for source in (ROOT/'src').glob('runtime*.c'):
            if pattern.search(source.read_text(encoding='utf-8')):
                offenders.append(source.name)
        self.assertEqual(offenders,[],f'compact SELF suffix used by host resolver: {offenders}')

    def test_object_imports_participate_in_strict_runtime_coverage(self):
        probe=(ROOT/'src/probe.c').read_text(encoding='utf-8')
        self.assertIn('if (!resolved) resolved = runtime_resolve(names[i], 1);',probe)

    def test_ajm_compressed_audio_does_not_abort_the_process(self):
        ajm=(ROOT/'src/runtime_ajm.c').read_text(encoding='utf-8')
        self.assertNotIn('Ajm codec %u (0=MP3, 2=AAC) is not implemented',ajm)
        self.assertNotRegex(ajm,r'codec\s*!=\s*1[^\n]*exit\s*\(')
        self.assertIn('AV_CODEC_ID_MP3',ajm)
        self.assertIn('AV_CODEC_ID_AAC',ajm)
        self.assertIn('ffmpeg_frame(in,&input,&in_size,&out,&written,&internal)',ajm)

    def test_launch_profile_selects_precise_readbacks_with_fast_mode_opt_in(self):
        settings=(ROOT/'gpu/shim/core/emulator_settings.h').read_text(encoding='utf-8')
        texture=(ROOT/'gpu/shadps4/video_core/texture_cache/texture_cache.cpp').read_text(
            encoding='utf-8')
        launcher=(ROOT/'run.bat').read_text(encoding='utf-8')
        self.assertIn('Number("GOW3R_READBACKS", GpuReadbacksMode::Disabled)',settings)
        self.assertIn('if not defined GOW3R_READBACKS set "GOW3R_READBACKS=2"',launcher)
        self.assertIn('if not defined GOW3R_READBACK_LINEAR set "GOW3R_READBACK_LINEAR=1"',launcher)
        self.assertIn('if not defined GOW3R_DIRECT_MEMORY_ACCESS set "GOW3R_DIRECT_MEMORY_ACCESS=0"',launcher)
        self.assertIn('readbacks=2', (ROOT/'gow3r.ini').read_text(encoding='utf-8'))
        self.assertIn('const bool sync = EmulatorSettings.GetReadbacksMode() !=',texture)
        self.assertIn('DownloadImageMemory(image_id, sync);',texture)

    def test_narrow_gow3_images_use_specialized_readback(self):
        source=(ROOT/'gpu/shadps4/video_core/texture_cache/texture_cache.cpp').read_text(
            encoding='utf-8')
        self.assertIn('image.info.size.width <= 8',source)

    def test_gow3_alias_sync_is_strict_and_switchable(self):
        settings=(ROOT/'gpu/shim/core/emulator_settings.h').read_text(encoding='utf-8')
        source=(ROOT/'gpu/shadps4/video_core/texture_cache/texture_cache.cpp').read_text(
            encoding='utf-8')
        launcher=(ROOT/'run.bat').read_text(encoding='utf-8')
        self.assertIn('Flag("GOW3R_ALIAS_SYNC", true)',settings)
        self.assertIn('if not defined GOW3R_ALIAS_SYNC set "GOW3R_ALIAS_SYNC=1"',launcher)
        self.assertIn('if not defined BB_ASYNC_PIPELINES set "BB_ASYNC_PIPELINES=0"',launcher)
        self.assertIn('src.tile_mode == dst.tile_mode',source)
        self.assertIn('src.bank_swizzle == dst.bank_swizzle',source)
        self.assertIn('src.pixel_format == dst.pixel_format',source)
        self.assertIn('!src.props.is_block',source)
        self.assertIn('CommitAliasWriter(state)',source)
        self.assertIn('runtime.CopyAlias(&current, &dst, *extent)',source)
        self.assertIn('GPU: image alias summary:',source)
        self.assertIn('GPU: image alias stats:',source)
        self.assertIn('cached.generation == registry_generation.load(std::memory_order_relaxed) || still_exact()',source)

    def test_complete_tiling_regions_coordinate_macro_tail_transition(self):
        source=(ROOT/'gpu/shadps4/video_core/host_shaders/tiling.comp').read_text(
            encoding='utf-8')
        self.assertIn('if (pitch < macro_tile_pitch || height < macro_tile_height)',source)
        self.assertIn('ComputeSurfaceAddrFromCoordMacroTiled(x, y, slice, pitch, height, 0)',source)
        self.assertIn('uint mip = region.mip',source)
        self.assertIn('region.first_tile +',source)
        self.assertIn('region.tile_width',source)
        self.assertIn('info.mips[mip].offset / BYTES_PER_PIXEL + mip_texel',source)
        self.assertNotIn('GetMipLevel(inout uint texel)',source)
        manager=(ROOT/'gpu/shadps4/video_core/texture_cache/tile_manager.cpp').read_text(
            encoding='utf-8')
        self.assertIn('MakeTilingRegions',manager)
        self.assertIn('complete tiling regions enabled',manager)
        self.assertIn('.size = sizeof(TilingRegion)',manager)
        self.assertIn('cmdbuf.pushConstants',manager)

    def test_large_mipped_texture_trace_identifies_streaming_replacements(self):
        source=(ROOT/'gpu/shadps4/video_core/texture_cache/texture_cache.cpp').read_text(
            encoding='utf-8')
        self.assertIn('GOW3R_TEXTURE_TRACE',source)
        self.assertIn('had_previous_upload ? "refresh" : "initial"',source)
        self.assertIn('GPU: texture trace #%u',source)
        self.assertIn('bank_swizzle',source)

    def test_host_texture_uploads_are_staged_before_detiling(self):
        source=(ROOT/'gpu/shadps4/video_core/texture_cache/tile_manager.cpp').read_text(
            encoding='utf-8')
        self.assertIn('const bool stage_input = !in_buffer->mapped_data.empty()',source)
        self.assertIn('GOW3R_STAGE_LARGE_BC3',source)
        self.assertIn('IsLargeBc3Texture(info)',source)
        self.assertIn('info.num_bits == 128',source)
        self.assertIn('info.size.width >= 2048',source)
        self.assertIn('large BC3 input path:',source)
        self.assertIn('cmdbuf.copyBuffer(src, dst, copy)',source)
        self.assertIn('vk::AccessFlagBits2::eShaderStorageRead',source)
        self.assertIn('vk::AccessFlagBits2::eShaderStorageWrite',source)
        self.assertIn('vk::AccessFlagBits2::eTransferRead',source)
        self.assertIn('device-local texture staging enabled',source)

    def test_scaled_min_max_uses_two_pass_blend(self):
        shader=(ROOT/'gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp').read_text(
            encoding='utf-8')
        pipeline=(ROOT/'gpu/shadps4/video_core/renderer_vulkan/vk_graphics_pipeline.cpp').read_text(
            encoding='utf-8')
        raster=(ROOT/'gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp').read_text(
            encoding='utf-8')
        serialization=(ROOT/'gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp').read_text(
            encoding='utf-8')
        self.assertNotIn('blend_self_scale) {\n            value = ctx.OpFMul',shader)
        self.assertIn('CreateSquarePipeline(',pipeline)
        self.assertIn('eDstColor',pipeline)
        self.assertIn('SquarePassHandle()',raster)
        self.assertIn('ShaderBinaryVersion = 8u',serialization)


if __name__ == '__main__':
    unittest.main()
