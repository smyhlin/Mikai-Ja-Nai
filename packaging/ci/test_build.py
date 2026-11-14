"""Regression coverage for diagnostics retained after a failed CI test run."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name('build.py')
SPEC = importlib.util.spec_from_file_location('mikai_ci_build', SCRIPT)
build_ci = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_ci)


class FailedBuildEvidenceTests(unittest.TestCase):
    def test_failed_ctest_retains_canary_verdict_and_frame_timing(self):
        for host in ('win32', 'linux'):
            with self.subTest(host=host), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                build = root / 'build'
                evidence = root / 'evidence'
                compiler = root / 'toolchain' / 'g++.exe'
                compiler.parent.mkdir()
                (compiler.parent / 'libwinpthread-1.dll').write_bytes(b'fixture')
                runtime = root / 'runtime.tar.zst'
                runtime.write_bytes(b'fixture')
                verdict = b'{"event":"hoverCanary","result":"INVALID","reason":"no video"}\n'
                timing = b'paint newframe=1 skip_reason=paced paint_delay_ms=42\n'

                def run(command, **kwargs):
                    if command[0] == 'cmake' and '-S' in command:
                        (build / 'CMakeCache.txt').write_text('fixture\n', encoding='utf-8')
                    failed = 'ctest' in command
                    if failed:
                        testing = build / 'Testing' / 'Temporary'
                        testing.mkdir(parents=True)
                        (testing / 'LastTestsFailed.log').write_text(
                            '77:HoverPlaybackCanary\n', encoding='utf-8')
                        canary = build / '_hover-canary'
                        canary.mkdir()
                        (canary / 'samples.jsonl').write_bytes(verdict)
                        (canary / 'hover-canary-frame-timing.jsonl').write_bytes(timing)
                    return subprocess.CompletedProcess(command, 8 if failed else 0)

                arguments = ['build.py', '--build-dir', str(build),
                             '--output-dir', str(root / 'packages'),
                             '--evidence', str(evidence), '--qt-prefix', str(root / 'qt')]
                with patch.object(build_ci, 'ROOT', root), \
                     patch.object(build_ci.sys, 'platform', host), \
                     patch.object(build_ci.sys, 'argv', arguments), \
                     patch.object(build_ci, 'ensure_linux_runtime', return_value=runtime), \
                     patch.object(build_ci.shutil, 'which', return_value=str(compiler)), \
                     patch.object(build_ci.subprocess, 'run', side_effect=run), \
                     contextlib.redirect_stdout(io.StringIO()):
                    with self.assertRaisesRegex(RuntimeError, 'ctest failed with exit 8'):
                        build_ci.main()

                self.assertEqual((evidence / '_hover-canary' / 'samples.jsonl').read_bytes(),
                                 verdict)
                self.assertEqual((evidence / '_hover-canary' /
                                  'hover-canary-frame-timing.jsonl').read_bytes(), timing)
                self.assertEqual((evidence / 'Testing' / 'Temporary' /
                                  'LastTestsFailed.log').read_text(encoding='utf-8'),
                                 '77:HoverPlaybackCanary\n')
                result = json.loads((evidence / 'verification.json').read_text(encoding='utf-8'))
                self.assertEqual(result['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
