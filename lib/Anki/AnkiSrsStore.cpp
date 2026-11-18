#include "AnkiSrsStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

#include "AnkiDeckIndex.h"

namespace anki {

namespace {

constexpr const char* DEFAULT_CACHE_DIR = "/.crosspoint/anki";

bool ensureFileParentDir(const std::string& filePath) {
  const size_t slash = filePath.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    return true;
  }
  const std::string parent = filePath.substr(0, slash);
  return AnkiDeckIndex::ensureDir(parent.c_str());
}

}  // namespace

void AnkiSrsStore::buildPaths(const char* cacheDir, std::string_view deckPath, std::string& idxPath,
                              std::string& srsOutPath, std::string& walOutPath) {
  std::string_view dir =
      (cacheDir && cacheDir[0] != '\0') ? std::string_view(cacheDir) : std::string_view(DEFAULT_CACHE_DIR);
  while (dir.size() > 1 && dir.back() == '/') {
    dir.remove_suffix(1);
  }

  char hex[17]{};
  formatDeckHashHex(deckPath, hex);

  const size_t baseLen = dir.size() + 1 + 16 + 4;
  idxPath.clear();
  srsOutPath.clear();
  walOutPath.clear();
  idxPath.reserve(baseLen);
  srsOutPath.reserve(baseLen);
  walOutPath.reserve(baseLen);

  const auto appendBase = [&](std::string& out, const char* ext) {
    out.append(dir.data(), dir.size());
    if (out.empty() || out.back() != '/') {
      out.push_back('/');
    }
    out.append(hex, 16);
    out.append(ext, 4);
  };

  appendBase(idxPath, ".idx");
  appendBase(srsOutPath, ".srs");
  appendBase(walOutPath, ".wal");
}

bool AnkiSrsStore::resetDeck(const char* cacheDir, std::string_view deckPath) {
  std::string idxPath;
  std::string srsFilePath;
  std::string walFilePath;
  buildPaths(cacheDir, deckPath, idxPath, srsFilePath, walFilePath);

  bool ok = true;
  if (Storage.exists(srsFilePath.c_str()) && !Storage.remove(srsFilePath.c_str())) {
    LOG_ERR("ANKI", "Failed to remove SRS file %s during deck reset", srsFilePath.c_str());
    ok = false;
  }
  if (Storage.exists(walFilePath.c_str()) && !Storage.remove(walFilePath.c_str())) {
    LOG_ERR("ANKI", "Failed to remove WAL file %s during deck reset", walFilePath.c_str());
    ok = false;
  }
  return ok;
}

void AnkiSrsStore::clear() {
  head = SrsFileHeader{};
  sortedRecords.clear();
  shrinkToFit(sortedRecords);
  srsPath.clear();
  walPath.clear();
  walSizeBytes = 0;
}

bool AnkiSrsStore::hasUncompactedWal() const {
  return walSizeBytes > 0 || (!walPath.empty() && Storage.exists(walPath.c_str()));
}

const CardSrsRecord* AnkiSrsStore::findCard(uint64_t cardIdHash) const {
  const auto it = std::lower_bound(sortedRecords.begin(), sortedRecords.end(), cardIdHash,
                                   [](const CardSrsRecord& rec, uint64_t target) { return rec.cardIdHash < target; });
  if (it != sortedRecords.end() && it->cardIdHash == cardIdHash) {
    return &(*it);
  }
  return nullptr;
}

bool AnkiSrsStore::reserveRecordsChecked(size_t targetCapacity) {
  const size_t clamped = std::min<size_t>(MAX_SRS_RECORDS, targetCapacity);
  if (clamped <= sortedRecords.capacity()) {
    return true;
  }
  const size_t bytes = clamped * sizeof(CardSrsRecord);
  auto probe = makeUniqueNoThrow<uint8_t[]>(bytes);
  if (!probe) {
    LOG_ERR("ANKI", "OOM reserving %u SRS records (%u B)", static_cast<unsigned>(clamped),
            static_cast<unsigned>(bytes));
    return false;
  }
  probe.reset();
  sortedRecords.reserve(clamped);
  return true;
}

bool AnkiSrsStore::ensureUpsertCapacity(uint64_t cardIdHash) {
  const auto it = std::lower_bound(sortedRecords.begin(), sortedRecords.end(), cardIdHash,
                                   [](const CardSrsRecord& rec, uint64_t target) { return rec.cardIdHash < target; });
  if (it != sortedRecords.end() && it->cardIdHash == cardIdHash) {
    return true;
  }
  if (sortedRecords.size() >= MAX_SRS_RECORDS) {
    LOG_ERR("ANKI", "SRS record limit (%u) reached", static_cast<unsigned>(MAX_SRS_RECORDS));
    return false;
  }
  if (sortedRecords.size() < sortedRecords.capacity()) {
    return true;
  }
  const size_t step = sortedRecords.empty() ? 32 : std::min<size_t>(sortedRecords.size(), 128);
  return reserveRecordsChecked(sortedRecords.size() + step);
}

