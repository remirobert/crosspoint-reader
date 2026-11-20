# Flashcards Subsystem — Technical Architecture (Simplified v1 · Stateless Deck Practice)

This document specifies the **Simplified v1 (Stateless Deck Practice, Zero SRS)** flashcards subsystem in this CrossPoint Reader fork.

The primary engineering goals are:
1. **First-class Home integration**: Dedicated Home menu entry and native e-ink deck picker without touching `openEpubPath`, `RECENT_BOOKS`, `CrossPointState`, or boot wake routing (`main.cpp`).
2. **Minimal upstream diff**: Isolate 95%+ of code in new directories (`lib/Flashcards/`, `src/activities/flashcards/`), add zero external dependencies, touch zero translation files, and keep upstream edits localized to Home menu routing and theme row-height clamping so merging future CrossPoint releases stays trivial.
3. **100% stateless & Xteink X4 (ESP32-C3) hardware fit**: Respect the ~380 KB SRAM / no-PSRAM budget, keep steady-state card flips allocation-free, write **zero** SRS/WAL/session state files during review (immune to sudden `0V` `GPIO13` MOSFET battery cutoffs), and cache only a tiny read-only byte-offset index (`.idx`) per deck.

---

## 1. Repository Layout & Upstream Touchpoints

### 1.1 Isolated Fork-Owned Files

```text
lib/Flashcards/
  FlashcardTypes.h              # POD .idx records, FlashcardItem, SessionProgress, PassSummary, FNV-1a 64-bit hash
  FlashcardDeckIndex.h / .cpp   # Streaming .jsonl parser & fixed-stride .idx builder/reader
  FlashcardSession.h / .cpp     # Stateless shuffled/sequential active-drill queue (<= 256 slots) & pass scoring
src/activities/flashcards/
  FlashcardsWidgets.h                   # Header-only 1-bit e-ink session ribbon, pass score capsule, & swatches
  FlashcardsDeckListActivity.h / .cpp   # Stateless outlined-card deck picker (/flashcards/*.jsonl) + shuffle toggle
  FlashcardsReviewActivity.h / .cpp     # Card front/back renderer, dual-tone ink ribbon, Again/Got It drill, & score summary
src/components/icons/
  flashcards.h                  # 32x32 1-bit monochrome icon bitmap in PROGMEM/flash
test/flashcards/
  CMakeLists.txt                # Host-native GoogleTest target (no hardware needed)
  FlashcardsTest.cpp            # Unit tests for .jsonl indexing, dedup, shuffle, >256 streaming, & OOM fail-soft
tools/
  deck2jsonl.py                 # Optional host Python stdlib converter (.apkg, .tsv, .csv -> .jsonl)
docs/
  flashcards-architecture.md    # This specification
```

### 1.2 Upstream Files Modified (Surgical Home & Theme Diffs Only)

To keep `git merge upstream/develop` nearly conflict-free:
- **Zero changes** to `platformio.ini` (reuses bundled `ArduinoJson 7.4.2` and `Serialization.h`).
- **Zero changes** to `lib/I18n/translations/*.yaml` (Flashcards UI strings live in `src/activities/flashcards/` as pure 7-bit ASCII `constexpr` literals).
- **Zero changes** to `src/CrossPointState.{h,cpp}`, `src/CrossPointSettings.{h,cpp}`, or `src/main.cpp`.

| Upstream Files | Exact Change | Purpose |
|---|---|---|
| `src/activities/ActivityManager.{h,cpp}` | Add `HomeMenuItem::FLASHCARDS`, `goToFlashcards()`, and map `"FlashcardsDeckList"` / `"FlashcardsReview"` -> `HomeMenuItem::FLASHCARDS` in `goHome()`. | Central navigation dispatch between Home and Flashcards. |
| `src/activities/home/HomeActivity.{h,cpp}` | Insert `HomeMenuItem::FLASHCARDS` into `menuItemToIndex()`, `indexToMenuItem()`, and `HomeActivity::render()` menu vectors; query `getHomeMenuRowHeight()` for touch hit-testing. | Dedicated **Flashcards** row on list-based Home themes. |
| `src/components/CoverGridHomeUi.{h,cpp}` | Grow `tabItems` array `5 -> 6` and draw `FlashcardsIcon` in `drawTabs()`. | Dedicated **Flashcards** tab on PSRAM cover-grid Home themes. |
| `src/components/themes/BaseTheme.{h,cpp}`, `lyra/LyraTheme.{h,cpp}`, & `roundedraff/RoundedRaffTheme.{h,cpp}` | Add `UIIcon::Flashcards` and clamp `getHomeMenuRowHeight()` on 6–7-row Home menus. | Prevents Home rows from overlapping bottom button hints (`y = 752` on X3, `y = 760` on X4) across Classic, Lyra, and RoundedRaff while keeping touch targets aligned. |

