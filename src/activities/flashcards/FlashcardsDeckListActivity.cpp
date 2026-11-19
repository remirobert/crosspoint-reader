#include "FlashcardsDeckListActivity.h"

#include <FlashcardDeckIndex.h>
#include <FlashcardSrsStore.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include "CrossPointState.h"
#include "FlashcardsReviewActivity.h"
#include "MappedInputManager.h"
#include "activities/ActivityManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {

constexpr const char* FLASHCARDS_DIR_PATH = "/flashcards";
constexpr const char* FLASHCARDS_CACHE_DIR = "/.crosspoint/flashcards";
constexpr size_t NAME_BUFFER_SIZE = 500;

constexpr const char* HEADER_TITLE = "Flashcards";
constexpr const char* EMPTY_HEADING = "No decks found.";
constexpr const char* EMPTY_SUBTITLE = "Copy .jsonl files to /flashcards on the SD card.";

struct SrsEditWorkspace {
  flashcards::FlashcardSrsStore store;
  std::string idxPath;
  std::string srsPath;
  std::string walPath;
};

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

uint16_t nextNewPerDayPreset(const uint16_t current) noexcept {
  if (current == 10) return 20;
  if (current == 20) return 30;
  if (current == 30) return 50;
  return 10;
}

}  // namespace

FlashcardsDeckListActivity::FlashcardsDeckListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("FlashcardsDeckList", renderer, mappedInput, /*wantsTouchLongPress=*/true) {}

void FlashcardsDeckListActivity::onEnter() {
  UiListActivity::onEnter();

  suppressConfirmRelease = mappedInput.isPressed(MappedInputManager::Button::Confirm);
  suppressBackRelease = mappedInput.isPressed(MappedInputManager::Button::Back);

  if (APP_STATE.clearFlashcardsResume() && !APP_STATE.saveToFile()) {
    LOG_ERR("FCARD", "Failed saving cleared Flashcards resume state");
  }

  loadDecks();
}

void FlashcardsDeckListActivity::onExit() {
  optionPopup.dismiss();
  Activity::onExit();
  decks.clear();
  shrinkToFit(decks);
  fileNameBuffer.reset();
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
  uint16_t active = 0;
  for (const DeckEntry& entry : decks) {
    if (!entry.summary.sessionCompleted && (entry.summary.dueCount > 0 || entry.summary.newCount > 0)) {
      ++active;
    }
  }
  std::snprintf(activeSubtitleBuf, sizeof(activeSubtitleBuf), "%u active", static_cast<unsigned>(active));
}

bool FlashcardsDeckListActivity::refreshDeckSummary(const size_t index) {
  if (index >= decks.size()) return false;
  DeckEntry& entry = decks[index];
  const std::string path = fullDeckPath(index);
  const bool ok = flashcards::FlashcardSession::inspectDeck(path.c_str(), FLASHCARDS_CACHE_DIR, entry.summary);
  if (!ok && entry.summary.title[0] == '\0') {
    formatStemFallback(entry.filename, entry.summary.title, sizeof(entry.summary.title));
  }
  prewarmedStart = -1;
  refreshActiveSubtitle();
  return ok;
}

void FlashcardsDeckListActivity::loadDecks() {
  decks.clear();
  prewarmedStart = -1;

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
    std::string srsPath;
    std::string walPath;
    for (const std::string& name : names) {
      fullPath.assign(FLASHCARDS_DIR_PATH);
      fullPath.push_back('/');
      fullPath.append(name);
      flashcards::FlashcardSrsStore::buildPaths(FLASHCARDS_CACHE_DIR, fullPath, idxPath, srsPath, walPath);
      if (!Storage.exists(idxPath.c_str())) {
        RenderLock lock(*this);
        GUI.drawPopup(renderer, "Indexing decks...");
        break;
      }
    }
  }

  for (std::string& name : names) {
    fullPath.assign(FLASHCARDS_DIR_PATH);
    fullPath.push_back('/');
    fullPath.append(name);

    DeckEntry entry;
    entry.filename = std::move(name);
    if (!flashcards::FlashcardSession::inspectDeck(fullPath.c_str(), FLASHCARDS_CACHE_DIR, entry.summary) &&
        entry.summary.title[0] == '\0') {
      formatStemFallback(entry.filename, entry.summary.title, sizeof(entry.summary.title));
    }
    decks.push_back(std::move(entry));
  }

  refreshActiveSubtitle();
}

