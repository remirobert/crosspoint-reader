#include "FlashcardsDeckListActivity.h"

#include <FlashcardDeckIndex.h>
#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>

#include "CrossPointSettings.h"
#include "FlashcardsReviewActivity.h"
#include "HapticFeedback.h"
#include "MappedInputManager.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/reader/ReaderUtils.h"
#include "components/HeaderBackTapTarget.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* FLASHCARDS_DIR_PATH = "/flashcards";
constexpr const char* FLASHCARDS_CACHE_DIR = "/.crosspoint/flashcards";
constexpr size_t NAME_BUFFER_SIZE = 500;

constexpr const char* HEADER_TITLE = "Flashcards";
constexpr const char* EMPTY_HEADING = "No decks found.";
constexpr const char* EMPTY_SUBTITLE = "Copy .jsonl files to /flashcards on the SD card.";

// Stateless practice order toggle: defaults to Shuffled on boot and persists in RAM across Deck List <-> Review.
bool gShuffleCardsEnabled = true;

void formatStemFallback(const std::string& filename, char* out, const size_t outSize) {
  if (!out || outSize == 0) return;
  const size_t dot = filename.rfind('.');
  const size_t len = (dot != std::string::npos && dot > 0) ? dot : filename.size();
  std::snprintf(out, outSize, "%.*s", static_cast<int>(len), filename.c_str());
}

void formatDeckTitle(const flashcards::DeckSummary& summary, const std::string& filename, char* out,
                     const size_t outSize) {
  if (!out || outSize == 0) return;
  if (summary.title[0] != '\0') {
    std::snprintf(out, outSize, "%s", summary.title);
  } else {
    formatStemFallback(filename, out, outSize);
  }
  utf8ComposeNfcInPlace(out);
}

bool consumeGuardedRelease(const MappedInputManager& input, const MappedInputManager::Button button, bool& guard) {
  if (!guard) {
    return false;
  }
  if (!input.isPressed(button)) {
    guard = false;
    return input.wasReleased(button);
  }
  return input.wasReleased(button);
}

}  // namespace

FlashcardsDeckListActivity::FlashcardsDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("FlashcardsDeckList", renderer, mappedInput) {}

void FlashcardsDeckListActivity::onEnter() {
  {
    RenderLock lock(*this);
    Activity::onEnter();

    suppressConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
    suppressBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);
    confirmLongPressHandled = false;

    sdFontSystem.ensureLoaded(renderer);
    appliedOrientation = SETTINGS.orientation;
    ReaderUtils::applyOrientation(renderer, appliedOrientation);

    selectedIndex = 0;
    topIndex = 0;
    visibleRowCount = 0;
    for (Rect& r : visibleRowRects) {
      r = Rect{};
    }
    forcedRefreshPending = true;
    popupTitlePrewarmPending = false;
    baseFrameRendered.store(false, std::memory_order_release);

    loadDecks();
  }
  requestUpdate();
}

void FlashcardsDeckListActivity::onExit() {
  optionPopup.dismiss();
  Activity::onExit();

  // ActivityManager::exitActivity holds RenderLock during onExit().
  if (auto* fc = renderer.getFontCacheManager()) {
    fc->releaseSdFontCaches();
  }

  decks.clear();
  shrinkToFit(decks);
  fileNameBuffer.reset();

  if (renderer.getOrientation() != GfxRenderer::Orientation::Portrait) {
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  }
}

bool FlashcardsDeckListActivity::handleForcedRefresh() {
  {
    RenderLock lock(*this);
    forcedRefreshPending = true;
  }
  requestUpdate();
  return true;
}

std::string FlashcardsDeckListActivity::fullDeckPath(const size_t index) const {
  if (index >= decks.size()) return {};
  std::string path;
  path.reserve(std::strlen(FLASHCARDS_DIR_PATH) + 1 + decks[index].filename.size());
  path.append(FLASHCARDS_DIR_PATH);
  path.push_back('/');
  path.append(decks[index].filename);
  return path;
}

