#include "FlashcardsReviewActivity.h"

#include <FontCacheManager.h>
#include <HalDisplay.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HapticFeedback.h"
#include "ReaderFontSizes.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr size_t MAX_WRAPPED_PROMPT_LINES = 16;
constexpr size_t MAX_WRAPPED_HINT_LINES = 2;
constexpr size_t MAX_WRAPPED_ANSWER_LINES = 128;
constexpr int PILL_CORNER_RADIUS = 10;
constexpr int FOCUS_RING_INNER_INSET = 5;  // 2px outer stroke + 3px white gap
constexpr int TOUCH_HIT_PAD_Y = 8;

struct RatingButtonSpec {
  flashcards::Rating rating;
  const char* label;
};

constexpr RatingButtonSpec RATING_SPECS[4] = {
    {flashcards::Rating::Again, "Again"},
    {flashcards::Rating::Hard, "Hard"},
    {flashcards::Rating::Good, "Good"},
    {flashcards::Rating::Easy, "Easy"},
};

int ratingToIndex(const flashcards::Rating rating) noexcept {
  switch (rating) {
    case flashcards::Rating::Again:
      return 0;
    case flashcards::Rating::Hard:
      return 1;
    case flashcards::Rating::Good:
      return 2;
    case flashcards::Rating::Easy:
      return 3;
  }
  return 2;
}

struct ReviewLayout {
  Rect body;
  Rect promptBand;
  int dividerY = 0;
  Rect answerArea;
  int previewLabelY = 0;
  int pillY = 0;
  int pillHeight = 0;
};

ReviewLayout computeReviewLayout(const GfxRenderer& renderer, const Rect& safeArea, const Rect& headerRect) {
  ReviewLayout out;
  const int sidePad = UITheme::getInstance().getMetrics().contentSidePadding;
  const int bodyTop = std::max(safeArea.y, headerRect.y + headerRect.height);
  const int bodyBottom = safeArea.y + safeArea.height;
  const int bodyX = safeArea.x + sidePad;
  const int bodyWidth = std::max(40, safeArea.width - sidePad * 2);
  const int bodyHeight = std::max(80, bodyBottom - bodyTop);

  constexpr int bottomPad = 12;
  out.body = Rect{bodyX, bodyTop, bodyWidth, bodyHeight};
  out.pillHeight = std::clamp(bodyHeight / 11, 42, 50);
  out.pillY = bodyBottom - bottomPad - out.pillHeight;

  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  out.previewLabelY = out.pillY - smallLineH - 4;
  const int ratingRegionTop = out.previewLabelY - 6;

  const int usableCardHeight = std::max(60, ratingRegionTop - bodyTop);
  const int promptTopPad = std::clamp(usableCardHeight / 30, 6, 14);
  const int promptHeight = std::max(44, (usableCardHeight * 38) / 100);
  out.promptBand = Rect{bodyX, bodyTop + promptTopPad, bodyWidth, promptHeight};

  out.dividerY = out.promptBand.y + out.promptBand.height + 6;
  const int answerTop = out.dividerY + 8;
  const int answerBottom = std::max(answerTop + 20, ratingRegionTop - 4);
  out.answerArea = Rect{bodyX, answerTop, bodyWidth, answerBottom - answerTop};
  return out;
}

bool consumeReleaseWithGuard(const MappedInputManager& input, const MappedInputManager::Button button, bool& guard) {
  if (guard) {
    if (!input.isPressed(button)) {
      guard = false;
      if (input.wasReleased(button)) {
        return false;
      }
    } else {
      return false;
    }
  }
  return input.wasReleased(button);
}

bool rectContains(const Rect& r, const int x, const int y, const int padY = 0) noexcept {
  return r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y - padY && y < r.y + r.height + padY;
}

// Crisp monochrome pill: double-border focus ring when focused, 1px outline otherwise, white interior.
void drawFocusPill(const GfxRenderer& renderer, const Rect& rect, const char* label, const int fontId,
                   const bool focused) {
  if (rect.width <= 0 || rect.height <= 0) {
    return;
  }

  renderer.fillRect(rect.x, rect.y, rect.width, rect.height, false);
  if (focused) {
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 2, PILL_CORNER_RADIUS, true);
    const int innerW = rect.width - FOCUS_RING_INNER_INSET * 2;
    const int innerH = rect.height - FOCUS_RING_INNER_INSET * 2;
    if (innerW > 4 && innerH > 4) {
      const int innerRadius = std::max(2, PILL_CORNER_RADIUS - FOCUS_RING_INNER_INSET);
      renderer.drawRoundedRect(rect.x + FOCUS_RING_INNER_INSET, rect.y + FOCUS_RING_INNER_INSET, innerW, innerH, 1,
                               innerRadius, true);
    }
  } else {
    renderer.drawRoundedRect(rect.x, rect.y, rect.width, rect.height, 1, PILL_CORNER_RADIUS, true);
  }

  if (label && label[0] != '\0') {
    const EpdFontFamily::Style style = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    const int lineH = renderer.getLineHeight(fontId);
    const int textW = renderer.getTextWidth(fontId, label, style);
    const int textX = rect.x + (rect.width - textW) / 2;
    const int textY = rect.y + (rect.height - lineH) / 2;
    renderer.drawText(fontId, textX, textY, label, true, style);
  }
}

void drawFocusPillTruncated(const GfxRenderer& renderer, const Rect& rect, const char* label, const int fontId,
                            const bool focused) {
  if (rect.width <= 0 || rect.height <= 0 || !label || label[0] == '\0') {
    drawFocusPill(renderer, rect, label, fontId, focused);
    return;
  }
  const EpdFontFamily::Style style = focused ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int maxTextW = std::max(8, rect.width - (FOCUS_RING_INNER_INSET + 4) * 2);
  if (renderer.getTextWidth(fontId, label, style) <= maxTextW) {
    drawFocusPill(renderer, rect, label, fontId, focused);
    return;
  }
  const std::string truncated = renderer.truncatedText(fontId, label, maxTextW, style);
  drawFocusPill(renderer, rect, truncated.c_str(), fontId, focused);
}

void drawCenteredLineTruncated(const GfxRenderer& renderer, const Rect& bounds, const int fontId, const int y,
                               const char* text, const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || text[0] == '\0' || bounds.width <= 0) {
    return;
  }
  if (renderer.getTextWidth(fontId, text, style) <= bounds.width) {
    UITheme::drawCenteredText(renderer, bounds, fontId, y, text, true, style);
    return;
  }
  const std::string clipped = renderer.truncatedText(fontId, text, bounds.width, style);
  UITheme::drawCenteredText(renderer, bounds, fontId, y, clipped.c_str(), true, style);
}

void drawCheckmarkCircle(const GfxRenderer& renderer, const int cx, const int cy, const int radius) {
  if (radius <= 4) {
    return;
  }
  renderer.drawArc(radius, cx, cy, -1, -1, 2, true);
  renderer.drawArc(radius, cx, cy, 1, -1, 2, true);
  renderer.drawArc(radius, cx, cy, 1, 1, 2, true);
  renderer.drawArc(radius, cx, cy, -1, 1, 2, true);

  const int x0 = cx - (radius * 9) / 20;
  const int y0 = cy + radius / 12;
  const int x1 = cx - radius / 8;
  const int y1 = cy + (radius * 7) / 20;
  const int x2 = cx + (radius * 9) / 20;
  const int y2 = cy - (radius * 6) / 20;

  for (int dy = -1; dy <= 1; ++dy) {
    renderer.drawLine(x0, y0 + dy, x1, y1 + dy, true);
    renderer.drawLine(x1, y1 + dy, x2, y2 + dy, true);
  }
}

