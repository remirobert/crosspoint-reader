#include "FlashcardSession.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace flashcards {

namespace {

constexpr const char* DEFAULT_FLASHCARDS_CACHE_DIR = "/.crosspoint/flashcards";
constexpr size_t INDEX_SCAN_BATCH_RECORDS = 128;  // 128 * 16 B = 2048 B temporary heap chunk
constexpr size_t AGAIN_REQUEUE_AHEAD = 6;

struct DeckScanStats {
  uint32_t dueAtOrBeforeTarget = 0;
  uint32_t unseenNewTotal = 0;
  uint16_t minDueAfterTarget = 0;
  bool hasDueAfterTarget = false;
};

bool containsCardHash(const std::vector<SessionSlot>& slots, uint64_t hash) noexcept {
  for (const SessionSlot& slot : slots) {
    if (slot.srs.cardIdHash == hash) {
      return true;
    }
  }
  return false;
}

bool scanDeckForDay(FlashcardDeckIndex& idx, const FlashcardSrsStore& store, uint32_t targetDay, DeckScanStats& out) {
  out = DeckScanStats{};
  const uint32_t cardCount = idx.header().cardCount;
  if (cardCount == 0) {
    return true;
  }

  const size_t batchCap = std::min<size_t>(INDEX_SCAN_BATCH_RECORDS, cardCount);
  auto batch = makeUniqueNoThrow<CardIndexRecord[]>(batchCap);
  if (!batch) {
    LOG_ERR("FCARD", "OOM allocating %u-byte index scan chunk",
            static_cast<unsigned>(batchCap * sizeof(CardIndexRecord)));
    return false;
  }

  for (uint32_t base = 0; base < cardCount; base += static_cast<uint32_t>(batchCap)) {
    const size_t count = std::min<size_t>(batchCap, cardCount - base);
    if (!idx.readIndexRecords(base, batch.get(), count)) {
      return false;
    }

    for (size_t i = 0; i < count; ++i) {
      const CardIndexRecord& rec = batch[i];
      const CardSrsRecord* found = store.findCard(rec.cardIdHash);
      if (!found || found->state == static_cast<uint8_t>(CardState::New)) {
        ++out.unseenNewTotal;
        continue;
      }

      if (static_cast<uint32_t>(found->dueDay) <= targetDay) {
        ++out.dueAtOrBeforeTarget;
      } else if (!out.hasDueAfterTarget || found->dueDay < out.minDueAfterTarget) {
        out.minDueAfterTarget = found->dueDay;
        out.hasDueAfterTarget = true;
      }
    }
  }

  return true;
}

struct InspectWorkspace {
  FlashcardDeckIndex idx;
  FlashcardSrsStore store;
  std::string idxPath;
  std::string srsPath;
  std::string walPath;
};

struct FreeReviewScanCounts {
  uint16_t todayStudied = 0;
  uint16_t anyStudied = 0;
};

bool scanFreeReviewCounts(FlashcardDeckIndex& idx, const FlashcardSrsStore& store, const uint16_t today16,
                          CardIndexRecord* batch, const size_t batchCap, FreeReviewScanCounts& out) {
  out = FreeReviewScanCounts{};
  const uint32_t cardCount = idx.header().cardCount;
  for (uint32_t base = 0; base < cardCount; base += static_cast<uint32_t>(batchCap)) {
    const size_t count = std::min<size_t>(batchCap, cardCount - base);
    if (!idx.readIndexRecords(base, batch, count)) {
      return false;
    }

    for (size_t i = 0; i < count; ++i) {
      const CardSrsRecord* found = store.findCard(batch[i].cardIdHash);
      if (!found || found->state == static_cast<uint8_t>(CardState::New)) {
        continue;
      }

      if (out.anyStudied < MAX_ACTIVE_QUEUE) {
        ++out.anyStudied;
      }
      if (found->lastReviewDay == today16 && out.todayStudied < MAX_ACTIVE_QUEUE) {
        ++out.todayStudied;
        if (out.todayStudied >= MAX_ACTIVE_QUEUE) {
          return true;
        }
      }
    }
  }
  return true;
}

}  // namespace

FlashcardSession::~FlashcardSession() { close(); }

