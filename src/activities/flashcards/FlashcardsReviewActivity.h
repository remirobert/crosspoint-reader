#pragma once

#include <EpdFontFamily.h>
#include <FlashcardSession.h>
#include <FlashcardTypes.h>

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
                                    bool shuffleCards = true);

  void onEnter() override;
  void onExit() override;
  void prepareForSleep() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool handleForcedRefresh() override;

 private:
  enum class State : uint8_t {
    CardFront,
    CardBack,
    DeckComplete,
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

  void clearHitRects() noexcept;
  bool loadActiveCardState();
  void enterDeckCompleteState();
  void exitToDeckList();
  void flipToBack();
  void stepFocusedChoice(int delta);
  void submitActiveChoice(bool repeatAgain);
  void restartDeckPass();
  bool stepAnswerPage(int delta);
  void ensureCardLayout(const Rect& promptBandRect, int answerWidth, int answerAvailHeight,
                        int paginationReserveHeight);
  void prewarmActiveCardGlyphs() const;

  void renderCardFront(const Rect& safeArea, const Rect& headerRect);
  void renderCardBack(const Rect& safeArea, const Rect& headerRect);
  void renderDeckComplete(const Rect& safeArea, const Rect& headerRect);
  void renderPromptBand(const Rect& promptBandRect) const;
  void scanPromptBandGlyphs() const;
  void scanAnswerPageGlyphs(uint16_t pageIdx) const;

  std::string deckPath;
  bool shuffleCards = true;
  std::unique_ptr<flashcards::FlashcardSession> session;
  State state = State::CardFront;

  flashcards::FlashcardItem currentCardData;

  std::vector<std::string> promptLines;
  std::vector<std::string> hintLines;
  int promptFontId = 0;
  std::vector<AnswerLine> answerLines;
  std::vector<AnswerPageSpan> answerPages;
  uint16_t extraPage = 0;
  int layoutPromptHeight = 0;
  int layoutWidth = 0;
  int layoutHeight = 0;
  bool cardLayoutValid = false;

  int cardsUntilFullRefresh = 0;
  uint8_t appliedOrientation = 0;
  bool forcedRefreshPending = false;
  bool suppressConfirmRelease = false;
  bool suppressBackRelease = false;
  bool loadFailed = false;
  bool focusGotIt = true;

  ButtonNavigator buttonNavigator;

  Rect showAnswerPillRect;
  Rect cardTapBodyRect;
  Rect pillRects[2];
  Rect practiceAgainPillRect;
};
