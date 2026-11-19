#!/usr/bin/env python3
"""Flashcard deck converter (.apkg, .tsv, .csv -> /flashcards/<deck>.jsonl).

Uses only the Python 3 standard library (zero external dependencies).
"""

from __future__ import annotations

import argparse
import csv
import html
import json
from pathlib import Path
import re
import sqlite3
import sys
import tempfile
import zipfile

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"
SQLITE_MAGIC = b"SQLite format 3\x00"
NOTE_FIELD_SEP = "\x1f"

DEFAULT_NEW_PER_DAY = 20
MAX_NEW_PER_DAY = 200
DEFAULT_MAX_REVIEWS = 200
MAX_REVIEWS_CAP = 500
MAX_JSONL_LINE_BYTES = 4096
MAX_LINE_CONTENT_BYTES = MAX_JSONL_LINE_BYTES - 1

_STYLE_SCRIPT_RE = re.compile(r"<(style|script)\b[^>]*>.*?</\1>", re.IGNORECASE | re.DOTALL)
_HTML_COMMENT_RE = re.compile(r"<!--.*?-->", re.DOTALL)
_SOUND_TAG_RE = re.compile(r"\[sound:[^\]]*\]", re.IGNORECASE)
_IMG_TAG_RE = re.compile(r"<img\b[^>]*>", re.IGNORECASE)
_BR_TAG_RE = re.compile(r"<br\b[^>]*>", re.IGNORECASE)
_ADJACENT_BLOCK_RE = re.compile(
    r"</(?:div|p|li|tr|blockquote|h[1-6])>\s*<(?:div|p|li|tr|blockquote|h[1-6])\b[^>]*>",
    re.IGNORECASE,
)
_BLOCK_TAG_RE = re.compile(r"</?(?:div|p|li|tr|blockquote|h[1-6])\b[^>]*>", re.IGNORECASE)
_ANY_TAG_RE = re.compile(r"</?[a-zA-Z][a-zA-Z0-9-]*(?:\s[^>]*)?/?>")
_MULTI_NEWLINE_RE = re.compile(r"\n{3,}")
_NONNEG_INT_RE = re.compile(r"\d+")

ZSTD_ACTIONABLE_MESSAGE = (
    "Detected zstd-compressed .apkg database ('collection.anki21b').\n"
    "deck2jsonl.py uses only the Python standard library and cannot decompress zstd directly.\n"
    "How to convert this deck:\n"
    "  1. Re-export the deck as a legacy uncompressed .apkg package ('collection.anki21' / 'collection.anki2'),\n"
    "     OR export as plain-text notes (.tsv / .txt) and pass that file to deck2jsonl.py.\n"
    "  2. Alternatively, decompress collection.anki21b with the 'zstd' CLI:\n"
    "     unzip deck.apkg collection.anki21b && unzstd collection.anki21b -o collection.anki21"
)


class ConversionError(Exception):
    """Raised when an input file cannot be converted cleanly."""


def clean_field(raw: str) -> str:
    """Normalize card HTML/media markup into clean UTF-8 plain text."""
    if not raw:
        return ""
    text = raw.replace("\r\n", "\n").replace("\r", "\n")
    text = _STYLE_SCRIPT_RE.sub("", text)
    text = _HTML_COMMENT_RE.sub("", text)
    text = _SOUND_TAG_RE.sub("", text)
    text = _IMG_TAG_RE.sub("", text)
    text = _BR_TAG_RE.sub("\n", text)
    text = _ADJACENT_BLOCK_RE.sub("\n", text)
    text = _BLOCK_TAG_RE.sub("\n", text)
    text = _ANY_TAG_RE.sub("", text)
    text = html.unescape(text)
    text = text.replace("\xa0", " ")
    lines = [line.strip(" \t") for line in text.split("\n")]
    text = "\n".join(lines)
    text = _MULTI_NEWLINE_RE.sub("\n\n", text)
    return text.strip()


def _serialize_compact(obj: dict[str, object]) -> str:
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


def format_meta_line(title: str, new_per_day: int, max_reviews: int) -> str:
    """Format line 1 deck metadata JSON object."""
    clamped_new = max(0, min(MAX_NEW_PER_DAY, new_per_day))
    clamped_reviews = max(1, min(MAX_REVIEWS_CAP, max_reviews))
    clean_title = title.strip() or "Deck"
    return _serialize_compact(
        {
            "_meta": True,
            "title": clean_title,
            "newPerDay": clamped_new,
            "maxReviews": clamped_reviews,
        }
    )