// Splits paragraphs on '\n' and word-wraps without ellipsis truncation so multi-page notes keep all text.
void wrapParagraphLines(const GfxRenderer& renderer, const int fontId, const EpdFontFamily::Style style,
                        const std::string_view text, const int maxWidth, const size_t maxLines,
                        std::vector<std::string>& outLines, bool& outOverflowed) {
  outLines.clear();
  outOverflowed = false;
  if (text.empty() || maxWidth <= 0 || maxLines == 0) {
    return;
  }
  const size_t initialReserve = std::min<size_t>(maxLines, 16);
  if (outLines.capacity() < initialReserve) {
    outLines.reserve(initialReserve);
  }

  std::string currentLine;
  std::string candidate;
  std::string piece;
  currentLine.reserve(64);
  candidate.reserve(64);
  piece.reserve(64);

  size_t paraPos = 0;
  while (paraPos <= text.size()) {
    const size_t nlPos = text.find('\n', paraPos);
    std::string_view para =
        (nlPos == std::string_view::npos) ? text.substr(paraPos) : text.substr(paraPos, nlPos - paraPos);
    if (!para.empty() && para.back() == '\r') {
      para.remove_suffix(1);
    }

    if (para.empty()) {
      if (!outLines.empty()) {
        if (outLines.size() >= maxLines) {
          outOverflowed = true;
          return;
        }
        outLines.emplace_back();
      }
    } else {
      currentLine.clear();
      size_t cursor = 0;
      while (cursor < para.size()) {
        while (cursor < para.size() && (para[cursor] == ' ' || para[cursor] == '\t')) {
          ++cursor;
        }
        if (cursor >= para.size()) {
          break;
        }

        size_t wordEnd = cursor;
        while (wordEnd < para.size() && para[wordEnd] != ' ' && para[wordEnd] != '\t') {
          ++wordEnd;
        }
        std::string_view word = para.substr(cursor, wordEnd - cursor);
        cursor = wordEnd;

        while (!word.empty()) {
          candidate.clear();
          if (!currentLine.empty()) {
            candidate.append(currentLine);
            candidate.push_back(' ');
          }
          candidate.append(word.data(), word.size());

          if (renderer.getTextWidth(fontId, candidate.c_str(), style) <= maxWidth) {
            currentLine = candidate;
            break;
          }

          if (!currentLine.empty()) {
            if (outLines.size() >= maxLines) {
              outOverflowed = true;
              return;
            }
            outLines.push_back(currentLine);
            currentLine.clear();
            continue;
          }

          // Single token wider than maxWidth (e.g. CJK run without spaces): split at UTF-8 codepoint boundary.
          size_t fit = 0;
          while (fit < word.size()) {
            size_t next = fit + 1;
            while (next < word.size() && (static_cast<unsigned char>(word[next]) & 0xC0u) == 0x80u) {
              ++next;
            }
            piece.assign(word.data(), next);
            if (fit > 0 && renderer.getTextWidth(fontId, piece.c_str(), style) > maxWidth) {
              break;
            }
            fit = next;
          }

          piece.assign(word.data(), fit);
          word.remove_prefix(fit);
          if (word.empty()) {
            currentLine = piece;
          } else {
            if (outLines.size() >= maxLines) {
              outOverflowed = true;
              return;
            }
            outLines.push_back(piece);
          }
        }
      }

      if (!currentLine.empty()) {
        if (outLines.size() >= maxLines) {
          outOverflowed = true;
          return;
        }
        outLines.push_back(currentLine);
      }
    }

    if (nlPos == std::string_view::npos) {
      break;
    }
    paraPos = nlPos + 1;
  }
}

int resolveScaledReaderFont(const GfxRenderer& renderer, const int baseFontId, const float scale) {
  int resolved = renderer.resolveFontVariant(baseFontId, scale);
  if (resolved == 0 || resolved == baseFontId) {
    const int builtin = builtinReaderFontVariant(baseFontId, scale);
    if (builtin != 0) {
      resolved = builtin;
    }
  }
  if (resolved != 0 && renderer.ensureFontLoaded(resolved)) {
    return resolved;
  }
  return baseFontId;
}

}  // namespace

FlashcardsReviewActivity::FlashcardsReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::string deckPath, const bool allowFastInitialRefresh)
    : Activity("FlashcardsReview", renderer, mappedInput),
      deckPath(std::move(deckPath)),
      allowFastInitialRefresh(allowFastInitialRefresh),
      buttonNavigator(300, 450) {}

void FlashcardsReviewActivity::onEnter() {
  Activity::onEnter();

  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("MEM", "flashcards review enter (fast_init=%d): free=%u max_block=%u", allowFastInitialRefresh ? 1 : 0,
          static_cast<unsigned>(heap.freeBytes), static_cast<unsigned>(heap.largestBlockBytes));

  suppressConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  suppressBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);

  if (deckPath.empty() || !Storage.exists(deckPath.c_str())) {
    LOG_ERR("FCARD", "Deck file missing: %s", deckPath.c_str());
    clearSleepResumeState();
    loadFailed = true;
    activityManager.goToFlashcards();
    return;
  }

  sdFontSystem.ensureLoaded(renderer);
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);

  session = makeUniqueNoThrow<flashcards::FlashcardSession>();
  if (!session) {
    LOG_ERR("FCARD", "OOM allocating %u-byte FlashcardSession",
            static_cast<unsigned>(sizeof(flashcards::FlashcardSession)));
    clearSleepResumeState();
    loadFailed = true;
    activityManager.goToFlashcards();
    return;
  }

  if (!session->open(deckPath.c_str())) {
    LOG_ERR("FCARD", "Failed opening Flashcards session: %s", deckPath.c_str());
    clearSleepResumeState();
    loadFailed = true;
    activityManager.goToFlashcards();
    return;
  }

  promptLines.reserve(8);
  hintLines.reserve(MAX_WRAPPED_HINT_LINES);
  answerLines.reserve(32);
  answerPages.reserve(8);

  cardsUntilFullRefresh = SETTINGS.getRefreshFrequency();
  forcedRefreshPending = !allowFastInitialRefresh;

  if (session->isSessionComplete() || session->remainingCount() == 0) {
    enterSessionCompleteState();
  } else if (!loadActiveCardState()) {
    LOG_ERR("FCARD", "Failed loading active card: %s", deckPath.c_str());
    clearSleepResumeState();
    loadFailed = true;
    activityManager.goToFlashcards();
    return;
  }

  requestUpdate();
}

void FlashcardsReviewActivity::onExit() {
  Activity::onExit();

  // ActivityManager::exitActivity holds RenderLock during onExit().
  if (session) {
    session->close();
    session.reset();
  }

  if (auto* fc = renderer.getFontCacheManager()) {
    fc->releaseSdFontCaches();
  }

  promptLines.clear();
  shrinkToFit(promptLines);
  hintLines.clear();
  shrinkToFit(hintLines);
  answerLines.clear();
  shrinkToFit(answerLines);
  answerPages.clear();
  shrinkToFit(answerPages);

  if (renderer.getOrientation() != GfxRenderer::Orientation::Portrait) {
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }

  const auto heap = HalMemory::getInternalHeap();
  LOG_INF("MEM", "flashcards review exit: free=%u max_block=%u", static_cast<unsigned>(heap.freeBytes),
          static_cast<unsigned>(heap.largestBlockBytes));
}