bool AnkiSrsStore::upsertRecordInMemory(const CardSrsRecord& record) {
  if (!ensureUpsertCapacity(record.cardIdHash)) {
    return false;
  }
  auto it = std::lower_bound(sortedRecords.begin(), sortedRecords.end(), record.cardIdHash,
                             [](const CardSrsRecord& rec, uint64_t target) { return rec.cardIdHash < target; });
  if (it != sortedRecords.end() && it->cardIdHash == record.cardIdHash) {
    *it = record;
    return true;
  }
  sortedRecords.insert(it, record);
  return true;
}

bool AnkiSrsStore::applyWalEntry(const WalEntry& entry) {
  switch (static_cast<WalOp>(entry.op)) {
    case WalOp::CardRated:
      if (!upsertRecordInMemory(entry.card)) {
        return false;
      }
      head.recordCount = static_cast<uint32_t>(sortedRecords.size());
      if (head.reviewsThisSession < std::numeric_limits<uint32_t>::max()) {
        ++head.reviewsThisSession;
      }
      if (entry.wasNewCard != 0 && head.newThisSession < std::numeric_limits<uint16_t>::max()) {
        ++head.newThisSession;
      }
      return true;

    case WalOp::SessionComplete:
      head.sessionCompleted = 1;
      return true;

    case WalOp::SessionAdvance: {
      uint32_t nextDay = 1;
      if (entry.card.cardIdHash > 0 && entry.card.cardIdHash <= std::numeric_limits<uint32_t>::max()) {
        nextDay = static_cast<uint32_t>(entry.card.cardIdHash);
      } else if (entry.card.dueDay > 0) {
        nextDay = entry.card.dueDay;
      } else if (entry.sessionDay > 0) {
        nextDay = entry.sessionDay;
      }
      head.currentSessionDay = nextDay;
      head.sessionCompleted = 0;
      head.newThisSession = 0;
      head.reviewsThisSession = 0;
      return true;
    }
  }
  return false;
}

bool AnkiSrsStore::loadSrsSnapshot(const char* srsFilePath, size_t extraCapacityHint) {
  if (!Storage.exists(srsFilePath)) {
    if (extraCapacityHint > 0) {
      return reserveRecordsChecked(extraCapacityHint);
    }
    return true;
  }

  HalFile srsFile;
  if (!Storage.openFileForRead("ANKI", srsFilePath, srsFile)) {
    LOG_ERR("ANKI", "Failed to open SRS file %s", srsFilePath);
    return false;
  }

  SrsFileHeader diskHead{};
  if (!serialization::tryReadPod(srsFile, diskHead)) {
    LOG_ERR("ANKI", "Truncated SRS header in %s", srsFilePath);
    srsFile.close();
    return false;
  }

  const uint64_t expectedBytes =
      sizeof(SrsFileHeader) + static_cast<uint64_t>(diskHead.recordCount) * sizeof(CardSrsRecord);
  if (diskHead.magic != SRS_FILE_MAGIC || diskHead.version != SRS_FILE_VERSION ||
      diskHead.recordCount > MAX_SRS_RECORDS || expectedBytes != static_cast<uint64_t>(srsFile.size())) {
    LOG_ERR("ANKI", "Invalid SRS header or size mismatch in %s", srsFilePath);
    srsFile.close();
    return false;
  }

  const size_t reserveCount =
      std::min<size_t>(MAX_SRS_RECORDS, static_cast<size_t>(diskHead.recordCount) + extraCapacityHint);
  if (reserveCount > 0 && !reserveRecordsChecked(reserveCount)) {
    srsFile.close();
    return false;
  }

  for (uint32_t i = 0; i < diskHead.recordCount; ++i) {
    CardSrsRecord rec{};
    if (!serialization::tryReadPod(srsFile, rec)) {
      LOG_ERR("ANKI", "Short read in SRS records of %s at index %u", srsFilePath, static_cast<unsigned>(i));
      srsFile.close();
      return false;
    }
    sortedRecords.push_back(rec);
  }
  srsFile.close();

  if (!std::is_sorted(sortedRecords.begin(), sortedRecords.end(),
                      [](const CardSrsRecord& a, const CardSrsRecord& b) { return a.cardIdHash < b.cardIdHash; })) {
    std::sort(sortedRecords.begin(), sortedRecords.end(),
              [](const CardSrsRecord& a, const CardSrsRecord& b) { return a.cardIdHash < b.cardIdHash; });
  }

  diskHead.currentSessionDay = std::max<uint32_t>(1u, diskHead.currentSessionDay);
  diskHead.newPerDayOverride = std::min<uint16_t>(diskHead.newPerDayOverride, MAX_NEW_PER_DAY);
  diskHead.recordCount = static_cast<uint32_t>(sortedRecords.size());
  head = diskHead;
  return true;
}