def _build_card_dict(
    front: str,
    back: str,
    card_id: str | None = None,
    hint: str = "",
    extra: str = "",
) -> dict[str, str]:
    card: dict[str, str] = {}
    if card_id:
        card["id"] = card_id
    card["front"] = front
    card["back"] = back
    if hint:
        card["hint"] = hint
    if extra:
        card["extra"] = extra
    return card


def _truncate_field_to_fit(
    card: dict[str, str],
    key: str,
    max_bytes: int,
) -> bool:
    """Truncate card[key] by whole Unicode characters so serialized JSON fits max_bytes."""
    value = card.get(key, "")
    if not value:
        return len(_serialize_compact(card).encode("utf-8")) <= max_bytes

    # Check if dropping this optional field (or reducing to 1 char + ellipsis for required) can fit.
    is_optional = key in ("extra", "hint")
    probe = dict(card)
    if is_optional:
        del probe[key]
        if len(_serialize_compact(probe).encode("utf-8")) > max_bytes:
            del card[key]
            return False
    else:
        probe[key] = "…"
        if len(_serialize_compact(probe).encode("utf-8")) > max_bytes:
            card[key] = "…"
            return False

    low = 0
    high = len(value)
    best = "" if is_optional else "…"
    while low <= high:
        mid = (low + high) // 2
        candidate = (value[:mid].rstrip() + "…") if mid < len(value) else value
        if is_optional and mid == 0:
            trial = {k: v for k, v in card.items() if k != key}
        else:
            if not is_optional and mid == 0:
                candidate = "…"
            trial = dict(card)
            trial[key] = candidate

        if len(_serialize_compact(trial).encode("utf-8")) <= max_bytes:
            best = "" if (is_optional and mid == 0) else candidate
            low = mid + 1
        else:
            high = mid - 1

    if is_optional and not best:
        card.pop(key, None)
    else:
        card[key] = best
    return len(_serialize_compact(card).encode("utf-8")) <= max_bytes


def _serialize_cleaned_card(
    cleaned_front: str,
    cleaned_back: str,
    card_id: str | None = None,
    cleaned_hint: str = "",
    cleaned_extra: str = "",
) -> str | None:
    if not cleaned_front or not cleaned_back:
        return None

    cleaned_id = card_id.strip() if card_id else None
    card = _build_card_dict(
        front=cleaned_front,
        back=cleaned_back,
        card_id=cleaned_id,
        hint=cleaned_hint,
        extra=cleaned_extra,
    )
    line = _serialize_compact(card)
    if len(line.encode("utf-8")) <= MAX_LINE_CONTENT_BYTES:
        return line

    for field_key in ("extra", "hint", "back", "front"):
        if _truncate_field_to_fit(card, field_key, MAX_LINE_CONTENT_BYTES):
            return _serialize_compact(card)

    return None


def format_card_line(
    front: str,
    back: str,
    card_id: str | None = None,
    hint: str = "",
    extra: str = "",
) -> str | None:
    """Clean fields and serialize a single card JSONL line within CrossPoint's 4096B limit."""
    return _serialize_cleaned_card(
        cleaned_front=clean_field(front),
        cleaned_back=clean_field(back),
        card_id=card_id,
        cleaned_hint=clean_field(hint),
        cleaned_extra=clean_field(extra),
    )


def _resolve_field_spec(spec: str | None, field_names: list[str]) -> tuple[int | None, bool]:
    """Resolve a 0-based index string or field name against field_names.

    Returns (resolved_index_or_none, matched_model).
    """
    if spec is None:
        return None, True
    if _NONNEG_INT_RE.fullmatch(spec):
        idx = int(spec)
        return idx, True
    for i, name in enumerate(field_names):
        if name == spec:
            return i, True
    spec_lower = spec.lower()
    for i, name in enumerate(field_names):
        if name.lower() == spec_lower:
            return i, True
    return None, False


def _get_field(fields: list[str], idx: int | None) -> str:
    if idx is None or idx < 0 or idx >= len(fields):
        return ""
    return fields[idx]


