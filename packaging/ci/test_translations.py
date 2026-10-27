import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name('check_translations.py')


class TranslationCompletenessTests(unittest.TestCase):
    def run_catalog(self, text):
        with tempfile.TemporaryDirectory() as directory:
            catalog = Path(directory) / 'mikai_uk.ts'
            catalog.write_text(text, encoding='utf-8')
            return subprocess.run(
                [sys.executable, str(SCRIPT), '--catalog', str(catalog)],
                check=False, capture_output=True, text=True)

    def test_unfinished_xml_message_fails(self):
        result = self.run_catalog(
            '<TS><context><name>Player</name><message><source>Play</source>'
            '<translation type="unfinished"/></message></context></TS>')
        self.assertEqual(result.returncode, 1)
        self.assertIn('Player: Play', result.stderr)

    def test_ordinary_text_and_comments_are_not_mistaken_for_attributes(self):
        result = self.run_catalog(
            '<TS><!-- type="unfinished" --><context><name>Player</name>'
            '<message><source>unfinished</source><translation>Готово</translation>'
            '</message></context></TS>')
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_malformed_catalog_is_not_accepted(self):
        result = self.run_catalog('<TS><context>')
        self.assertEqual(result.returncode, 2)
        self.assertIn('cannot parse', result.stderr)


if __name__ == '__main__':
    unittest.main()
