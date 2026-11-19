#pragma once

#include <cstdint>

#include "FlashcardTypes.h"

namespace flashcards {

class FlashcardScheduler {
 public:
  static CardSrsRecord reviewCard(const CardSrsRecord& current, Rating rating, uint16_t sessionDay) noexcept;
  static IntervalPreview previewIntervals(const CardSrsRecord& current, uint16_t sessionDay) noexcept;
};

}  // namespace flashcards