def _table_exists(conn: sqlite3.Connection, table_name: str) -> bool:
    row = conn.execute(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=? LIMIT 1",
        (table_name,),
    ).fetchone()
    return row is not None


def _load_apkg_models(conn: sqlite3.Connection) -> dict[int, list[str]]:
    models_map: dict[int, list[str]] = {}
    if _table_exists(conn, "col"):
        row = conn.execute("SELECT models FROM col LIMIT 1").fetchone()
        if row and row[0]:
            try:
                raw_models = json.loads(row[0])
            except json.JSONDecodeError as exc:
                raise ConversionError(f"Invalid col.models JSON in .apkg database: {exc}") from exc
            if isinstance(raw_models, dict):
                for mid_key, model_obj in raw_models.items():
                    if not isinstance(model_obj, dict):
                        continue
                    try:
                        mid = int(model_obj.get("id", mid_key))
                    except (TypeError, ValueError):
                        continue
                    raw_flds = model_obj.get("flds", [])
                    if isinstance(raw_flds, list):
                        ordered = sorted(
                            enumerate(raw_flds),
                            key=lambda pair: (
                                pair[1].get("ord", pair[0]) if isinstance(pair[1], dict) else pair[0]
                            ),
                        )
                        models_map[mid] = [
                            str(item.get("name", "")) if isinstance(item, dict) else ""
                            for _, item in ordered
                        ]

    if not models_map and _table_exists(conn, "fields"):
        for ntid, _ord, name in conn.execute("SELECT ntid, ord, name FROM fields ORDER BY ntid, ord"):
            models_map.setdefault(int(ntid), []).append(str(name))

    return models_map


def _load_apkg_deck_title(conn: sqlite3.Connection) -> str | None:
    deck_names: list[str] = []
    if _table_exists(conn, "col"):
        row = conn.execute("SELECT decks FROM col LIMIT 1").fetchone()
        if row and row[0]:
            try:
                raw_decks = json.loads(row[0])
            except json.JSONDecodeError:
                raw_decks = {}
            if isinstance(raw_decks, dict):
                for did_key, deck_obj in raw_decks.items():
                    if not isinstance(deck_obj, dict):
                        continue
                    name = str(deck_obj.get("name", "")).strip()
                    did_str = str(deck_obj.get("id", did_key))
                    if name and not (did_str == "1" and name == "Default"):
                        deck_names.append(name.replace(NOTE_FIELD_SEP, "::"))

    if not deck_names and _table_exists(conn, "decks"):
        for did, name in conn.execute("SELECT id, name FROM decks"):
            clean_name = str(name).strip()
            if clean_name and not (int(did) == 1 and clean_name == "Default"):
                deck_names.append(clean_name.replace(NOTE_FIELD_SEP, "::"))

    if len(deck_names) == 1:
        return deck_names[0]
    return None


def _read_apkg(
    input_path: Path,
    front_spec: str | None,
    back_spec: str | None,
    hint_spec: str | None,
    extra_spec: str | None,
) -> tuple[str | None, list[str]]:
    try:
        zf_ctx = zipfile.ZipFile(input_path, "r")
    except zipfile.BadZipFile as exc:
        raise ConversionError(f"Invalid .apkg archive '{input_path}': {exc}") from exc

    with zf_ctx as zf:
        names = set(zf.namelist())
        if "collection.anki21" in names:
            member_name = "collection.anki21"
        elif "collection.anki21b" in names:
            raise ConversionError(ZSTD_ACTIONABLE_MESSAGE)
        elif "collection.anki2" in names:
            member_name = "collection.anki2"
        else:
            raise ConversionError(
                f"Archive '{input_path}' does not contain 'collection.anki21' or 'collection.anki2'."
            )

        with zf.open(member_name, "r") as member_stream:
            header_bytes = member_stream.read(16)

        if header_bytes.startswith(ZSTD_MAGIC):
            raise ConversionError(ZSTD_ACTIONABLE_MESSAGE)
        if not header_bytes.startswith(SQLITE_MAGIC):
            raise ConversionError(
                f"Member '{member_name}' in '{input_path}' is not an uncompressed SQLite 3 database."
            )

        with tempfile.TemporaryDirectory(prefix="deck2jsonl_") as tmp_dir:
            db_path = Path(tmp_dir) / member_name
            with zf.open(member_name, "r") as src, db_path.open("wb") as dst:
                while True:
                    chunk = src.read(65536)
                    if not chunk:
                        break
                    dst.write(chunk)

            conn = sqlite3.connect(str(db_path))
            try:
                return _extract_sqlite_cards(
                    conn=conn,
                    front_spec=front_spec,
                    back_spec=back_spec,
                    hint_spec=hint_spec,
                    extra_spec=extra_spec,
                )
            finally:
                conn.close()