void FlashcardsReviewActivity::prepareForSleep() {
  // ActivityManager::prepareForSleep() holds RenderLock before invoking this hook.
  if (loadFailed) {
    return;
  }

  const bool inFreeReview = session && session->isFreeReview();
  if (state != State::SessionComplete && !inFreeReview && firstReviewFrameRendered.load(std::memory_order_acquire)) {
    APP_STATE.openFlashcardsPath = deckPath;
    APP_STATE.lastSleepFromFlashcards = true;
    APP_STATE.flashcardsActivityLoadCount = 0;
    reviewStateSaved = true;
  }

  if (session) {
    session->close();
  }
}

bool FlashcardsReviewActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}

bool FlashcardsReviewActivity::handleHomeGesture() {
  clearSleepResumeState();
  return false;
}

bool FlashcardsReviewActivity::loadActiveCardState() {
  if (!session || !session->isOpen()) {
    return false;
  }
  if (!session->currentCard(currentCardData, currentSrs, currentPreview)) {
    return false;
  }

  state = State::CardFront;
  focusedRating = flashcards::Rating::Good;
  freeReviewFocusGotIt = true;
  extraPage = 0;
  cardLayoutValid = false;
  showAnswerPillRect = Rect{};
  cardTapBodyRect = Rect{};
  for (Rect& r : ratingPillRects) {
    r = Rect{};
  }
  freeReviewPillRect = Rect{};
  nextSessionPillRect = Rect{};

  prewarmActiveCardGlyphs();
  return true;
}

void FlashcardsReviewActivity::populateSessionCompleteMetrics() {
  state = State::SessionComplete;
  cardLayoutValid = false;
  showAnswerPillRect = Rect{};
  cardTapBodyRect = Rect{};
  for (Rect& r : ratingPillRects) {
    r = Rect{};
  }
  freeReviewPillRect = Rect{};
  nextSessionPillRect = Rect{};
  focusFreeReviewOnComplete = false;

  if (session && session->isOpen()) {
    nextSessionDay = session->computeNextSessionDay();
    session->previewNextSession(nextDueCount, nextNewCount, &nextScheduledDueDay);
    freeReviewCount = session->countFreeReviewCards();
  } else {
    nextSessionDay = 1;
    nextDueCount = 0;
    nextNewCount = 0;
    nextScheduledDueDay = 0;
    freeReviewCount = 0;
  }
}

void FlashcardsReviewActivity::enterSessionCompleteState() {
  populateSessionCompleteMetrics();
  if (session && session->isOpen()) {
    renderer.prewarmFallbackText(UI_12_FONT_ID, session->deckTitle(), EpdFontFamily::BOLD);
  }
  clearSleepResumeState();
}

void FlashcardsReviewActivity::clearSleepResumeState() {
  reviewStateSaved = false;
  firstReviewFrameRendered.store(false, std::memory_order_release);
  if (APP_STATE.clearFlashcardsResume()) {
    APP_STATE.saveToFile();
  }
}

void FlashcardsReviewActivity::saveSleepResumeStateIfReady() {
  if (reviewStateSaved || state == State::SessionComplete || (session && session->isFreeReview()) ||
      !firstReviewFrameRendered.load(std::memory_order_acquire)) {
    return;
  }
  reviewStateSaved = true;
  if (APP_STATE.openFlashcardsPath != deckPath || !APP_STATE.lastSleepFromFlashcards ||
      APP_STATE.flashcardsActivityLoadCount != 0) {
    APP_STATE.openFlashcardsPath = deckPath;
    APP_STATE.lastSleepFromFlashcards = true;
    APP_STATE.flashcardsActivityLoadCount = 0;
    APP_STATE.saveToFile();
  }
}

void FlashcardsReviewActivity::exitToDeckList() {
  clearSleepResumeState();
  activityManager.goToFlashcards();
}

void FlashcardsReviewActivity::flipToBack() {
  {
    RenderLock lock(*this);
    if (state != State::CardFront) {
      return;
    }
    state = State::CardBack;
    focusedRating = flashcards::Rating::Good;
    freeReviewFocusGotIt = true;
    extraPage = 0;
  }
  requestUpdate();
}

void FlashcardsReviewActivity::stepFocusedRating(const int delta) {
  bool changed = false;
  {
    RenderLock lock(*this);
    if (state != State::CardBack) {
      return;
    }
    if (session && session->isFreeReview()) {
      const int curIdx = freeReviewFocusGotIt ? 1 : 0;
      const int nextIdx = std::clamp(curIdx + delta, 0, 1);
      if (nextIdx != curIdx) {
        freeReviewFocusGotIt = (nextIdx == 1);
        changed = true;
      }
    } else {
      const int curIdx = ratingToIndex(focusedRating);
      const int nextIdx = std::clamp(curIdx + delta, 0, 3);
      if (nextIdx != curIdx) {
        focusedRating = RATING_SPECS[nextIdx].rating;
        changed = true;
      }
    }
  }
  if (changed) {
    requestUpdate();
  }
}

void FlashcardsReviewActivity::submitActiveRating(const flashcards::Rating rating) {
  bool enteredComplete = false;
  bool failed = false;
  {
    RenderLock lock(*this);
    if (!session || !session->isOpen() || state != State::CardBack) {
      return;
    }

    if (!session->submitRating(rating)) {
      LOG_ERR("FCARD", "Failed submitting rating for deck: %s", deckPath.c_str());
      failed = true;
    } else if (session->isSessionComplete() || session->remainingCount() == 0) {
      forcedRefreshPending = true;
      enteredComplete = true;
      populateSessionCompleteMetrics();
    } else {
      if (cardsUntilFullRefresh <= 1) {
        forcedRefreshPending = true;
        cardsUntilFullRefresh = SETTINGS.getRefreshFrequency();
      } else {
        --cardsUntilFullRefresh;
      }

      if (!loadActiveCardState()) {
        LOG_ERR("FCARD", "Failed loading next card: %s", deckPath.c_str());
        failed = true;
      }
    }
  }

  if (failed) {
    exitToDeckList();
    return;
  }

  if (enteredComplete) {
    clearSleepResumeState();
  }

  requestUpdate();
}

void FlashcardsReviewActivity::submitFreeReviewChoice(const bool repeatAgain) {
  bool enteredComplete = false;
  bool failed = false;
  {
    RenderLock lock(*this);
    if (!session || !session->isOpen() || !session->isFreeReview() || state != State::CardBack) {
      return;
    }

    if (!session->submitFreeReview(repeatAgain)) {
      LOG_ERR("FCARD", "Failed submitting free review for deck: %s", deckPath.c_str());
      failed = true;
    } else if (session->isSessionComplete() || session->remainingCount() == 0) {
      forcedRefreshPending = true;
      enteredComplete = true;
      populateSessionCompleteMetrics();
    } else {
      if (cardsUntilFullRefresh <= 1) {
        forcedRefreshPending = true;
        cardsUntilFullRefresh = SETTINGS.getRefreshFrequency();
      } else {
        --cardsUntilFullRefresh;
      }

      if (!loadActiveCardState()) {
        LOG_ERR("FCARD", "Failed loading next free-review card: %s", deckPath.c_str());
        failed = true;
      }
    }
  }

  if (failed) {
    exitToDeckList();
    return;
  }

  if (enteredComplete) {
    clearSleepResumeState();
  }

  requestUpdate();
}

