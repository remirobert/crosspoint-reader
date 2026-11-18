#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "AnkiTypes.h"

namespace anki {

inline constexpr uint32_t MAX_SRS_RECORDS = 4096;

class AnkiSrsStore {
 public:
  AnkiSrsStore() = default;

  static void buildPaths(const char* cacheDir, std::string_view deckPath, std::string& idxPath, std::string& srsPath,
                         std::string& walPath);
  static bool resetDeck(const char* cacheDir, std::string_view deckPath);

  bool load(const char* srsFilePath, const char* walFilePath, size_t extraCapacityHint = 0);
  void clear();

  const SrsFileHeader& header() const { return head; }
  uint16_t effectiveNewPerDay(uint16_t deckDefault) const {
    return head.newPerDayOverride > 0 ? head.newPerDayOverride : deckDefault;
  }

  const CardSrsRecord* findCard(uint64_t cardIdHash) const;
  const std::vector<CardSrsRecord>& records() const { return sortedRecords; }
  bool hasUncompactedWal() const;

  bool recordRating(const CardSrsRecord& updated, bool wasNewCard);
  bool markSessionComplete();
  bool advanceSession(uint32_t nextSessionDay);
  bool setNewPerDayOverride(uint16_t limit);
  bool compact();

 private:
  bool reserveRecordsChecked(size_t targetCapacity);
  bool ensureUpsertCapacity(uint64_t cardIdHash);
  bool loadSrsSnapshot(const char* srsFilePath, size_t extraCapacityHint);
  bool replayWal(const char* walFilePath);
  bool upsertRecordInMemory(const CardSrsRecord& record);
  bool applyWalEntry(const WalEntry& entry);
  bool appendWalEntry(const WalEntry& entry);

  SrsFileHeader head{};
  std::vector<CardSrsRecord> sortedRecords;
  std::string srsPath;
  std::string walPath;
  size_t walSizeBytes = 0;
};

}  // namespace anki