def _extract_sqlite_cards(
    conn: sqlite3.Connection,
    front_spec: str | None,
    back_spec: str | None,
    hint_spec: str | None,
    extra_spec: str | None,
) -> tuple[str | None, list[str]]:
    if not _table_exists(conn, "notes"):
        raise ConversionError("Deck SQLite database is missing the 'notes' table.")

    models_map = _load_apkg_models(conn)
    detected_title = _load_apkg_deck_title(conn)

    effective_front = front_spec if front_spec is not None else "0"
    effective_back = back_spec if back_spec is not None else "1"

    has_cards_rows = False
    if _table_exists(conn, "cards"):
        count_row = conn.execute("SELECT COUNT(*) FROM cards").fetchone()
        has_cards_rows = bool(count_row and count_row[0] > 0)

    if has_cards_rows:
        rows = conn.execute(
            "SELECT c.id, n.id, n.guid, n.mid, n.flds "
            "FROM cards c JOIN notes n ON c.nid = n.id "
            "ORDER BY c.id ASC"
        ).fetchall()
    else:
        rows = [
            (None, nid, guid, mid, flds)
            for nid, guid, mid, flds in conn.execute(
                "SELECT id, guid, mid, flds FROM notes ORDER BY id ASC"
            ).fetchall()
        ]

    if not rows:
        return detected_title, []

    used_mids = {int(row[3]) for row in rows}
    resolved_per_mid: dict[int, tuple[int | None, int | None, int | None, int | None]] = {}
    spec_matched_any: dict[str, bool] = {}
    for label, spec in (
        ("front", effective_front),
        ("back", effective_back),
        ("hint", hint_spec),
        ("extra", extra_spec),
    ):
        if spec is not None:
            spec_matched_any[label] = False

    for mid in used_mids:
        field_names = models_map.get(mid, [])
        f_idx, f_ok = _resolve_field_spec(effective_front, field_names)
        b_idx, b_ok = _resolve_field_spec(effective_back, field_names)
        h_idx, h_ok = _resolve_field_spec(hint_spec, field_names)
        e_idx, e_ok = _resolve_field_spec(extra_spec, field_names)
        resolved_per_mid[mid] = (f_idx, b_idx, h_idx, e_idx)
        if f_ok:
            spec_matched_any["front"] = True
        if b_ok:
            spec_matched_any["back"] = True
        if hint_spec is not None and h_ok:
            spec_matched_any["hint"] = True
        if extra_spec is not None and e_ok:
            spec_matched_any["extra"] = True

    for label, spec in (
        ("--front-field", front_spec),
        ("--back-field", back_spec),
        ("--hint-field", hint_spec),
        ("--extra-field", extra_spec),
    ):
        key = label.removeprefix("--").removesuffix("-field")
        if spec is not None and not spec_matched_any.get(key, True):
            available: list[str] = []
            for mid in sorted(used_mids):
                for fname in models_map.get(mid, []):
                    if fname and fname not in available:
                        available.append(fname)
            avail_str = ", ".join(available) if available else "(none)"
            raise ConversionError(
                f"{label} '{spec}' was not found in deck models. Available fields: {avail_str}"
            )

    card_lines: list[str] = []
    seen_note_pairs: set[tuple[int, str, str]] = set()

    for card_id, note_id, guid, mid, flds_str in rows:
        fields = str(flds_str).split(NOTE_FIELD_SEP)
        f_idx, b_idx, h_idx, e_idx = resolved_per_mid[int(mid)]
        raw_front = _get_field(fields, f_idx)
        raw_back = _get_field(fields, b_idx)
        raw_hint = _get_field(fields, h_idx)
        raw_extra = _get_field(fields, e_idx)

        cleaned_front = clean_field(raw_front)
        cleaned_back = clean_field(raw_back)
        if not cleaned_front or not cleaned_back:
            continue

        dedup_key = (int(note_id), cleaned_front, cleaned_back)
        if dedup_key in seen_note_pairs:
            continue
        seen_note_pairs.add(dedup_key)

        stable_id = str(card_id) if card_id is not None else (str(guid).strip() or str(note_id))
        line = _serialize_cleaned_card(
            cleaned_front=cleaned_front,
            cleaned_back=cleaned_back,
            card_id=stable_id,
            cleaned_hint=clean_field(raw_hint),
            cleaned_extra=clean_field(raw_extra),
        )
        if line is not None:
            card_lines.append(line)

    return detected_title, card_lines