void FlashcardsDeckListActivity::refreshActiveSubtitle() {
  const unsigned count = static_cast<unsigned>(decks.size());
  std::snprintf(activeSubtitleBuf, sizeof(activeSubtitleBuf), "%u %s | %s", count, count == 1u ? "deck" : "decks",
                gShuffleCardsEnabled ? "Shuffle" : "In Order");
}

bool FlashcardsDeckListActivity::refreshDeckSummary(const size_t index) {
  if (index >= decks.size()) return false;
  DeckEntry& entry = decks[index];
  entry.summary = flashcards::DeckSummary{};
  const std::string path = fullDeckPath(index);
  const bool ok = flashcards::FlashcardSession::inspectDeck(path.c_str(), FLASHCARDS_CACHE_DIR, entry.summary);
  if (!ok && entry.summary.title[0] == '\0') {
    formatStemFallback(entry.filename, entry.summary.title, sizeof(entry.summary.title));
  }
  prewarmedStart = -1;
  prewarmedCount = 0;
  refreshActiveSubtitle();
  return ok;
}

void FlashcardsDeckListActivity::loadDecks() {
  // Caller must hold RenderLock.
  HalPowerManager::Lock powerLock;

  decks.clear();
  prewarmedStart = -1;
  prewarmedCount = 0;

  if (!Storage.exists(FLASHCARDS_DIR_PATH) && !Storage.mkdir(FLASHCARDS_DIR_PATH)) {
    LOG_ERR("FCARD", "Failed creating directory %s", FLASHCARDS_DIR_PATH);
    refreshActiveSubtitle();
    return;
  }

  auto root = Storage.open(FLASHCARDS_DIR_PATH);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    refreshActiveSubtitle();
    return;
  }

  fileNameBuffer = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  if (!fileNameBuffer) {
    LOG_ERR("FCARD", "OOM allocating %u-byte filename buffer", static_cast<unsigned>(NAME_BUFFER_SIZE));
    root.close();
    refreshActiveSubtitle();
    return;
  }

  root.rewindDirectory();
  size_t entryCount = 0;
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    ++entryCount;
    file.close();
  }
  root.rewindDirectory();

  std::vector<std::string> names;
  const size_t reserveCount = std::min<size_t>(entryCount, MAX_DECKS);
  if (reserveCount > 0) {
    const size_t namesBytes = reserveCount * sizeof(std::string);
    auto probe = makeUniqueNoThrow<uint8_t[]>(namesBytes);
    if (!probe) {
      LOG_ERR("FCARD", "OOM reserving %u deck filename slots (%u B)", static_cast<unsigned>(reserveCount),
              static_cast<unsigned>(namesBytes));
      fileNameBuffer.reset();
      root.close();
      refreshActiveSubtitle();
      return;
    }
    probe.reset();
    names.reserve(reserveCount);
  }

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    const bool isDir = file.isDirectory();
    file.getName(fileNameBuffer.get(), NAME_BUFFER_SIZE);
    file.close();
    if (isDir || fileNameBuffer[0] == '\0' || fileNameBuffer[0] == '.') {
      continue;
    }
    const std::string_view nameView{fileNameBuffer.get()};
    if (!FsHelpers::checkFileExtension(nameView, ".jsonl")) {
      continue;
    }
    if (names.size() < MAX_DECKS) {
      names.emplace_back(nameView);
    }
  }
  root.close();
  fileNameBuffer.reset();

  FsHelpers::sortFileList(names);

  if (!names.empty()) {
    const size_t deckBytes = names.size() * sizeof(DeckEntry);
    auto probe = makeUniqueNoThrow<uint8_t[]>(deckBytes);
    if (!probe) {
      LOG_ERR("FCARD", "OOM reserving %u deck entries (%u B)", static_cast<unsigned>(names.size()),
              static_cast<unsigned>(deckBytes));
      refreshActiveSubtitle();
      return;
    }
    probe.reset();
    decks.reserve(names.size());
  }

  std::string fullPath;
  fullPath.reserve(96);

  // First open of a deck builds its .idx; show feedback once before the slow pass.
  {
    std::string idxPath;
    for (const std::string& name : names) {
      fullPath.assign(FLASHCARDS_DIR_PATH);
      fullPath.push_back('/');
      fullPath.append(name);
      flashcards::FlashcardDeckIndex::buildIndexPath(FLASHCARDS_CACHE_DIR, fullPath, idxPath);
      if (!Storage.exists(idxPath.c_str())) {
        GUI.drawPopup(renderer, "Indexing decks...");
        break;
      }
    }
  }

  for (std::string& name : names) {
    fullPath.assign(FLASHCARDS_DIR_PATH);
    fullPath.push_back('/');
    fullPath.append(name);

    decks.emplace_back();
    DeckEntry& entry = decks.back();
    entry.filename = std::move(name);
    if (!flashcards::FlashcardSession::inspectDeck(fullPath.c_str(), FLASHCARDS_CACHE_DIR, entry.summary) &&
        entry.summary.title[0] == '\0') {
      formatStemFallback(entry.filename, entry.summary.title, sizeof(entry.summary.title));
    }
  }

  refreshActiveSubtitle();
}

