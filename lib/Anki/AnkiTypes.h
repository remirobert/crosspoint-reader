#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

namespace anki {

inline constexpr uint32_t DECK_INDEX_MAGIC = 0x314B4E41u;
inline constexpr uint8_t DECK_INDEX_VERSION = 1;
inline constexpr uint32_t SRS_FILE_MAGIC = 0x31535253u;
inline constexpr uint8_t SRS_FILE_VERSION = 1;

inline constexpr size_t MAX_JSONL_LINE_BYTES = 4096;
inline constexpr uint16_t DEFAULT_NEW_PER_DAY = 20;
inline constexpr uint16_t MAX_NEW_PER_DAY = 200;
inline constexpr uint16_t DEFAULT_MAX_REVIEWS = 200;
inline constexpr uint16_t MAX_REVIEWS_CAP = 500;
inline constexpr size_t MAX_ACTIVE_QUEUE = 256;
inline constexpr size_t WAL_COMPACT_THRESHOLD_BYTES = 4096;

enum class CardState : uint8_t {
  New = 0,
  Learning = 1,
  Review = 2,
  Relearning = 3,
};

enum class Rating : uint8_t {
  Again = 1,
  Hard = 2,
  Good = 3,
  Easy = 4,
};

enum class WalOp : uint8_t {
  CardRated = 1,
  SessionComplete = 2,
  SessionAdvance = 3,
};

struct DeckIndexHeader {
  uint32_t magic = DECK_INDEX_MAGIC;
  uint8_t version = DECK_INDEX_VERSION;
  uint8_t reserved[3]{};
  uint32_t deckFileSize = 0;
  uint32_t cardCount = 0;
  uint16_t newPerDay = DEFAULT_NEW_PER_DAY;
  uint16_t maxReviews = DEFAULT_MAX_REVIEWS;
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

struct SrsFileHeader {
  uint32_t magic = SRS_FILE_MAGIC;
  uint8_t version = SRS_FILE_VERSION;
  uint8_t sessionCompleted = 0;
  uint16_t newThisSession = 0;
  uint32_t currentSessionDay = 1;
  uint32_t recordCount = 0;
  uint32_t reviewsThisSession = 0;
  uint16_t newPerDayOverride = 0;
  uint16_t reserved = 0;
};
static_assert(sizeof(SrsFileHeader) == 24);
static_assert(std::is_trivially_copyable_v<SrsFileHeader>);

struct CardSrsRecord {
  uint64_t cardIdHash = 0;
  float stability = 0.0f;
  uint16_t difficultyPermille = 0;
  uint16_t dueDay = 0;
  uint16_t lastReviewDay = 0;
  uint8_t state = static_cast<uint8_t>(CardState::New);
  uint8_t lapses = 0;
  uint8_t reserved[4]{};
};
static_assert(sizeof(CardSrsRecord) == 24);
static_assert(std::is_trivially_copyable_v<CardSrsRecord>);

struct WalEntry {
  CardSrsRecord card{};
  uint8_t op = 0;
  uint8_t wasNewCard = 0;
  uint16_t sessionDay = 0;
  uint32_t crc32 = 0;  // CRC32 over the preceding 28 bytes
};
static_assert(std::is_standard_layout_v<WalEntry>);
static_assert(offsetof(WalEntry, crc32) == 28);
static_assert(sizeof(WalEntry) == 32);
static_assert(std::is_trivially_copyable_v<WalEntry>);

struct AnkiCard {
  std::string front;
  std::string back;
  std::string hint;
  std::string extra;
  uint64_t cardIdHash = 0;
};

struct IntervalPreview {
  uint16_t hardDays = 1;
  uint16_t goodDays = 1;
  uint16_t easyDays = 2;
};

constexpr uint64_t fnv1a64(std::string_view data) noexcept {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (char c : data) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

inline uint32_t crc32Anki(const void* data, size_t len) noexcept {
  if (data == nullptr || len == 0) {
    return 0u;
  }
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= bytes[i];
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1u) != 0u ? 0xEDB88320u : 0u);
    }
  }
  return ~crc;
}

inline void formatDeckHashHex(std::string_view deckPath, char outHex[17]) noexcept {
  constexpr char kHexDigits[] = "0123456789abcdef";
  const uint64_t hash = fnv1a64(deckPath);
  for (int i = 15; i >= 0; --i) {
    outHex[15 - i] = kHexDigits[(hash >> (i * 4)) & 0xFu];
  }
  outHex[16] = '\0';
}

}  // namespace anki
