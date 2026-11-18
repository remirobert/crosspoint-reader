#pragma once

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>

#include "AnkiTypes.h"

namespace anki {

class AnkiDeckIndex {
 public:
  AnkiDeckIndex() = default;
  ~AnkiDeckIndex();
  AnkiDeckIndex(const AnkiDeckIndex&) = delete;
  AnkiDeckIndex& operator=(const AnkiDeckIndex&) = delete;

  static bool ensureDir(const char* dirPath);

  bool openOrBuild(const char* jsonlPath, const char* idxPath);
  void close();
  bool isOpen() const { return opened; }
  const DeckIndexHeader& header() const { return head; }

  bool readIndexRecord(uint32_t ordinal, CardIndexRecord& out);
  bool readIndexRecords(uint32_t startOrdinal, CardIndexRecord* outBuf, size_t count);
  bool readCard(uint32_t ordinal, char* lineScratch, size_t scratchSize, AnkiCard& outCard);

 private:
  bool tryOpenValidIndex(const char* idxPath, uint32_t actualDeckSize);
  bool buildIndex(const char* jsonlPath, const char* idxPath, uint32_t actualDeckSize);

  HalFile jsonlFile;
  HalFile idxFile;
  DeckIndexHeader head{};
  bool opened = false;
};

}  // namespace anki