void FlashcardsReviewActivity::stepCompleteFocus(const int delta) {
  bool changed = false;
  {
    RenderLock lock(*this);
    if (state != State::SessionComplete || freeReviewCount == 0) {
      return;
    }
    const int curIdx = focusFreeReviewOnComplete ? 0 : 1;
    const int nextIdx = std::clamp(curIdx + delta, 0, 1);
    if (nextIdx != curIdx) {
      focusFreeReviewOnComplete = (nextIdx == 0);
      changed = true;
    }
  }
  if (changed) {
    requestUpdate();
  }
}

void FlashcardsReviewActivity::beginFreeReview() {
  bool failed = false;
  {
    RenderLock lock(*this);
    if (!session || !session->isOpen() || state != State::SessionComplete || freeReviewCount == 0) {
      return;
    }

    if (!session->startFreeReview()) {
      LOG_ERR("FCARD", "Could not start free review: %s", deckPath.c_str());
      freeReviewCount = 0;
      focusFreeReviewOnComplete = false;
      freeReviewPillRect = Rect{};
    } else {
      forcedRefreshPending = true;
      cardsUntilFullRefresh = SETTINGS.getRefreshFrequency();
      if (!loadActiveCardState()) {
        LOG_ERR("FCARD", "Failed loading first free-review card: %s", deckPath.c_str());
        failed = true;
      }
    }
  }

  if (failed) {
    exitToDeckList();
    return;
  }

  requestUpdate();
}

void FlashcardsReviewActivity::advanceToNextSession() {
  bool failed = false;
  {
    RenderLock lock(*this);
    if (!session || !session->isOpen() || state != State::SessionComplete) {
      return;
    }
    if (session->totalDeckCards() == 0) {
      return;
    }

    if (!session->startNextSession()) {
      LOG_ERR("FCARD", "Failed advancing to next session: %s", deckPath.c_str());
      failed = true;
    } else if (!session->isSessionComplete() && session->remainingCount() > 0) {
      forcedRefreshPending = true;
      cardsUntilFullRefresh = SETTINGS.getRefreshFrequency();
      if (!loadActiveCardState()) {
        failed = true;
      } else {
        // Reset under the lock so a render of the new card cannot race the flag.
        reviewStateSaved = false;
        firstReviewFrameRendered.store(false, std::memory_order_release);
      }
    } else {
      forcedRefreshPending = true;
      populateSessionCompleteMetrics();
    }
  }

  if (failed) {
    exitToDeckList();
    return;
  }

  requestUpdate();
}

bool FlashcardsReviewActivity::stepAnswerPage(const int delta) {
  bool changed = false;
  {
    RenderLock lock(*this);
    if (state != State::CardBack || answerPages.size() <= 1) {
      return false;
    }
    const int cur = static_cast<int>(extraPage);
    const int maxPage = static_cast<int>(answerPages.size()) - 1;
    const int next = std::clamp(cur + delta, 0, maxPage);
    if (next != cur) {
      extraPage = static_cast<uint16_t>(next);
      changed = true;
    }
  }
  if (changed) {
    requestUpdate();
  }
  return changed;
}

void FlashcardsReviewActivity::prewarmActiveCardGlyphs() const {
  if (!session || !session->isOpen()) {
    return;
  }

  renderer.prewarmFallbackText(UI_12_FONT_ID, session->deckTitle(), EpdFontFamily::BOLD);

  const auto ui10Getter = [](const void* ctx, const uint32_t index) -> const char* {
    const auto* self = static_cast<const FlashcardsReviewActivity*>(ctx);
    switch (index) {
      case 0:
        return self->session ? self->session->deckTitle() : nullptr;
      case 1:
        return self->currentCardData.hint.empty() ? nullptr : self->currentCardData.hint.c_str();
      case 2:
        return self->currentCardData.extra.empty() ? nullptr : self->currentCardData.extra.c_str();
      default:
        return nullptr;
    }
  };
  renderer.prewarmFallbackText(UI_10_FONT_ID, ui10Getter, this, 3, EpdFontFamily::REGULAR);

  const int readerFontId = SETTINGS.getReaderFontId();
  constexpr uint8_t boldAndRegularMask = static_cast<uint8_t>((1u << static_cast<uint8_t>(EpdFontFamily::REGULAR)) |
                                                              (1u << static_cast<uint8_t>(EpdFontFamily::BOLD)));
  if (!currentCardData.front.empty()) {
    renderer.ensureSdCardFontReady(readerFontId, currentCardData.front.c_str(), boldAndRegularMask);
  }
  if (!currentCardData.back.empty()) {
    renderer.ensureSdCardFontReady(readerFontId, currentCardData.back.c_str(), boldAndRegularMask);
  }
}