void FlashcardsDeckListActivity::prewarmRowGlyphs(const int start, const int visibleRows) {
  const int total = static_cast<int>(decks.size());
  if (total <= 0) return;

  const int neededEnd = std::min(total, start + std::max(1, visibleRows));
  if (prewarmedStart >= 0 && start >= prewarmedStart && neededEnd <= prewarmedStart + prewarmedCount) {
    return;
  }

  int clamped = start;
  if (clamped > total - PREWARM_WINDOW) clamped = total - PREWARM_WINDOW;
  if (clamped < 0) clamped = 0;
  const int count = std::min(total - clamped, PREWARM_WINDOW);
  prewarmedStart = clamped;
  prewarmedCount = count;

  struct PrewarmCtx {
    FlashcardsDeckListActivity* self;
    int first;
    int count;
  } prewarmCtx{this, clamped, count};

  renderer.prewarmFallbackText(
      UI_10_FONT_ID,
      [](const void* ctx, const uint32_t i) -> const char* {
        auto* c = const_cast<PrewarmCtx*>(static_cast<const PrewarmCtx*>(ctx));
        if (i < static_cast<uint32_t>(c->count)) {
          const DeckEntry& entry = c->self->decks[static_cast<size_t>(c->first) + i];
          formatDeckTitle(entry.summary, entry.filename, c->self->rowTitleBuf, sizeof(c->self->rowTitleBuf));
          return c->self->rowTitleBuf;
        }
        return "";
      },
      &prewarmCtx, static_cast<uint32_t>(count), EpdFontFamily::BOLD);
}

