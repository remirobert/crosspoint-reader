#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "FlashcardDeckIndex.h"
#include "FlashcardScheduler.h"
#include "FlashcardSrsStore.h"
#include "FlashcardTypes.h"

namespace flashcards {

struct SessionSlot {
  uint32_t cardOrdinal = 0;
  CardSrsRecord srs{};
};

struct DeckSummary {
  uint16_t dueCount = 0;
  uint16_t newCount = 0;
  uint32_t totalCards = 0;
  uint32_t sessionDay = 1;
  bool sessionCompleted = false;
  uint16_t newPerDay = DEFAULT_NEW_PER_DAY;
  char title[64]{};
};

class FlashcardSession {
 public:
  using DeckSummary = ::flashcards::DeckSummary;
  using SessionSlot = ::flashcards::SessionSlot;

  FlashcardSession() = default;
  ~FlashcardSession();
  FlashcardSession(const FlashcardSession&) = delete;
  FlashcardSession& operator=(const FlashcardSession&) = delete;

  static bool inspectDeck(const char* jsonlPath, const char* cacheDir, DeckSummary& out);
  static bool inspectDeck(const char* jsonlPath, DeckSummary& out) {
    return inspectDeck(jsonlPath, "/.crosspoint/flashcards", out);
  }

  bool open(const char* jsonlPath, const char* cacheDir = "/.crosspoint/flashcards");
  void close();

  bool isOpen() const { return opened; }
  bool isSessionComplete() const { return opened && srs.header().sessionCompleted != 0; }
  size_t remainingCount() const { return queue.size(); }
  uint32_t totalDeckCards() const { return opened ? deckIndex.header().cardCount : 0; }
  uint32_t sessionDay() const { return opened ? srs.header().currentSessionDay : 1; }
  uint32_t reviewsThisSession() const { return opened ? srs.header().reviewsThisSession : 0; }
  const char* deckTitle() const { return deckIndex.header().title; }
  uint16_t effectiveNewPerDay() const {
    return opened ? srs.effectiveNewPerDay(deckIndex.header().newPerDay) : DEFAULT_NEW_PER_DAY;
  }

  bool setNewPerDayOverride(uint16_t limit);
  uint32_t computeNextSessionDay() const;
  void previewNextSession(uint16_t& outDue, uint16_t& outNew) const;

  bool currentCard(FlashcardItem& outCard, CardSrsRecord& outSrs, IntervalPreview& outPreview);
  bool submitRating(Rating rating);
  bool startNextSession();

  const FlashcardSrsStore& srsStore() const { return srs; }
  const FlashcardDeckIndex& index() const { return deckIndex; }

 private:
  bool rebuildQueue(uint32_t* outUnseenNew = nullptr, uint16_t* outMinFutureDue = nullptr,
                    bool* outHasFutureDue = nullptr);

  mutable FlashcardDeckIndex deckIndex;
  FlashcardSrsStore srs;
  std::vector<SessionSlot> queue;
  std::unique_ptr<char[]> lineBuf;
  bool opened = false;
};

}  // namespace flashcards