void FlashcardsReviewActivity::ensureCardLayout(const Rect& promptBandRect, const int answerWidth,
                                                const int answerAvailHeight, const int paginationReserveHeight) {
  if (cardLayoutValid && layoutWidth == answerWidth && layoutHeight == answerAvailHeight) {
    return;
  }

  layoutWidth = answerWidth;
  layoutHeight = answerAvailHeight;
  promptLines.clear();
  hintLines.clear();
  answerLines.clear();
  answerPages.clear();

  // 1. Prompt band layout (identical in CardFront and CardBack).
  const bool isFree = session && session->isFreeReview();
  const bool isNew = !isFree && (currentSrs.state == static_cast<uint8_t>(flashcards::CardState::New));
  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int newBadgeReserve = isNew ? (smallLineH + 10) : 0;

  if (!currentCardData.hint.empty()) {
    bool hintOverflow = false;
    wrapParagraphLines(renderer, UI_10_FONT_ID, EpdFontFamily::REGULAR, currentCardData.hint, promptBandRect.width,
                       MAX_WRAPPED_HINT_LINES, hintLines, hintOverflow);
    if (hintOverflow && !hintLines.empty()) {
      hintLines.back() =
          renderer.truncatedText(UI_10_FONT_ID, hintLines.back().c_str(), promptBandRect.width, EpdFontFamily::REGULAR);
    }
  }
  const int hintLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int hintReserve = hintLines.empty() ? 0 : (static_cast<int>(hintLines.size()) * hintLineH + 6);
  const int frontAvailH = std::max(20, promptBandRect.height - newBadgeReserve - hintReserve);

  const int baseReaderFontId = SETTINGS.getReaderFontId();
  // Scaled variants load only after the previous candidate overflowed.
  int candidates[5] = {};
  promptFontId = baseReaderFontId;
  bool promptOverflow = false;
  for (size_t i = 0; i < 5; ++i) {
    switch (i) {
      case 0:
        candidates[i] = baseReaderFontId;
        break;
      case 1:
        candidates[i] = resolveScaledReaderFont(renderer, baseReaderFontId, 0.85f);
        break;
      case 2:
        candidates[i] = resolveScaledReaderFont(renderer, baseReaderFontId, 0.70f);
        break;
      case 3:
        candidates[i] = UI_12_FONT_ID;
        break;
      default:
        candidates[i] = UI_10_FONT_ID;
        break;
    }
    const int cand = candidates[i];
    if (cand == 0) continue;
    bool duplicate = false;
    for (size_t prev = 0; prev < i; ++prev) {
      if (candidates[prev] == cand) {
        duplicate = true;
        break;
      }
    }
    if (duplicate || !renderer.ensureFontLoaded(cand)) continue;

    renderer.ensureSdCardFontReady(cand, currentCardData.front.c_str(),
                                   static_cast<uint8_t>(1u << static_cast<uint8_t>(EpdFontFamily::BOLD)));
    const int lineH = std::max(1, renderer.getLineHeight(cand));
    const size_t maxLines =
        std::min<size_t>(MAX_WRAPPED_PROMPT_LINES, static_cast<size_t>(std::max(1, frontAvailH / lineH)));

    promptFontId = cand;
    wrapParagraphLines(renderer, cand, EpdFontFamily::BOLD, currentCardData.front, promptBandRect.width, maxLines,
                       promptLines, promptOverflow);
    if (!promptOverflow) {
      break;
    }
  }

  if (promptOverflow && !promptLines.empty()) {
    promptLines.back() =
        renderer.truncatedText(promptFontId, promptLines.back().c_str(), promptBandRect.width, EpdFontFamily::BOLD);
  }

  // 2. Answer + Extra pagination layout.
  std::vector<std::string> wrappedScratch;
  wrappedScratch.reserve(16);
  bool answerOverflow = false;

  const int backFontId = baseReaderFontId;
  const int backLineH = std::max(1, renderer.getLineHeight(backFontId));
  wrapParagraphLines(renderer, backFontId, EpdFontFamily::REGULAR, currentCardData.back, answerWidth,
                     MAX_WRAPPED_ANSWER_LINES, wrappedScratch, answerOverflow);
  const size_t backReserve = std::min<size_t>(MAX_WRAPPED_ANSWER_LINES, wrappedScratch.size());
  if (answerLines.capacity() < backReserve) {
    answerLines.reserve(backReserve);
  }
  for (std::string& line : wrappedScratch) {
    if (answerLines.size() >= MAX_WRAPPED_ANSWER_LINES) break;
    answerLines.push_back(AnswerLine{std::move(line), backFontId, EpdFontFamily::REGULAR, backLineH, 0});
  }

  if (!currentCardData.extra.empty() && answerLines.size() < MAX_WRAPPED_ANSWER_LINES) {
    const size_t remLines = MAX_WRAPPED_ANSWER_LINES - answerLines.size();
    const int extraLineH = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
    wrapParagraphLines(renderer, UI_10_FONT_ID, EpdFontFamily::REGULAR, currentCardData.extra, answerWidth, remLines,
                       wrappedScratch, answerOverflow);
    const size_t totalReserve = std::min<size_t>(MAX_WRAPPED_ANSWER_LINES, answerLines.size() + wrappedScratch.size());
    if (answerLines.capacity() < totalReserve) {
      answerLines.reserve(totalReserve);
    }
    bool firstExtra = true;
    for (std::string& line : wrappedScratch) {
      if (answerLines.size() >= MAX_WRAPPED_ANSWER_LINES) break;
      const int gap = (firstExtra && !answerLines.empty()) ? 8 : 0;
      firstExtra = false;
      answerLines.push_back(AnswerLine{std::move(line), UI_10_FONT_ID, EpdFontFamily::REGULAR, extraLineH, gap});
    }
  }

  int singlePageHeight = 0;
  for (size_t i = 0; i < answerLines.size(); ++i) {
    if (i > 0) singlePageHeight += answerLines[i].gapBefore;
    singlePageHeight += answerLines[i].lineHeight;
  }

  if (answerLines.empty()) {
    answerPages.reserve(1);
    answerPages.push_back(AnswerPageSpan{0, 0});
  } else if (singlePageHeight <= answerAvailHeight) {
    answerPages.reserve(1);
    answerPages.push_back(AnswerPageSpan{0, static_cast<uint16_t>(answerLines.size())});
  } else {
    const size_t estPages = answerLines.size() / 2 + 1;
    if (answerPages.capacity() < estPages) {
      answerPages.reserve(estPages);
    }
    const int pagedAvailH = std::max(backLineH, answerAvailHeight - paginationReserveHeight);
    uint16_t idx = 0;
    const auto total = static_cast<uint16_t>(answerLines.size());
    while (idx < total) {
      int usedH = answerLines[idx].lineHeight;
      uint16_t count = 1;
      while (idx + count < total) {
        const AnswerLine& nextLine = answerLines[idx + count];
        const int nextCost = nextLine.gapBefore + nextLine.lineHeight;
        if (usedH + nextCost > pagedAvailH) {
          break;
        }
        usedH += nextCost;
        ++count;
      }
      answerPages.push_back(AnswerPageSpan{idx, count});
      idx = static_cast<uint16_t>(idx + count);
    }
  }

  if (!answerPages.empty() && extraPage >= answerPages.size()) {
    extraPage = static_cast<uint16_t>(answerPages.size() - 1);
  }
  cardLayoutValid = true;
}

