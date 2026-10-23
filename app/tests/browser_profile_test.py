#!/usr/bin/env python3
"""Benchmark harness contracts; no display, Chromium, or GPU required."""
import contextlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest

from browser_profile import trial_directory


class ProfileArtifactsTest(unittest.TestCase):
    def test_child_can_use_retained_paths_from_its_own_working_directory(self):
        # CLI users may request a relative artifacts directory. Host trials pass
        # paths to a subprocess whose cwd is the retained trial directory.
        with tempfile.TemporaryDirectory() as temporary:
            previous = Path.cwd()
            try:
                os.chdir(temporary)
                artifacts = Path('results')
                artifacts.mkdir()
                with contextlib.redirect_stderr(io.StringIO()):
                    with trial_directory(SimpleNamespace(artifacts_dir=artifacts), 'host-') as directory:
                        session = Path(directory) / 'session.jsonl'
                        session.write_text('fixture session\n')
                        result = subprocess.check_output([
                            sys.executable, '-c',
                            'from pathlib import Path; import sys; print(Path(sys.argv[1]).read_text(), end="")',
                            str(session),
                        ], cwd=directory, text=True)
                self.assertEqual(result, 'fixture session\n')
                self.assertTrue(session.exists())  # explicitly retained after the context
            finally:
                os.chdir(previous)


if __name__ == '__main__':
    unittest.main()
