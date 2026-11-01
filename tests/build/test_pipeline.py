# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for stale verification evidence and early build-input failures."""

import hashlib
import tempfile
import unittest
from pathlib import Path

from beauti_tools.build.pipeline import default_paths, validate_windows_inputs, verification_binding

ROOT = Path(__file__).resolve().parents[2]


class PipelineTests(unittest.TestCase):
    def test_default_variants_do_not_share_cache_or_package(self):
        cpu = default_paths(ROOT, 'windows', True)
        tensorrt = default_paths(ROOT, 'windows', False)
        self.assertNotEqual(cpu[0], tensorrt[0])
        self.assertNotEqual(cpu[1], tensorrt[1])

    def test_missing_sdk_is_rejected_for_compile_only_too(self):
        with (
            self.assertRaisesRegex(ValueError, 'Missing native build inputs'),
            tempfile.TemporaryDirectory() as path,
        ):
            validate_windows_inputs(Path(path) / 'sdk', Path(path) / 'ort')

    def test_historical_report_without_hashes_never_matches(self):
        self.assertEqual(verification_binding(Path('unused'), {}, 'tensorrt')['status'], 'historical')

    def test_changed_runtime_or_effect_cannot_reuse_a_pass(self):
        with tempfile.TemporaryDirectory() as path:
            bundle = Path(path)
            names = [
                'beauti.dll',
                'onnxruntime.dll',
                'data/models/face.onnx',
                'data/beauti.effect',
                'data/continuity.effect',
                'data/cache-key.txt',
                'data/locale/en-US.ini',
                'data/locale/de-DE.ini',
            ]
            expected = {}
            for name in names:
                file = bundle / name
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b'verified bytes')
                expected[name] = hashlib.sha256(file.read_bytes()).hexdigest()
            report = {'artifact_sha256': {'tensorrt': expected}}
            self.assertEqual(verification_binding(bundle, report, 'tensorrt')['status'], 'matched')
            for name in names:
                with self.subTest(changed=name):
                    (bundle / name).write_bytes(b'changed')
                    result = verification_binding(bundle, report, 'tensorrt')
                    self.assertEqual(result['status'], 'historical')
                    self.assertIn(name, result['mismatches'])
                    (bundle / name).write_bytes(b'verified bytes')
            (bundle / 'tensorrt_rtx_1_7.dll').write_bytes(b'test copy with the NVIDIA runtime')
            self.assertEqual(verification_binding(bundle, report, 'tensorrt')['status'], 'matched')
            extra = bundle / 'extra-provider.dll'
            extra.write_bytes(b'not tested')
            self.assertEqual(verification_binding(bundle, report, 'tensorrt')['status'], 'historical')


if __name__ == '__main__':
    unittest.main()