_SEPARATOR_ALIASES = {
    "tab": "\t",
    "\\t": "\t",
    "\t": "\t",
    "comma": ",",
    ",": ",",
    "semicolon": ";",
    ";": ";",
    "pipe": "|",
    "|": "|",
    "space": " ",
}

_EXPORT_DIRECTIVE_PREFIXES = (
    "#separator:",
    "#html:",
    "#tags column:",
    "#columns:",
    "#deck:",
    "#notetype:",
    "#guid column:",
    "#notetype column:",
    "#deck column:",
)


def _is_preamble_comment(line: str, default_delim: str) -> bool:
    stripped = line.strip()
    if not stripped.startswith("#"):
        return False
    lower = stripped.lower()
    if lower.startswith(_EXPORT_DIRECTIVE_PREFIXES):
        return True
    return default_delim not in stripped


def _read_delimited(
    input_path: Path,
    front_spec: str | None,
    back_spec: str | None,
    hint_spec: str | None,
    extra_spec: str | None,
    id_spec: str | None = None,
    delimiter_override: str | None = None,
) -> tuple[str | None, list[str]]:
    suffix = input_path.suffix.lower()
    default_delim = "," if suffix == ".csv" else "\t"

    try:
        raw_text = input_path.read_text(encoding="utf-8-sig")
    except UnicodeDecodeError as exc:
        raise ConversionError(
            f"Input file '{input_path}' is not valid UTF-8 plain text: {exc}"
        ) from exc

    raw_lines = raw_text.splitlines(keepends=True)
    detected_delim: str | None = None
    detected_title: str | None = None
    columns_header_raw: str | None = None
    guid_col_1based: int | None = None
    metadata_prefix_cols: set[int] = set()

    data_start = 0
    while data_start < len(raw_lines):
        line = raw_lines[data_start]
        if not line.strip():
            data_start += 1
            continue
        effective_probe_delim = detected_delim or default_delim
        if not _is_preamble_comment(line, effective_probe_delim):
            break
        directive = line.strip()
        lower = directive.lower()
        if lower.startswith("#separator:"):
            sep_val = directive.split(":", 1)[1].strip().lower()
            detected_delim = _SEPARATOR_ALIASES.get(sep_val, sep_val[:1] or default_delim)
        elif lower.startswith("#deck:"):
            deck_val = directive.split(":", 1)[1].strip()
            if deck_val:
                detected_title = deck_val
        elif lower.startswith("#columns:"):
            columns_header_raw = directive.split(":", 1)[1]
        elif lower.startswith("#guid column:"):
            val = directive.split(":", 1)[1].strip()
            if _NONNEG_INT_RE.fullmatch(val) and int(val) >= 1:
                guid_col_1based = int(val)
                metadata_prefix_cols.add(guid_col_1based - 1)
        elif lower.startswith(("#notetype column:", "#deck column:")):
            val = directive.split(":", 1)[1].strip()
            if _NONNEG_INT_RE.fullmatch(val) and int(val) >= 1:
                metadata_prefix_cols.add(int(val) - 1)
        data_start += 1

    if delimiter_override is not None:
        delim = _SEPARATOR_ALIASES.get(delimiter_override.lower(), delimiter_override)
        if len(delim) != 1:
            raise ConversionError(f"Invalid delimiter '{delimiter_override}': must be 1 character.")
    else:
        delim = detected_delim or default_delim

    reader = csv.reader(raw_lines[data_start:], delimiter=delim)
    rows = [row for row in reader if any(cell.strip() for cell in row)]
    if not rows:
        return detected_title, []

    named_spec_used = any(
        spec is not None and not _NONNEG_INT_RE.fullmatch(spec)
        for spec in (front_spec, back_spec, hint_spec, extra_spec, id_spec)
    )

    header_names: list[str] = []
    if columns_header_raw is not None:
        header_names = [c.strip() for c in next(csv.reader([columns_header_raw], delimiter=delim))]
    elif named_spec_used:
        header_names = [c.strip() for c in rows[0]]
        rows = rows[1:]

    # Determine default front/back column indices (accounting for #guid/#notetype/#deck prefix columns).
    col_upper = max((len(r) for r in rows), default=len(header_names)) + len(metadata_prefix_cols) + 4
    content_cols = [idx for idx in range(col_upper) if idx not in metadata_prefix_cols]
    default_front_str = str(content_cols[0])
    default_back_str = str(content_cols[1])

    effective_front = front_spec if front_spec is not None else default_front_str
    effective_back = back_spec if back_spec is not None else default_back_str
    effective_id = (
        id_spec
        if id_spec is not None
        else (str(guid_col_1based - 1) if guid_col_1based is not None else None)
    )

    resolved: dict[str, int | None] = {}
    for flag_name, key, spec in (
        ("--front-field", "front", effective_front),
        ("--back-field", "back", effective_back),
        ("--hint-field", "hint", hint_spec),
        ("--extra-field", "extra", extra_spec),
        ("--id-field", "id", effective_id),
    ):
        idx, ok = _resolve_field_spec(spec, header_names)
        if not ok:
            avail_str = ", ".join(header_names) if header_names else "(none)"
            raise ConversionError(
                f"{flag_name} '{spec}' was not found in header columns. Available columns: {avail_str}"
            )
        resolved[key] = idx

    card_lines: list[str] = []
    for row in rows:
        raw_front = _get_field(row, resolved["front"])
        raw_back = _get_field(row, resolved["back"])
        raw_hint = _get_field(row, resolved["hint"])
        raw_extra = _get_field(row, resolved["extra"])
        raw_id = _get_field(row, resolved["id"]).strip() or None

        line = format_card_line(
            front=raw_front,
            back=raw_back,
            card_id=raw_id,
            hint=raw_hint,
            extra=raw_extra,
        )
        if line is not None:
            card_lines.append(line)

    return detected_title, card_lines


