#include "FlashcardSession.h"

#include <Logging.h>
#include <Memory.h>

#ifdef ESP_PLATFORM
#include <esp_random.h>
#else
#include <atomic>
#include <chrono>
#endif

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace flashcards {

namespace {

constexpr const char* DEFAULT_FLASHCARDS_CACHE_DIR = "/.crosspoint/flashcards";
constexpr size_t AGAIN_REQUEUE_AHEAD = 6;
constexpr uint32_t MAX_SHUFFLE_ORDER_CARDS = 4096;

uint32_t nextXorshift32(uint32_t& state) noexcept {
  if (state == 0) {
    state = 0x9E3779B9u;
  }
  uint32_t x = state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  state = x;
  return x;
}

uint32_t generateAutoSeed(const char* deckTitle, const void* sessionAddr) noexcept {
#ifdef ESP_PLATFORM
  (void)deckTitle;
  (void)sessionAddr;
  uint32_t rng = esp_random();
#else
  static std::atomic<uint32_t> sPassCounter{1};
  const uint32_t passSeq = sPassCounter.fetch_add(1, std::memory_order_relaxed);
  const uint64_t titleHash = fnv1a64(deckTitle ? deckTitle : "");
  const uint64_t tickEntropy = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const uint64_t addrEntropy = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(sessionAddr));
  const uint64_t mix = titleHash ^ (tickEntropy + 0x9E3779B97F4A7C15ULL * passSeq) ^ addrEntropy;
  uint32_t rng = static_cast<uint32_t>(mix ^ (mix >> 32));
#endif
  if (rng == 0) {
    rng = 0x9E3779B9u;
  }
  return rng;
}

struct InspectWorkspace {
  FlashcardDeckIndex idx;
  std::string idxPath;
};

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

  FlashcardDeckIndex::buildIndexPath(effectiveCache, jsonlPath, ws->idxPath);
  if (!ws->idx.openOrBuild(jsonlPath, ws->idxPath.c_str())) {
    return false;
  }

  const DeckIndexHeader& idxHead = ws->idx.header();
  out.totalCards = idxHead.cardCount;
  std::memcpy(out.title, idxHead.title, sizeof(out.title));
  out.title[sizeof(out.title) - 1] = '\0';
  return true;
}

bool FlashcardSession::open(const char* jsonlPath, const bool shuffle, const uint32_t seed, const char* cacheDir) {
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
  FlashcardDeckIndex::buildIndexPath(effectiveCache, jsonlPath, idxPath);
  if (!deckIndex.openOrBuild(jsonlPath, idxPath.c_str())) {
    return false;
  }

  lineBuf = makeUniqueNoThrow<char[]>(MAX_JSONL_LINE_BYTES);
  if (!lineBuf) {
    LOG_ERR("FCARD", "OOM allocating %u-byte session line buffer", static_cast<unsigned>(MAX_JSONL_LINE_BYTES));
    deckIndex.close();
    return false;
  }

  opened = true;
  if (!restartPass(shuffle, seed)) {
    close();
    return false;
  }

  return true;
}

void FlashcardSession::close() {
  deckIndex.close();
  queue.clear();
  shrinkToFit(queue);
  orderBuf.reset();
  lineBuf.reset();
  summary = PassSummary{};
  nextStreamCursor = 0;
  clearedCount = 0;
  opened = false;
  passComplete = false;
  shuffledPass = false;
}

