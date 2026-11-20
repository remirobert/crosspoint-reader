import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT_PATH = Path(__file__).resolve().parents[1] / "tools" / "deck2jsonl.py"
SPEC = importlib.util.spec_from_file_location("deck2jsonl", SCRIPT_PATH)
DECK2JSONL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DECK2JSONL)

SEP = "\x1f"


def _create_apkg_sqlite(
    db_path: Path,
    *,
    models: dict,
    decks: dict,
    notes: list[tuple[int, str, int, list[str]]],
    cards: list[tuple[int, int, int, int]],
) -> None:
    conn = sqlite3.connect(str(db_path))
    try:
        conn.executescript(
            """
            CREATE TABLE col (
                id integer primary key,
                crt integer not null,
                mod integer not null,
                scm integer not null,
                ver integer not null,
                dty integer not null,
                usn integer not null,
                ls integer not null,
                conf text not null,
                models text not null,
                decks text not null,
                dconf text not null,
                tags text not null
            );
            CREATE TABLE notes (
                id integer primary key,
                guid text not null,
                mid integer not null,
                mod integer not null,
                usn integer not null,
                tags text not null,
                flds text not null,
                sfld integer not null,
                csum integer not null,
                flags integer not null,
                data text not null
            );
            CREATE TABLE cards (
                id integer primary key,
                nid integer not null,
                did integer not null,
                ord integer not null,
                mod integer not null,
                usn integer not null,
                type integer not null,
                queue integer not null,
                due integer not null,
                ivl integer not null,
                factor integer not null,
                reps integer not null,
                lapses integer not null,
                left integer not null,
                odue integer not null,
                odid integer not null,
                flags integer not null,
                data text not null
            );
            """
        )
        conn.execute(
            "INSERT INTO col VALUES (1, 0, 0, 0, 11, 0, 0, 0, '{}', ?, ?, '{}', '{}')",
            (json.dumps(models), json.dumps(decks)),
        )
        for nid, guid, mid, fields in notes:
            conn.execute(
                "INSERT INTO notes VALUES (?, ?, ?, 0, 0, '', ?, 0, 0, 0, '')",
                (nid, guid, mid, SEP.join(fields)),
            )
        for cid, nid, did, ord_idx in cards:
            conn.execute(
                "INSERT INTO cards VALUES (?, ?, ?, ?, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '')",
                (cid, nid, did, ord_idx),
            )
        conn.commit()
    finally:
        conn.close()