void FlashcardsDeckListActivity::provideRow(void* ctx, const uint16_t index, fui::ListItem& item) {
  auto* self = static_cast<FlashcardsDeckListActivity*>(ctx);
  if (index >= self->decks.size()) return;

  const DeckEntry& entry = self->decks[index];
  const flashcards::DeckSummary& sum = entry.summary;

  formatDeckTitle(sum, entry.filename, self->rowTitleBuf, sizeof(self->rowTitleBuf));
  item.label = self->rowTitleBuf;

  std::snprintf(self->rowSubtitleBuf, sizeof(self->rowSubtitleBuf), "Session %u \xc2\xb7 %u %s",
                static_cast<unsigned>(sum.sessionDay), static_cast<unsigned>(sum.totalCards),
                sum.totalCards == 1 ? "card" : "cards");
  item.subtitle = self->rowSubtitleBuf;

  if (sum.sessionCompleted) {
    std::snprintf(self->rowValueBuf, sizeof(self->rowValueBuf), "Done \xc2\xb7 S%u",
                  static_cast<unsigned>(sum.sessionDay));
  } else {
    std::snprintf(self->rowValueBuf, sizeof(self->rowValueBuf), "%u due \xc2\xb7 %u new",
                  static_cast<unsigned>(sum.dueCount), static_cast<unsigned>(sum.newCount));
  }
  item.value = self->rowValueBuf;
  item.actionValue = static_cast<int16_t>(index);
}

void FlashcardsDeckListActivity::prewarmRowGlyphs(const int start) {
  const int total = static_cast<int>(decks.size());
  if (total <= 0) return;

  int clamped = start;
  if (clamped > total - PREWARM_WINDOW) clamped = total - PREWARM_WINDOW;
  if (clamped < 0) clamped = 0;
  if (clamped == prewarmedStart) return;
  prewarmedStart = clamped;
  const int count = (total - clamped < PREWARM_WINDOW) ? (total - clamped) : PREWARM_WINDOW;

  struct PrewarmCtx {
    FlashcardsDeckListActivity* self;
    int first;
    int count;
  } prewarmCtx{this, clamped, count};

  renderer.prewarmFallbackText(
      uiScaleSpec().bodyFontId,
      [](const void* ctx, const uint32_t i) -> const char* {
        auto* c = const_cast<PrewarmCtx*>(static_cast<const PrewarmCtx*>(ctx));
        if (i < static_cast<uint32_t>(c->count)) {
          const DeckEntry& entry = c->self->decks[static_cast<size_t>(c->first) + i];
          formatDeckTitle(entry.summary, entry.filename, c->self->rowTitleBuf, sizeof(c->self->rowTitleBuf));
          return c->self->rowTitleBuf;
        }
        return "";
      },
      &prewarmCtx, static_cast<uint32_t>(count));
}

void FlashcardsDeckListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (decks.empty()) {
    const auto& theme = screen.theme();
    const fui::Rect body = screen.body().inset(fui::Insets{0, static_cast<int16_t>(metrics.contentSidePadding), 0,
                                                           static_cast<int16_t>(metrics.contentSidePadding)});
    fui::TextStyle headingStyle = theme.bodyText;
    headingStyle.bold = true;
    headingStyle.align = fui::TextAlign::Center;
    fui::TextStyle subStyle = theme.smallText;
    subStyle.align = fui::TextAlign::Center;
    subStyle.maxLines = 2;

    const int16_t headingH = screen.target().lineHeight(headingStyle.font);
    const int16_t subH = fui::measureWrappedText(screen.target(), EMPTY_SUBTITLE, subStyle,
                                                 body.width > 1 ? body.width : static_cast<int16_t>(1))
                             .height;
    const int16_t totalH = static_cast<int16_t>(headingH + theme.spaceSm + subH);
    const int16_t topY = static_cast<int16_t>(body.y + std::max<int>(0, (body.height - totalH) / 2));

    screen.target().text(fui::Rect{body.x, topY, body.width, headingH}, EMPTY_HEADING, headingStyle);
    screen.target().text(fui::Rect{body.x, static_cast<int16_t>(topY + headingH + theme.spaceSm), body.width, subH},
                         EMPTY_SUBTITLE, subStyle);
    return;
  }

  fui::ListProps props;
  props.rowProvider = &FlashcardsDeckListActivity::provideRow;
  props.rowProviderCtx = this;
  props.count = static_cast<uint16_t>(decks.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  props.valueInset = 8;
  props.labelText = screen.theme().bodyText;
  props.labelText.maxLines = 1;
  props.rowHeight = static_cast<int16_t>(metrics.listWithSubtitleRowHeight);
  props.rowGap = std::max<int16_t>(screen.theme().listRowGap, 6);

  syncListViewport(screen, props);
  prewarmRowGlyphs(nav.top);
  screen.list(props);
}

