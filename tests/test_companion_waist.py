"""The second-screen waist (docs/companion-waist.md) and its state header (games/totk/runtime/companion_state.h) agree.

The ledger's "Waist fields" table lists every state word, landed or proposed. The words marked landed must be exactly the
CompanionState fields and the companion_state_words order, and must match the Java index constants
(CompanionState.java). Words marked proposed must not be in the header yet. A field added to the header without
updating the ledger fails here, and so does a ledger row that says landed while the header lacks it. Each row names a
ledger entry that exists."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / 'docs/companion-waist.md'
HEADER = ROOT / 'games/totk/runtime/companion_state.h'
JAVA = ROOT / 'games/totk/android/java/com/aure/totk/companion/CompanionState.java'


def waist_rows():
    text = DOC.read_text()
    block = re.search(r'<!-- waist-fields:begin -->\n(.*?)<!-- waist-fields:end -->', text, re.S)
    if not block:
        raise AssertionError('docs/companion-waist.md: no waist-fields block')
    rows = []
    for line in block.group(1).splitlines():
        cells = [c.strip() for c in line.strip().strip('|').split('|')]
        if len(cells) != 6 or not cells[0].isdigit():
            continue
        rows.append({'word': int(cells[0]), 'field': cells[1], 'status': cells[4], 'ledger': cells[5]})
    return text, rows


def header_fields(header):
    struct = re.search(r'typedef struct CompanionState \{(.*?)\} CompanionState;', header, re.S).group(1)
    fields = []
    for decl in re.findall(r'^\s*int32_t ([\w, ]+);', struct, re.M):
        fields += [name.strip() for name in decl.split(',')]
    return fields


class CompanionWaistTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.doc, cls.rows = waist_rows()
        cls.header = HEADER.read_text()
        cls.java = JAVA.read_text()

    def test_rows_are_numbered_without_gaps_and_well_formed(self):
        self.assertEqual([r['word'] for r in self.rows], list(range(len(self.rows))))
        for r in self.rows:
            self.assertIn(r['status'], ('landed', 'proposed'), r)
            self.assertRegex(r['field'], r'^[a-z][a-z_]*$', r)
        landed = [r['status'] == 'landed' for r in self.rows]
        self.assertEqual(landed, sorted(landed, reverse=True), 'landed words come first: new words are appended')

    def test_every_row_names_a_ledger_entry(self):
        entries = set(re.findall(r'^\| (L\d\d) \|', self.doc, re.M))
        for r in self.rows:
            self.assertIn(r['ledger'], entries, r)

    def test_landed_words_are_the_header(self):
        landed = [r['field'] for r in self.rows if r['status'] == 'landed']
        words = re.findall(r'out\[(\d+)\] = (?:s->)?(\w+);', self.header)
        self.assertEqual([(int(i), n) for i, n in words], list(enumerate(landed)),
                         'companion_state_words and the ledger\'s landed words differ')
        self.assertEqual(int(re.search(r'COMPANION_STATE_FIELDS = (\d+)', self.header).group(1)), len(landed))
        self.assertEqual(header_fields(self.header), [f for f in landed if f != 'age_ms'],
                         'CompanionState fields and the ledger\'s landed words differ')

    def test_proposed_words_are_not_in_the_header_yet(self):
        fields = set(header_fields(self.header))
        for r in self.rows:
            if r['status'] == 'proposed':
                self.assertNotIn(r['field'], fields, f'{r["field"]} is in companion_state.h: mark it landed in the ledger')

    def test_java_indices_match(self):
        constants = {k: int(v) for k, v in re.findall(r'\b([A-Z_]+) = (\d+)', self.java)}
        landed = [r for r in self.rows if r['status'] == 'landed']
        for r in landed:
            self.assertEqual(constants.get(r['field'].upper()), r['word'], r['field'])
        self.assertEqual(constants['FIELDS'], len(landed))


if __name__ == '__main__':
    unittest.main()