def convert_deck(
    input_path: Path,
    output_path: Path,
    *,
    title: str | None = None,
    new_per_day: int = DEFAULT_NEW_PER_DAY,
    max_reviews: int = DEFAULT_MAX_REVIEWS,
    front_field: str | None = None,
    back_field: str | None = None,
    hint_field: str | None = None,
    extra_field: str | None = None,
    id_field: str | None = None,
    delimiter: str | None = None,
) -> int:
    """Convert an .apkg, .tsv, or .csv file into a CrossPoint .jsonl file.

    Returns the number of card lines written (excluding the _meta line).
    """
    if not input_path.is_file():
        raise ConversionError(f"Input file not found: '{input_path}'")

    suffix = input_path.suffix.lower()
    if suffix == ".anki21b":
        raise ConversionError(ZSTD_ACTIONABLE_MESSAGE)

    with input_path.open("rb") as fh:
        magic_prefix = fh.read(16)

    if magic_prefix.startswith(ZSTD_MAGIC):
        raise ConversionError(ZSTD_ACTIONABLE_MESSAGE)

    if suffix in (".apkg", ".colpkg") or zipfile.is_zipfile(input_path):
        detected_title, card_lines = _read_apkg(
            input_path=input_path,
            front_spec=front_field,
            back_spec=back_field,
            hint_spec=hint_field,
            extra_spec=extra_field,
        )
    elif magic_prefix.startswith(SQLITE_MAGIC):
        conn = sqlite3.connect(str(input_path))
        try:
            detected_title, card_lines = _extract_sqlite_cards(
                conn=conn,
                front_spec=front_field,
                back_spec=back_field,
                hint_spec=hint_field,
                extra_spec=extra_field,
            )
        finally:
            conn.close()
    else:
        detected_title, card_lines = _read_delimited(
            input_path=input_path,
            front_spec=front_field,
            back_spec=back_field,
            hint_spec=hint_field,
            extra_spec=extra_field,
            id_spec=id_field,
            delimiter_override=delimiter,
        )

    effective_title = (
        title.strip()
        if (title is not None and title.strip())
        else (detected_title or input_path.stem)
    )
    meta_line = format_meta_line(
        title=effective_title,
        new_per_day=new_per_day,
        max_reviews=max_reviews,
    )

    output_path.parent.mkdir(parents=True, exist_ok=True)
    with output_path.open("w", encoding="utf-8", newline="\n") as out:
        out.write(meta_line)
        out.write("\n")
        for line in card_lines:
            out.write(line)
            out.write("\n")

    return len(card_lines)


