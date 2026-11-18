#pragma once

#include <cstdint>

#include "AnkiTypes.h"

namespace anki {

class AnkiScheduler {
 public:
  static CardSrsRecord reviewCard(const CardSrsRecord& current, Rating rating, uint16_t sessionDay) noexcept;
  static IntervalPreview previewIntervals(const CardSrsRecord& current, uint16_t sessionDay) noexcept;
};

}  // namespace anki
