#include "FlashcardScheduler.h"

#include <algorithm>
#include <cmath>

namespace flashcards {

namespace {

// Default FSRS-5 weights (Section 4.2 of docs/flashcards-architecture.md).
constexpr float W[19] = {
    0.40255f, 1.18385f, 3.17300f, 15.69105f, 7.19490f, 0.53450f, 1.46040f, 0.00460f, 1.54575f, 0.11920f,
    1.01925f, 1.93950f, 0.11000f, 0.29605f,  2.26980f, 0.23150f, 2.98980f, 0.51655f, 0.66210f,
};

constexpr float MIN_STABILITY = 0.1f;
constexpr float MAX_STABILITY = 65535.0f;
constexpr float MIN_DIFFICULTY = 1.0f;
constexpr float MAX_DIFFICULTY = 10.0f;
constexpr uint16_t MIN_DIFFICULTY_PERMILLE = 1000;
constexpr uint16_t MAX_DIFFICULTY_PERMILLE = 10000;
constexpr uint16_t MAX_SESSION_DAY = 65535;

struct FsrsOutcome {
  float stability = MIN_STABILITY;
  uint16_t difficultyPermille = MIN_DIFFICULTY_PERMILLE;
};

CardState sanitizeState(uint8_t rawState) noexcept {
  if (rawState > static_cast<uint8_t>(CardState::Relearning)) {
    return CardState::New;
  }
  return static_cast<CardState>(rawState);
}

int sanitizeGrade(Rating rating) noexcept { return std::clamp(static_cast<int>(rating), 1, 4); }

float clampStability(float s) noexcept {
  if (!std::isfinite(s) || s < MIN_STABILITY) {
    return MIN_STABILITY;
  }
  return s > MAX_STABILITY ? MAX_STABILITY : s;
}

float clampDifficulty(float d) noexcept {
  if (!std::isfinite(d) || d < MIN_DIFFICULTY) {
    return MIN_DIFFICULTY;
  }
  return d > MAX_DIFFICULTY ? MAX_DIFFICULTY : d;
}

uint16_t clampDifficultyPermille(uint16_t permille) noexcept {
  return std::clamp(permille, MIN_DIFFICULTY_PERMILLE, MAX_DIFFICULTY_PERMILLE);
}

float permilleToDifficulty(uint16_t permille) noexcept {
  return static_cast<float>(clampDifficultyPermille(permille)) * 0.001f;
}

uint16_t difficultyToPermille(float d) noexcept {
  const float clamped = clampDifficulty(d);
  const long scaled = lroundf(clamped * 1000.0f);
  if (scaled <= static_cast<long>(MIN_DIFFICULTY_PERMILLE)) {
    return MIN_DIFFICULTY_PERMILLE;
  }
  if (scaled >= static_cast<long>(MAX_DIFFICULTY_PERMILLE)) {
    return MAX_DIFFICULTY_PERMILLE;
  }
  return static_cast<uint16_t>(scaled);
}

uint16_t stabilityToDays(float s) noexcept {
  if (!std::isfinite(s) || s <= 1.0f) {
    return 1;
  }
  if (s >= MAX_STABILITY) {
    return MAX_SESSION_DAY;
  }
  const long rounded = lroundf(s);
  if (rounded <= 1) {
    return 1;
  }
  if (rounded >= static_cast<long>(MAX_SESSION_DAY)) {
    return MAX_SESSION_DAY;
  }
  return static_cast<uint16_t>(rounded);
}

uint16_t saturatingAddDays(uint16_t baseDay, uint16_t deltaDays) noexcept {
  const uint32_t sum = static_cast<uint32_t>(baseDay) + static_cast<uint32_t>(deltaDays);
  return sum > MAX_SESSION_DAY ? MAX_SESSION_DAY : static_cast<uint16_t>(sum);
}

float initialDifficulty(int grade) noexcept {
  const float raw = W[4] - expf(W[5] * static_cast<float>(grade - 1)) + 1.0f;
  return clampDifficulty(raw);
}

FsrsOutcome computeFsrs(const CardSrsRecord& current, Rating rating, uint16_t sessionDay) noexcept {
  const CardState state = sanitizeState(current.state);
  const int g = sanitizeGrade(rating);

  if (state == CardState::New) {
    const float s0 = clampStability(W[g - 1]);
    const float d0 = initialDifficulty(g);
    return {s0, difficultyToPermille(d0)};
  }

  const float s = clampStability(current.stability);
  if (sessionDay <= current.lastReviewDay) {
    const float sShort = clampStability(s * expf(W[17] * (static_cast<float>(g - 3) + W[18])));
    return {sShort, clampDifficultyPermille(current.difficultyPermille)};
  }

  const float elapsedDays = static_cast<float>(sessionDay - current.lastReviewDay);
  const float d = permilleToDifficulty(current.difficultyPermille);
  const float retrievability = powf(1.0f + (19.0f / 81.0f) * (elapsedDays / s), -0.5f);

  const float dTmp = d - W[6] * static_cast<float>(g - 3) * ((10.0f - d) / 9.0f);
  const float dNext = clampDifficulty(W[7] * initialDifficulty(4) + (1.0f - W[7]) * dTmp);

  if (g == 1) {
    float sForget =
        W[11] * powf(dNext, -W[12]) * (powf(s + 1.0f, W[13]) - 1.0f) * expf(W[14] * (1.0f - retrievability));
    if (!std::isfinite(sForget)) {
      sForget = MIN_STABILITY;
    }
    const float sNext = std::max(MIN_STABILITY, std::min(s, sForget));
    return {sNext, difficultyToPermille(dNext)};
  }

  const float hardPenalty = (g == 2) ? W[15] : 1.0f;
  const float easyBonus = (g == 4) ? W[16] : 1.0f;
  const float growth = expf(W[8]) * (11.0f - dNext) * powf(s, -W[9]) * (expf(W[10] * (1.0f - retrievability)) - 1.0f) *
                       hardPenalty * easyBonus;
  float sRecall = s * (1.0f + growth);
  if (!std::isfinite(sRecall)) {
    sRecall = MAX_STABILITY;
  }
  return {clampStability(sRecall), difficultyToPermille(dNext)};
}

}  // namespace

IntervalPreview FlashcardScheduler::previewIntervals(const CardSrsRecord& current, uint16_t sessionDay) noexcept {
  const uint32_t rawHard = stabilityToDays(computeFsrs(current, Rating::Hard, sessionDay).stability);
  const uint32_t rawGood = stabilityToDays(computeFsrs(current, Rating::Good, sessionDay).stability);
  const uint32_t rawEasy = stabilityToDays(computeFsrs(current, Rating::Easy, sessionDay).stability);

  const uint32_t maxRemaining = MAX_SESSION_DAY - static_cast<uint32_t>(sessionDay);
  if (maxRemaining < 2) {
    return IntervalPreview{1, 1, 2};
  }

  const uint32_t maxEasy = maxRemaining;
  const uint32_t maxGood = maxEasy - 1;
  const uint32_t hardDays = std::clamp<uint32_t>(rawHard, 1u, maxGood);
  const uint32_t goodDays = std::clamp<uint32_t>(std::max(rawGood, hardDays), hardDays, maxGood);
  const uint32_t easyDays = std::clamp<uint32_t>(std::max(rawEasy, goodDays + 1u), goodDays + 1u, maxEasy);

  return IntervalPreview{
      static_cast<uint16_t>(hardDays),
      static_cast<uint16_t>(goodDays),
      static_cast<uint16_t>(easyDays),
  };
}

CardSrsRecord FlashcardScheduler::reviewCard(const CardSrsRecord& current, Rating rating,
                                             uint16_t sessionDay) noexcept {
  const CardState prevState = sanitizeState(current.state);
  const int g = sanitizeGrade(rating);
  const FsrsOutcome outcome = computeFsrs(current, static_cast<Rating>(g), sessionDay);

  CardSrsRecord next{};
  next.cardIdHash = current.cardIdHash;
  next.stability = outcome.stability;
  next.difficultyPermille = outcome.difficultyPermille;
  next.lastReviewDay = sessionDay;
  next.lapses = current.lapses;

  if (g == 1) {
    next.dueDay = sessionDay;
    if (prevState == CardState::New || prevState == CardState::Learning) {
      next.state = static_cast<uint8_t>(CardState::Learning);
    } else {
      next.state = static_cast<uint8_t>(CardState::Relearning);
      if (prevState == CardState::Review && next.lapses < 255) {
        ++next.lapses;
      }
    }
    return next;
  }

  const IntervalPreview preview = previewIntervals(current, sessionDay);
  uint16_t intervalDays = preview.goodDays;
  if (g == 2) {
    intervalDays = preview.hardDays;
  } else if (g == 4) {
    intervalDays = preview.easyDays;
  }

  next.dueDay = saturatingAddDays(sessionDay, intervalDays);
  next.state = static_cast<uint8_t>(CardState::Review);
  return next;
}

}  // namespace flashcards