bool AnkiSrsStore::replayWal(const char* walFilePath) {
  if (!Storage.exists(walFilePath)) {
    return true;
  }

  HalFile walFile;
  if (!Storage.openFileForRead("ANKI", walFilePath, walFile)) {
    LOG_ERR("ANKI", "Failed to open WAL file %s", walFilePath);
    return false;
  }

  const size_t totalWalBytes = walFile.size();
  size_t validBytes = 0;
  bool corruptTail = false;

  while (validBytes + sizeof(WalEntry) <= totalWalBytes) {
    WalEntry entry{};
    if (!serialization::tryReadPod(walFile, entry)) {
      corruptTail = true;
      break;
    }
    const uint32_t expectedCrc = crc32Anki(&entry, offsetof(WalEntry, crc32));
    const bool validOp = (entry.op == static_cast<uint8_t>(WalOp::CardRated) ||
                          entry.op == static_cast<uint8_t>(WalOp::SessionComplete) ||
                          entry.op == static_cast<uint8_t>(WalOp::SessionAdvance));
    if (entry.crc32 != expectedCrc || !validOp) {
      LOG_ERR("ANKI", "Corrupt WAL frame at offset %u in %s; recovering prefix", static_cast<unsigned>(validBytes),
              walFilePath);
      corruptTail = true;
      break;
    }
    if (!applyWalEntry(entry)) {
      walFile.close();
      return false;
    }
    validBytes += sizeof(WalEntry);
  }

  if (validBytes < totalWalBytes && !corruptTail) {
    LOG_ERR("ANKI", "Partial trailing WAL frame (%u trailing bytes) in %s; recovering prefix",
            static_cast<unsigned>(totalWalBytes - validBytes), walFilePath);
    corruptTail = true;
  }

  walFile.close();
  head.recordCount = static_cast<uint32_t>(sortedRecords.size());
  walSizeBytes = validBytes;

  if (corruptTail || walSizeBytes >= WAL_COMPACT_THRESHOLD_BYTES) {
    return compact();
  }
  return true;
}

bool AnkiSrsStore::load(const char* srsFilePath, const char* walFilePath, size_t extraCapacityHint) {
  if (!srsFilePath || srsFilePath[0] == '\0' || !walFilePath || walFilePath[0] == '\0') {
    LOG_ERR("ANKI", "Invalid SRS or WAL path");
    return false;
  }

  head = SrsFileHeader{};
  sortedRecords.clear();
  srsPath.assign(srsFilePath);
  walPath.assign(walFilePath);
  walSizeBytes = 0;

  return loadSrsSnapshot(srsFilePath, extraCapacityHint) && replayWal(walFilePath);
}

bool AnkiSrsStore::appendWalEntry(const WalEntry& entry) {
  if (walPath.empty()) {
    LOG_ERR("ANKI", "Cannot append WAL entry: store not loaded");
    return false;
  }
  if (!ensureFileParentDir(walPath)) {
    return false;
  }

  HalFile walFile = Storage.open(walPath.c_str(), O_WRONLY | O_CREAT | O_APPEND);
  if (!walFile) {
    LOG_ERR("ANKI", "Failed to open WAL %s for append", walPath.c_str());
    return false;
  }

  if (!serialization::tryWritePod(walFile, entry)) {
    LOG_ERR("ANKI", "Failed to write 32-byte WAL frame to %s", walPath.c_str());
    walFile.close();
    return false;
  }

  walFile.flush();
  const size_t updatedSize = walFile.size();
  if (!walFile.close()) {
    LOG_ERR("ANKI", "Failed closing WAL %s after append", walPath.c_str());
    return false;
  }

  walSizeBytes = updatedSize;
  return true;
}

bool AnkiSrsStore::recordRating(const CardSrsRecord& updated, bool wasNewCard) {
  CardSrsRecord cleanCard = updated;
  std::memset(cleanCard.reserved, 0, sizeof(cleanCard.reserved));

  if (!ensureUpsertCapacity(cleanCard.cardIdHash)) {
    return false;
  }

  WalEntry entry{};
  entry.card = cleanCard;
  entry.op = static_cast<uint8_t>(WalOp::CardRated);
  entry.wasNewCard = wasNewCard ? 1 : 0;
  entry.sessionDay = static_cast<uint16_t>(std::min<uint32_t>(head.currentSessionDay, 65535u));
  entry.crc32 = crc32Anki(&entry, offsetof(WalEntry, crc32));

  if (!appendWalEntry(entry)) {
    return false;
  }

  if (!upsertRecordInMemory(cleanCard)) {
    return false;
  }
  head.recordCount = static_cast<uint32_t>(sortedRecords.size());
  if (head.reviewsThisSession < std::numeric_limits<uint32_t>::max()) {
    ++head.reviewsThisSession;
  }
  if (wasNewCard && head.newThisSession < std::numeric_limits<uint16_t>::max()) {
    ++head.newThisSession;
  }

  if (walSizeBytes >= WAL_COMPACT_THRESHOLD_BYTES) {
    return compact();
  }
  return true;
}

