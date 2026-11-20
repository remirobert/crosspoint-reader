#pragma once

#include <FlashcardSession.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

class FlashcardsDeckListActivity final : public Activity {
 public:
  explicit FlashcardsDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool handleForcedRefresh() override;

 private:
  struct DeckEntry {
    std::string filename;
    flashcards::DeckSummary summary{};
  };

  static constexpr size_t MAX_DECKS = 64;
  static constexpr size_t MAX_VISIBLE_ROWS = 16;
  static constexpr int PREWARM_WINDOW = 24;
  static constexpr unsigned long OPTION_LONG_PRESS_MS = 600;

  std::vector<DeckEntry> decks;
  std::unique_ptr<char[]> fileNameBuffer;
  OptionPopup optionPopup;
  ButtonNavigator buttonNavigator;

  // Bounded scratch buffers reused on the render task and popup setup (avoids stack pressure).
  char rowTitleBuf[128]{};
  char rowValueBuf[32]{};
  char activeSubtitleBuf[32]{};
  char popupTitleBuf[96]{};

  Rect visibleRowRects[MAX_VISIBLE_ROWS];
  int visibleRowIndices[MAX_VISIBLE_ROWS]{};
  int visibleRowCount = 0;
  int lastVisibleRows = 8;

  int selectedIndex = 0;
  int topIndex = 0;
  int prewarmedStart = -1;
  int prewarmedCount = 0;
  uint8_t appliedOrientation = 0;

  bool forcedRefreshPending = true;
  bool popupTitlePrewarmPending = false;
  bool suppressConfirmRelease = false;
  bool suppressBackRelease = false;
  bool confirmLongPressHandled = false;
  std::atomic<bool> baseFrameRendered{false};

  void drawDeckListFrame();
  void prewarmRowGlyphs(int start, int visibleRows);
  int hitTestVisibleRow(int x, int y) const noexcept;
  void moveSelectionTo(int index);

  void loadDecks();
  void refreshActiveSubtitle();
  bool refreshDeckSummary(size_t index);
  std::string fullDeckPath(size_t index) const;

  void openSelectedDeck();
  void showDeckOptions();
};
