#pragma once

#include <FlashcardSession.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"
#include "components/OptionPopup.h"

class FlashcardsDeckListActivity final : public UiListActivity {
 public:
  explicit FlashcardsDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void render(RenderLock&& lock) override;

 private:
  struct DeckEntry {
    std::string filename;
    flashcards::DeckSummary summary{};
  };

  static constexpr size_t MAX_DECKS = 64;
  static constexpr int PREWARM_WINDOW = 24;
  static constexpr unsigned long OPTION_LONG_PRESS_MS = 600;

  std::vector<DeckEntry> decks;
  std::unique_ptr<char[]> fileNameBuffer;
  OptionPopup optionPopup;

  // Bounded scratch buffers reused on the render task across provideRow() and drawChrome().
  char rowTitleBuf[128]{};
  char rowSubtitleBuf[48]{};
  char rowValueBuf[32]{};
  char activeSubtitleBuf[24]{};

  int prewarmedStart = -1;
  bool suppressConfirmRelease = false;
  bool suppressBackRelease = false;

  static void provideRow(void* ctx, uint16_t index, freeink::ui::ListItem& item);
  void prewarmRowGlyphs(int start);

  int listCount() const override { return static_cast<int>(decks.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  bool handleCustomInput() override;
  bool handleButtons() override;
  void drawChrome() override;
  void drawFooter() override;

  void loadDecks();
  void refreshActiveSubtitle();
  bool refreshDeckSummary(size_t index);
  std::string fullDeckPath(size_t index) const;

  void openSelectedDeck();
  void showDeckOptions();
  void startNextSessionAndOpen(size_t index);
  void cycleNewCardsPerSession(size_t index);
  void confirmResetDeck(size_t index);
};