bool FlashcardSession::restartPass(const bool shuffle, const uint32_t seed) {
  if (!opened) {
    LOG_ERR("FCARD", "Cannot restart pass: session not open");
    return false;
  }

  const uint32_t cardCount = deckIndex.header().cardCount;
  if (cardCount == 0) {
    queue.clear();
    orderBuf.reset();
    summary = PassSummary{0, 0, 0, 0};
    nextStreamCursor = 0;
    clearedCount = 0;
    shuffledPass = shuffle;
    passComplete = true;
    return true;
  }

  const size_t initialSlots = std::min<size_t>(cardCount, MAX_ACTIVE_QUEUE);
  if (initialSlots > queue.capacity()) {
    const size_t queueBytes = initialSlots * sizeof(SessionSlot);
    auto probe = makeUniqueNoThrow<uint8_t[]>(queueBytes);
    if (!probe) {
      LOG_ERR("FCARD", "OOM reserving %u session queue slots (%u B)", static_cast<unsigned>(initialSlots),
              static_cast<unsigned>(queueBytes));
      return false;
    }
    probe.reset();
    queue.reserve(initialSlots);
  }

  orderBuf.reset();
  queue.clear();

  uint32_t rng = seed;
  if (shuffle && rng == 0) {
    rng = generateAutoSeed(deckIndex.header().title, this);
  }

  bool shuffledFullOrder = false;
  if (shuffle && cardCount <= MAX_SHUFFLE_ORDER_CARDS) {
    orderBuf = makeUniqueNoThrow<uint16_t[]>(cardCount);
    if (orderBuf) {
      for (uint32_t i = 0; i < cardCount; ++i) {
        orderBuf[i] = static_cast<uint16_t>(i);
      }
      for (uint32_t i = cardCount - 1; i > 0; --i) {
        const uint32_t j = nextXorshift32(rng) % (i + 1u);
        std::swap(orderBuf[i], orderBuf[j]);
      }
      shuffledFullOrder = true;
    } else {
      LOG_ERR("FCARD", "OOM allocating %u-byte shuffle order buffer; shuffling %u-slot active window",
              static_cast<unsigned>(cardCount * sizeof(uint16_t)), static_cast<unsigned>(initialSlots));
    }
  }

  for (size_t i = 0; i < initialSlots; ++i) {
    const uint32_t ord = shuffledFullOrder ? static_cast<uint32_t>(orderBuf[i]) : static_cast<uint32_t>(i);
    queue.push_back(SessionSlot{ord, false});
  }

  if (shuffle && !shuffledFullOrder && queue.size() > 1) {
    for (size_t i = queue.size() - 1; i > 0; --i) {
      const size_t j = static_cast<size_t>(nextXorshift32(rng) % static_cast<uint32_t>(i + 1));
      std::swap(queue[i], queue[j]);
    }
  }

  nextStreamCursor = static_cast<uint32_t>(initialSlots);
  if (nextStreamCursor >= cardCount) {
    orderBuf.reset();
  }
  clearedCount = 0;
  summary = PassSummary{cardCount, 0, 0, 0};
  shuffledPass = shuffle;
  passComplete = false;
  return true;
}

size_t FlashcardSession::remainingCount() const noexcept {
  if (!opened) {
    return 0;
  }
  const uint32_t cardCount = deckIndex.header().cardCount;
  const size_t unstreamed = (cardCount > nextStreamCursor) ? static_cast<size_t>(cardCount - nextStreamCursor) : 0u;
  return queue.size() + unstreamed;
}

SessionProgress FlashcardSession::sessionProgress() const noexcept {
  SessionProgress out{};
  if (!opened || deckIndex.header().cardCount == 0) {
    return out;
  }

  uint32_t drilling = 0;
  uint32_t unseen = 0;
  for (const SessionSlot& slot : queue) {
    if (slot.drilling) {
      ++drilling;
    } else {
      ++unseen;
    }
  }

  const uint32_t cardCount = deckIndex.header().cardCount;
  if (cardCount > nextStreamCursor) {
    unseen += (cardCount - nextStreamCursor);
  }

  out.cleared = clearedCount;
  out.drilling = static_cast<uint16_t>(std::min<uint32_t>(drilling, 65535u));
  out.unseen = static_cast<uint16_t>(std::min<uint32_t>(unseen, 65535u));
  return out;
}

PassSummary FlashcardSession::passSummary() const noexcept { return opened ? summary : PassSummary{}; }

bool FlashcardSession::currentCard(FlashcardItem& outCard) {
  if (!opened || passComplete || queue.empty() || !lineBuf) {
    return false;
  }
  return deckIndex.readCard(queue.front().cardOrdinal, lineBuf.get(), MAX_JSONL_LINE_BYTES, outCard);
}

bool FlashcardSession::submitChoice(const bool repeatAgain) {
  if (!opened || passComplete || queue.empty()) {
    LOG_ERR("FCARD", "Cannot submit choice: no active card");
    return false;
  }

  const SessionSlot current = queue.front();
  queue.erase(queue.begin());
  if (summary.totalReviews < std::numeric_limits<uint32_t>::max()) {
    ++summary.totalReviews;
  }

  if (repeatAgain) {
    const size_t insertOffset = std::min<size_t>(queue.size(), AGAIN_REQUEUE_AHEAD);
    queue.insert(queue.begin() + static_cast<std::ptrdiff_t>(insertOffset), SessionSlot{current.cardOrdinal, true});
  } else {
    if (current.drilling) {
      ++summary.retriedCount;
    } else {
      ++summary.firstTryCount;
    }
    if (clearedCount < 65535u) {
      ++clearedCount;
    }

    const uint32_t cardCount = deckIndex.header().cardCount;
    if (nextStreamCursor < cardCount) {
      const uint32_t nextOrd = orderBuf ? static_cast<uint32_t>(orderBuf[nextStreamCursor]) : nextStreamCursor;
      ++nextStreamCursor;
      queue.push_back(SessionSlot{nextOrd, false});
      if (nextStreamCursor >= cardCount) {
        orderBuf.reset();
      }
    }
  }

  if (queue.empty()) {
    passComplete = true;
    orderBuf.reset();
  }

  return true;
}

}  // namespace flashcards
