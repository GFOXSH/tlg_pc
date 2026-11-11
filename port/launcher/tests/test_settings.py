"""Regression tests for launcher settings persistence and backend bindings.

Run from the repository root:
    python -m unittest discover -s port/launcher/tests -v
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import tlg_run


class LauncherSettingsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.config = root / 'config' / 'settings.json'
        self.user = root / 'user'
        self.patch_config = mock.patch.object(tlg_run, 'config_path', return_value=self.config)
        self.patch_user = mock.patch.object(tlg_run, 'USER_DIR', self.user)
        self.patch_config.start()
        self.patch_user.start()
        self.addCleanup(self.patch_user.stop)
        self.addCleanup(self.patch_config.stop)

    def test_normalization_uses_typed_defaults(self):
        settings = tlg_run.normalize_settings({
            'vsync': '0',
            'show_fps': 'false',
            'keep_gpu_textures': 0,
            'mouse_sensitivity': '99',
            'vram_budget_mb': 'not-a-number',
            'frame_rate': 'invalid',
            'resolution': 'invalid',
        })
        self.assertFalse(settings['vsync'])
        self.assertFalse(settings['show_fps'])
        self.assertFalse(settings['keep_gpu_textures'])
        self.assertEqual(settings['mouse_sensitivity'], 5.0)
        self.assertEqual(settings['vram_budget_mb'], tlg_run.DEFAULT_SETTINGS['vram_budget_mb'])
        self.assertEqual(settings['frame_rate'], '30')
        self.assertEqual(settings['resolution'], '1080p')

    def test_save_persists_json_and_live_ini_controls(self):
        settings = tlg_run.normalize_settings({
            'display_mode': 'Borderless',
            'vsync': False,
            'show_fps': True,
            'keep_gpu_textures': False,
            'mouse_enabled': False,
            'mouse_sensitivity': 1.75,
            'mouse_invert_y': True,
            'key_map': {'cross': 'K'},
        })
        tlg_run.save_settings(settings)

        stored = json.loads(self.config.read_text(encoding='utf-8'))
        ini = tlg_run.read_ini(self.user / 'tlg.ini')
        self.assertEqual(stored['display_mode'], 'Borderless')
        self.assertEqual(ini['display_mode'], 'borderless')
        self.assertEqual(ini['vsync'], '0')
        self.assertEqual(ini['show_fps'], '1')
        self.assertEqual(ini['keep_gpu_textures'], '0')
        self.assertEqual(ini['mouse_enabled'], '0')
        self.assertEqual(ini['mouse_sensitivity'], '1.75')
        self.assertEqual(ini['mouse_invert_y'], '1')
        self.assertEqual(ini['key_map'], 'cross=K')

    def test_load_reads_display_settings_updated_by_the_game(self):
        self.user.mkdir(parents=True)
        (self.user / 'tlg.ini').write_text(
            'display_mode=fullscreen\nvsync=0\nshow_fps=1\n', encoding='utf-8')
        settings = tlg_run.load_settings()
        self.assertEqual(settings['display_mode'], 'Fullscreen')
        self.assertFalse(settings['vsync'])
        self.assertTrue(settings['show_fps'])

    def test_ini_update_preserves_comments_and_removes_duplicate_owned_keys(self):
        ini_path = self.user / 'tlg.ini'
        ini_path.parent.mkdir(parents=True)
        ini_path.write_text('# keep this\nvsync=0\nvsync=0\nother=unchanged\n; keep too\n',
                            encoding='utf-8')
        tlg_run.update_ini(ini_path, {'vsync': '1'})
        text = ini_path.read_text(encoding='utf-8')
        self.assertIn('# keep this', text)
        self.assertIn('; keep too', text)
        self.assertIn('other=unchanged', text)
        self.assertEqual(text.count('vsync='), 1)
        self.assertIn('vsync=1', text)

    def test_advanced_reset_restores_every_option_and_ini_batch(self):
        custom = {
            'gpu_readbacks': '0',
            'draw_pipe': 'off',
            'keep_gpu_textures': False,
            'rb_hot': False,
            'deferred_readback': False,
            'fast_read_fault': False,
            'readback_linear': False,
            'stale_readback': True,
            'gc_batch_download': True,
            'async_fences': True,
        }
        reset = tlg_run.reset_advanced_settings({**tlg_run.DEFAULT_SETTINGS, **custom})
        self.assertNotIn('gpu_readbacks', reset)
        self.assertEqual(reset['draw_pipe'], 'auto')
        self.assertTrue(reset['keep_gpu_textures'])
        for key, _env, default in tlg_run.READBACK_FLAGS:
            self.assertEqual(reset.get(key, default), default)
        self.assertEqual(tlg_run.read_ini(self.user / 'tlg.ini')['readback_batch'], '0')
        self.assertEqual(tlg_run.read_ini(self.user / 'tlg.ini')['keep_gpu_textures'], '1')

    def test_russian_launcher_language_is_preserved_without_mislabeling_runtime_overlay(self):
        settings = tlg_run.normalize_settings({'ui_language': 'ru_RU'})
        self.assertEqual(settings['ui_language'], 'ru_RU')
        with mock.patch.dict('os.environ', {}, clear=True), \
                mock.patch.object(tlg_run, 'window_icon', return_value=None):
            env = tlg_run.build_env(settings)
        self.assertNotIn('TLG_UI_LANG', env)

    def test_backend_environment_receives_advanced_and_performance_settings(self):
        settings = tlg_run.normalize_settings({
            'game_dir': '',
            'game_language': 'en',
            'ui_language': 'en_US',
            'gpu_readbacks': '1',
            'rb_hot': False,
            'async_fences': True,
            'keep_gpu_textures': False,
            'draw_pipe': 'off',
            'vram_budget_mb': 5600,
            'stick_keys': {'walk_up': 'UP'},
        })
        with mock.patch.dict('os.environ', {}, clear=True), \
                mock.patch.object(tlg_run, 'window_icon', return_value=None):
            env = tlg_run.build_env(settings)
        self.assertEqual(env['TLG_READBACKS'], '1')
        self.assertEqual(env['TLG_RB_HOT'], '0')
        self.assertEqual(env['TLG_ASYNC_FENCES'], '1')
        self.assertEqual(env['TLG_KEEP_GPU_TEXTURES'], '0')
        self.assertEqual(env['TLG_DRAW_PIPE'], '0')
        self.assertEqual(env['TLG_GC_BUDGET_MB'], '5600')
        self.assertEqual(env['TLG_WALK_UP'], 'UP')
        self.assertEqual(env['TLG_LANGUAGE'], '1')
        self.assertEqual(env['TLG_UI_LANG'], 'en_US')


if __name__ == '__main__':
    unittest.main()