class Deck2JsonlTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)

    def test_apkg_v21_custom_fields_cjk_html_and_media(self) -> None:
        real_db = self.root / "real.sqlite"
        stub_db = self.root / "stub.sqlite"

        models = {
            "100": {
                "id": 100,
                "name": "Unused Basic",
                "flds": [{"name": "Front", "ord": 0}, {"name": "Back", "ord": 1}],
            },
            "200": {
                "id": 200,
                "name": "Japanese Core",
                "flds": [
                    {"name": "Kanji", "ord": 0},
                    {"name": "Meaning", "ord": 1},
                    {"name": "Furigana", "ord": 2},
                    {"name": "Sentence", "ord": 3},
                ],
            },
        }
        decks = {
            "1": {"id": 1, "name": "Default"},
            "50": {"id": 50, "name": "Japanese Core 2k"},
        }
        notes = [
            (
                10,
                "guid-eat",
                200,
                [
                    "<b>食べる</b> [sound:taberu.mp3]",
                    "to eat<br>to consume",
                    "たべる",
                    (
                        "<div>毎日野菜を食べる。</div>"
                        "<div><img src='veg.png'>I eat veggies &amp; fruit.</div>"
                    ),
                ],
            ),
            (
                11,
                "guid-drink",
                200,
                [
                    "飲む<img src=\"drink.jpg\" />",
                    "<p>to drink</p><p>a &lt; b &amp;&amp; b &gt; c</p>",
                    "",
                    "   ",
                ],
            ),
            (
                12,
                "guid-media-only",
                200,
                [
                    "[sound:only_audio.mp3]<img src='blank.png'><br>",
                    "should be skipped",
                    "hint",
                    "extra",
                ],
            ),
            (
                13,
                "guid-empty-back",
                200,
                [
                    "見る",
                    "<style>.card { color: black; }</style>   ",
                    "みる",
                    "",
                ],
            ),
        ]
        cards = [
            (1001, 10, 50, 0),
            (1002, 10, 50, 1),  # duplicate note mapping -> deduplicated
            (1003, 11, 50, 0),
            (1004, 12, 50, 0),
            (1005, 13, 50, 0),
        ]
        _create_apkg_sqlite(real_db, models=models, decks=decks, notes=notes, cards=cards)

        # Create a dummy collection.anki2 placeholder alongside collection.anki21.
        _create_apkg_sqlite(
            stub_db,
            models={"1": {"id": 1, "name": "Basic", "flds": [{"name": "Front"}, {"name": "Back"}]}},
            decks={"1": {"id": 1, "name": "Default"}},
            notes=[(1, "stub", 1, ["Legacy stub row.", "Stub"])],
            cards=[(1, 1, 1, 0)],
        )

        apkg_path = self.root / "core2k.apkg"
        with zipfile.ZipFile(apkg_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            zf.write(stub_db, "collection.anki2")
            zf.write(real_db, "collection.anki21")
            zf.writestr("media", "{}")

        out_path = self.root / "core2k.jsonl"
        stdout_buf = io.StringIO()
        with contextlib.redirect_stdout(stdout_buf):
            rc = DECK2JSONL.main(
                [
                    str(apkg_path),
                    str(out_path),
                    "--title",
                    "Japanese Core 2k",
                    "--front-field",
                    "Kanji",
                    "--back-field",
                    "Meaning",
                    "--hint-field",
                    "Furigana",
                    "--extra-field",
                    "Sentence",
                ]
            )

        self.assertEqual(rc, 0)
        lines = out_path.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            lines,
            [
                '{"_meta":true,"title":"Japanese Core 2k"}',
                (
                    '{"id":"1001","front":"食べる","back":"to eat\\nto consume",'
                    '"hint":"たべる","extra":"毎日野菜を食べる。\\nI eat veggies & fruit."}'
                ),
                '{"id":"1003","front":"飲む","back":"to drink\\na < b && b > c"}',
            ],
        )

    def test_apkg_v2_numeric_indices_and_deck_title_autodetect(self) -> None:
        db_path = self.root / "legacy.sqlite"
        models = {
            "10": {
                "id": 10,
                "name": "Custom3",
                "flds": [
                    {"name": "Prompt", "ord": 0},
                    {"name": "Answer", "ord": 1},
                    {"name": "Notes", "ord": 2},
                ],
            }
        }
        decks = {
            "1": {"id": 1, "name": "Default"},
            "99": {"id": 99, "name": "French Essentials"},
        }
        notes = [
            (501, "fr-01", 10, ["Bonjour&nbsp;!", "Hello!", "Formal &amp; polite greeting"]),
        ]
        cards = [(9001, 501, 99, 0)]
        _create_apkg_sqlite(db_path, models=models, decks=decks, notes=notes, cards=cards)

        apkg_path = self.root / "french.apkg"
        with zipfile.ZipFile(apkg_path, "w") as zf:
            zf.write(db_path, "collection.anki2")

        out_path = self.root / "french.jsonl"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = DECK2JSONL.main(
                [
                    str(apkg_path),
                    "-o",
                    str(out_path),
                    "--front-field",
                    "0",
                    "--back-field",
                    "1",
                    "--extra-field",
                    "2",
                ]
            )

        self.assertEqual(rc, 0)
        lines = out_path.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            lines,
            [
                '{"_meta":true,"title":"French Essentials"}',
                '{"id":"9001","front":"Bonjour !","back":"Hello!","extra":"Formal & polite greeting"}',
            ],
        )

    def test_apkg_notes_guid_fallback_when_cards_table_empty(self) -> None:
        db_path = self.root / "notes_only.sqlite"
        models = {
            "1": {
                "id": 1,
                "name": "Basic",
                "flds": [{"name": "Front", "ord": 0}, {"name": "Back", "ord": 1}],
            }
        }
        _create_apkg_sqlite(
            db_path,
            models=models,
            decks={"1": {"id": 1, "name": "Default"}},
            notes=[(42, "n5-001", 1, ["猫", "cat"])],
            cards=[],
        )
        apkg_path = self.root / "cats.apkg"
        with zipfile.ZipFile(apkg_path, "w") as zf:
            zf.write(db_path, "collection.anki21")

        out_path = self.root / "cats.jsonl"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = DECK2JSONL.main([str(apkg_path), str(out_path)])

        self.assertEqual(rc, 0)
        lines = out_path.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            lines,
            [
                '{"_meta":true,"title":"cats"}',
                '{"id":"n5-001","front":"猫","back":"cat"}',
            ],
        )

    def test_zstd_v21b_detected_cleanly_without_traceback(self) -> None:
        stub_db = self.root / "stub.sqlite"
        _create_apkg_sqlite(
            stub_db,
            models={"1": {"id": 1, "name": "Basic", "flds": [{"name": "Front"}, {"name": "Back"}]}},
            decks={"1": {"id": 1, "name": "Default"}},
            notes=[(1, "stub", 1, ["Legacy stub", "Stub"])],
            cards=[(1, 1, 1, 0)],
        )

        apkg_21b = self.root / "modern_zstd.apkg"
        with zipfile.ZipFile(apkg_21b, "w") as zf:
            zf.write(stub_db, "collection.anki2")
            zf.writestr("collection.anki21b", DECK2JSONL.ZSTD_MAGIC + b"\x00\x01\x02\x03")

        out_path = self.root / "should_not_exist.jsonl"
        proc = subprocess.run(
            [sys.executable, str(SCRIPT_PATH), str(apkg_21b), str(out_path)],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("collection.anki21b", proc.stderr)
        self.assertIn("zstd", proc.stderr.lower())
        self.assertNotIn("Traceback", proc.stderr)
        self.assertFalse(out_path.exists())

        # Also verify passing a raw zstd-compressed .anki21b file directly.
        raw_21b = self.root / "collection.anki21b"
        raw_21b.write_bytes(DECK2JSONL.ZSTD_MAGIC + b"compressed-payload")
        err_buf = io.StringIO()
        with contextlib.redirect_stderr(err_buf):
            rc = DECK2JSONL.main([str(raw_21b), str(out_path)])
        self.assertEqual(rc, 1)
        self.assertIn("collection.anki21b", err_buf.getvalue())
        self.assertFalse(out_path.exists())

    def test_tsv_plain_text_and_export_directives(self) -> None:
        tsv_path = self.root / "vocab.tsv"
        tsv_path.write_text(
            "食べる [sound:eat.mp3]\tto eat<br>consume\tたべる\t<div>毎日野菜を食べる。</div>\n"
            "飲む\tto drink\t\t\n"
            "#include <stdio.h>\tStandard C I/O header\t\t\n"
            "   \tempty front skipped\t\t\n"
            "empty back skipped\t[sound:none.mp3]\t\t\n",
            encoding="utf-8",
        )
        out_path = self.root / "vocab.jsonl"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = DECK2JSONL.main(
                [
                    str(tsv_path),
                    str(out_path),
                    "--hint-field",
                    "2",
                    "--extra-field",
                    "3",
                ]
            )

        self.assertEqual(rc, 0)
        lines = out_path.read_text(encoding="utf-8").splitlines()
        self.assertEqual(
            lines,
            [
                '{"_meta":true,"title":"vocab"}',
                (
                    '{"front":"食べる","back":"to eat\\nconsume",'
                    '"hint":"たべる","extra":"毎日野菜を食べる。"}'
                ),
                '{"front":"飲む","back":"to drink"}',
                '{"front":"#include <stdio.h>","back":"Standard C I/O header"}',
            ],
        )

        # Also test plain-text export headers (#separator:tab, #deck:, #guid column:1).
        export_txt = self.root / "deck_export.tsv"
        export_txt.write_text(
            "#separator:tab\n"
            "#html:true\n"
            "#guid column:1\n"
            "#deck:Kanji N5\n"
            "g-001\t山\tmountain\n"
            "g-002\t川\triver\n",
            encoding="utf-8",
        )
        export_out = self.root / "deck_export.jsonl"
        with contextlib.redirect_stdout(io.StringIO()):
            rc2 = DECK2JSONL.main([str(export_txt), str(export_out)])
        self.assertEqual(rc2, 0)
        self.assertEqual(
            export_out.read_text(encoding="utf-8").splitlines(),
            [
                '{"_meta":true,"title":"Kanji N5"}',
                '{"id":"g-001","front":"山","back":"mountain"}',
                '{"id":"g-002","front":"川","back":"river"}',
            ],
        )

    def test_csv_named_headers_and_oversized_line_truncation(self) -> None:
        long_extra = "解説文です。" * 400  # > 7000 UTF-8 bytes
        csv_path = self.root / "cards.csv"
        csv_path.write_text(
            "CardID,Expression,Meaning,Reading,Notes\n"
            'c-1,"行く, 参る","to go\n(humble)",いく,"' + long_extra + '"\n',
            encoding="utf-8",
        )
        out_path = self.root / "cards.jsonl"
        with contextlib.redirect_stdout(io.StringIO()):
            rc = DECK2JSONL.main(
                [
                    str(csv_path),
                    str(out_path),
                    "--title",
                    "CSV Deck",
                    "--id-field",
                    "CardID",
                    "--front-field",
                    "Expression",
                    "--back-field",
                    "Meaning",
                    "--hint-field",
                    "Reading",
                    "--extra-field",
                    "Notes",
                ]
            )

        self.assertEqual(rc, 0)
        lines = out_path.read_text(encoding="utf-8").splitlines()
        self.assertEqual(len(lines), 2)
        self.assertEqual(
            lines[0],
            '{"_meta":true,"title":"CSV Deck"}',
        )
        raw_bytes = lines[1].encode("utf-8")
        self.assertLessEqual(len(raw_bytes), 4095)
        parsed = json.loads(lines[1])
        self.assertEqual(parsed["id"], "c-1")
        self.assertEqual(parsed["front"], "行く, 参る")
        self.assertEqual(parsed["back"], "to go\n(humble)")
        self.assertEqual(parsed["hint"], "いく")
        self.assertTrue(parsed["extra"].endswith("…"))

    def test_unknown_field_name_reports_available_fields(self) -> None:
        db_path = self.root / "err.sqlite"
        _create_apkg_sqlite(
            db_path,
            models={
                "1": {
                    "id": 1,
                    "name": "Vocab",
                    "flds": [{"name": "Word", "ord": 0}, {"name": "Def", "ord": 1}],
                }
            },
            decks={"1": {"id": 1, "name": "Default"}},
            notes=[(1, "g1", 1, ["hello", "world"])],
            cards=[(10, 1, 1, 0)],
        )
        apkg_path = self.root / "err.apkg"
        with zipfile.ZipFile(apkg_path, "w") as zf:
            zf.write(db_path, "collection.anki21")

        err_buf = io.StringIO()
        with contextlib.redirect_stderr(err_buf):
            rc = DECK2JSONL.main([str(apkg_path), "--front-field", "WrongName"])
        self.assertEqual(rc, 1)
        self.assertIn("WrongName", err_buf.getvalue())
        self.assertIn("Word, Def", err_buf.getvalue())


if __name__ == "__main__":
    unittest.main()