bool FlashcardSession::inspectDeck(const char* jsonlPath, const char* cacheDir, DeckSummary& out) {
  out = DeckSummary{};
  if (!jsonlPath || jsonlPath[0] == '\0') {
    LOG_ERR("FCARD", "Cannot inspect deck: empty path");
    return false;
  }

  const char* effectiveCache = (cacheDir && cacheDir[0] != '\0') ? cacheDir : DEFAULT_FLASHCARDS_CACHE_DIR;
  if (!FlashcardDeckIndex::ensureDir(effectiveCache)) {
    return false;
  }

  auto ws = makeUniqueNoThrow<InspectWorkspace>();
  if (!ws) {
    LOG_ERR("FCARD", "OOM allocating %u-byte inspect workspace", static_cast<unsigned>(sizeof(InspectWorkspace)));
    return false;
  }

  FlashcardSrsStore::buildPaths(effectiveCache, jsonlPath, ws->idxPath, ws->srsPath, ws->walPath);
  if (!ws->idx.openOrBuild(jsonlPath, ws->idxPath.c_str())) {
    return false;
  }
  if (!ws->store.load(ws->srsPath.c_str(), ws->walPath.c_str())) {
    return false;
  }

  const DeckIndexHeader& idxHead = ws->idx.header();
  const SrsFileHeader& srsHead = ws->store.header();

  out.totalCards = idxHead.cardCount;
  std::memcpy(out.title, idxHead.title, sizeof(out.title));
  out.title[sizeof(out.title) - 1] = '\0';
  out.newPerDay = ws->store.effectiveNewPerDay(idxHead.newPerDay);
  out.sessionDay = srsHead.currentSessionDay;
  out.sessionCompleted = (srsHead.sessionCompleted != 0);

  if (out.sessionCompleted || out.totalCards == 0) {
    return true;
  }

  DeckScanStats stats{};
  if (!scanDeckForDay(ws->idx, ws->store, out.sessionDay, stats)) {
    return false;
  }

  const size_t maxRev = std::min<size_t>(idxHead.maxReviews, MAX_ACTIVE_QUEUE);
  const size_t remNew =
      (out.newPerDay > srsHead.newThisSession) ? static_cast<size_t>(out.newPerDay - srsHead.newThisSession) : 0u;
  const size_t dueSlots = std::min<size_t>(stats.dueAtOrBeforeTarget, maxRev);
  const size_t newSlots = std::min<size_t>(std::min<size_t>(stats.unseenNewTotal, remNew), MAX_ACTIVE_QUEUE - dueSlots);

  if (dueSlots == 0 && newSlots == 0) {
    out.sessionCompleted = true;
    return true;
  }

  out.dueCount = static_cast<uint16_t>(dueSlots);
  out.newCount = static_cast<uint16_t>(newSlots);
  return true;
}

