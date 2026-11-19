#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "FlashcardTypes.h"

namespace flashcards {

class FlashcardDeckIndex {
 public:
  FlashcardDeckIndex() = default;
  ~FlashcardDeckIndex();
  FlashcardDeckIndex(const FlashcardDeckIndex&) = delete;
  FlashcardDeckIndex& operator=(const FlashcardDeckIndex&) = delete;

  static bool ensureDir(const char* dirPath);

  bool openOrBuild(const char* jsonlPath, const char* idxPath);
  void close();
  bool isOpen() const { return opened; }
  const DeckIndexHeader& header() const { return head; }

  bool readIndexRecord(uint32_t ordinal, CardIndexRecord& out);
  bool readIndexRecords(uint32_t startOrdinal, CardIndexRecord* outBuf, size_t count);
  bool readCard(uint32_t ordinal, char* lineScratch, size_t scratchSize, FlashcardItem& outCard);

 private:
  bool tryOpenValidIndex(const char* idxPath, uint32_t actualDeckSize);
  bool buildIndex(const char* jsonlPath, const char* idxPath, uint32_t actualDeckSize);

  HalFile jsonlFile;
  HalFile idxFile;
  DeckIndexHeader head{};
  bool opened = false;
};

}  // namespace flashcards
