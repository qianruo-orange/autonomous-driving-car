"""Historical filename normalization preserves content, collisions and dates."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location('names', Path(__file__).resolve().parents[1] / 'scripts/normalize_file_names.py')
names = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(names)

@unittest.skipUnless(os.open in os.supports_dir_fd, 'POSIX directory operations')
class NamesTests(unittest.TestCase):
    def test_content_collisions_and_repeat_deployment(self):
        with tempfile.TemporaryDirectory() as scratch:
            day = Path(scratch) / '20261027'
            day.mkdir()
            fixtures = {'rec_123456.mp4': b'old video',
                        'rec_20261027_123456_789.mp4': b'new video',
                        'rockchip-smart-gateway_info_20261027_123456_789.log': b'log',
                        'rockchip-smart-gateway_error.log': b'error'}
            for file, data in fixtures.items():
                (day / file).write_bytes(data)
            (day / 'video_20261027_123456_000.mp4').write_bytes(b'collision kept')
            (day / 'rec_999999.mp4').symlink_to('/etc/passwd')
            self.assertEqual(names.normalize(scratch), 4)
            self.assertEqual((day / 'video_20261027_123456_000.mp4').read_bytes(), b'collision kept')
            self.assertEqual((day / 'video_20261027_123456_000_1.mp4').read_bytes(), b'old video')
            self.assertEqual((day / 'video_20261027_123456_789.mp4').read_bytes(), b'new video')
            self.assertEqual((day / 'log_info_20261027_123456_789.log').read_bytes(), b'log')
            self.assertTrue((day / 'rec_999999.mp4').is_symlink())
            self.assertEqual(names.normalize(scratch), 0)

if __name__ == '__main__':
    unittest.main()
