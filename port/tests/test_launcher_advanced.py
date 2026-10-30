"""Advanced GPU readback UI metadata and launcher environment mapping."""
import os
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))

import tlg_launcher  # noqa: E402
import tlg_run  # noqa: E402


class AdvancedReadbackSettingsTest(unittest.TestCase):
    def test_advanced_rows_match_supported_runtime_switches(self):
        expected = (
            ('rb_hot', 'TLG_RB_HOT', True),
            ('deferred_readback', 'TLG_DEFERRED_READBACK', True),
            ('fast_read_fault', 'TLG_FAST_READ_FAULT', True),
            ('readback_linear', 'TLG_READBACK_LINEAR', True),
            ('stale_readback', 'TLG_STALE_READBACK', False),
            ('gc_batch_download', 'TLG_GC_BATCH_DOWNLOAD', False),
            ('async_fences', 'TLG_ASYNC_FENCES', False),
        )
        self.assertEqual(tlg_run.READBACK_FLAGS, expected)
        for key, _env, default in expected:
            with self.subTest(key=key):
                self.assertIn(key, tlg_launcher.TEXT)
                self.assertIn(key + '_hint', tlg_launcher.TEXT)
                self.assertIn(key + '_why', tlg_launcher.TEXT)
                self.assertEqual(len(tlg_launcher.TEXT[key]), 2)
                self.assertEqual(len(tlg_launcher.TEXT[key + '_hint']), 2)
                self.assertEqual(len(tlg_launcher.TEXT[key + '_why']), 2)
                self.assertIn('Default', tlg_launcher.TEXT['mark_default'][0])
                self.assertIsInstance(default, bool)

    def test_hot_readbacks_default_on_and_environment_override_wins(self):
        settings = dict(tlg_run.DEFAULT_SETTINGS)
        settings['rb_hot'] = True
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch.object(tlg_run, 'window_icon', return_value=None):
                env = tlg_run.build_env(settings)
        # The backend's default is enabled; no redundant environment override is needed.
        self.assertNotIn('TLG_RB_HOT', env)

        with mock.patch.dict(os.environ, {'TLG_RB_HOT': '0'}, clear=True):
            with mock.patch.object(tlg_run, 'window_icon', return_value=None):
                env = tlg_run.build_env(settings)
        self.assertEqual(env['TLG_RB_HOT'], '0')

    def test_draw_pipe_advanced_control_is_localized_and_defaults_to_auto(self):
        self.assertEqual(tlg_run.DEFAULT_SETTINGS['draw_pipe'], 'auto')
        for key in ('draw_pipe', 'draw_pipe_auto', 'draw_pipe_on', 'draw_pipe_off', 'draw_pipe_hint'):
            with self.subTest(key=key):
                self.assertIn(key, tlg_launcher.TEXT)
                self.assertEqual(len(tlg_launcher.TEXT[key]), 2)
                self.assertTrue(all(tlg_launcher.TEXT[key]))

    def test_draw_pipe_auto_on_off_and_environment_override(self):
        settings = dict(tlg_run.DEFAULT_SETTINGS)
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch.object(tlg_run, 'window_icon', return_value=None):
                env = tlg_run.build_env(settings)
        self.assertNotIn('TLG_DRAW_PIPE', env)

        for value, expected in (('on', '1'), ('off', '0')):
            settings['draw_pipe'] = value
            with mock.patch.dict(os.environ, {}, clear=True):
                with mock.patch.object(tlg_run, 'window_icon', return_value=None):
                    env = tlg_run.build_env(settings)
            self.assertEqual(env['TLG_DRAW_PIPE'], expected)

        settings['draw_pipe'] = 'off'
        with mock.patch.dict(os.environ, {'TLG_DRAW_PIPE': '1'}, clear=True):
            with mock.patch.object(tlg_run, 'window_icon', return_value=None):
                env = tlg_run.build_env(settings)
        self.assertEqual(env['TLG_DRAW_PIPE'], '1')

    def test_gpu_readback_mode_and_batch_explanations_are_present(self):
        for key in ('gpu_readbacks_hint', 'gpu_readbacks_why', 'readback_batch_hint', 'readback_batch_why'):
            with self.subTest(key=key):
                self.assertEqual(len(tlg_launcher.TEXT[key]), 2)
                self.assertTrue(all(tlg_launcher.TEXT[key]))

    def test_build_env_passes_supported_switches_including_hot_readbacks(self):
        settings = dict(tlg_run.DEFAULT_SETTINGS)
        settings.update({
            'gpu_readbacks': '1',
            'deferred_readback': False,
            'fast_read_fault': False,
            'readback_linear': False,
            'stale_readback': True,
            'gc_batch_download': True,
            'async_fences': True,
            'rb_hot': False,  # explicitly disable the backend's default-on hot-readback prefetch.
        })
        with mock.patch.dict(os.environ, {}, clear=True):
            with mock.patch.object(tlg_run, 'window_icon', return_value=None):
                env = tlg_run.build_env(settings)
        self.assertEqual(env['TLG_READBACKS'], '1')
        self.assertEqual(env['TLG_DEFERRED_READBACK'], '0')
        self.assertEqual(env['TLG_FAST_READ_FAULT'], '0')
        self.assertEqual(env['TLG_READBACK_LINEAR'], '0')
        self.assertEqual(env['TLG_STALE_READBACK'], '1')
        self.assertEqual(env['TLG_GC_BATCH_DOWNLOAD'], '1')
        self.assertEqual(env['TLG_ASYNC_FENCES'], '1')
        self.assertEqual(env['TLG_RB_HOT'], '0')


if __name__ == '__main__':
    unittest.main()