def _non_negative_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer value: '{value}'") from exc
    if parsed < 0:
        raise argparse.ArgumentTypeError("value must be >= 0")
    return parsed


def _positive_int(value: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"invalid integer value: '{value}'") from exc
    if parsed < 1:
        raise argparse.ArgumentTypeError("value must be >= 1")
    return parsed


def _validate_field_arg(value: str) -> str:
    stripped = value.strip()
    if not stripped:
        raise argparse.ArgumentTypeError("field specifier must not be empty")
    if stripped.lstrip("-").isdigit() and int(stripped) < 0:
        raise argparse.ArgumentTypeError("field index must be >= 0")
    return stripped


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="deck2jsonl.py",
        description=(
            "Flashcard deck converter (.apkg, .tsv, .csv -> /flashcards/<deck>.jsonl)."
        ),
    )
    parser.add_argument(
        "input",
        type=Path,
        help="Input .apkg, .tsv, or .csv file path.",
    )
    parser.add_argument(
        "output",
        nargs="?",
        type=Path,
        help="Output .jsonl file path (defaults to <input-stem>.jsonl).",
    )
    parser.add_argument(
        "-o",
        "--output",
        dest="output_opt",
        type=Path,
        help="Output .jsonl file path (alternative to positional output).",
    )
    parser.add_argument(
        "--title",
        type=str,
        default=None,
        help="Deck display title for _meta (defaults to package deck name or input filename stem).",
    )
    parser.add_argument(
        "--new-per-day",
        type=_non_negative_int,
        default=DEFAULT_NEW_PER_DAY,
        help=f"New cards per session day (default: {DEFAULT_NEW_PER_DAY}, max: {MAX_NEW_PER_DAY}).",
    )
    parser.add_argument(
        "--max-reviews",
        type=_positive_int,
        default=DEFAULT_MAX_REVIEWS,
        help=f"Maximum due reviews per session day (default: {DEFAULT_MAX_REVIEWS}, max: {MAX_REVIEWS_CAP}).",
    )
    parser.add_argument(
        "--front-field",
        type=_validate_field_arg,
        default=None,
        help="Front field by 0-based index or model/CSV field name (default: 0).",
    )
    parser.add_argument(
        "--back-field",
        type=_validate_field_arg,
        default=None,
        help="Back field by 0-based index or model/CSV field name (default: 1).",
    )
    parser.add_argument(
        "--hint-field",
        type=_validate_field_arg,
        default=None,
        help="Optional hint field by 0-based index or model/CSV field name.",
    )
    parser.add_argument(
        "--extra-field",
        type=_validate_field_arg,
        default=None,
        help="Optional extra/notes field by 0-based index or model/CSV field name.",
    )
    parser.add_argument(
        "--id-field",
        type=_validate_field_arg,
        default=None,
        help="Optional stable card ID column for .tsv/.csv inputs (0-based index or column name).",
    )
    parser.add_argument(
        "--delimiter",
        type=str,
        default=None,
        help="Optional column delimiter override for plain-text inputs (e.g. ',', 'tab').",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    if args.output is not None and args.output_opt is not None:
        parser.error("specify output either positionally or via -o/--output, not both")

    out_path: Path = args.output_opt or args.output or args.input.with_suffix(".jsonl")

    try:
        card_count = convert_deck(
            input_path=args.input,
            output_path=out_path,
            title=args.title,
            new_per_day=args.new_per_day,
            max_reviews=args.max_reviews,
            front_field=args.front_field,
            back_field=args.back_field,
            hint_field=args.hint_field,
            extra_field=args.extra_field,
            id_field=args.id_field,
            delimiter=args.delimiter,
        )
    except ConversionError as err:
        print(f"Error: {err}", file=sys.stderr)
        return 1
    except OSError as err:
        print(f"I/O error: {err}", file=sys.stderr)
        return 1

    print(f"Wrote {card_count} cards to {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