void FlashcardsDeckListActivity::drawChrome() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, HEADER_TITLE,
                 decks.empty() ? nullptr : activeSubtitleBuf);
}

void FlashcardsDeckListActivity::drawFooter() {
  const bool hasDecks = !decks.empty();
  const auto labels =
      mappedInput.mapLabels("Home", hasDecks ? "Open" : "", hasDecks ? "Up" : "", hasDecks ? "Down" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void FlashcardsDeckListActivity::render(RenderLock&& lock) {
  if (optionPopup.processRender(renderer, mappedInput)) return;
  UiListActivity::render(std::move(lock));
}

void FlashcardsDeckListActivity::activateIndex(const int index) {
  (void)index;
  app.clearTapFlash();
  openSelectedDeck();
}

void FlashcardsDeckListActivity::onRowLongPress(const int index) {
  (void)index;
  app.clearTapFlash();
  showDeckOptions();
}

bool FlashcardsDeckListActivity::handleCustomInput() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;

  if (suppressConfirmRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Confirm)) {
      suppressConfirmRelease = false;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      return true;
    }
  }
  if (suppressBackRelease) {
    if (!mappedInput.isPressed(MappedInputManager::Button::Back)) {
      suppressBackRelease = false;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      return true;
    }
  }
  return false;
}

bool FlashcardsDeckListActivity::handleButtons() {
  if (!decks.empty() && mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, OPTION_LONG_PRESS_MS)) {
    app.clearTapFlash();
    showDeckOptions();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    app.clearTapFlash();
    openSelectedDeck();
    return true;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    onGoHome(HomeMenuItem::FLASHCARDS);
    return true;
  }

  return false;
}

void FlashcardsDeckListActivity::openSelectedDeck() {
  const int sel = nav.selected.load();
  if (decks.empty() || sel < 0 || sel >= listCount()) return;

  std::string path = fullDeckPath(static_cast<size_t>(sel));
  auto review = makeUniqueNoThrow<FlashcardsReviewActivity>(renderer, mappedInput, std::move(path), false);
  if (!review) {
    LOG_ERR("FCARD", "OOM allocating %u-byte FlashcardsReviewActivity",
            static_cast<unsigned>(sizeof(FlashcardsReviewActivity)));
    return;
  }
  activityManager.replaceActivity(std::move(review));
}

void FlashcardsDeckListActivity::showDeckOptions() {
  const int sel = nav.selected.load();
  if (decks.empty() || optionPopup.isActive() || sel < 0 || sel >= listCount()) {
    return;
  }

  const size_t index = static_cast<size_t>(sel);
  const DeckEntry& entry = decks[index];
  char titleBuf[96]{};
  formatDeckTitle(entry.summary, entry.filename, titleBuf, sizeof(titleBuf));

  const uint32_t nextDay = (entry.summary.sessionDay < std::numeric_limits<uint32_t>::max())
                               ? (entry.summary.sessionDay + 1u)
                               : std::numeric_limits<uint32_t>::max();
  char optNext[44]{};
  char optNewLimit[36]{};
  std::snprintf(optNext, sizeof(optNext), "Start Next Session (S%u)", static_cast<unsigned>(nextDay));
  std::snprintf(optNewLimit, sizeof(optNewLimit), "New Cards / Session: %u",
                static_cast<unsigned>(entry.summary.newPerDay));
  const char* options[3] = {optNext, optNewLimit, "Reset Deck Progress"};

  optionPopup.show(titleBuf, options, 3, 0, [this, index](const int optionIdx) {
    if (optionIdx == 0) {
      startNextSessionAndOpen(index);
    } else if (optionIdx == 1) {
      cycleNewCardsPerSession(index);
    } else if (optionIdx == 2) {
      confirmResetDeck(index);
    }
  });
  requestUpdate();
}

