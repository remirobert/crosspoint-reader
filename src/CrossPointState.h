#pragma once
#include <ArduinoJson.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

class CrossPointState : public PersistableStore<CrossPointState> {
  CrossPointState() = default;

  friend class PersistableStore<CrossPointState>;

 public:
  static constexpr uint8_t SLEEP_AVOID_REPEAT_LOOKBACK = 5;

  std::string openEpubPath;
  std::string openFlashcardsPath;
  uint32_t sleepLcgCount = 0;
  uint32_t sleepLcgState = 0;
  uint32_t sleepOverlayLcgCount = 0;
  uint32_t sleepOverlayLcgState = 0;
  uint8_t readerActivityLoadCount = 0;
  uint8_t flashcardsActivityLoadCount = 0;
  bool lastSleepFromReader = false;
  bool lastSleepFromFlashcards = false;
  bool showBootScreen = true;

  static const char* getFilePath() { return "/.crosspoint/state.json"; }

  // Returns true if any Flashcards resume field changed, so callers save only when dirty.
  bool clearFlashcardsResume() {
    const bool dirty = !openFlashcardsPath.empty() || lastSleepFromFlashcards || flashcardsActivityLoadCount != 0;
    openFlashcardsPath.clear();
    lastSleepFromFlashcards = false;
    flashcardsActivityLoadCount = 0;
    return dirty;
  }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  void getSleepLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft, uint16_t& indexesTotal) const;
  void getSleepOverlayLCG(uint32_t& seed, uint32_t& state, uint16_t& indexesLeft, uint16_t& indexesTotal) const;

  void setSleepLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal);
  void setSleepOverlayLCG(uint32_t seed, uint32_t state, uint16_t indexesLeft, uint16_t indexesTotal);
};

#define APP_STATE CrossPointState::getInstance()
