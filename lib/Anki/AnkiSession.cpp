#include "AnkiSession.h"

#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace anki {

namespace {

constexpr const char* DEFAULT_ANKI_CACHE_DIR = "/.crosspoint/anki";
constexpr size_t INDEX_SCAN_BATCH_RECORDS = 128;  // 128 * 16 B = 2048 B temporary heap chunk
constexpr size_t AGAIN_REQUEUE_AHEAD = 6;

struct DeckScanStats {
  uint32_t dueAtOrBeforeTarget = 0;
  uint32_t unseenNewTotal = 0;
  uint16_t minDueAfterTarget = 0;
  uint16_t minReviewedDue = 0;
  bool hasDueAfterTarget = false;
  bool hasReviewedCards = false;
};

bool containsCardHash(const std::vector<SessionSlot>& slots, uint64_t hash) noexcept {
  for (const SessionSlot& slot : slots) {
    if (slot.srs.cardIdHash == hash) {
      return true;
    }
  }
  return false;
}

bool scanDeckForDay(AnkiDeckIndex& idx, const AnkiSrsStore& store, uint32_t targetDay, DeckScanStats& out) {
  out = DeckScanStats{};
  const uint32_t cardCount = idx.header().cardCount;
  if (cardCount == 0) {
    return true;
  }

  const size_t batchCap = std::min<size_t>(INDEX_SCAN_BATCH_RECORDS, cardCount);
  auto batch = makeUniqueNoThrow<CardIndexRecord[]>(batchCap);
  if (!batch) {
    LOG_ERR("ANKI", "OOM allocating %u-byte index scan chunk",
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

      if (!out.hasReviewedCards || found->dueDay < out.minReviewedDue) {
        out.minReviewedDue = found->dueDay;
        out.hasReviewedCards = true;
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
  AnkiDeckIndex idx;
  AnkiSrsStore store;
  std::string idxPath;
  std::string srsPath;
  std::string walPath;
};

}  // namespace

AnkiSession::~AnkiSession() { close(); }

bool AnkiSession::inspectDeck(const char* jsonlPath, const char* cacheDir, DeckSummary& out) {
  out = DeckSummary{};
  if (!jsonlPath || jsonlPath[0] == '\0') {
    LOG_ERR("ANKI", "Cannot inspect deck: empty path");
    return false;
  }

  const char* effectiveCache = (cacheDir && cacheDir[0] != '\0') ? cacheDir : DEFAULT_ANKI_CACHE_DIR;
  if (!AnkiDeckIndex::ensureDir(effectiveCache)) {
    return false;
  }

  auto ws = makeUniqueNoThrow<InspectWorkspace>();
  if (!ws) {
    LOG_ERR("ANKI", "OOM allocating %u-byte inspect workspace", static_cast<unsigned>(sizeof(InspectWorkspace)));
    return false;
  }

  AnkiSrsStore::buildPaths(effectiveCache, jsonlPath, ws->idxPath, ws->srsPath, ws->walPath);
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
  size_t dueSlots = std::min<size_t>(stats.dueAtOrBeforeTarget, maxRev);
  size_t newSlots = std::min<size_t>(std::min<size_t>(stats.unseenNewTotal, remNew), MAX_ACTIVE_QUEUE - dueSlots);

  if (dueSlots == 0 && newSlots == 0) {
    if (srsHead.reviewsThisSession > 0) {
      out.sessionCompleted = true;
      return true;
    }
    if (stats.hasDueAfterTarget && stats.minDueAfterTarget > out.sessionDay) {
      out.sessionDay = stats.minDueAfterTarget;
      if (!scanDeckForDay(ws->idx, ws->store, out.sessionDay, stats)) {
        return false;
      }
      dueSlots = std::min<size_t>(stats.dueAtOrBeforeTarget, maxRev);
      newSlots = std::min<size_t>(std::min<size_t>(stats.unseenNewTotal, out.newPerDay), MAX_ACTIVE_QUEUE - dueSlots);
    }
  }

  out.dueCount = static_cast<uint16_t>(dueSlots);
  out.newCount = static_cast<uint16_t>(newSlots);
  return true;
}

bool AnkiSession::rebuildQueue(uint32_t* outUnseenNew, uint16_t* outMinFutureDue, bool* outHasFutureDue) {
  queue.clear();
  if (outUnseenNew) *outUnseenNew = 0;
  if (outMinFutureDue) *outMinFutureDue = 0;
  if (outHasFutureDue) *outHasFutureDue = false;

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
      LOG_ERR("ANKI", "OOM reserving %u session queue slots (%u B)", static_cast<unsigned>(reserveCap),
              static_cast<unsigned>(queueBytes));
      return false;
    }
    probe.reset();
    queue.reserve(reserveCap);
  }

  const size_t batchCap = std::min<size_t>(INDEX_SCAN_BATCH_RECORDS, idxHead.cardCount);
  auto batch = makeUniqueNoThrow<CardIndexRecord[]>(batchCap);
  if (!batch) {
    LOG_ERR("ANKI", "OOM allocating %u-byte queue build chunk",
            static_cast<unsigned>(batchCap * sizeof(CardIndexRecord)));
    return false;
  }

  uint32_t unseenNewCount = 0;
  uint16_t minFutureDue = 0;
  bool hasFutureDue = false;

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
      } else if (!hasFutureDue || found->dueDay < minFutureDue) {
        minFutureDue = found->dueDay;
        hasFutureDue = true;
      }
    }
  }

  // Pass 2: fill remaining session budget with unseen New cards while queue.size() < MAX_ACTIVE_QUEUE.
  if (remNew > 0 && unseenNewCount > 0 && queue.size() < MAX_ACTIVE_QUEUE) {
    const size_t newLimit = std::min<size_t>(remNew, MAX_ACTIVE_QUEUE - queue.size());
    size_t addedNew = 0;

    for (uint32_t base = 0; base < idxHead.cardCount && addedNew < newLimit;
         base += static_cast<uint32_t>(batchCap)) {
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

  if (outUnseenNew) *outUnseenNew = unseenNewCount;
  if (outMinFutureDue) *outMinFutureDue = minFutureDue;
  if (outHasFutureDue) *outHasFutureDue = hasFutureDue;
  return true;
}

bool AnkiSession::open(const char* jsonlPath, const char* cacheDir) {
  close();
  if (!jsonlPath || jsonlPath[0] == '\0') {
    LOG_ERR("ANKI", "Cannot open session: empty deck path");
    return false;
  }

  const char* effectiveCache = (cacheDir && cacheDir[0] != '\0') ? cacheDir : DEFAULT_ANKI_CACHE_DIR;
  if (!AnkiDeckIndex::ensureDir(effectiveCache)) {
    return false;
  }

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  AnkiSrsStore::buildPaths(effectiveCache, jsonlPath, idxPath, srsPath, walPath);

  if (!deckIndex.openOrBuild(jsonlPath, idxPath.c_str())) {
    return false;
  }
  if (!srs.load(srsPath.c_str(), walPath.c_str(), MAX_NEW_PER_DAY)) {
    deckIndex.close();
    return false;
  }

  lineBuf = makeUniqueNoThrow<char[]>(MAX_JSONL_LINE_BYTES);
  if (!lineBuf) {
    LOG_ERR("ANKI", "OOM allocating %u-byte session line buffer", static_cast<unsigned>(MAX_JSONL_LINE_BYTES));
    deckIndex.close();
    srs.clear();
    return false;
  }

  opened = true;

  uint32_t unseenNew = 0;
  uint16_t minFutureDue = 0;
  bool hasFutureDue = false;
  if (!rebuildQueue(&unseenNew, &minFutureDue, &hasFutureDue)) {
    close();
    return false;
  }

  if (srs.header().sessionCompleted == 0 && queue.empty() && deckIndex.header().cardCount > 0) {
    if (srs.header().reviewsThisSession > 0) {
      if (!srs.markSessionComplete()) {
        close();
        return false;
      }
    } else if (hasFutureDue && minFutureDue > srs.header().currentSessionDay) {
      if (!srs.advanceSession(minFutureDue) || !rebuildQueue()) {
        close();
        return false;
      }
    }
  }

  return true;
}

void AnkiSession::close() {
  if (opened && srs.hasUncompactedWal()) {
    if (!srs.compact()) {
      LOG_ERR("ANKI", "Failed compacting SRS WAL during session close");
    }
  }
  deckIndex.close();
  srs.clear();
  queue.clear();
  shrinkToFit(queue);
  lineBuf.reset();
  opened = false;
}

bool AnkiSession::setNewPerDayOverride(uint16_t limit) {
  if (!opened) {
    LOG_ERR("ANKI", "Cannot set newPerDay override: session not open");
    return false;
  }
  if (!srs.setNewPerDayOverride(limit)) {
    return false;
  }
  if (srs.header().sessionCompleted == 0) {
    return rebuildQueue();
  }
  return true;
}

uint32_t AnkiSession::computeNextSessionDay() const {
  const uint32_t curDay = srs.header().currentSessionDay;
  const uint32_t nextSequential =
      (curDay < std::numeric_limits<uint32_t>::max()) ? (curDay + 1u) : std::numeric_limits<uint32_t>::max();
  if (!opened || deckIndex.header().cardCount == 0) {
    return nextSequential;
  }

  DeckScanStats stats{};
  if (!scanDeckForDay(deckIndex, srs, nextSequential, stats)) {
    return nextSequential;
  }

  const uint16_t effNew = srs.effectiveNewPerDay(deckIndex.header().newPerDay);
  if ((stats.unseenNewTotal > 0 && effNew > 0) || stats.dueAtOrBeforeTarget > 0) {
    return nextSequential;
  }

  if (stats.hasReviewedCards && stats.minReviewedDue > curDay) {
    return stats.minReviewedDue;
  }

  return nextSequential;
}

void AnkiSession::previewNextSession(uint16_t& outDue, uint16_t& outNew) const {
  outDue = 0;
  outNew = 0;
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
  const size_t newSlots =
      std::min<size_t>(std::min<size_t>(stats.unseenNewTotal, effNew), MAX_ACTIVE_QUEUE - dueSlots);

  outDue = static_cast<uint16_t>(dueSlots);
  outNew = static_cast<uint16_t>(newSlots);
}

bool AnkiSession::currentCard(AnkiCard& outCard, CardSrsRecord& outSrs, IntervalPreview& outPreview) {
  if (!opened || queue.empty() || !lineBuf) {
    return false;
  }

  const SessionSlot& front = queue.front();
  if (!deckIndex.readCard(front.cardOrdinal, lineBuf.get(), MAX_JSONL_LINE_BYTES, outCard)) {
    return false;
  }

  outSrs = front.srs;
  const uint16_t today = static_cast<uint16_t>(std::min<uint32_t>(srs.header().currentSessionDay, 65535u));
  outPreview = AnkiScheduler::previewIntervals(front.srs, today);
  return true;
}

bool AnkiSession::submitRating(Rating rating) {
  if (!opened || queue.empty() || srs.header().sessionCompleted != 0) {
    LOG_ERR("ANKI", "Cannot submit rating: no active session card");
    return false;
  }

  const SessionSlot current = queue.front();
  const bool wasNewCard = (current.srs.state == static_cast<uint8_t>(CardState::New));
  const uint16_t today = static_cast<uint16_t>(std::min<uint32_t>(srs.header().currentSessionDay, 65535u));
  const CardSrsRecord updated = AnkiScheduler::reviewCard(current.srs, rating, today);

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

bool AnkiSession::startNextSession() {
  if (!opened) {
    LOG_ERR("ANKI", "Cannot start next session: session not open");
    return false;
  }

  const uint32_t nextDay = computeNextSessionDay();
  if (!srs.advanceSession(nextDay)) {
    return false;
  }

  return rebuildQueue();
}

}  // namespace anki
