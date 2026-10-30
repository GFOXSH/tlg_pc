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
        self.assertNotIn('rb_hot', {key for key, _env, _default in expected})

    def test_gpu_readback_mode_and_batch_explanations_are_present(self):
        for key in ('gpu_readbacks_hint', 'gpu_readbacks_why', 'readback_batch_hint', 'readback_batch_why'):
            with self.subTest(key=key):
                self.assertEqual(len(tlg_launcher.TEXT[key]), 2)
                self.assertTrue(all(tlg_launcher.TEXT[key]))

    def test_build_env_passes_only_supported_switches_and_ignores_legacy_rb_hot(self):
        settings = dict(tlg_run.DEFAULT_SETTINGS)
        settings.update({
            'gpu_readbacks': '1',
            'deferred_readback': False,
            'fast_read_fault': False,
            'readback_linear': False,
            'stale_readback': True,
            'gc_batch_download': True,
            'async_fences': True,
            'rb_hot': True,  # old settings.json key; the target GPU code has no TLG_RB_HOT.
        })
        with mock.patch.dict(os.environ, {}, clear=True), \\
                mock.patch.object(tlg_run, 'window_icon', return_value=None):
            env = tlg_run.build_env(settings)
        self.assertEqual(env['TLG_READBACKS'], '1')
        self.assertEqual(env['TLG_DEFERRED_READBACK'], '0')
        self.assertEqual(env['TLG_FAST_READ_FAULT'], '0')
        self.assertEqual(env['TLG_READBACK_LINEAR'], '0')
        self.assertEqual(env['TLG_STALE_READBACK'], '1')
        self.assertEqual(env['TLG_GC_BATCH_DOWNLOAD'], '1')
        self.assertEqual(env['TLG_ASYNC_FENCES'], '1')
        self.assertNotIn('TLG_RB_HOT', env)


if __name__ == '__main__':
    unittest.main()