bool AnkiSrsStore::markSessionComplete() {
  WalEntry entry{};
  entry.op = static_cast<uint8_t>(WalOp::SessionComplete);
  entry.sessionDay = static_cast<uint16_t>(std::min<uint32_t>(head.currentSessionDay, 65535u));
  entry.crc32 = crc32Anki(&entry, offsetof(WalEntry, crc32));

  if (!appendWalEntry(entry)) {
    return false;
  }

  head.sessionCompleted = 1;
  return compact();
}

bool AnkiSrsStore::advanceSession(uint32_t nextSessionDay) {
  const uint32_t targetDay = std::max<uint32_t>(1u, nextSessionDay);

  WalEntry entry{};
  entry.op = static_cast<uint8_t>(WalOp::SessionAdvance);
  entry.sessionDay = static_cast<uint16_t>(std::min<uint32_t>(targetDay, 65535u));
  entry.card.dueDay = entry.sessionDay;
  entry.card.cardIdHash = targetDay;
  entry.crc32 = crc32Anki(&entry, offsetof(WalEntry, crc32));

  if (!appendWalEntry(entry)) {
    return false;
  }

  head.currentSessionDay = targetDay;
  head.sessionCompleted = 0;
  head.newThisSession = 0;
  head.reviewsThisSession = 0;

  if (walSizeBytes >= WAL_COMPACT_THRESHOLD_BYTES) {
    return compact();
  }
  return true;
}

bool AnkiSrsStore::setNewPerDayOverride(uint16_t limit) {
  head.newPerDayOverride = std::min<uint16_t>(limit, MAX_NEW_PER_DAY);
  return compact();
}

bool AnkiSrsStore::compact() {
  if (srsPath.empty()) {
    LOG_ERR("ANKI", "Cannot compact SRS store: path not initialized");
    return false;
  }
  if (!ensureFileParentDir(srsPath)) {
    return false;
  }

  head.magic = SRS_FILE_MAGIC;
  head.version = SRS_FILE_VERSION;
  head.recordCount = static_cast<uint32_t>(sortedRecords.size());

  std::string tmpPath = srsPath;
  tmpPath.append(".tmp");
  if (Storage.exists(tmpPath.c_str())) {
    Storage.remove(tmpPath.c_str());
  }

  HalFile tmpFile;
  if (!Storage.openFileForWrite("ANKI", tmpPath, tmpFile)) {
    LOG_ERR("ANKI", "Failed to open %s for SRS compaction", tmpPath.c_str());
    return false;
  }

  const auto abortTmp = [&]() {
    if (tmpFile.isOpen()) {
      tmpFile.close();
    }
    Storage.remove(tmpPath.c_str());
    return false;
  };

  if (!serialization::tryWritePod(tmpFile, head)) {
    LOG_ERR("ANKI", "Failed writing SRS header to %s", tmpPath.c_str());
    return abortTmp();
  }

  if (!sortedRecords.empty()) {
    const size_t recordBytes = sortedRecords.size() * sizeof(CardSrsRecord);
    if (tmpFile.write(reinterpret_cast<const uint8_t*>(sortedRecords.data()), recordBytes) != recordBytes) {
      LOG_ERR("ANKI", "Failed writing %u bytes of SRS records to %s", static_cast<unsigned>(recordBytes),
              tmpPath.c_str());
      return abortTmp();
    }
  }

  tmpFile.flush();
  if (!tmpFile.close()) {
    LOG_ERR("ANKI", "Failed closing %s during SRS compaction", tmpPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (Storage.exists(srsPath.c_str()) && !Storage.remove(srsPath.c_str())) {
    LOG_ERR("ANKI", "Failed removing existing SRS file %s before rename", srsPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!Storage.rename(tmpPath.c_str(), srsPath.c_str())) {
    LOG_ERR("ANKI", "Failed renaming %s to %s", tmpPath.c_str(), srsPath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }

  if (!walPath.empty() && Storage.exists(walPath.c_str()) && !Storage.remove(walPath.c_str())) {
    LOG_ERR("ANKI", "Failed removing compacted WAL %s", walPath.c_str());
    return false;
  }

  walSizeBytes = 0;
  return true;
}

}  // namespace anki
