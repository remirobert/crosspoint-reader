#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "FlashcardDeckIndex.h"
#include "FlashcardTypes.h"

namespace flashcards {

struct SessionSlot {
  uint32_t cardOrdinal = 0;
  bool drilling = false;
};

struct DeckSummary {
  uint32_t totalCards = 0;
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

  bool open(const char* jsonlPath, bool shuffle = true, uint32_t seed = 0,
            const char* cacheDir = "/.crosspoint/flashcards");
  void close();

  bool restartPass(bool shuffle = true, uint32_t seed = 0);

  bool isOpen() const noexcept { return opened; }
  bool isPassComplete() const noexcept { return opened && passComplete; }
  bool isShuffled() const noexcept { return shuffledPass; }
  size_t remainingCount() const noexcept;
  uint32_t totalDeckCards() const noexcept { return opened ? deckIndex.header().cardCount : 0; }
  const char* deckTitle() const noexcept { return deckIndex.header().title; }

  SessionProgress sessionProgress() const noexcept;
  PassSummary passSummary() const noexcept;

  bool currentCard(FlashcardItem& outCard);
  bool submitChoice(bool repeatAgain);

  const FlashcardDeckIndex& index() const noexcept { return deckIndex; }

 private:
  FlashcardDeckIndex deckIndex;
  std::vector<SessionSlot> queue;
  std::unique_ptr<uint16_t[]> orderBuf;
  std::unique_ptr<char[]> lineBuf;
  PassSummary summary{};
  uint32_t nextStreamCursor = 0;
  uint16_t clearedCount = 0;
  bool opened = false;
  bool passComplete = false;
  bool shuffledPass = false;
};

}  // namespace flashcards