void FlashcardsReviewActivity::loop() {
  if (loadFailed) {
    return;
  }

  saveSleepResumeStateIfReady();

  if (consumeReleaseWithGuard(mappedInput, MappedInputManager::Button::Back, suppressBackRelease)) {
    exitToDeckList();
    return;
  }

  State currentState = State::CardFront;
  {
    RenderLock lock(*this);
    currentState = state;
  }

  switch (currentState) {
    case State::CardFront: {
      if (consumeReleaseWithGuard(mappedInput, MappedInputManager::Button::Confirm, suppressConfirmRelease) ||
          mappedInput.wasReleased(MappedInputManager::Button::PageForward) ||
          mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        flipToBack();
        return;
      }

      if (mappedInput.hasTouch()) {
        int tx = 0;
        int ty = 0;
        if (mappedInput.wasScreenTapped(tx, ty)) {
          bool hit = false;
          {
            RenderLock lock(*this);
            hit = rectContains(showAnswerPillRect, tx, ty, TOUCH_HIT_PAD_Y) || rectContains(cardTapBodyRect, tx, ty, 0);
          }
          if (hit) {
            haptic_feedback::touchAction();
            flipToBack();
            return;
          }
        }
      }
      break;
    }

    case State::CardBack: {
      if (consumeReleaseWithGuard(mappedInput, MappedInputManager::Button::Confirm, suppressConfirmRelease)) {
        bool isFree = false;
        bool repeatAgain = false;
        flashcards::Rating chosen = flashcards::Rating::Good;
        {
          RenderLock lock(*this);
          isFree = session && session->isFreeReview();
          repeatAgain = !freeReviewFocusGotIt;
          chosen = focusedRating;
        }
        if (isFree) {
          submitFreeReviewChoice(repeatAgain);
        } else {
          submitActiveRating(chosen);
        }
        return;
      }

      const bool swapFront = mappedInput.isNavDirectionSwapped();
      const auto prevRatingBtn = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
      const auto nextRatingBtn = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
      int ratingDelta = 0;
      buttonNavigator.onPressAndContinuous({prevRatingBtn}, [&ratingDelta] { ratingDelta = -1; });
      buttonNavigator.onPressAndContinuous({nextRatingBtn}, [&ratingDelta] { ratingDelta = 1; });
      if (ratingDelta != 0) {
        stepFocusedRating(ratingDelta);
        return;
      }

      if (mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
        stepAnswerPage(-1);
        return;
      }
      if (mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
        stepAnswerPage(1);
        return;
      }

      if (mappedInput.hasTouch()) {
        const auto swipe = mappedInput.wasSwipe();
        if (swipe == MappedInputManager::SwipeDir::Up) {
          if (stepAnswerPage(1)) {
            haptic_feedback::touchAction();
          }
          return;
        }
        if (swipe == MappedInputManager::SwipeDir::Down) {
          if (stepAnswerPage(-1)) {
            haptic_feedback::touchAction();
          }
          return;
        }

        int tx = 0;
        int ty = 0;
        if (mappedInput.wasScreenTapped(tx, ty)) {
          bool isFree = false;
          int hitIndex = -1;
          {
            RenderLock lock(*this);
            isFree = session && session->isFreeReview();
            const int pillCount = isFree ? 2 : 4;
            for (int i = 0; i < pillCount; ++i) {
              if (rectContains(ratingPillRects[i], tx, ty, TOUCH_HIT_PAD_Y + 12)) {
                hitIndex = i;
                break;
              }
            }
          }
          if (hitIndex >= 0) {
            haptic_feedback::touchAction();
            if (isFree) {
              submitFreeReviewChoice(hitIndex == 0);
            } else {
              submitActiveRating(RATING_SPECS[hitIndex].rating);
            }
            return;
          }
        }
      }
      break;
    }

    case State::SessionComplete: {
      if (consumeReleaseWithGuard(mappedInput, MappedInputManager::Button::Confirm, suppressConfirmRelease)) {
        bool startFree = false;
        {
          RenderLock lock(*this);
          startFree = (freeReviewCount > 0 && focusFreeReviewOnComplete);
        }
        if (startFree) {
          beginFreeReview();
        } else {
          advanceToNextSession();
        }
        return;
      }

      const bool swapFront = mappedInput.isNavDirectionSwapped();
      const auto prevBtn = swapFront ? MappedInputManager::Button::Right : MappedInputManager::Button::Left;
      const auto nextBtn = swapFront ? MappedInputManager::Button::Left : MappedInputManager::Button::Right;
      int focusDelta = 0;
      buttonNavigator.onPressAndContinuous({prevBtn}, [&focusDelta] { focusDelta = -1; });
      buttonNavigator.onPressAndContinuous({nextBtn}, [&focusDelta] { focusDelta = 1; });
      if (focusDelta != 0) {
        stepCompleteFocus(focusDelta);
        return;
      }

      if (mappedInput.hasTouch()) {
        int tx = 0;
        int ty = 0;
        if (mappedInput.wasScreenTapped(tx, ty)) {
          bool hitFree = false;
          bool hitNext = false;
          {
            RenderLock lock(*this);
            hitFree = (freeReviewCount > 0) && rectContains(freeReviewPillRect, tx, ty, TOUCH_HIT_PAD_Y);
            hitNext = !hitFree && rectContains(nextSessionPillRect, tx, ty, TOUCH_HIT_PAD_Y);
          }
          if (hitFree) {
            haptic_feedback::touchAction();
            beginFreeReview();
            return;
          }
          if (hitNext) {
            haptic_feedback::touchAction();
            advanceToNextSession();
            return;
          }
        }
      }
      break;
    }
  }
}

void FlashcardsReviewActivity::renderPromptBand(const Rect& promptBandRect) const {
  const bool isFree = session && session->isFreeReview();
  const bool isNew = !isFree && (currentSrs.state == static_cast<uint8_t>(flashcards::CardState::New));
  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int newPillH = smallLineH + 6;
  const int promptLineH = std::max(1, renderer.getLineHeight(promptFontId));
  const int hintLineH = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));

  int blockHeight = static_cast<int>(promptLines.size()) * promptLineH;
  if (isNew) {
    blockHeight += newPillH + 6;
  }
  if (!hintLines.empty()) {
    blockHeight += 6 + static_cast<int>(hintLines.size()) * hintLineH;
  }

  int y = promptBandRect.y + std::max(0, (promptBandRect.height - blockHeight) / 2);

  if (isNew) {
    constexpr const char* NEW_LABEL = "NEW";
    const int labelW = renderer.getTextWidth(SMALL_FONT_ID, NEW_LABEL, EpdFontFamily::REGULAR);
    const int badgeW = labelW + 16;
    const int badgeX = promptBandRect.x + (promptBandRect.width - badgeW) / 2;
    renderer.drawRoundedRect(badgeX, y, badgeW, newPillH, 1, 6, true);
    renderer.drawText(SMALL_FONT_ID, badgeX + (badgeW - labelW) / 2, y + (newPillH - smallLineH) / 2, NEW_LABEL, true,
                      EpdFontFamily::REGULAR);
    y += newPillH + 6;
  }

  for (const std::string& line : promptLines) {
    if (!line.empty()) {
      UITheme::drawCenteredText(renderer, promptBandRect, promptFontId, y, line.c_str(), true, EpdFontFamily::BOLD);
    }
    y += promptLineH;
  }

  if (!hintLines.empty()) {
    y += 6;
    for (const std::string& line : hintLines) {
      if (!line.empty()) {
        UITheme::drawCenteredText(renderer, promptBandRect, UI_10_FONT_ID, y, line.c_str(), true,
                                  EpdFontFamily::REGULAR);
      }
      y += hintLineH;
    }
  }
}

void FlashcardsReviewActivity::scanPromptBandGlyphs() const {
  for (const std::string& line : promptLines) {
    if (!line.empty()) {
      renderer.drawText(promptFontId, 0, 0, line.c_str(), true, EpdFontFamily::BOLD);
    }
  }
  for (const std::string& line : hintLines) {
    if (!line.empty()) {
      renderer.drawText(UI_10_FONT_ID, 0, 0, line.c_str(), true, EpdFontFamily::REGULAR);
    }
  }
}

void FlashcardsReviewActivity::scanAnswerPageGlyphs(const uint16_t pageIdx) const {
  if (pageIdx >= answerPages.size()) {
    return;
  }
  const AnswerPageSpan span = answerPages[pageIdx];
  for (uint16_t i = 0; i < span.lineCount; ++i) {
    const AnswerLine& al = answerLines[span.startLine + i];
    if (!al.text.empty()) {
      renderer.drawText(al.fontId, 0, 0, al.text.c_str(), true, al.style);
    }
  }
}

