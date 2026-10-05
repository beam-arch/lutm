import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location('collect', Path(__file__).with_name('collect.py'))
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)


class CollectorTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        (self.root / 'bootstrap.exit').write_text('0')

    def test_running_build_and_incremental_refresh(self):
        log = self.root / 'build.log'
        log.write_text('OUT_DIR=out/non-ab\nTARGET_BUILD_VARIANT=userdebug\n[ 28% 28/100] compile fixture\nsecret value must not appear\n')
        first = collector.collect(self.root)
        self.assertEqual(first['layouts'][0]['progress']['percent'], 28)
        self.assertEqual(first['layouts'][1]['status'], 'queued')
        self.assertNotIn('secret value', json.dumps(first))
        with log.open('a') as stream:
            stream.write('[ 29% 29/100] compile fixture\n')
        self.assertEqual(collector.collect(self.root)['layouts'][0]['progress']['done'], 29)

    def test_verified_uploads_expose_only_public_links(self):
        (self.root / 'upload-non-ab.exit').write_text('0')
        (self.root / 'upload-non-ab.log').write_text('  "fixture.zip" (123 bytes, MD5 abc)\n    https://gofile.io/d/file-id\n    folder: https://gofile.io/d/folder-id\n')
        result = collector.collect(self.root)
        self.assertEqual(result['layouts'][0]['status'], 'complete')
        self.assertEqual(result['layouts'][0]['downloads'], [{'name': 'fixture.zip', 'url': 'https://gofile.io/d/folder-id'}])

    def test_failure_is_visible_and_does_not_claim_completion(self):
        (self.root / 'build.log').write_text('OUT_DIR=out/ab\nFAILED: fixture target\n')
        (self.root / 'build.exit').write_text('1')
        result = collector.collect(self.root)
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['layouts'][1]['status'], 'failed')
        self.assertEqual(result['failure'], 'FAILED: fixture target')


if __name__ == '__main__':
    unittest.main()