void FlashcardsDeckListActivity::drawDeckListFrame() {
  renderer.clearScreen();

  const Rect safeArea = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect headerRect = UITheme::getInstance().getHeaderRect(renderer, true, false);
  GUI.drawHeader(renderer, headerRect, HEADER_TITLE, decks.empty() ? nullptr : activeSubtitleBuf, true);

  visibleRowCount = 0;
  for (Rect& r : visibleRowRects) {
    r = Rect{};
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const int sidePad = metrics.contentSidePadding;
  const int contentTop = std::max(safeArea.y, headerRect.y + headerRect.height);
  const int contentBottom = safeArea.y + safeArea.height;
  const int bodyX = safeArea.x + sidePad;
  const int bodyWidth = std::max(80, safeArea.width - sidePad * 2);

  if (decks.empty()) {
    const int bodyHeight = std::max(40, contentBottom - contentTop);
    const int headingH = renderer.getLineHeight(UI_12_FONT_ID);
    const int subLineH = renderer.getLineHeight(UI_10_FONT_ID);
    constexpr int gap = 8;

    if (renderer.getTextWidth(UI_10_FONT_ID, EMPTY_SUBTITLE, EpdFontFamily::REGULAR) <= bodyWidth) {
      const int totalTextH = headingH + gap + subLineH;
      const int topY = contentTop + std::max(0, (bodyHeight - totalTextH) / 2);
      const Rect lineBounds{bodyX, topY, bodyWidth, totalTextH};
      UITheme::drawCenteredText(renderer, lineBounds, UI_12_FONT_ID, topY, EMPTY_HEADING, true, EpdFontFamily::BOLD);
      UITheme::drawCenteredText(renderer, lineBounds, UI_10_FONT_ID, topY + headingH + gap, EMPTY_SUBTITLE, true,
                                EpdFontFamily::REGULAR);
    } else {
      const auto subLines = renderer.wrappedText(UI_10_FONT_ID, EMPTY_SUBTITLE, bodyWidth, 3, EpdFontFamily::REGULAR);
      const int subBlockH = static_cast<int>(subLines.size()) * subLineH;
      const int totalTextH = headingH + gap + subBlockH;
      const int topY = contentTop + std::max(0, (bodyHeight - totalTextH) / 2);
      const Rect lineBounds{bodyX, topY, bodyWidth, totalTextH};
      UITheme::drawCenteredText(renderer, lineBounds, UI_12_FONT_ID, topY, EMPTY_HEADING, true, EpdFontFamily::BOLD);
      int lineY = topY + headingH + gap;
      for (const auto& line : subLines) {
        UITheme::drawCenteredText(renderer, lineBounds, UI_10_FONT_ID, lineY, line.c_str(), true,
                                  EpdFontFamily::REGULAR);
        lineY += subLineH;
      }
    }

    const auto labels = mappedInput.mapLabels("Home", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    baseFrameRendered.store(true, std::memory_order_release);
    return;
  }

  const int listTop = contentTop + 8;
  const int listBottom = contentBottom - 6;
  const int listAvailH = std::max(80, listBottom - listTop);

  const int titleLineH = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int rowGap = 6;
  const int naturalRowH = 12 + titleLineH + 12;
  const int minRowH = 8 + titleLineH + 8;

  int rowH = naturalRowH;
  if (listAvailH < 4 * naturalRowH + 3 * rowGap) {
    rowH = std::clamp((listAvailH - 3 * rowGap) / 4, minRowH, naturalRowH);
  }

  const int visibleRows = std::clamp((listAvailH + rowGap) / (rowH + rowGap), 1, static_cast<int>(MAX_VISIBLE_ROWS));
  lastVisibleRows = visibleRows;

  const int total = static_cast<int>(decks.size());
  selectedIndex = std::clamp(selectedIndex, 0, total - 1);
  if (selectedIndex < topIndex) {
    topIndex = selectedIndex;
  } else if (selectedIndex >= topIndex + visibleRows) {
    topIndex = selectedIndex - visibleRows + 1;
  }
  topIndex = std::clamp(topIndex, 0, std::max(0, total - visibleRows));

  prewarmRowGlyphs(topIndex, visibleRows);

  const bool hasScrollbar = total > visibleRows;
  constexpr int scrollWidth = 3;
  constexpr int scrollGap = 7;
  const int rowX = bodyX;
  const int rowWidth = hasScrollbar ? std::max(60, bodyWidth - scrollWidth - scrollGap) : bodyWidth;

  const int countToDraw = std::min(visibleRows, total - topIndex);
  for (int vis = 0; vis < countToDraw; ++vis) {
    const int idx = topIndex + vis;
    const int rowY = listTop + vis * (rowH + rowGap);
    const Rect rowRect{rowX, rowY, rowWidth, rowH};
    visibleRowRects[vis] = rowRect;
    visibleRowIndices[vis] = idx;
    ++visibleRowCount;

    renderer.fillRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, false);
    if (idx == selectedIndex) {
      renderer.drawRoundedRect(rowRect.x, rowRect.y, rowRect.width, rowRect.height, 2, 8, true);
    } else if (idx + 1 != selectedIndex) {
      const int divY = rowRect.y + rowRect.height + rowGap / 2;
      if (rowRect.width > 14) {
        renderer.drawLine(rowRect.x + 6, divY, rowRect.x + rowRect.width - 7, divY, true);
      }
    }

    const DeckEntry& entry = decks[static_cast<size_t>(idx)];
    const flashcards::DeckSummary& sum = entry.summary;

    constexpr int insetX = 14;
    const int innerLeft = rowRect.x + insetX;
    const int innerRight = rowRect.x + rowRect.width - insetX;
    const int innerW = std::max(24, rowRect.width - insetX * 2);
    const int textY = rowRect.y + (rowRect.height - titleLineH) / 2;

    std::snprintf(rowValueBuf, sizeof(rowValueBuf), "%u %s", static_cast<unsigned>(sum.totalCards),
                  sum.totalCards == 1u ? "card" : "cards");
    const int countW = renderer.getTextWidth(UI_10_FONT_ID, rowValueBuf, EpdFontFamily::REGULAR);
    renderer.drawText(UI_10_FONT_ID, std::max(innerLeft, innerRight - countW), textY, rowValueBuf, true,
                      EpdFontFamily::REGULAR);

    formatDeckTitle(sum, entry.filename, rowTitleBuf, sizeof(rowTitleBuf));
    const int maxTitleW = std::max(24, innerW - countW - 12);
    if (renderer.getTextWidth(UI_10_FONT_ID, rowTitleBuf, EpdFontFamily::BOLD) <= maxTitleW) {
      renderer.drawText(UI_10_FONT_ID, innerLeft, textY, rowTitleBuf, true, EpdFontFamily::BOLD);
    } else {
      const std::string clippedTitle =
          renderer.truncatedText(UI_10_FONT_ID, rowTitleBuf, maxTitleW, EpdFontFamily::BOLD);
      renderer.drawText(UI_10_FONT_ID, innerLeft, textY, clippedTitle.c_str(), true, EpdFontFamily::BOLD);
    }
  }

  if (hasScrollbar) {
    const int trackX = bodyX + bodyWidth - scrollWidth;
    const int trackY = listTop;
    const int trackH = visibleRows * rowH + (visibleRows - 1) * rowGap;
    renderer.fillRectDither(trackX, trackY, scrollWidth, trackH, Color::LightGray);
    const int maxTop = std::max(1, total - visibleRows);
    const int thumbH = std::max(16, (trackH * visibleRows) / total);
    const int thumbTravel = std::max(0, trackH - thumbH);
    const int thumbY = trackY + (thumbTravel * topIndex) / maxTop;
    renderer.fillRect(trackX, thumbY, scrollWidth, thumbH, true);
  }

  const auto labels = mappedInput.mapLabels("Home", "Open", "Up", "Down");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  baseFrameRendered.store(true, std::memory_order_release);
}

void FlashcardsDeckListActivity::render(RenderLock&& lock) {
  (void)lock;

  drawDeckListFrame();

  if (optionPopup.isActive()) {
    HeaderBackTapTarget::clear();
    if (popupTitlePrewarmPending) {
      popupTitlePrewarmPending = false;
      renderer.prewarmFallbackText(uiScaleSpec().bodyFontId, popupTitleBuf, EpdFontFamily::BOLD);
    }
    if (forcedRefreshPending) {
      forcedRefreshPending = false;
      renderer.promoteNextRefresh(HalDisplay::HALF_REFRESH);
    }
    if (optionPopup.processRender(renderer, mappedInput)) {
      return;
    }
  }

  const HalDisplay::RefreshMode refreshMode =
      forcedRefreshPending ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH;
  forcedRefreshPending = false;
  renderer.displayBuffer(refreshMode);
}

int FlashcardsDeckListActivity::hitTestVisibleRow(const int x, const int y) const noexcept {
  for (int i = 0; i < visibleRowCount && i < static_cast<int>(MAX_VISIBLE_ROWS); ++i) {
    const Rect& r = visibleRowRects[i];
    if (r.width > 0 && r.height > 0 && x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height) {
      return visibleRowIndices[i];
    }
  }
  return -1;
}

void FlashcardsDeckListActivity::moveSelectionTo(const int index) {
  bool changed = false;
  {
    RenderLock lock(*this);
    const int total = static_cast<int>(decks.size());
    if (total <= 0) return;
    const int clamped = std::clamp(index, 0, total - 1);
    if (clamped != selectedIndex) {
      selectedIndex = clamped;
      if (selectedIndex < topIndex) {
        topIndex = selectedIndex;
      } else if (selectedIndex >= topIndex + lastVisibleRows) {
        topIndex = selectedIndex - lastVisibleRows + 1;
      }
      topIndex = std::clamp(topIndex, 0, std::max(0, total - lastVisibleRows));
      changed = true;
    }
  }
  if (changed) {
    requestUpdate();
  }
}

void FlashcardsDeckListActivity::loop() {
  if (appliedOrientation != SETTINGS.orientation) {
    {
      RenderLock lock(*this);
      appliedOrientation = SETTINGS.orientation;
      ReaderUtils::applyOrientation(renderer, appliedOrientation);
      forcedRefreshPending = true;
      baseFrameRendered.store(false, std::memory_order_release);
    }
    requestUpdate();
  }

  if (consumeGuardedRelease(mappedInput, MappedInputManager::Button::Confirm, suppressConfirmRelease)) {
    return;
  }
  if (consumeGuardedRelease(mappedInput, MappedInputManager::Button::Confirm, confirmLongPressHandled)) {
    return;
  }
  if (consumeGuardedRelease(mappedInput, MappedInputManager::Button::Back, suppressBackRelease)) {
    return;
  }

  if (optionPopup.handleInput(mappedInput, [this] {
        if (!optionPopup.isActive()) {
          RenderLock lock(*this);
          forcedRefreshPending = true;
        }
        requestUpdate();
      })) {
    return;
  }

  if (!decks.empty() && !suppressConfirmRelease &&
      mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, OPTION_LONG_PRESS_MS)) {
    confirmLongPressHandled = true;
    showDeckOptions();
    return;
  }

  if (!suppressConfirmRelease && !confirmLongPressHandled &&
      mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedDeck();
    return;
  }

  if (!suppressBackRelease && mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::FLASHCARDS);
    return;
  }

  if (mappedInput.hasTouch() && !decks.empty() && baseFrameRendered.load(std::memory_order_acquire)) {
    int lx = 0;
    int ly = 0;
    if (mappedInput.wasScreenLongPress(lx, ly)) {
      int hitIdx = -1;
      {
        RenderLock lock(*this);
        hitIdx = hitTestVisibleRow(lx, ly);
        if (hitIdx >= 0 && hitIdx < static_cast<int>(decks.size())) {
          selectedIndex = hitIdx;
        }
      }
      if (hitIdx >= 0 && hitIdx < static_cast<int>(decks.size())) {
        haptic_feedback::touchAction(true);
        showDeckOptions();
      }
      return;
    }

    int tx = 0;
    int ty = 0;
    if (mappedInput.wasScreenTapped(tx, ty)) {
      int hitIdx = -1;
      {
        RenderLock lock(*this);
        hitIdx = hitTestVisibleRow(tx, ty);
        if (hitIdx >= 0 && hitIdx < static_cast<int>(decks.size())) {
          selectedIndex = hitIdx;
        }
      }
      if (hitIdx >= 0 && hitIdx < static_cast<int>(decks.size())) {
        haptic_feedback::touchAction();
        openSelectedDeck();
      }
      return;
    }

    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      bool scrolled = false;
      {
        RenderLock lock(*this);
        const int total = static_cast<int>(decks.size());
        const int pageRows = std::max(1, lastVisibleRows);
        const int delta = (swipe == MappedInputManager::SwipeDir::Up) ? pageRows : -pageRows;
        const int nextTop = std::clamp(topIndex + delta, 0, std::max(0, total - pageRows));
        const int maxSel = std::min(total - 1, nextTop + pageRows - 1);
        const int nextSel = std::clamp(selectedIndex, nextTop, std::max(nextTop, maxSel));
        if (nextTop != topIndex || nextSel != selectedIndex) {
          topIndex = nextTop;
          selectedIndex = nextSel;
          scrolled = true;
        }
      }
      if (scrolled) {
        requestUpdate();
      }
      return;
    }
  }

  if (decks.empty()) {
    return;
  }

  const int total = static_cast<int>(decks.size());
  buttonNavigator.onNextPress([this, total] { moveSelectionTo(ButtonNavigator::nextIndex(selectedIndex, total)); });
  buttonNavigator.onPreviousPress(
      [this, total] { moveSelectionTo(ButtonNavigator::previousIndex(selectedIndex, total)); });
  buttonNavigator.onNextContinuous([this, total] {
    moveSelectionTo(ButtonNavigator::nextPageIndex(selectedIndex, total, std::max(1, lastVisibleRows)));
  });
  buttonNavigator.onPreviousContinuous([this, total] {
    moveSelectionTo(ButtonNavigator::previousPageIndex(selectedIndex, total, std::max(1, lastVisibleRows)));
  });
}

