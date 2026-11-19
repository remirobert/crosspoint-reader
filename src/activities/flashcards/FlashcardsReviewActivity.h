#pragma once

#include <EpdFontFamily.h>
#include <FlashcardScheduler.h>
#include <FlashcardSession.h>
#include <FlashcardTypes.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class FlashcardsReviewActivity final : public Activity {
 public:
  explicit FlashcardsReviewActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string deckPath,
                                    bool allowFastInitialRefresh = false);

  void onEnter() override;
  void onExit() override;
  void prepareForSleep() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool handleForcedRefresh() override;
  bool handleHomeGesture() override;

 private:
  enum class State : uint8_t {
    CardFront,
    CardBack,
    SessionComplete,
  };

  struct AnswerLine {
    std::string text;
    int fontId = 0;
    EpdFontFamily::Style style = EpdFontFamily::REGULAR;
    int lineHeight = 0;
    int gapBefore = 0;
  };

  struct AnswerPageSpan {
    uint16_t startLine = 0;
    uint16_t lineCount = 0;
  };

  bool loadActiveCardState();
  void enterSessionCompleteState();
  void clearSleepResumeState();
  void saveSleepResumeStateIfReady();
  void exitToDeckList();
  void flipToBack();
  void stepFocusedRating(int delta);
  void submitActiveRating(flashcards::Rating rating);
  void advanceToNextSession();
  bool stepAnswerPage(int delta);
  void ensureCardLayout(const Rect& promptBandRect, int answerWidth, int answerAvailHeight,
                        int paginationReserveHeight);
  void prewarmActiveCardGlyphs() const;

  void renderCardFront(const Rect& safeArea, const Rect& headerRect);
  void renderCardBack(const Rect& safeArea, const Rect& headerRect);
  void renderSessionComplete(const Rect& safeArea, const Rect& headerRect);
  void renderPromptBand(const Rect& promptBandRect) const;
  void scanPromptBandGlyphs() const;
  void scanAnswerPageGlyphs(uint16_t pageIdx) const;

  std::string deckPath;
  bool allowFastInitialRefresh = false;
  std::unique_ptr<flashcards::FlashcardSession> session;
  State state = State::CardFront;

  flashcards::FlashcardItem currentCardData;
  flashcards::CardSrsRecord currentSrs;
  flashcards::IntervalPreview currentPreview;
  flashcards::Rating focusedRating = flashcards::Rating::Good;

  std::vector<std::string> promptLines;
  std::vector<std::string> hintLines;
  int promptFontId = 0;
  std::vector<AnswerLine> answerLines;
  std::vector<AnswerPageSpan> answerPages;
  uint16_t extraPage = 0;
  int layoutWidth = 0;
  int layoutHeight = 0;
  bool cardLayoutValid = false;

  uint16_t nextDueCount = 0;
  uint16_t nextNewCount = 0;
  uint32_t nextSessionDay = 1;

  int cardsUntilFullRefresh = 0;
  bool forcedRefreshPending = false;
  bool suppressConfirmRelease = false;
  bool suppressBackRelease = false;
  bool loadFailed = false;
  bool reviewStateSaved = false;
  std::atomic<bool> firstReviewFrameRendered{false};

  ButtonNavigator buttonNavigator;

  Rect showAnswerPillRect;
  Rect cardTapBodyRect;
  Rect ratingPillRects[4];
  Rect nextSessionPillRect;
};
