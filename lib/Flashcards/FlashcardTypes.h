#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace flashcards {

inline constexpr uint32_t DECK_INDEX_MAGIC = 0x31444346u;
inline constexpr uint8_t DECK_INDEX_VERSION = 2;

inline constexpr size_t MAX_JSONL_LINE_BYTES = 4096;
inline constexpr size_t MAX_ACTIVE_QUEUE = 256;

struct DeckIndexHeader {
  uint32_t magic = DECK_INDEX_MAGIC;
  uint8_t version = DECK_INDEX_VERSION;
  uint8_t reserved[3]{};
  uint32_t deckFileSize = 0;
  uint32_t cardCount = 0;
  uint16_t reserved2[2]{};
  char title[64]{};
};
static_assert(sizeof(DeckIndexHeader) == 84);
static_assert(std::is_trivially_copyable_v<DeckIndexHeader>);

struct CardIndexRecord {
  uint64_t cardIdHash = 0;
  uint32_t byteOffset = 0;
  uint16_t byteLength = 0;
  uint16_t flags = 0;
};
static_assert(sizeof(CardIndexRecord) == 16);
static_assert(std::is_trivially_copyable_v<CardIndexRecord>);

struct FlashcardItem {
  std::string front;
  std::string back;
  std::string hint;
  std::string extra;
  uint64_t cardIdHash = 0;
};

struct SessionProgress {
  uint16_t cleared = 0;   // cards cleared via "Got It"
  uint16_t drilling = 0;  // cards currently in queue that hit "Again" >= 1 time
  uint16_t unseen = 0;    // cards not yet cleared and never hit "Again" in this pass
};

struct PassSummary {
  uint32_t totalCards = 0;     // total cards in deck
  uint32_t firstTryCount = 0;  // cards cleared on first attempt (never hit "Again")
  uint32_t retriedCount = 0;   // cards that needed "Again" >= 1 time before clearing
  uint32_t totalReviews = 0;   // total submissions (Again + Got It) in this pass
};

constexpr uint64_t fnv1a64(std::string_view data) noexcept {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (char c : data) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

inline void formatDeckHashHex(std::string_view deckPath, char outHex[17]) noexcept {
  constexpr char kHexDigits[] = "0123456789abcdef";
  const uint64_t hash = fnv1a64(deckPath);
  for (int i = 15; i >= 0; --i) {
    outHex[15 - i] = kHexDigits[(hash >> (i * 4)) & 0xFu];
  }
  outHex[16] = '\0';
}

}  // namespace flashcards