void FlashcardsReviewActivity::renderCardFront(const Rect& safeArea, const Rect& headerRect) {
  char rightLabel[32];
  if (session->isFreeReview()) {
    snprintf(rightLabel, sizeof(rightLabel), "Free | %u left", static_cast<unsigned>(session->remainingCount()));
  } else {
    snprintf(rightLabel, sizeof(rightLabel), "%u left", static_cast<unsigned>(session->remainingCount()));
  }
  GUI.drawHeader(renderer, headerRect, session->deckTitle(), rightLabel, true);

  const ReviewLayout layout = computeReviewLayout(renderer, safeArea, headerRect);
  const int pageReserve = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  ensureCardLayout(layout.promptBand, layout.answerArea.width, layout.answerArea.height, pageReserve);

  if (auto* fc = renderer.getFontCacheManager()) {
    auto scope = fc->createPrewarmScope();
    scanPromptBandGlyphs();
    scope.endScanAndPrewarm();
    renderPromptBand(layout.promptBand);
  } else {
    renderPromptBand(layout.promptBand);
  }

  showAnswerPillRect = Rect{layout.body.x, layout.pillY, layout.body.width, layout.pillHeight};
  cardTapBodyRect = Rect{layout.body.x, layout.body.y, layout.body.width, layout.pillY - layout.body.y};
  drawFocusPill(renderer, showAnswerPillRect, "Show answer", UI_10_FONT_ID, true);

  const auto labels = mappedInput.mapLabels("Back", "Show", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardsReviewActivity::renderCardBack(const Rect& safeArea, const Rect& headerRect) {
  char rightLabel[32];
  if (session->isFreeReview()) {
    snprintf(rightLabel, sizeof(rightLabel), "Free | %u left", static_cast<unsigned>(session->remainingCount()));
  } else {
    snprintf(rightLabel, sizeof(rightLabel), "%u left", static_cast<unsigned>(session->remainingCount()));
  }
  GUI.drawHeader(renderer, headerRect, session->deckTitle(), rightLabel, true);

  const ReviewLayout layout = computeReviewLayout(renderer, safeArea, headerRect);
  const int smallLineH = renderer.getLineHeight(SMALL_FONT_ID);
  const int pageReserve = smallLineH + 4;
  ensureCardLayout(layout.promptBand, layout.answerArea.width, layout.answerArea.height, pageReserve);

  const uint16_t pageIdx =
      answerPages.empty() ? 0 : std::min<uint16_t>(extraPage, static_cast<uint16_t>(answerPages.size() - 1));

  const auto drawCardContent = [this, &layout, smallLineH, pageReserve, pageIdx]() {
    renderPromptBand(layout.promptBand);

    const int divWidth = (layout.body.width * 2) / 3;
    const int divX = layout.body.x + (layout.body.width - divWidth) / 2;
    renderer.drawLine(divX, layout.dividerY, divX + divWidth - 1, layout.dividerY, true);

    if (!answerPages.empty()) {
      const AnswerPageSpan span = answerPages[pageIdx];
      const bool multiPage = answerPages.size() > 1;
      const int textAvailH = multiPage ? std::max(1, layout.answerArea.height - pageReserve) : layout.answerArea.height;

      int pageBlockH = 0;
      for (uint16_t i = 0; i < span.lineCount; ++i) {
        const AnswerLine& al = answerLines[span.startLine + i];
        if (i > 0) pageBlockH += al.gapBefore;
        pageBlockH += al.lineHeight;
      }

      int y = layout.answerArea.y + std::max(0, (textAvailH - pageBlockH) / 2);
      for (uint16_t i = 0; i < span.lineCount; ++i) {
        const AnswerLine& al = answerLines[span.startLine + i];
        if (i > 0) y += al.gapBefore;
        if (!al.text.empty()) {
          UITheme::drawCenteredText(renderer, layout.answerArea, al.fontId, y, al.text.c_str(), true, al.style);
        }
        y += al.lineHeight;
      }

      if (multiPage) {
        char pageBuf[24];
        snprintf(pageBuf, sizeof(pageBuf), "Note %u/%u", static_cast<unsigned>(pageIdx + 1),
                 static_cast<unsigned>(answerPages.size()));
        UITheme::drawCenteredText(renderer, layout.answerArea, SMALL_FONT_ID,
                                  layout.answerArea.y + layout.answerArea.height - smallLineH, pageBuf, true,
                                  EpdFontFamily::REGULAR);
      }
    }
  };

  if (auto* fc = renderer.getFontCacheManager()) {
    auto scope = fc->createPrewarmScope();
    scanPromptBandGlyphs();
    scanAnswerPageGlyphs(pageIdx);
    scope.endScanAndPrewarm();
    drawCardContent();
  } else {
    drawCardContent();
  }

  if (session->isFreeReview()) {
    UITheme::drawCenteredText(renderer, layout.body, SMALL_FONT_ID, layout.previewLabelY, "Free review - SRS unchanged",
                              true, EpdFontFamily::REGULAR);

    constexpr int pillGap = 8;
    const int pillWidth = std::max(24, (layout.body.width - pillGap) / 2);
    const int totalBarWidth = pillWidth * 2 + pillGap;
    const int startX = layout.body.x + (layout.body.width - totalBarWidth) / 2;

    ratingPillRects[0] = Rect{startX, layout.pillY, pillWidth, layout.pillHeight};
    ratingPillRects[1] = Rect{startX + pillWidth + pillGap, layout.pillY, pillWidth, layout.pillHeight};
    ratingPillRects[2] = Rect{};
    ratingPillRects[3] = Rect{};

    drawFocusPillTruncated(renderer, ratingPillRects[0], "Again", UI_10_FONT_ID, !freeReviewFocusGotIt);
    drawFocusPillTruncated(renderer, ratingPillRects[1], "Got It", UI_10_FONT_ID, freeReviewFocusGotIt);

    const auto labels = mappedInput.mapLabels("Back", freeReviewFocusGotIt ? "Got It" : "Again", "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }

  constexpr int pillGap = 8;
  const int pillWidth = std::max(24, (layout.body.width - pillGap * 3) / 4);
  const int totalBarWidth = pillWidth * 4 + pillGap * 3;
  const int startX = layout.body.x + (layout.body.width - totalBarWidth) / 2;

  const uint16_t previewDays[4] = {0, currentPreview.hardDays, currentPreview.goodDays, currentPreview.easyDays};
  char ivlBuf[16];
  for (int i = 0; i < 4; ++i) {
    const int px = startX + i * (pillWidth + pillGap);
    ratingPillRects[i] = Rect{px, layout.pillY, pillWidth, layout.pillHeight};

    const char* ivlLabel = "Again";
    if (i > 0) {
      snprintf(ivlBuf, sizeof(ivlBuf), "%u d", static_cast<unsigned>(previewDays[i]));
      ivlLabel = ivlBuf;
    }
    const int ivlW = renderer.getTextWidth(SMALL_FONT_ID, ivlLabel, EpdFontFamily::REGULAR);
    renderer.drawText(SMALL_FONT_ID, px + (pillWidth - ivlW) / 2, layout.previewLabelY, ivlLabel, true,
                      EpdFontFamily::REGULAR);

    const bool isFocused = (focusedRating == RATING_SPECS[i].rating);
    drawFocusPill(renderer, ratingPillRects[i], RATING_SPECS[i].label, UI_10_FONT_ID, isFocused);
  }

  const char* focusedLabel = RATING_SPECS[ratingToIndex(focusedRating)].label;
  const auto labels = mappedInput.mapLabels("Back", focusedLabel, "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardsReviewActivity::renderSessionComplete(const Rect& safeArea, const Rect& headerRect) {
  char textBuf[96];
  snprintf(textBuf, sizeof(textBuf), "S%u Done", static_cast<unsigned>(session->sessionDay()));
  GUI.drawHeader(renderer, headerRect, session->deckTitle(), textBuf, true);

  const int sidePad = UITheme::getInstance().getMetrics().contentSidePadding;
  const int bodyTop = std::max(safeArea.y, headerRect.y + headerRect.height);
  const int bodyBottom = safeArea.y + safeArea.height;
  const int bodyX = safeArea.x + sidePad;
  const int bodyWidth = std::max(40, safeArea.width - sidePad * 2);
  const int bodyHeight = std::max(80, bodyBottom - bodyTop);
  const Rect bodyRect{bodyX, bodyTop, bodyWidth, bodyHeight};

  constexpr int bottomPad = 12;
  const int pillHeight = std::clamp(bodyHeight / 11, 42, 50);
  const int pillY = bodyBottom - bottomPad - pillHeight;

  const int summaryAvailH = std::max(80, pillY - 10 - bodyTop);
  const int circleRadius = (summaryAvailH < 340) ? 18 : 24;
  const int titleLineH = renderer.getLineHeight(UI_12_FONT_ID);
  const int subLineH = renderer.getLineHeight(UI_10_FONT_ID);
  const int boxHeight = subLineH * 2 + 12;
  const int gap1 = (summaryAvailH < 340) ? 10 : 16;
  const int gap2 = 6;
  const int gap3 = (summaryAvailH < 340) ? 12 : 20;
  const int totalSummaryH = circleRadius * 2 + gap1 + titleLineH + gap2 + subLineH + gap3 + boxHeight;

  int y = bodyTop + std::max(4, (summaryAvailH - totalSummaryH) / 2);
  const int cx = bodyX + bodyWidth / 2;
  drawCheckmarkCircle(renderer, cx, y + circleRadius, circleRadius);
  y += circleRadius * 2 + gap1;

  if (session->totalDeckCards() == 0) {
    UITheme::drawCenteredText(renderer, bodyRect, UI_12_FONT_ID, y, "Empty Deck", true, EpdFontFamily::BOLD);
    y += titleLineH + gap2;
    UITheme::drawCenteredText(renderer, bodyRect, UI_10_FONT_ID, y, "No valid cards found in this .jsonl file.", true,
                              EpdFontFamily::REGULAR);

    const auto labels = mappedInput.mapLabels("Decks", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    freeReviewPillRect = Rect{};
    nextSessionPillRect = Rect{};
    return;
  }

  if (session->lastPassWasFreeReview()) {
    UITheme::drawCenteredText(renderer, bodyRect, UI_12_FONT_ID, y, "Free Review Complete", true, EpdFontFamily::BOLD);
    y += titleLineH + gap2;

    const uint32_t freeReviewed = session->freeReviewRatingsCount();
    snprintf(textBuf, sizeof(textBuf), "Reviewed %u %s (SRS unchanged)", static_cast<unsigned>(freeReviewed),
             freeReviewed == 1 ? "review" : "reviews");
    drawCenteredLineTruncated(renderer, bodyRect, UI_10_FONT_ID, y, textBuf);
    y += subLineH + gap3;
  } else {
    snprintf(textBuf, sizeof(textBuf), "Session %u Complete", static_cast<unsigned>(session->sessionDay()));
    UITheme::drawCenteredText(renderer, bodyRect, UI_12_FONT_ID, y, textBuf, true, EpdFontFamily::BOLD);
    y += titleLineH + gap2;

    const uint32_t reviewed = session->reviewsThisSession();
    if (reviewed == 0) {
      snprintf(textBuf, sizeof(textBuf), "No cards due this session");
    } else {
      snprintf(textBuf, sizeof(textBuf), "Reviewed %u %s", static_cast<unsigned>(reviewed),
               reviewed == 1 ? "rating" : "ratings");
    }
    drawCenteredLineTruncated(renderer, bodyRect, UI_10_FONT_ID, y, textBuf);
    y += subLineH + gap3;
  }

  const Rect boxRect{bodyX, y, bodyWidth, boxHeight};
  renderer.drawRoundedRect(boxRect.x, boxRect.y, boxRect.width, boxRect.height, 1, 8, true);
  snprintf(textBuf, sizeof(textBuf), "Up Next (Session %u): %u due, %u new", static_cast<unsigned>(nextSessionDay),
           static_cast<unsigned>(nextDueCount), static_cast<unsigned>(nextNewCount));
  const Rect innerBox{boxRect.x + 10, boxRect.y + 4, std::max(10, boxRect.width - 20),
                      std::max(10, boxRect.height - 8)};
  if (nextDueCount == 0 && nextNewCount == 0 && static_cast<uint32_t>(nextScheduledDueDay) > nextSessionDay) {
    const int twoLineTop = innerBox.y + std::max(0, (innerBox.height - subLineH * 2) / 2);
    drawCenteredLineTruncated(renderer, innerBox, UI_10_FONT_ID, twoLineTop, textBuf);
    snprintf(textBuf, sizeof(textBuf), "Next due: Session %u", static_cast<unsigned>(nextScheduledDueDay));
    drawCenteredLineTruncated(renderer, innerBox, UI_10_FONT_ID, twoLineTop + subLineH, textBuf);
  } else {
    UITheme::drawCenteredWrappedText(renderer, innerBox, UI_10_FONT_ID, textBuf, 2, true, EpdFontFamily::REGULAR,
                                     UITheme::TextVerticalAlignment::CENTER);
  }

  if (freeReviewCount > 0) {
    constexpr int pillGap = 8;
    const int halfPillWidth = std::max(24, (bodyWidth - pillGap) / 2);
    const int totalPillsWidth = halfPillWidth * 2 + pillGap;
    const int startX = bodyX + (bodyWidth - totalPillsWidth) / 2;
    freeReviewPillRect = Rect{startX, pillY, halfPillWidth, pillHeight};
    nextSessionPillRect = Rect{startX + halfPillWidth + pillGap, pillY, halfPillWidth, pillHeight};

    drawFocusPillTruncated(renderer, freeReviewPillRect, "Practice Again", UI_10_FONT_ID, focusFreeReviewOnComplete);
    snprintf(textBuf, sizeof(textBuf), "Start Session %u", static_cast<unsigned>(nextSessionDay));
    drawFocusPillTruncated(renderer, nextSessionPillRect, textBuf, UI_10_FONT_ID, !focusFreeReviewOnComplete);

    const auto labels = mappedInput.mapLabels("Decks", focusFreeReviewOnComplete ? "Practice" : "Start", "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    freeReviewPillRect = Rect{};
    nextSessionPillRect = Rect{bodyX, pillY, bodyWidth, pillHeight};
    snprintf(textBuf, sizeof(textBuf), "Start Session %u Now", static_cast<unsigned>(nextSessionDay));
    drawFocusPillTruncated(renderer, nextSessionPillRect, textBuf, UI_10_FONT_ID, true);

    const auto labels = mappedInput.mapLabels("Decks", "Start", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
}

void FlashcardsReviewActivity::render(RenderLock&&) {
  if (loadFailed || !session || !session->isOpen()) {
    return;
  }

  renderer.clearScreen();

  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect headerRect = UITheme::getInstance().getHeaderRect(renderer, true, false);

  switch (state) {
    case State::CardFront:
      renderCardFront(safeArea, headerRect);
      break;
    case State::CardBack:
      renderCardBack(safeArea, headerRect);
      break;
    case State::SessionComplete:
      renderSessionComplete(safeArea, headerRect);
      break;
  }

  const HalDisplay::RefreshMode mode = forcedRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  forcedRefreshPending = false;
  renderer.displayBuffer(mode);

  if (state != State::SessionComplete) {
    firstReviewFrameRendered.store(true, std::memory_order_release);
  }
}