bool FlashcardSession::rebuildQueue() {
  queue.clear();

  const DeckIndexHeader& idxHead = deckIndex.header();
  const SrsFileHeader& srsHead = srs.header();
  if (srsHead.sessionCompleted != 0 || idxHead.cardCount == 0) {
    return true;
  }

  const uint32_t today = srsHead.currentSessionDay;
  const size_t maxRev = std::min<size_t>(idxHead.maxReviews, MAX_ACTIVE_QUEUE);
  const uint16_t effNew = srs.effectiveNewPerDay(idxHead.newPerDay);
  const size_t remNew = (effNew > srsHead.newThisSession) ? static_cast<size_t>(effNew - srsHead.newThisSession) : 0u;

  const size_t reserveCap = std::min<size_t>(MAX_ACTIVE_QUEUE, std::min<size_t>(idxHead.cardCount, maxRev + remNew));
  if (reserveCap > queue.capacity()) {
    const size_t queueBytes = reserveCap * sizeof(SessionSlot);
    auto probe = makeUniqueNoThrow<uint8_t[]>(queueBytes);
    if (!probe) {
      LOG_ERR("FCARD", "OOM reserving %u session queue slots (%u B)", static_cast<unsigned>(reserveCap),
              static_cast<unsigned>(queueBytes));
      return false;
    }
    probe.reset();
    queue.reserve(reserveCap);
  }

  const size_t batchCap = std::min<size_t>(INDEX_SCAN_BATCH_RECORDS, idxHead.cardCount);
  auto batch = makeUniqueNoThrow<CardIndexRecord[]>(batchCap);
  if (!batch) {
    LOG_ERR("FCARD", "OOM allocating %u-byte queue build chunk",
            static_cast<unsigned>(batchCap * sizeof(CardIndexRecord)));
    return false;
  }

  uint32_t unseenNewCount = 0;

  // Pass 1: collect due/learning/relearning cards up to min(maxReviews, MAX_ACTIVE_QUEUE).
  for (uint32_t base = 0; base < idxHead.cardCount; base += static_cast<uint32_t>(batchCap)) {
    const size_t count = std::min<size_t>(batchCap, idxHead.cardCount - base);
    if (!deckIndex.readIndexRecords(base, batch.get(), count)) {
      return false;
    }

    for (size_t i = 0; i < count; ++i) {
      const uint32_t ordinal = base + static_cast<uint32_t>(i);
      const CardIndexRecord& rec = batch[i];
      const CardSrsRecord* found = srs.findCard(rec.cardIdHash);

      if (!found || found->state == static_cast<uint8_t>(CardState::New)) {
        ++unseenNewCount;
        continue;
      }

      if (static_cast<uint32_t>(found->dueDay) <= today) {
        if (queue.size() < maxRev && !containsCardHash(queue, rec.cardIdHash)) {
          queue.push_back(SessionSlot{ordinal, *found});
        }
      }
    }
  }

  // Pass 2: fill remaining session budget with unseen New cards while queue.size() < MAX_ACTIVE_QUEUE.
  if (remNew > 0 && unseenNewCount > 0 && queue.size() < MAX_ACTIVE_QUEUE) {
    const size_t newLimit = std::min<size_t>(remNew, MAX_ACTIVE_QUEUE - queue.size());
    size_t addedNew = 0;

    for (uint32_t base = 0; base < idxHead.cardCount && addedNew < newLimit; base += static_cast<uint32_t>(batchCap)) {
      const size_t count = std::min<size_t>(batchCap, idxHead.cardCount - base);
      if (!deckIndex.readIndexRecords(base, batch.get(), count)) {
        return false;
      }

      for (size_t i = 0; i < count && addedNew < newLimit; ++i) {
        const uint32_t ordinal = base + static_cast<uint32_t>(i);
        const CardIndexRecord& rec = batch[i];
        const CardSrsRecord* found = srs.findCard(rec.cardIdHash);
        if ((!found || found->state == static_cast<uint8_t>(CardState::New)) &&
            !containsCardHash(queue, rec.cardIdHash)) {
          CardSrsRecord fresh{};
          fresh.cardIdHash = rec.cardIdHash;
          fresh.state = static_cast<uint8_t>(CardState::New);
          queue.push_back(SessionSlot{ordinal, fresh});
          ++addedNew;
        }
      }
    }
  }

  return true;
}

bool FlashcardSession::open(const char* jsonlPath, const char* cacheDir) {
  close();
  if (!jsonlPath || jsonlPath[0] == '\0') {
    LOG_ERR("FCARD", "Cannot open session: empty deck path");
    return false;
  }

  const char* effectiveCache = (cacheDir && cacheDir[0] != '\0') ? cacheDir : DEFAULT_FLASHCARDS_CACHE_DIR;
  if (!FlashcardDeckIndex::ensureDir(effectiveCache)) {
    return false;
  }

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  FlashcardSrsStore::buildPaths(effectiveCache, jsonlPath, idxPath, srsPath, walPath);

  if (!deckIndex.openOrBuild(jsonlPath, idxPath.c_str())) {
    return false;
  }
  if (!srs.load(srsPath.c_str(), walPath.c_str(), MAX_NEW_PER_DAY)) {
    deckIndex.close();
    return false;
  }

  lineBuf = makeUniqueNoThrow<char[]>(MAX_JSONL_LINE_BYTES);
  if (!lineBuf) {
    LOG_ERR("FCARD", "OOM allocating %u-byte session line buffer", static_cast<unsigned>(MAX_JSONL_LINE_BYTES));
    deckIndex.close();
    srs.clear();
    return false;
  }

  freeReviewsCompleted = 0;
  freeReviewActive = false;
  lastCompletedFree = false;
  opened = true;

  if (!rebuildQueue()) {
    close();
    return false;
  }

  if (srs.header().sessionCompleted == 0 && queue.empty() && deckIndex.header().cardCount > 0) {
    if (!srs.markSessionComplete()) {
      close();
      return false;
    }
  }

  return true;
}

void FlashcardSession::close() {
  if (opened && srs.hasUncompactedWal()) {
    if (!srs.compact()) {
      LOG_ERR("FCARD", "Failed compacting SRS WAL during session close");
    }
  }
  deckIndex.close();
  srs.clear();
  queue.clear();
  shrinkToFit(queue);
  lineBuf.reset();
  freeReviewsCompleted = 0;
  freeReviewActive = false;
  lastCompletedFree = false;
  opened = false;
}