void FlashcardsDeckListActivity::openSelectedDeck() {
  if (decks.empty() || selectedIndex < 0 || selectedIndex >= static_cast<int>(decks.size())) {
    return;
  }

  std::string path = fullDeckPath(static_cast<size_t>(selectedIndex));
  auto review =
      makeUniqueNoThrow<FlashcardsReviewActivity>(renderer, mappedInput, std::move(path), gShuffleCardsEnabled);
  if (!review) {
    LOG_ERR("FCARD", "OOM allocating %u-byte FlashcardsReviewActivity",
            static_cast<unsigned>(sizeof(FlashcardsReviewActivity)));
    return;
  }
  activityManager.replaceActivity(std::move(review));
}

void FlashcardsDeckListActivity::showDeckOptions() {
  if (decks.empty() || optionPopup.isActive() || selectedIndex < 0 || selectedIndex >= static_cast<int>(decks.size())) {
    return;
  }

  const size_t index = static_cast<size_t>(selectedIndex);
  const DeckEntry& entry = decks[index];
  {
    RenderLock lock(*this);
    formatDeckTitle(entry.summary, entry.filename, popupTitleBuf, sizeof(popupTitleBuf));
    popupTitlePrewarmPending = true;

    const char* options[2] = {
        gShuffleCardsEnabled ? "Card Order: Shuffled" : "Card Order: Original",
        "Rebuild Deck Index",
    };

    optionPopup.show(popupTitleBuf, options, 2, 0, [this, index](const int optionIdx) {
      if (optionIdx == 0) {
        {
          RenderLock cbLock(*this);
          gShuffleCardsEnabled = !gShuffleCardsEnabled;
          refreshActiveSubtitle();
          forcedRefreshPending = true;
        }
        requestUpdate();
      } else if (optionIdx == 1) {
        {
          RenderLock cbLock(*this);
          if (index >= decks.size()) return;
          HalPowerManager::Lock powerLock;
          const std::string path = fullDeckPath(index);
          if (!flashcards::FlashcardDeckIndex::removeIndex(FLASHCARDS_CACHE_DIR, path)) {
            LOG_ERR("FCARD", "Failed removing deck index for %s", path.c_str());
          }
          refreshDeckSummary(index);
          forcedRefreshPending = true;
        }
        requestUpdate();
      }
    });
  }
  requestUpdate();
}