---

## 2. Deck Discovery & Input Format (`/flashcards/*.jsonl`)

### 2.1 Discovery Contract
- Decks live as flat `*.jsonl` files directly inside **`/flashcards`** at the SD card root (case-insensitive on FAT32/exFAT, matching CrossPoint's `/dictionaries`, `/plugins`, and `/fonts` convention).
- Entering `FlashcardsDeckListActivity` creates `/flashcards` (`Storage.mkdir("/flashcards")`) if absent so the directory is immediately visible over USB or Wi-Fi File Transfer.
- Hidden files (`name[0] == '.'`) and non-`.jsonl` files are skipped.
- Because `LibraryBuilder` and `FileBrowserActivity` ignore `.jsonl`, flashcard decks never pollute the EPUB library index (`CLX1`) or book browser.

### 2.2 JSON Lines (`.jsonl`) Schema
Each line (`\n` or `\r\n` terminated, UTF-8, max `4096` bytes per line; overlong or malformed lines are logged and skipped safely) is a standalone JSON object parsed one line at a time via `ArduinoJson`:

```jsonl
{"_meta":true,"title":"Japanese Core 2k"}
{"id":"n5-001","front":"食べる","back":"to eat","hint":"たべる","extra":"毎日野菜を食べる。"}
{"front":"飲む","back":"to drink"}
```

- **Optional metadata line**: Any object with `"_meta": true` sets deck metadata:
  - `"title"` (`const char*`): Display name in deck picker; defaults to filename stem (`japanese.jsonl` -> `"japanese"`).
  - Optional legacy keys (`"newPerDay"`, `"maxReviews"` in older or hand-written decks) are ignored gracefully.
- **Card lines**:
  - `"front"` (**required**, non-empty UTF-8 string): Prompt side.
  - `"back"` (**required**, non-empty UTF-8 string): Answer side.
  - `"id"` (optional string or integer): Stable card identifier (`fnv1a64(id)` when present, otherwise `fnv1a64(front)`). Duplicate card IDs within a `.jsonl` file are deduplicated during `.idx` generation (keeping the first occurrence).
  - `"hint"` (optional string): Secondary cue on the front (e.g., reading hint or part of speech).
  - `"extra"` (optional string): Example sentence or notes shown below `"back"` after flipping.
- **HTML & entity normalization**: Card fields containing basic HTML (`<br>`, `<div>`, `<p>`, `<b>`, named/numeric entities) are normalized to clean UTF-8 plain text on card load.

---

## 3. Read-Only Byte-Offset Index (`/.crosspoint/flashcards/<hash>.idx`)

User `.jsonl` files under `/flashcards/` are **strictly read-only**, and **zero** `.srs` or `.wal` files are ever created. The only file stored under `/.crosspoint/flashcards/` is `<16-hex-fnv1a(deckPath)>.idx` in little-endian POD format (`lib/Serialization/Serialization.h`), built atomically via `.idx.tmp` + rename when missing, version-mismatched, or `deckFileSize != jsonlFile.size()`:

```cpp
struct DeckIndexHeader {
  uint32_t magic;          // 0x31444346 ('FCD1')
  uint8_t  version;        // 2
  uint8_t  reserved[3];
  uint32_t deckFileSize;   // Exact byte size of /flashcards/<deck>.jsonl
  uint32_t cardCount;      // Number of valid unique card records following header
  uint16_t reserved2[2];   // Reserved (0)
  char     title[64];      // Null-terminated UTF-8 display title
}; // 84 bytes

struct CardIndexRecord {
  uint64_t cardIdHash;     // FNV-1a 64-bit card identity
  uint32_t byteOffset;     // Exact byte offset of line in .jsonl
  uint16_t byteLength;     // Exact byte length of line in .jsonl (< 4096)
  uint16_t flags;          // Reserved (0)
}; // 16 bytes
```

- **Instant deck inspection (`O(1)`)**: Once `<hash>.idx` exists, `FlashcardSession::inspectDeck()` validates the `84 B` `DeckIndexHeader` against `jsonlFile.size()` and returns `{totalCards, title}` without scanning card lines.
- **Random card access (`O(1)`)**: Card ordinal `k` (`0 <= k < cardCount`) sits at `sizeof(DeckIndexHeader) + k * sizeof(CardIndexRecord)`, seeking directly into the `.jsonl` line with zero resident per-card index array in RAM.

---

## 4. Stateless Active-Drill Engine (`FlashcardSession`)

### 4.1 Shuffled & Sequential Full-Deck Streaming
Opening a deck (`FlashcardSession::open(jsonlPath, shuffle, seed, cacheDir)`) or restarting a pass (`restartPass(shuffle, seed)`) initiates a stateless full-deck active-drill pass:
- **Empty decks (`cardCount == 0`)**: Open cleanly with `isPassComplete() == true` and `totalDeckCards() == 0`.
- **Fisher–Yates shuffle (`shuffle == true`)**:
  - When `seed == 0`, seeds `xorshift32` from hardware `esp_random()` on ESP32 (or monotonic pass counter + title hash + clock entropy on host), guaranteeing non-zero state (`0x9E3779B9u` fallback).
  - For decks up to `4,096` cards, allocates a compact `uint16_t[]` permutation buffer (`<= 8 KB` max; e.g., `24 B` for 12 cards, freed immediately once all cards enter the active queue) and shuffles `0 .. cardCount - 1` uniformly via Fisher–Yates.
  - Fail-soft fallback: if `cardCount > 4,096` or `orderBuf` cannot be allocated under low heap, shuffles the active `256`-slot window in-place without aborting.
- **Bounded `256`-slot sliding window (`MAX_ACTIVE_QUEUE = 256`)**:
  - Reserves `min(cardCount, 256)` `SessionSlot` entries (`8 B` each -> `<= 2,048 B` total) upfront after a `makeUniqueNoThrow` contiguous heap probe.
  - Seeds the initial `min(cardCount, 256)` cards and tracks `nextStreamCursor` for decks larger than `256` cards.

### 4.2 Active-Drill Choices & Pass Scoring
Every revealed card presents two choices via `submitChoice(bool repeatAgain)`:
- **`Again` (`repeatAgain == true`)**:
  - Marks `slot.drilling = true` and re-inserts the card `min(queue.size(), 6)` positions ahead in RAM so missed cards are drilled again soon until mastered.
- **`Got It` (`repeatAgain == false`)**:
  - Clears the card from the pass: increments `PassSummary::retriedCount` if `slot.drilling` was set, otherwise increments `PassSummary::firstTryCount`.
  - If `nextStreamCursor < cardCount`, streams the next card ordinal into the back of `queue`, keeping `queue.size() <= 256` with **zero mid-pass vector reallocations**.
- **In-RAM Progress & Pass Summary (`0` SD I/O)**:
  - `sessionProgress()` returns `{cleared, drilling, unseen}` (`cleared + drilling + unseen == min(cardCount, 65535)`) for the 6 px Dual-Tone Ink Ribbon.
  - `passSummary()` returns `{totalCards, firstTryCount, retriedCount, totalReviews}` (`firstTryCount + retriedCount == totalCards` at completion) for the Deck Complete score screen.

---

## 5. RAM, Stack, & Rendering Discipline (ESP32-C3 Budget)

Following `.agents/rules/hardware-resources.md`:

| Allocation | Worst-Case Size | Lifetime | Notes |
|---|---|---|---|
| `FlashcardSession::lineBuf` (`std::unique_ptr<char[]>`) | `4.0 KB` (`4096 B`) | `FlashcardsReviewActivity` (`onEnter` -> `onExit`) | Reused to read and parse the active card's single `.jsonl` line in-place. |
| `FlashcardSession::queue` (`std::vector<SessionSlot>`) | `<= 2.0 KB` (`256 * 8 B`) | `FlashcardsReviewActivity` (`onEnter` -> `onExit`) | Probed via `makeUniqueNoThrow` and `.reserve()`d once; `0 B` mid-pass growth. |
| `FlashcardSession::orderBuf` (`std::unique_ptr<uint16_t[]>`) | `<= 8.0 KB` (`4096 * 2 B`) | Only while `nextStreamCursor < cardCount` | Released immediately in `restartPass()` for decks `<= 256` cards, or once the last card streams into `queue`. |
| Cold `.idx` build buffers (`IndexBuildWorkspace`, `readBuf`, `lineBuf`) | `~8.2 KB` (`+32 KB` dedup table only if `> 256` cards) | Function-scoped during `buildIndex()` | Freed before `open()` / `inspectDeck()` returns; zero coexistence with review buffers. |
| Steady-state per-card heap allocations | **`0 B`** | — | Bounded member buffers and `snprintf` formatting reused across flips and choices. |
| Local stack frames | **`< 256 B`** per function | — | All file/JSON workspaces live on heap via `makeUniqueNoThrow`. |

---

## 6. UX & Interaction Specification

1. **Deck Picker (`FlashcardsDeckListActivity`)**:
   - **Header**: `"Flashcards"` on the left; `"<N> deck(s) | Shuffle"` (or `"In Order"`) on the right.
   - **Single-Band Outlined Cards**: Clean outlined card rows (`2 px` rounded outline frame on white paper when selected) showing the bold NFC-composed deck title on the left and `"<M> card(s)"` on the right, with a `3 px` scrollbar when scrolling.
   - **Hold `Confirm` / Long-Press `OptionPopup`**:
     1. `Card Order: Shuffled` / `Card Order: Original` — toggles RAM-only `gShuffleCardsEnabled` (defaults to Shuffled on boot).
     2. `Rebuild Deck Index` — removes `<hash>.idx` via `FlashcardDeckIndex::removeIndex()` and re-indexes the `.jsonl` file.

2. **Card Front (`FlashcardsReviewActivity`)**:
   - **Header & 6 px Dual-Tone Ink Ribbon**: Deck title on the left, `"<N> left"` + battery on the right, and `drawSessionRibbon()` directly below (`Solid` black = cleared, `Dither50` checkerboard = drilling, `Paper` white = unseen).
   - **Anchored prompt**: Centered bold `front` prompt (auto-scaling down if needed), optional `hint`, and full-width double-border **`"Show answer"`** pill.

3. **Card Back (`FlashcardsReviewActivity`)**:
   - **Zero-jump reveal**: Keeps `front` anchored at the exact same Y coordinate for clean `HalDisplay::FAST_REFRESH`, followed by a horizontal divider and paginated `back` + `extra` notes (`"Note p/P"` via `PageBack` / `PageForward` or vertical touch swipe).
   - **2 Wide Action Pills**: **`"Again"`** (`"Repeat soon"`, left) and **`"Got It"`** (`"Clear card"`, right, **pre-focused on flip** for one-thumb `Confirm -> Confirm` flow).

4. **Deck Complete (`FlashcardsReviewActivity`)**:
   - **Header & Solid Ribbon**: `"Done"` subtitle + 100% solid black 6 px ink ribbon.
   - **Pass Score Summary**: Checkmark badge + `"Deck Complete"`, `"<firstTry> / <total> first-try (<pct>%)"` subtitle, **12 px Dual-Tone Score Capsule** (`drawPassScoreBar`: `Solid` first-try vs `Dither50` retried), 2-swatch legend (`First-try <N>` / `Retried <N>`), total reviews line, and a single focused **`"Practice Deck"`** pill that calls `restartPass(shuffleCards)` for another pass.

### Physical Button & Touch Mapping

| Screen / State | `Back` (`btn1`) | `Confirm` (`btn2`) | `Left` (`btn3`) | `Right` (`btn4`) | Side `Up` / `Down` | Touch Boards |
|---|---|---|---|---|---|---|
| **Deck Picker** | Go Home (`FLASHCARDS` selected) | Open deck (Hold: `OptionPopup`) | Previous deck | Next deck | Previous / Next deck | Tap card to open; long-press for `OptionPopup` |
| **Card Front** | Return to Deck Picker | **Show answer** (`focus = Got It`) | — | — | `PageForward` reveals answer | Tap card or `"Show answer"` pill; header `<` exits |
| **Card Back** | Return to Deck Picker | **Submit focused choice** (`Again` / `Got It`) | Move focus left | Move focus right | `PageBack` / `PageForward` scrolls multi-page notes | Tap `"Again"` or `"Got It"` pill; swipe notes |
| **Deck Complete** | Return to Deck Picker | **Practice Deck** (fresh pass) | — | — | — | Tap `"Practice Deck"` pill; header `<` exits |