bool FlashcardSession::setNewPerDayOverride(uint16_t limit) {
  if (!opened) {
    LOG_ERR("FCARD", "Cannot set newPerDay override: session not open");
    return false;
  }
  if (!srs.setNewPerDayOverride(limit)) {
    return false;
  }
  if (srs.header().sessionCompleted == 0) {
    if (!rebuildQueue()) {
      return false;
    }
    if (queue.empty() && deckIndex.header().cardCount > 0) {
      return srs.markSessionComplete();
    }
  }
  return true;
}

uint32_t FlashcardSession::computeNextSessionDay() const {
  const uint32_t curDay = srs.header().currentSessionDay;
  return (curDay < std::numeric_limits<uint32_t>::max()) ? (curDay + 1u) : std::numeric_limits<uint32_t>::max();
}

void FlashcardSession::previewNextSession(uint16_t& outDue, uint16_t& outNew, uint16_t* outNextScheduledDay) const {
  outDue = 0;
  outNew = 0;
  if (outNextScheduledDay != nullptr) {
    *outNextScheduledDay = 0;
  }
  if (!opened || deckIndex.header().cardCount == 0) {
    return;
  }

  const uint32_t nextDay = computeNextSessionDay();
  DeckScanStats stats{};
  if (!scanDeckForDay(deckIndex, srs, nextDay, stats)) {
    return;
  }

  const size_t maxRev = std::min<size_t>(deckIndex.header().maxReviews, MAX_ACTIVE_QUEUE);
  const size_t dueSlots = std::min<size_t>(stats.dueAtOrBeforeTarget, maxRev);
  const size_t effNew = srs.effectiveNewPerDay(deckIndex.header().newPerDay);
  const size_t newSlots = std::min<size_t>(std::min<size_t>(stats.unseenNewTotal, effNew), MAX_ACTIVE_QUEUE - dueSlots);

  outDue = static_cast<uint16_t>(dueSlots);
  outNew = static_cast<uint16_t>(newSlots);
  if (outNextScheduledDay != nullptr) {
    *outNextScheduledDay = (outDue == 0 && outNew == 0 && stats.hasDueAfterTarget) ? stats.minDueAfterTarget : 0;
  }
}

uint16_t FlashcardSession::countFreeReviewCards() const {
  if (!opened || deckIndex.header().cardCount == 0 || srs.header().sessionCompleted == 0) {
    return 0;
  }

  const uint32_t cardCount = deckIndex.header().cardCount;
  const size_t batchCap = std::min<size_t>(INDEX_SCAN_BATCH_RECORDS, cardCount);
  auto batch = makeUniqueNoThrow<CardIndexRecord[]>(batchCap);
  if (!batch) {
    LOG_ERR("FCARD", "OOM allocating %u-byte free-review scan chunk",
            static_cast<unsigned>(batchCap * sizeof(CardIndexRecord)));
    return 0;
  }

  const uint16_t today16 = static_cast<uint16_t>(std::min<uint32_t>(srs.header().currentSessionDay, 65535u));
  FreeReviewScanCounts counts{};
  if (!scanFreeReviewCounts(deckIndex, srs, today16, batch.get(), batchCap, counts)) {
    return 0;
  }

  return counts.todayStudied > 0 ? counts.todayStudied : counts.anyStudied;
}

bool FlashcardSession::currentCard(FlashcardItem& outCard, CardSrsRecord& outSrs, IntervalPreview& outPreview) {
  if (!opened || queue.empty() || !lineBuf) {
    return false;
  }

  const SessionSlot& front = queue.front();
  if (!deckIndex.readCard(front.cardOrdinal, lineBuf.get(), MAX_JSONL_LINE_BYTES, outCard)) {
    return false;
  }

  outSrs = front.srs;
  if (freeReviewActive) {
    outPreview = IntervalPreview{0, 0, 0};
    return true;
  }

  const uint16_t today = static_cast<uint16_t>(std::min<uint32_t>(srs.header().currentSessionDay, 65535u));
  outPreview = FlashcardScheduler::previewIntervals(front.srs, today);
  return true;
}

bool FlashcardSession::submitRating(Rating rating) {
  if (!opened || freeReviewActive || queue.empty() || srs.header().sessionCompleted != 0) {
    LOG_ERR("FCARD", "Cannot submit rating: no active session card");
    return false;
  }

  const SessionSlot current = queue.front();
  const bool wasNewCard = (current.srs.state == static_cast<uint8_t>(CardState::New));
  const uint16_t today = static_cast<uint16_t>(std::min<uint32_t>(srs.header().currentSessionDay, 65535u));
  const CardSrsRecord updated = FlashcardScheduler::reviewCard(current.srs, rating, today);

  if (!srs.recordRating(updated, wasNewCard)) {
    return false;
  }

  queue.erase(queue.begin());

  if (rating == Rating::Again) {
    const size_t insertOffset = std::min<size_t>(queue.size(), AGAIN_REQUEUE_AHEAD);
    queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(insertOffset), SessionSlot{current.cardOrdinal, updated});
  }

  if (queue.empty()) {
    if (!srs.markSessionComplete()) {
      return false;
    }
  }

  return true;
}