void FlashcardsDeckListActivity::startNextSessionAndOpen(const size_t index) {
  if (index >= decks.size()) return;
  std::string path = fullDeckPath(index);

  auto session = makeUniqueNoThrow<flashcards::FlashcardSession>();
  if (!session) {
    LOG_ERR("FCARD", "OOM allocating %u-byte FlashcardSession for next session",
            static_cast<unsigned>(sizeof(flashcards::FlashcardSession)));
    return;
  }
  if (!session->open(path.c_str(), FLASHCARDS_CACHE_DIR)) {
    LOG_ERR("FCARD", "Failed opening %s to start next session", path.c_str());
    return;
  }
  if (!session->startNextSession()) {
    LOG_ERR("FCARD", "Failed advancing session for %s", path.c_str());
    session->close();
    return;
  }
  session->close();
  session.reset();

  auto review = makeUniqueNoThrow<FlashcardsReviewActivity>(renderer, mappedInput, std::move(path), false);
  if (!review) {
    LOG_ERR("FCARD", "OOM allocating %u-byte FlashcardsReviewActivity",
            static_cast<unsigned>(sizeof(FlashcardsReviewActivity)));
    return;
  }
  activityManager.replaceActivity(std::move(review));
}

void FlashcardsDeckListActivity::cycleNewCardsPerSession(const size_t index) {
  if (index >= decks.size()) return;
  const std::string path = fullDeckPath(index);
  const uint16_t nextLimit = nextNewPerDayPreset(decks[index].summary.newPerDay);

  if (!flashcards::FlashcardDeckIndex::ensureDir(FLASHCARDS_CACHE_DIR)) {
    return;
  }

  auto ws = makeUniqueNoThrow<SrsEditWorkspace>();
  if (!ws) {
    LOG_ERR("FCARD", "OOM allocating %u-byte SrsEditWorkspace", static_cast<unsigned>(sizeof(SrsEditWorkspace)));
    return;
  }
  flashcards::FlashcardSrsStore::buildPaths(FLASHCARDS_CACHE_DIR, path, ws->idxPath, ws->srsPath, ws->walPath);
  if (!ws->store.load(ws->srsPath.c_str(), ws->walPath.c_str())) {
    LOG_ERR("FCARD", "Failed loading SRS store for %s", path.c_str());
    return;
  }
  if (!ws->store.setNewPerDayOverride(nextLimit)) {
    LOG_ERR("FCARD", "Failed saving newPerDay override (%u) for %s", static_cast<unsigned>(nextLimit), path.c_str());
    return;
  }
  ws.reset();

  {
    RenderLock lock(*this);
    refreshDeckSummary(index);
  }
  requestUpdate();
}

void FlashcardsDeckListActivity::confirmResetDeck(const size_t index) {
  if (index >= decks.size()) return;

  std::string path = fullDeckPath(index);
  char titleBuf[96]{};
  formatDeckTitle(decks[index].summary, decks[index].filename, titleBuf, sizeof(titleBuf));
  std::string title(titleBuf);

  auto confirmation =
      makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, "Reset Deck Progress?", title, "Cancel", "Reset");
  if (!confirmation) {
    LOG_ERR("FCARD", "OOM allocating %u-byte ConfirmationActivity",
            static_cast<unsigned>(sizeof(ConfirmationActivity)));
    return;
  }

  startActivityForResult(std::move(confirmation), [this, index, path = std::move(path)](const ActivityResult& res) {
    if (res.isCancelled) return;
    if (!flashcards::FlashcardSrsStore::resetDeck(FLASHCARDS_CACHE_DIR, path)) {
      LOG_ERR("FCARD", "Failed resetting deck progress for %s", path.c_str());
    }
    {
      RenderLock lock(*this);
      refreshDeckSummary(index);
    }
    requestUpdate(true);
  });
}