bool FlashcardSession::startFreeReview() {
  if (!opened || deckIndex.header().cardCount == 0 || srs.header().sessionCompleted == 0) {
    LOG_ERR("FCARD", "Cannot start free review: session not completed");
    return false;
  }

  const uint32_t cardCount = deckIndex.header().cardCount;
  const size_t batchCap = std::min<size_t>(INDEX_SCAN_BATCH_RECORDS, cardCount);
  auto batch = makeUniqueNoThrow<CardIndexRecord[]>(batchCap);
  if (!batch) {
    LOG_ERR("FCARD", "OOM allocating %u-byte free-review build chunk",
            static_cast<unsigned>(batchCap * sizeof(CardIndexRecord)));
    return false;
  }

  const uint16_t today16 = static_cast<uint16_t>(std::min<uint32_t>(srs.header().currentSessionDay, 65535u));
  FreeReviewScanCounts counts{};
  if (!scanFreeReviewCounts(deckIndex, srs, today16, batch.get(), batchCap, counts)) {
    return false;
  }

  const bool onlyToday = (counts.todayStudied > 0);
  const size_t reserveCap = onlyToday ? counts.todayStudied : counts.anyStudied;
  if (reserveCap == 0) {
    return false;
  }

  queue.clear();
  if (reserveCap > queue.capacity()) {
    const size_t queueBytes = reserveCap * sizeof(SessionSlot);
    auto probe = makeUniqueNoThrow<uint8_t[]>(queueBytes);
    if (!probe) {
      LOG_ERR("FCARD", "OOM reserving %u free-review queue slots (%u B)", static_cast<unsigned>(reserveCap),
              static_cast<unsigned>(queueBytes));
      return false;
    }
    probe.reset();
    queue.reserve(reserveCap);
  }

  for (uint32_t base = 0; base < cardCount && queue.size() < reserveCap; base += static_cast<uint32_t>(batchCap)) {
    const size_t count = std::min<size_t>(batchCap, cardCount - base);
    if (!deckIndex.readIndexRecords(base, batch.get(), count)) {
      queue.clear();
      return false;
    }

    for (size_t i = 0; i < count && queue.size() < reserveCap; ++i) {
      const CardSrsRecord* found = srs.findCard(batch[i].cardIdHash);
      if (!found || found->state == static_cast<uint8_t>(CardState::New)) {
        continue;
      }
      if (onlyToday && found->lastReviewDay != today16) {
        continue;
      }
      queue.push_back(SessionSlot{base + static_cast<uint32_t>(i), *found});
    }
  }

  if (queue.empty()) {
    return false;
  }

  freeReviewActive = true;
  freeReviewsCompleted = 0;
  lastCompletedFree = false;
  return true;
}

bool FlashcardSession::submitFreeReview(const bool repeatAgain) {
  if (!opened || !freeReviewActive || queue.empty()) {
    LOG_ERR("FCARD", "Cannot submit free review: no active free-review card");
    return false;
  }

  const SessionSlot current = queue.front();
  queue.erase(queue.begin());
  ++freeReviewsCompleted;

  if (repeatAgain) {
    const size_t insertOffset = std::min<size_t>(queue.size(), AGAIN_REQUEUE_AHEAD);
    queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(insertOffset), current);
  }

  if (queue.empty()) {
    freeReviewActive = false;
    lastCompletedFree = true;
  }

  return true;
}

bool FlashcardSession::startNextSession() {
  if (!opened) {
    LOG_ERR("FCARD", "Cannot start next session: session not open");
    return false;
  }

  freeReviewsCompleted = 0;
  freeReviewActive = false;
  lastCompletedFree = false;

  const uint32_t nextDay = computeNextSessionDay();
  if (!srs.advanceSession(nextDay)) {
    return false;
  }
  if (!rebuildQueue()) {
    return false;
  }
  if (queue.empty() && deckIndex.header().cardCount > 0) {
    if (!srs.markSessionComplete()) {
      return false;
    }
  }
  return true;
}

}  // namespace flashcards
