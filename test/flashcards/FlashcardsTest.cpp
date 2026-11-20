#include <HalStorage.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "FlashcardDeckIndex.h"
#include "FlashcardScheduler.h"
#include "FlashcardSession.h"
#include "FlashcardSrsStore.h"
#include "FlashcardTypes.h"

namespace flashcards {
namespace {

class FlashcardsStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    flashcards_fs::reset();
    flashcards_test_mem::failAllocCountdown = -1;
  }

  // Deep-copies the in-memory filesystem to simulate sudden 0V battery cutoff before close()/destructors run.
  static std::map<std::string, flashcards_fs::Node> snapshotFs() {
    std::map<std::string, flashcards_fs::Node> snap;
    for (const auto& [path, node] : flashcards_fs::files) {
      if (node) {
        snap.emplace(path, *node);
      }
    }
    return snap;
  }

  static void restoreFs(const std::map<std::string, flashcards_fs::Node>& snap) {
    flashcards_fs::reset();
    flashcards_fs::files.clear();
    for (const auto& [path, nodeCopy] : snap) {
      flashcards_fs::files.emplace(path, std::make_shared<flashcards_fs::Node>(nodeCopy));
    }
  }
};

// ============================================================================
// FlashcardTypes Tests
// ============================================================================

TEST(FlashcardTypesTest, StructSizeAndLayoutInvariants) {
  EXPECT_EQ(DECK_INDEX_MAGIC, 0x31444346u);
  EXPECT_EQ(sizeof(DeckIndexHeader), 84u);
  EXPECT_EQ(sizeof(CardIndexRecord), 16u);
  EXPECT_EQ(sizeof(SrsFileHeader), 24u);
  EXPECT_EQ(sizeof(CardSrsRecord), 24u);
  EXPECT_EQ(sizeof(WalEntry), 32u);
  EXPECT_EQ(offsetof(WalEntry, crc32), 28u);
}

TEST(FlashcardTypesTest, Fnv1a64KnownVectors) {
  static_assert(fnv1a64("") == 0xcbf29ce484222325ULL);
  static_assert(fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
  static_assert(fnv1a64("foobar") == 0x85944171f73967e8ULL);

  EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv1a64("foobar"), 0x85944171f73967e8ULL);
  EXPECT_NE(fnv1a64("食べる"), fnv1a64("飲む"));
  EXPECT_EQ(fnv1a64("食べる"), fnv1a64(std::string_view("食べる")));
}

TEST(FlashcardTypesTest, Crc32KnownCheckVector) {
  constexpr const char kCheck[] = "123456789";
  EXPECT_EQ(crc32Flashcards(kCheck, 9), 0xCBF43926u);
  EXPECT_EQ(crc32Flashcards(nullptr, 0), 0u);
  EXPECT_EQ(crc32Flashcards(kCheck, 0), 0u);

  char flipped[10];
  std::memcpy(flipped, kCheck, sizeof(kCheck));
  flipped[4] ^= 0x01;
  EXPECT_NE(crc32Flashcards(flipped, 9), 0xCBF43926u);
}

TEST(FlashcardTypesTest, FormatDeckHashHex) {
  char hex[17];
  std::memset(hex, 'X', sizeof(hex));
  formatDeckHashHex("", hex);
  EXPECT_STREQ(hex, "cbf29ce484222325");
  EXPECT_EQ(std::strlen(hex), 16u);

  formatDeckHashHex("foobar", hex);
  EXPECT_STREQ(hex, "85944171f73967e8");
}

// ============================================================================
// FlashcardScheduler Tests (FSRS-5)
// ============================================================================

TEST(FlashcardSchedulerTest, NewCardAllRatings) {
  CardSrsRecord card{};
  card.cardIdHash = 0x1234ULL;
  card.state = static_cast<uint8_t>(CardState::New);

  const IntervalPreview preview = FlashcardScheduler::previewIntervals(card, 1);
  EXPECT_EQ(preview.hardDays, 1u);
  EXPECT_EQ(preview.goodDays, 3u);
  EXPECT_EQ(preview.easyDays, 16u);

  const CardSrsRecord onAgain = FlashcardScheduler::reviewCard(card, Rating::Again, 1);
  EXPECT_EQ(onAgain.cardIdHash, 0x1234ULL);
  EXPECT_EQ(static_cast<CardState>(onAgain.state), CardState::Learning);
  EXPECT_EQ(onAgain.dueDay, 1u);
  EXPECT_EQ(onAgain.lastReviewDay, 1u);
  EXPECT_EQ(onAgain.lapses, 0u);
  EXPECT_NEAR(onAgain.stability, 0.40255f, 1e-4f);
  EXPECT_GE(onAgain.difficultyPermille, 1000u);
  EXPECT_LE(onAgain.difficultyPermille, 10000u);

  const CardSrsRecord onHard = FlashcardScheduler::reviewCard(card, Rating::Hard, 1);
  EXPECT_EQ(static_cast<CardState>(onHard.state), CardState::Review);
  EXPECT_EQ(onHard.dueDay, static_cast<uint16_t>(1u + preview.hardDays));
  EXPECT_NEAR(onHard.stability, 1.18385f, 1e-4f);

  const CardSrsRecord onGood = FlashcardScheduler::reviewCard(card, Rating::Good, 1);
  EXPECT_EQ(static_cast<CardState>(onGood.state), CardState::Review);
  EXPECT_EQ(onGood.dueDay, static_cast<uint16_t>(1u + preview.goodDays));
  EXPECT_NEAR(onGood.stability, 3.17300f, 1e-4f);

  const CardSrsRecord onEasy = FlashcardScheduler::reviewCard(card, Rating::Easy, 1);
  EXPECT_EQ(static_cast<CardState>(onEasy.state), CardState::Review);
  EXPECT_EQ(onEasy.dueDay, static_cast<uint16_t>(1u + preview.easyDays));
  EXPECT_NEAR(onEasy.stability, 15.69105f, 1e-4f);

  EXPECT_GE(onAgain.difficultyPermille, onHard.difficultyPermille);
  EXPECT_GE(onHard.difficultyPermille, onGood.difficultyPermille);
  EXPECT_GE(onGood.difficultyPermille, onEasy.difficultyPermille);
}

TEST(FlashcardSchedulerTest, SameSessionLearningStepElapsedZero) {
  CardSrsRecord card{};
  card.cardIdHash = 42ULL;
  const CardSrsRecord learning = FlashcardScheduler::reviewCard(card, Rating::Again, 5);
  ASSERT_EQ(static_cast<CardState>(learning.state), CardState::Learning);
  ASSERT_EQ(learning.lastReviewDay, 5u);

  // Another Again in the same session keeps Learning state and difficultyPermille unchanged.
  const CardSrsRecord againStep = FlashcardScheduler::reviewCard(learning, Rating::Again, 5);
  EXPECT_EQ(static_cast<CardState>(againStep.state), CardState::Learning);
  EXPECT_EQ(againStep.dueDay, 5u);
  EXPECT_EQ(againStep.lastReviewDay, 5u);
  EXPECT_EQ(againStep.lapses, 0u);
  EXPECT_EQ(againStep.difficultyPermille, learning.difficultyPermille);
  EXPECT_LT(againStep.stability, learning.stability);

  // Good in the same session (elapsed == 0) graduates to Review with short-term stability growth.
  const IntervalPreview stepPreview = FlashcardScheduler::previewIntervals(learning, 5);
  const CardSrsRecord graduated = FlashcardScheduler::reviewCard(learning, Rating::Good, 5);
  EXPECT_EQ(static_cast<CardState>(graduated.state), CardState::Review);
  EXPECT_EQ(graduated.dueDay, static_cast<uint16_t>(5u + stepPreview.goodDays));
  EXPECT_EQ(graduated.difficultyPermille, learning.difficultyPermille);
  EXPECT_GT(graduated.stability, learning.stability);
}

TEST(FlashcardSchedulerTest, InterSessionReviewRecallAndLapseSaturation) {
  CardSrsRecord card{};
  card.cardIdHash = 99ULL;
  card = FlashcardScheduler::reviewCard(card, Rating::Good, 1);
  ASSERT_EQ(static_cast<CardState>(card.state), CardState::Review);
  ASSERT_EQ(card.dueDay, 4u);

  const IntervalPreview previewDay4 = FlashcardScheduler::previewIntervals(card, 4);
  EXPECT_LE(previewDay4.hardDays, previewDay4.goodDays);
  EXPECT_LT(previewDay4.goodDays, previewDay4.easyDays);

  const CardSrsRecord recalledHard = FlashcardScheduler::reviewCard(card, Rating::Hard, 4);
  const CardSrsRecord recalledGood = FlashcardScheduler::reviewCard(card, Rating::Good, 4);
  const CardSrsRecord recalledEasy = FlashcardScheduler::reviewCard(card, Rating::Easy, 4);

  EXPECT_EQ(recalledHard.dueDay, static_cast<uint16_t>(4u + previewDay4.hardDays));
  EXPECT_EQ(recalledGood.dueDay, static_cast<uint16_t>(4u + previewDay4.goodDays));
  EXPECT_EQ(recalledEasy.dueDay, static_cast<uint16_t>(4u + previewDay4.easyDays));
  EXPECT_LT(recalledHard.stability, recalledGood.stability);
  EXPECT_LT(recalledGood.stability, recalledEasy.stability);
  EXPECT_GT(recalledGood.stability, card.stability);

  // Lapse transitions Review -> Relearning, increments lapses, sets dueDay = sessionDay.
  const CardSrsRecord lapsed = FlashcardScheduler::reviewCard(recalledGood, Rating::Again, 20);
  EXPECT_EQ(static_cast<CardState>(lapsed.state), CardState::Relearning);
  EXPECT_EQ(lapsed.dueDay, 20u);
  EXPECT_EQ(lapsed.lastReviewDay, 20u);
  EXPECT_EQ(lapsed.lapses, 1u);
  EXPECT_GE(lapsed.stability, 0.1f);
  EXPECT_LE(lapsed.stability, recalledGood.stability);

  // Saturate uint8_t lapses at 255 without wrapping to 0.
  CardSrsRecord highLapse = recalledGood;
  highLapse.lapses = 254;
  highLapse = FlashcardScheduler::reviewCard(highLapse, Rating::Again, 25);
  EXPECT_EQ(highLapse.lapses, 255u);
  highLapse = FlashcardScheduler::reviewCard(highLapse, Rating::Good, 25);
  ASSERT_EQ(static_cast<CardState>(highLapse.state), CardState::Review);
  highLapse = FlashcardScheduler::reviewCard(highLapse, Rating::Again, 30);
  EXPECT_EQ(highLapse.lapses, 255u);
}

TEST(FlashcardSchedulerTest, MonotonicPreviewsAndUint16DaySaturation) {
  for (uint16_t diff : {1000u, 5000u, 10000u}) {
    for (float stab : {0.1f, 0.4f, 1.2f, 10.0f, 500.0f, 50000.0f}) {
      CardSrsRecord rec{};
      rec.stability = stab;
      rec.difficultyPermille = diff;
      rec.lastReviewDay = 1;
      rec.dueDay = 10;
      rec.state = static_cast<uint8_t>(CardState::Review);

      const IntervalPreview p = FlashcardScheduler::previewIntervals(rec, 10);
      EXPECT_GE(p.hardDays, 1u);
      EXPECT_LE(p.hardDays, p.goodDays);
      EXPECT_LT(p.goodDays, p.easyDays);
    }
  }

  // Near uint16_t max sessionDay (65530..65535), dueDay saturates at 65535 without overflow.
  CardSrsRecord huge{};
  huge.stability = 20000.0f;
  huge.difficultyPermille = 3000;
  huge.lastReviewDay = 60000;
  huge.dueDay = 65530;
  huge.state = static_cast<uint8_t>(CardState::Review);

  const IntervalPreview nearMax = FlashcardScheduler::previewIntervals(huge, 65530);
  EXPECT_GE(nearMax.hardDays, 1u);
  EXPECT_LE(nearMax.hardDays, nearMax.goodDays);
  EXPECT_LT(nearMax.goodDays, nearMax.easyDays);
  EXPECT_EQ(FlashcardScheduler::reviewCard(huge, Rating::Easy, 65530).dueDay, 65535u);
  EXPECT_EQ(FlashcardScheduler::reviewCard(huge, Rating::Easy, 65535).dueDay, 65535u);
  EXPECT_EQ(FlashcardScheduler::reviewCard(huge, Rating::Good, 65535).dueDay, 65535u);
}

// ============================================================================
// FlashcardDeckIndex Tests
// ============================================================================

TEST_F(FlashcardsStorageTest, DeckIndexMetaUtf8IdsHtmlMalformedAndLongLines) {
  std::string overlongLine = R"({"id":"too-long","front":")";
  overlongLine.append(4100, 'x');
  overlongLine += R"(","back":"ignored"})";
  ASSERT_GT(overlongLine.size(), MAX_JSONL_LINE_BYTES);

  std::string jsonl;
  jsonl += R"({"_meta":true,"title":"Japanese Core 2k","newPerDay":25,"maxReviews":150})"
           "\r\n";
  jsonl += "\r\n";
  jsonl += "   \t  \n";
  jsonl += R"({"id":"n5-001","front":"食べる","back":"to eat","hint":"たべる","extra":"毎日野菜を食べる。"})"
           "\r\n";
  jsonl += "{not valid json at all\n";
  jsonl += R"({"front":"","back":"missing front"})"
           "\n";
  jsonl += R"({"front":"missing back"})"
           "\n";
  jsonl += overlongLine + "\r\n";
  jsonl += R"({"id":987654,"front":"<div>Line 1<br>Line 2</div>","back":"<p>A &amp; B &lt;C&gt;</p>",)"
           R"("hint":"&nbsp;<b>noun</b> ","extra":"&#39;quote&#39; &quot;dbl&quot;<br/>next"})"
           "\n";
  // Final line without trailing newline and without "id" (uses fallback hash of front).
  jsonl += R"({"front":"飲む","back":"to drink"})";

  flashcards_fs::setFile("/flashcards/jp.jsonl", jsonl);
  ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));
  EXPECT_TRUE(Storage.exists("/.crosspoint"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/flashcards"));

  FlashcardDeckIndex index;
  ASSERT_TRUE(index.openOrBuild("/flashcards/jp.jsonl", "/.crosspoint/flashcards/jp.idx"));
  EXPECT_TRUE(index.isOpen());

  const DeckIndexHeader& hdr = index.header();
  EXPECT_EQ(hdr.magic, DECK_INDEX_MAGIC);
  EXPECT_EQ(hdr.version, DECK_INDEX_VERSION);
  EXPECT_EQ(hdr.deckFileSize, jsonl.size());
  EXPECT_EQ(hdr.cardCount, 3u);
  EXPECT_EQ(hdr.newPerDay, 25u);
  EXPECT_EQ(hdr.maxReviews, 150u);
  EXPECT_STREQ(hdr.title, "Japanese Core 2k");

  auto scratch = makeUniqueNoThrow<char[]>(MAX_JSONL_LINE_BYTES);
  ASSERT_NE(scratch, nullptr);

  FlashcardItem c0;
  ASSERT_TRUE(index.readCard(0, scratch.get(), MAX_JSONL_LINE_BYTES, c0));
  EXPECT_EQ(c0.cardIdHash, fnv1a64("n5-001"));
  EXPECT_EQ(c0.front, "食べる");
  EXPECT_EQ(c0.back, "to eat");
  EXPECT_EQ(c0.hint, "たべる");
  EXPECT_EQ(c0.extra, "毎日野菜を食べる。");

  FlashcardItem c1;
  ASSERT_TRUE(index.readCard(1, scratch.get(), MAX_JSONL_LINE_BYTES, c1));
  EXPECT_EQ(c1.cardIdHash, fnv1a64("987654"));
  EXPECT_EQ(c1.front, "Line 1\nLine 2");
  EXPECT_EQ(c1.back, "A & B <C>");
  EXPECT_EQ(c1.hint, "noun");
  EXPECT_EQ(c1.extra, "'quote' \"dbl\"\nnext");

  FlashcardItem c2;
  ASSERT_TRUE(index.readCard(2, scratch.get(), MAX_JSONL_LINE_BYTES, c2));
  EXPECT_EQ(c2.cardIdHash, fnv1a64("飲む"));
  EXPECT_EQ(c2.front, "飲む");
  EXPECT_EQ(c2.back, "to drink");
  EXPECT_TRUE(c2.hint.empty());
  EXPECT_TRUE(c2.extra.empty());

  CardIndexRecord outOfRange{};
  EXPECT_FALSE(index.readIndexRecord(3, outOfRange));
  EXPECT_FALSE(index.readCard(3, scratch.get(), MAX_JSONL_LINE_BYTES, c2));
  index.close();
  EXPECT_FALSE(index.isOpen());
}

TEST_F(FlashcardsStorageTest, DeckIndexCacheReuseAndRebuildOnSizeChange) {
  const std::string initialJsonl = R"({"front":"card1","back":"ans1"})"
                                   "\n"
                                   R"({"front":"card2","back":"ans2"})"
                                   "\n";
  flashcards_fs::setFile("/flashcards/vocab.jsonl", initialJsonl);
  ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));

  const std::string idxPath = "/.crosspoint/flashcards/vocab.idx";
  const std::string tmpIdxPath = "/.crosspoint/flashcards/vocab.idx.tmp";

  {
    FlashcardDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild("/flashcards/vocab.jsonl", idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 2u);
    EXPECT_STREQ(idx.header().title, "vocab");
    EXPECT_EQ(idx.header().newPerDay, DEFAULT_NEW_PER_DAY);
    EXPECT_EQ(idx.header().maxReviews, DEFAULT_MAX_REVIEWS);
    idx.close();
  }

  const unsigned writesAfterFirstBuild = flashcards_fs::writeCount(idxPath) + flashcards_fs::writeCount(tmpIdxPath);
  ASSERT_GT(writesAfterFirstBuild, 0u);

  // Re-opening unchanged .jsonl reuses the cached .idx without writing a new index.
  {
    FlashcardDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild("/flashcards/vocab.jsonl", idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 2u);
    idx.close();
  }
  const unsigned writesAfterReuse = flashcards_fs::writeCount(idxPath) + flashcards_fs::writeCount(tmpIdxPath);
  EXPECT_EQ(writesAfterReuse, writesAfterFirstBuild);

  // Appending a card changes .jsonl size -> rebuilds .idx cleanly (even when .idx already exists on SdFat).
  const std::string expandedJsonl = initialJsonl + R"({"front":"card3","back":"ans3"})"
                                                   "\n";
  flashcards_fs::setFile("/flashcards/vocab.jsonl", expandedJsonl);
  {
    FlashcardDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild("/flashcards/vocab.jsonl", idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 3u);
    EXPECT_EQ(idx.header().deckFileSize, expandedJsonl.size());
    idx.close();
  }
  const unsigned writesAfterRebuild = flashcards_fs::writeCount(idxPath) + flashcards_fs::writeCount(tmpIdxPath);
  EXPECT_GT(writesAfterRebuild, writesAfterReuse);
}

// ============================================================================
// FlashcardSrsStore & FlashcardSession Tests
// ============================================================================

TEST_F(FlashcardsStorageTest, FullSessionFlowAndAgainIntraSessionRequeue) {
  std::string jsonl = R"({"_meta":true,"title":"Ten Deck","newPerDay":10,"maxReviews":100})"
                      "\n";
  for (int i = 0; i < 12; ++i) {
    jsonl += "{\"id\":\"c" + std::to_string(i) + "\",\"front\":\"Q" + std::to_string(i) + "\",\"back\":\"A" +
             std::to_string(i) + "\"}\n";
  }
  flashcards_fs::setFile("/flashcards/ten.jsonl", jsonl);

  FlashcardSession::DeckSummary summary{};
  ASSERT_TRUE(FlashcardSession::inspectDeck("/flashcards/ten.jsonl", "/.crosspoint/flashcards", summary));
  EXPECT_EQ(summary.totalCards, 12u);
  EXPECT_EQ(summary.dueCount, 0u);
  EXPECT_EQ(summary.newCount, 10u);
  EXPECT_EQ(summary.sessionDay, 1u);
  EXPECT_FALSE(summary.sessionCompleted);
  EXPECT_STREQ(summary.title, "Ten Deck");

  FlashcardSession session;
  ASSERT_TRUE(session.open("/flashcards/ten.jsonl", "/.crosspoint/flashcards"));
  EXPECT_FALSE(session.isSessionComplete());
  EXPECT_EQ(session.totalDeckCards(), 12u);
  EXPECT_EQ(session.sessionDay(), 1u);
  EXPECT_EQ(session.remainingCount(), 10u);
  EXPECT_STREQ(session.deckTitle(), "Ten Deck");

  FlashcardItem card;
  CardSrsRecord srs;
  IntervalPreview preview;
  ASSERT_TRUE(session.currentCard(card, srs, preview));
  EXPECT_EQ(card.front, "Q0");
  EXPECT_EQ(static_cast<CardState>(srs.state), CardState::New);

  // Rate Q0 Again: popped from front and re-queued 6 slots ahead (after Q1..Q6), keeping remainingCount == 10.
  ASSERT_TRUE(session.submitRating(Rating::Again));
  EXPECT_EQ(session.remainingCount(), 10u);
  EXPECT_EQ(session.reviewsThisSession(), 1u);

  for (int expected = 1; expected <= 6; ++expected) {
    ASSERT_TRUE(session.currentCard(card, srs, preview));
    EXPECT_EQ(card.front, "Q" + std::to_string(expected));
    ASSERT_TRUE(session.submitRating(Rating::Good));
  }

  // Now Q0 reappears at position 6 in Learning state!
  ASSERT_TRUE(session.currentCard(card, srs, preview));
  EXPECT_EQ(card.front, "Q0");
  EXPECT_EQ(static_cast<CardState>(srs.state), CardState::Learning);
  ASSERT_TRUE(session.submitRating(Rating::Good));

  // Drain remaining Q7, Q8, Q9.
  for (int expected = 7; expected <= 9; ++expected) {
    ASSERT_FALSE(session.isSessionComplete());
    ASSERT_TRUE(session.currentCard(card, srs, preview));
    EXPECT_EQ(card.front, "Q" + std::to_string(expected));
    ASSERT_TRUE(session.submitRating(Rating::Good));
  }

  EXPECT_EQ(session.remainingCount(), 0u);
  EXPECT_TRUE(session.isSessionComplete());
  EXPECT_EQ(session.reviewsThisSession(), 11u);
  session.close();

  // Re-opening a completed session preserves the SessionComplete boundary.
  FlashcardSession reopened;
  ASSERT_TRUE(reopened.open("/flashcards/ten.jsonl", "/.crosspoint/flashcards"));
  EXPECT_TRUE(reopened.isSessionComplete());
  EXPECT_EQ(reopened.sessionDay(), 1u);
  EXPECT_EQ(reopened.remainingCount(), 0u);

  uint16_t nextDue = 0;
  uint16_t nextNew = 0;
  reopened.previewNextSession(nextDue, nextNew);
  EXPECT_EQ(nextNew, 2u);  // Only Q10 and Q11 remain unseen.

  // Advance to Session 2 and verify Q10, Q11 (plus any cards due on day 2, such as Q0 graduated from Learning).
  ASSERT_TRUE(reopened.startNextSession());
  EXPECT_EQ(reopened.sessionDay(), 2u);
  EXPECT_FALSE(reopened.isSessionComplete());
  EXPECT_GE(reopened.remainingCount(), 2u);
  reopened.close();
}

TEST_F(FlashcardsStorageTest, WalReplayOnPowerCutAndTornWalRecovery) {
  const std::string deckPath = "/flashcards/power.jsonl";
  std::string jsonl = R"({"_meta":true,"title":"Power Deck","newPerDay":5,"maxReviews":50})"
                      "\n";
  for (int i = 0; i < 6; ++i) {
    jsonl += "{\"id\":\"p" + std::to_string(i) + "\",\"front\":\"Front" + std::to_string(i) + "\",\"back\":\"Back" +
             std::to_string(i) + "\"}\n";
  }
  flashcards_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  FlashcardSrsStore::buildPaths("/.crosspoint/flashcards", deckPath, idxPath, srsPath, walPath);

  std::map<std::string, flashcards_fs::Node> midSessionPowerCut;
  {
    FlashcardSession session;
    ASSERT_TRUE(session.open(deckPath.c_str(), "/.crosspoint/flashcards"));
    ASSERT_EQ(session.remainingCount(), 5u);

    // Rate p0 Good (graduated), p1 Again (Learning, due today), p2 Easy (graduated).
    ASSERT_TRUE(session.submitRating(Rating::Good));
    ASSERT_TRUE(session.submitRating(Rating::Again));
    ASSERT_TRUE(session.submitRating(Rating::Easy));

    // Confirm WAL exists on disk without .srs compaction yet (< 4096 B).
    EXPECT_TRUE(Storage.exists(walPath.c_str()));
    EXPECT_EQ(flashcards_fs::getFileBytes(walPath).size(), 3u * sizeof(WalEntry));

    // Capture exact SD card state mid-session before close() or destructor runs.
    midSessionPowerCut = snapshotFs();
    session.close();
  }

  // Restore mid-session power-cut snapshot and append 13 torn bytes to .wal (simulating mid-write brownout).
  restoreFs(midSessionPowerCut);
  const uint8_t tornTail[13] = {0xAA, 0x55, 0x01, 0x02, 0x03, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
  flashcards_fs::appendRawBytes(walPath, tornTail, sizeof(tornTail));

  {
    FlashcardSession recovered;
    ASSERT_TRUE(recovered.open(deckPath.c_str(), "/.crosspoint/flashcards"));
    EXPECT_EQ(recovered.sessionDay(), 1u);
    EXPECT_FALSE(recovered.isSessionComplete());
    EXPECT_EQ(recovered.reviewsThisSession(), 3u);
    // Remaining in Session 1: p1 (due today in Learning) + 2 remaining New budget (p3, p4) = 3 cards.
    EXPECT_EQ(recovered.remainingCount(), 3u);

    FlashcardItem c;
    CardSrsRecord srs;
    IntervalPreview prev;
    ASSERT_TRUE(recovered.currentCard(c, srs, prev));
    EXPECT_EQ(c.front, "Front1");
    EXPECT_EQ(static_cast<CardState>(srs.state), CardState::Learning);
    recovered.close();
  }

  // Also test a full 32-byte WalEntry with a corrupted CRC32.
  restoreFs(midSessionPowerCut);
  WalEntry badFrame{};
  badFrame.op = static_cast<uint8_t>(WalOp::CardRated);
  badFrame.sessionDay = 1;
  badFrame.card.cardIdHash = fnv1a64("p3");
  badFrame.crc32 = 0xDEADBEEFu;
  flashcards_fs::appendRawBytes(walPath, &badFrame, sizeof(badFrame));

  {
    FlashcardSrsStore store;
    ASSERT_TRUE(store.load(srsPath.c_str(), walPath.c_str()));
    EXPECT_EQ(store.header().reviewsThisSession, 3u);
    EXPECT_EQ(store.header().newThisSession, 3u);
    EXPECT_NE(store.findCard(fnv1a64("p0")), nullptr);
    EXPECT_NE(store.findCard(fnv1a64("p1")), nullptr);
    EXPECT_NE(store.findCard(fnv1a64("p2")), nullptr);
    EXPECT_EQ(store.findCard(fnv1a64("p3")), nullptr);
  }
}

TEST_F(FlashcardsStorageTest, StrictSequentialProgressionNewPerDayOverrideAndResetDeck) {
  const std::string deckPath = "/flashcards/mastered.jsonl";
  const std::string jsonl = R"({"_meta":true,"title":"Short Deck","newPerDay":20,"maxReviews":100})"
                            "\n"
                            R"({"id":"m1","front":"Alpha","back":"One"})"
                            "\n"
                            R"({"id":"m2","front":"Beta","back":"Two"})"
                            "\n"
                            R"({"id":"m3","front":"Gamma","back":"Three"})"
                            "\n";
  flashcards_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  FlashcardSrsStore::buildPaths("/.crosspoint/flashcards", deckPath, idxPath, srsPath, walPath);

  // Override newPerDay to 2 before opening session.
  {
    ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));
    FlashcardSrsStore store;
    ASSERT_TRUE(store.load(srsPath.c_str(), walPath.c_str()));
    ASSERT_TRUE(store.setNewPerDayOverride(2));
    EXPECT_EQ(store.effectiveNewPerDay(20), 2u);
  }

  // Session 1 introduces only 2 cards (m1, m2) because of newPerDayOverride == 2.
  {
    FlashcardSession s1;
    ASSERT_TRUE(s1.open(deckPath.c_str(), "/.crosspoint/flashcards"));
    EXPECT_EQ(s1.remainingCount(), 2u);
    ASSERT_TRUE(s1.submitRating(Rating::Easy));  // dueDay = 17
    ASSERT_TRUE(s1.submitRating(Rating::Easy));  // dueDay = 17
    EXPECT_TRUE(s1.isSessionComplete());

    // 1 unseen New card (m3) still remains, so next session day is 2 with 1 new card and nextScheduledDay == 0.
    EXPECT_EQ(s1.computeNextSessionDay(), 2u);
    uint16_t duePreview = 99;
    uint16_t newPreview = 99;
    uint16_t nextSched = 99;
    s1.previewNextSession(duePreview, newPreview, &nextSched);
    EXPECT_EQ(duePreview, 0u);
    EXPECT_EQ(newPreview, 1u);
    EXPECT_EQ(nextSched, 0u);

    ASSERT_TRUE(s1.startNextSession());
    EXPECT_EQ(s1.sessionDay(), 2u);
    EXPECT_FALSE(s1.isSessionComplete());
    EXPECT_EQ(s1.remainingCount(), 1u);

    // Rate m3 Good on day 2 -> dueDay = 5. Now 0 unseen New cards remain; earliest due is day 5.
    ASSERT_TRUE(s1.submitRating(Rating::Good));
    EXPECT_TRUE(s1.isSessionComplete());

    // Strict +1 progression: computeNextSessionDay() returns 3 (never fast-forwards to 5).
    EXPECT_EQ(s1.computeNextSessionDay(), 3u);
    s1.previewNextSession(duePreview, newPreview, &nextSched);
    EXPECT_EQ(duePreview, 0u);
    EXPECT_EQ(newPreview, 0u);
    EXPECT_EQ(nextSched, 5u);

    // Stepping S2 -> S3 enters empty Session 3 marked complete immediately with 0 reviews.
    ASSERT_TRUE(s1.startNextSession());
    EXPECT_EQ(s1.sessionDay(), 3u);
    EXPECT_TRUE(s1.isSessionComplete());
    EXPECT_EQ(s1.reviewsThisSession(), 0u);
    EXPECT_EQ(s1.remainingCount(), 0u);
    EXPECT_EQ(s1.computeNextSessionDay(), 4u);
    s1.previewNextSession(duePreview, newPreview, &nextSched);
    EXPECT_EQ(duePreview, 0u);
    EXPECT_EQ(newPreview, 0u);
    EXPECT_EQ(nextSched, 5u);

    // Stepping S3 -> S4 enters empty Session 4; Up Next (Session 5) now previews 1 due, 0 new (nextSched == 0).
    ASSERT_TRUE(s1.startNextSession());
    EXPECT_EQ(s1.sessionDay(), 4u);
    EXPECT_TRUE(s1.isSessionComplete());
    EXPECT_EQ(s1.reviewsThisSession(), 0u);
    EXPECT_EQ(s1.computeNextSessionDay(), 5u);
    s1.previewNextSession(duePreview, newPreview, &nextSched);
    EXPECT_EQ(duePreview, 1u);
    EXPECT_EQ(newPreview, 0u);
    EXPECT_EQ(nextSched, 0u);

    // Stepping S4 -> S5 makes m3 due on Session 5.
    ASSERT_TRUE(s1.startNextSession());
    EXPECT_EQ(s1.sessionDay(), 5u);
    EXPECT_FALSE(s1.isSessionComplete());
    EXPECT_EQ(s1.remainingCount(), 1u);
    s1.close();
  }

  // Reset deck removes .srs and .wal while preserving .idx.
  EXPECT_TRUE(Storage.exists(idxPath.c_str()));
  EXPECT_TRUE(Storage.exists(srsPath.c_str()));
  ASSERT_TRUE(FlashcardSrsStore::resetDeck("/.crosspoint/flashcards", deckPath));
  EXPECT_TRUE(Storage.exists(idxPath.c_str()));
  EXPECT_FALSE(Storage.exists(srsPath.c_str()));
  EXPECT_FALSE(Storage.exists(walPath.c_str()));

  FlashcardSession afterReset;
  ASSERT_TRUE(afterReset.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  EXPECT_EQ(afterReset.sessionDay(), 1u);
  EXPECT_EQ(afterReset.remainingCount(), 3u);
  afterReset.close();
}

TEST_F(FlashcardsStorageTest, WalThresholdAutoCompactionAndSingleCardAgainLoop) {
  const std::string deckPath = "/flashcards/single.jsonl";
  const std::string jsonl = R"({"_meta":true,"title":"Clamp Deck","newPerDay":999,"maxReviews":999})"
                            "\n"
                            R"({"id":"only","front":"SoloQ","back":"SoloA"})"
                            "\n";
  flashcards_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  FlashcardSrsStore::buildPaths("/.crosspoint/flashcards", deckPath, idxPath, srsPath, walPath);

  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  EXPECT_EQ(session.index().header().newPerDay, MAX_NEW_PER_DAY);
  EXPECT_EQ(session.index().header().maxReviews, MAX_REVIEWS_CAP);
  ASSERT_EQ(session.remainingCount(), 1u);

  // Rate Again 128 times (128 * 32 B = 4096 B == WAL_COMPACT_THRESHOLD_BYTES) to trigger automatic .srs compaction.
  for (size_t i = 0; i < WAL_COMPACT_THRESHOLD_BYTES / sizeof(WalEntry); ++i) {
    ASSERT_TRUE(session.submitRating(Rating::Again));
    EXPECT_EQ(session.remainingCount(), 1u);
    EXPECT_FALSE(session.isSessionComplete());
  }
  EXPECT_TRUE(Storage.exists(srsPath.c_str()));
  EXPECT_FALSE(Storage.exists(walPath.c_str()));

  // Finally rate Good to complete the session.
  ASSERT_TRUE(session.submitRating(Rating::Good));
  EXPECT_EQ(session.remainingCount(), 0u);
  EXPECT_TRUE(session.isSessionComplete());
  session.close();
}

TEST_F(FlashcardsStorageTest, MaxSrsRecordsCapAndZeroMidSessionReallocation) {
  ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));
  const char* srsPath = "/.crosspoint/flashcards/cap.srs";
  const char* walPath = "/.crosspoint/flashcards/cap.wal";

  // 1. Oversized .srs file (MAX_SRS_RECORDS + 1) is rejected cleanly without allocating.
  {
    SrsFileHeader overHead{};
    overHead.recordCount = MAX_SRS_RECORDS + 1;
    std::vector<uint8_t> raw(sizeof(SrsFileHeader) + static_cast<size_t>(overHead.recordCount) * sizeof(CardSrsRecord),
                             0);
    std::memcpy(raw.data(), &overHead, sizeof(overHead));
    flashcards_fs::setFileBytes(srsPath, raw);

    FlashcardSrsStore store;
    EXPECT_FALSE(store.load(srsPath, walPath));
  }

  // 2. At exact MAX_SRS_RECORDS (4096), in-place updates succeed while new insertions fail soft before touching .wal.
  {
    SrsFileHeader fullHead{};
    fullHead.recordCount = MAX_SRS_RECORDS;
    std::vector<uint8_t> raw(sizeof(SrsFileHeader) + static_cast<size_t>(MAX_SRS_RECORDS) * sizeof(CardSrsRecord), 0);
    std::memcpy(raw.data(), &fullHead, sizeof(fullHead));
    for (uint32_t i = 0; i < MAX_SRS_RECORDS; ++i) {
      CardSrsRecord rec{};
      rec.cardIdHash = static_cast<uint64_t>(i + 1);
      rec.stability = 2.0f;
      rec.difficultyPermille = 5000;
      rec.dueDay = 5;
      rec.lastReviewDay = 1;
      rec.state = static_cast<uint8_t>(CardState::Review);
      std::memcpy(raw.data() + sizeof(SrsFileHeader) + static_cast<size_t>(i) * sizeof(CardSrsRecord), &rec,
                  sizeof(rec));
    }
    flashcards_fs::setFileBytes(srsPath, raw);

    FlashcardSrsStore store;
    ASSERT_TRUE(store.load(srsPath, walPath, MAX_NEW_PER_DAY));
    ASSERT_EQ(store.records().size(), MAX_SRS_RECORDS);

    CardSrsRecord existingUpdate = *store.findCard(2000ULL);
    existingUpdate.dueDay = 12;
    ASSERT_TRUE(store.recordRating(existingUpdate, false));
    EXPECT_EQ(store.records().size(), MAX_SRS_RECORDS);
    EXPECT_EQ(store.findCard(2000ULL)->dueDay, 12u);
    const size_t walAfterValidUpdate = flashcards_fs::getFileBytes(walPath).size();
    EXPECT_EQ(walAfterValidUpdate, sizeof(WalEntry));

    CardSrsRecord overflowNew{};
    overflowNew.cardIdHash = static_cast<uint64_t>(MAX_SRS_RECORDS + 99);
    overflowNew.state = static_cast<uint8_t>(CardState::Review);
    EXPECT_FALSE(store.recordRating(overflowNew, true));
    EXPECT_EQ(store.records().size(), MAX_SRS_RECORDS);
    EXPECT_EQ(flashcards_fs::getFileBytes(walPath).size(), walAfterValidUpdate);
  }

  // 3. FlashcardSession::open pre-reserves sortedRecords upfront so rating new cards causes zero mid-session
  // reallocations.
  {
    const std::string deckPath = "/flashcards/prerev.jsonl";
    const std::string jsonl = R"({"id":"r1","front":"Q1","back":"A1"})"
                              "\n"
                              R"({"id":"r2","front":"Q2","back":"A2"})"
                              "\n"
                              R"({"id":"r3","front":"Q3","back":"A3"})"
                              "\n";
    flashcards_fs::setFile(deckPath, jsonl);

    FlashcardSession s1;
    ASSERT_TRUE(s1.open(deckPath.c_str(), "/.crosspoint/flashcards"));
    ASSERT_GE(s1.srsStore().records().capacity(), static_cast<size_t>(MAX_NEW_PER_DAY));
    const CardSrsRecord* initialBufPtr = s1.srsStore().records().data();
    ASSERT_TRUE(s1.submitRating(Rating::Good));
    ASSERT_TRUE(s1.submitRating(Rating::Good));
    EXPECT_EQ(s1.srsStore().records().data(), initialBufPtr);
    s1.close();
  }
}

TEST_F(FlashcardsStorageTest, OomProbeFailurePathsFailSoft) {
  const std::string deckPath = "/flashcards/oom.jsonl";
  const std::string jsonl = R"({"id":"o1","front":"Front1","back":"Back1"})"
                            "\n"
                            R"({"id":"o2","front":"Front2","back":"Back2"})"
                            "\n";
  flashcards_fs::setFile(deckPath, jsonl);
  ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));

  // Direct FlashcardSrsStore::load OOM on extraCapacityHint probe.
  {
    FlashcardSrsStore store;
    flashcards_test_mem::failAllocCountdown = 0;
    EXPECT_FALSE(store.load("/.crosspoint/flashcards/oom_direct.srs", "/.crosspoint/flashcards/oom_direct.wal",
                            MAX_NEW_PER_DAY));
    flashcards_test_mem::failAllocCountdown = -1;
  }

  // Direct FlashcardSrsStore::recordRating OOM when growing from 0 capacity without hint.
  {
    FlashcardSrsStore store;
    ASSERT_TRUE(store.load("/.crosspoint/flashcards/oom_grow.srs", "/.crosspoint/flashcards/oom_grow.wal", 0));
    CardSrsRecord card{};
    card.cardIdHash = 123ULL;
    card.state = static_cast<uint8_t>(CardState::Review);
    flashcards_test_mem::failAllocCountdown = 0;
    EXPECT_FALSE(store.recordRating(card, true));
    EXPECT_FALSE(Storage.exists("/.crosspoint/flashcards/oom_grow.wal"));
    flashcards_test_mem::failAllocCountdown = -1;
  }

  // Every heap allocation point in FlashcardSession::open fails soft without crashing or leaving opened == true.
  for (int failStep = 0; failStep < 5; ++failStep) {
    flashcards_fs::reset();
    flashcards_fs::setFile(deckPath, jsonl);
    FlashcardSession session;
    flashcards_test_mem::failAllocCountdown = failStep;
    const bool ok = session.open(deckPath.c_str(), "/.crosspoint/flashcards");
    if (flashcards_test_mem::failAllocCountdown == -1 && !ok) {
      EXPECT_FALSE(session.isOpen());
    } else {
      session.close();
    }
    flashcards_test_mem::failAllocCountdown = -1;
  }

  // Subsequent open with normal heap succeeds cleanly.
  FlashcardSession healthy;
  ASSERT_TRUE(healthy.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  EXPECT_EQ(healthy.remainingCount(), 2u);
  healthy.close();
}

TEST_F(FlashcardsStorageTest, FreeReviewDrillingPreservesSrsAndWalState) {
  const std::string deckPath = "/flashcards/freerev.jsonl";
  std::string jsonl = R"({"_meta":true,"title":"Practice Deck","newPerDay":8,"maxReviews":100})"
                      "\n";
  for (int i = 0; i < 12; ++i) {
    jsonl += "{\"id\":\"f" + std::to_string(i) + "\",\"front\":\"Front" + std::to_string(i) + "\",\"back\":\"Back" +
             std::to_string(i) + "\"}\n";
  }
  flashcards_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  FlashcardSrsStore::buildPaths("/.crosspoint/flashcards", deckPath, idxPath, srsPath, walPath);

  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  ASSERT_EQ(session.remainingCount(), 8u);
  EXPECT_FALSE(session.isFreeReview());
  EXPECT_FALSE(session.lastPassWasFreeReview());
  EXPECT_EQ(session.countFreeReviewCards(), 0u);
  EXPECT_FALSE(session.startFreeReview());
  EXPECT_FALSE(session.submitFreeReview(false));

  // Complete Session 1 with varied ratings across f0..f7 (leaving f8..f11 unseen New).
  ASSERT_TRUE(session.submitRating(Rating::Again));  // f0 -> Learning, requeued 6 ahead
  ASSERT_TRUE(session.submitRating(Rating::Hard));   // f1 -> dueDay 2
  ASSERT_TRUE(session.submitRating(Rating::Good));   // f2 -> dueDay 4
  ASSERT_TRUE(session.submitRating(Rating::Easy));   // f3 -> dueDay 17
  ASSERT_TRUE(session.submitRating(Rating::Good));   // f4 -> dueDay 4
  ASSERT_TRUE(session.submitRating(Rating::Hard));   // f5 -> dueDay 2
  ASSERT_TRUE(session.submitRating(Rating::Easy));   // f6 -> dueDay 17
  ASSERT_TRUE(session.submitRating(Rating::Good));   // f0 (requeued) -> Review
  ASSERT_TRUE(session.submitRating(Rating::Good));   // f7 -> dueDay 4

  ASSERT_TRUE(session.isSessionComplete());
  EXPECT_FALSE(session.isFreeReview());
  EXPECT_FALSE(session.lastPassWasFreeReview());
  EXPECT_EQ(session.sessionDay(), 1u);
  EXPECT_EQ(session.reviewsThisSession(), 9u);
  EXPECT_EQ(session.countFreeReviewCards(), 8u);

  // Snapshot exact SRS records, WAL bytes, and write counters before Free Review begins.
  const std::vector<CardSrsRecord> recordsBefore = session.srsStore().records();
  ASSERT_EQ(recordsBefore.size(), 8u);
  const std::vector<uint8_t> srsBytesBefore =
      Storage.exists(srsPath.c_str()) ? flashcards_fs::getFileBytes(srsPath) : std::vector<uint8_t>{};
  const std::vector<uint8_t> walBytesBefore = flashcards_fs::getFileBytes(walPath);
  const unsigned srsWritesBefore = flashcards_fs::writeCount(srsPath);
  const unsigned walWritesBefore = flashcards_fs::writeCount(walPath);
  uint16_t dueBefore = 0;
  uint16_t newBefore = 0;
  session.previewNextSession(dueBefore, newBefore);

  // Start Free Review over the 8 Session 1 cards.
  ASSERT_TRUE(session.startFreeReview());
  EXPECT_TRUE(session.isFreeReview());
  EXPECT_FALSE(session.isSessionComplete());
  EXPECT_FALSE(session.lastPassWasFreeReview());
  EXPECT_EQ(session.freeReviewRatingsCount(), 0u);
  EXPECT_EQ(session.remainingCount(), 8u);

  // Normal FSRS ratings are rejected during Free Review.
  EXPECT_FALSE(session.submitRating(Rating::Good));
  EXPECT_FALSE(session.submitRating(Rating::Again));

  FlashcardItem card;
  CardSrsRecord srs;
  IntervalPreview preview{99, 99, 99};
  ASSERT_TRUE(session.currentCard(card, srs, preview));
  EXPECT_EQ(card.front, "Front0");
  EXPECT_EQ(preview.hardDays, 0u);
  EXPECT_EQ(preview.goodDays, 0u);
  EXPECT_EQ(preview.easyDays, 0u);

  // "Again" on Front0 re-queues it 6 cards ahead (after Front1..Front6) in RAM only.
  ASSERT_TRUE(session.submitFreeReview(true));
  EXPECT_EQ(session.remainingCount(), 8u);
  EXPECT_EQ(session.freeReviewRatingsCount(), 1u);

  for (int expected = 1; expected <= 6; ++expected) {
    ASSERT_TRUE(session.currentCard(card, srs, preview));
    EXPECT_EQ(card.front, "Front" + std::to_string(expected));
    ASSERT_TRUE(session.submitFreeReview(false));
  }

  // Front0 reappears at slot 6; clear it with "Got It".
  ASSERT_TRUE(session.currentCard(card, srs, preview));
  EXPECT_EQ(card.front, "Front0");
  ASSERT_TRUE(session.submitFreeReview(false));
  EXPECT_EQ(session.remainingCount(), 1u);

  // Last remaining card Front7: "Again" keeps it at the front without completing the pass.
  ASSERT_TRUE(session.currentCard(card, srs, preview));
  EXPECT_EQ(card.front, "Front7");
  ASSERT_TRUE(session.submitFreeReview(true));
  EXPECT_EQ(session.remainingCount(), 1u);
  EXPECT_TRUE(session.isFreeReview());
  EXPECT_FALSE(session.isSessionComplete());

  ASSERT_TRUE(session.currentCard(card, srs, preview));
  EXPECT_EQ(card.front, "Front7");
  ASSERT_TRUE(session.submitFreeReview(false));

  // Free Review pass complete: back at SessionComplete boundary with zero SRS/WAL mutations.
  EXPECT_EQ(session.remainingCount(), 0u);
  EXPECT_FALSE(session.isFreeReview());
  EXPECT_TRUE(session.isSessionComplete());
  EXPECT_TRUE(session.lastPassWasFreeReview());
  EXPECT_EQ(session.freeReviewRatingsCount(), 10u);

  EXPECT_EQ(session.sessionDay(), 1u);
  EXPECT_EQ(session.reviewsThisSession(), 9u);
  EXPECT_EQ(flashcards_fs::writeCount(srsPath), srsWritesBefore);
  EXPECT_EQ(flashcards_fs::writeCount(walPath), walWritesBefore);
  EXPECT_EQ(Storage.exists(srsPath.c_str()) ? flashcards_fs::getFileBytes(srsPath) : std::vector<uint8_t>{},
            srsBytesBefore);
  EXPECT_EQ(flashcards_fs::getFileBytes(walPath), walBytesBefore);

  const std::vector<CardSrsRecord>& recordsAfter = session.srsStore().records();
  ASSERT_EQ(recordsAfter.size(), recordsBefore.size());
  for (size_t i = 0; i < recordsBefore.size(); ++i) {
    EXPECT_EQ(recordsAfter[i].cardIdHash, recordsBefore[i].cardIdHash);
    EXPECT_FLOAT_EQ(recordsAfter[i].stability, recordsBefore[i].stability);
    EXPECT_EQ(recordsAfter[i].difficultyPermille, recordsBefore[i].difficultyPermille);
    EXPECT_EQ(recordsAfter[i].dueDay, recordsBefore[i].dueDay);
    EXPECT_EQ(recordsAfter[i].lastReviewDay, recordsBefore[i].lastReviewDay);
    EXPECT_EQ(recordsAfter[i].state, recordsBefore[i].state);
    EXPECT_EQ(recordsAfter[i].lapses, recordsBefore[i].lapses);
  }

  uint16_t dueAfter = 0;
  uint16_t newAfter = 0;
  session.previewNextSession(dueAfter, newAfter);
  EXPECT_EQ(dueAfter, dueBefore);
  EXPECT_EQ(newAfter, newBefore);
  session.close();

  // Re-opening from disk still finds the 8 Session 1 cards via lastReviewDay == 1, then advances cleanly to Session 2.
  FlashcardSession reopened;
  ASSERT_TRUE(reopened.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  EXPECT_TRUE(reopened.isSessionComplete());
  EXPECT_FALSE(reopened.lastPassWasFreeReview());
  EXPECT_EQ(reopened.countFreeReviewCards(), 8u);
  ASSERT_TRUE(reopened.startFreeReview());
  EXPECT_EQ(reopened.remainingCount(), 8u);

  ASSERT_TRUE(reopened.startNextSession());
  EXPECT_FALSE(reopened.isFreeReview());
  EXPECT_FALSE(reopened.lastPassWasFreeReview());
  EXPECT_EQ(reopened.freeReviewRatingsCount(), 0u);
  EXPECT_EQ(reopened.sessionDay(), 2u);
  EXPECT_FALSE(reopened.isSessionComplete());
  EXPECT_EQ(reopened.remainingCount(), static_cast<size_t>(dueBefore + newBefore));
  reopened.close();
}

TEST_F(FlashcardsStorageTest, FreeReviewFallbackPassAndOomFailSoft) {
  const std::string deckPath = "/flashcards/fallback.jsonl";
  const std::string jsonl = R"({"_meta":true,"title":"Fallback Deck","newPerDay":2,"maxReviews":50})"
                            "\n"
                            R"({"id":"a","front":"QA","back":"AA"})"
                            "\n"
                            R"({"id":"b","front":"QB","back":"AB"})"
                            "\n"
                            R"({"id":"c","front":"QC","back":"AC"})"
                            "\n";
  flashcards_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  FlashcardSrsStore::buildPaths("/.crosspoint/flashcards", deckPath, idxPath, srsPath, walPath);

  // Study 2 cards on Day 1, then write a completed Day 3 header where no card has lastReviewDay == 3.
  {
    FlashcardSession s1;
    ASSERT_TRUE(s1.open(deckPath.c_str(), "/.crosspoint/flashcards"));
    ASSERT_TRUE(s1.submitRating(Rating::Easy));
    ASSERT_TRUE(s1.submitRating(Rating::Easy));
    ASSERT_TRUE(s1.isSessionComplete());
    s1.close();

    FlashcardSrsStore store;
    ASSERT_TRUE(store.load(srsPath.c_str(), walPath.c_str()));
    ASSERT_TRUE(store.advanceSession(3));
    ASSERT_TRUE(store.markSessionComplete());
    ASSERT_TRUE(store.compact());
  }

  FlashcardSession fallbackSession;
  ASSERT_TRUE(fallbackSession.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  ASSERT_TRUE(fallbackSession.isSessionComplete());
  EXPECT_EQ(fallbackSession.sessionDay(), 3u);
  // Primary pass (lastReviewDay == 3) finds 0; fallback pass finds the 2 studied cards (skipping unseen "c").
  EXPECT_EQ(fallbackSession.countFreeReviewCards(), 2u);

  // OOM during countFreeReviewCards and startFreeReview fails soft without entering Free Review.
  flashcards_test_mem::failAllocCountdown = 0;
  EXPECT_EQ(fallbackSession.countFreeReviewCards(), 0u);
  flashcards_test_mem::failAllocCountdown = 0;
  EXPECT_FALSE(fallbackSession.startFreeReview());
  flashcards_test_mem::failAllocCountdown = -1;
  EXPECT_FALSE(fallbackSession.isFreeReview());
  EXPECT_TRUE(fallbackSession.isSessionComplete());

  ASSERT_TRUE(fallbackSession.startFreeReview());
  EXPECT_TRUE(fallbackSession.isFreeReview());
  EXPECT_EQ(fallbackSession.remainingCount(), 2u);
  fallbackSession.close();
}

TEST_F(FlashcardsStorageTest, StrictSequentialEmptySessionsFreeReviewAndElapsedDays) {
  const std::string deckPath = "/flashcards/seq5.jsonl";
  const std::string jsonl = R"({"_meta":true,"title":"Seq Deck","newPerDay":20,"maxReviews":100})"
                            "\n"
                            R"({"id":"s1","front":"CardOne","back":"AnsOne"})"
                            "\n"
                            R"({"id":"s2","front":"CardTwo","back":"AnsTwo"})"
                            "\n";
  flashcards_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  FlashcardSrsStore::buildPaths("/.crosspoint/flashcards", deckPath, idxPath, srsPath, walPath);

  // Seed Session 1 completion where all cards in S1 were reviewed on day 1 and scheduled for S5.
  CardSrsRecord rec1{};
  rec1.cardIdHash = fnv1a64("s1");
  rec1.stability = 4.0f;
  rec1.difficultyPermille = 5200;
  rec1.dueDay = 5;
  rec1.lastReviewDay = 1;
  rec1.state = static_cast<uint8_t>(CardState::Review);

  CardSrsRecord rec2 = rec1;
  rec2.cardIdHash = fnv1a64("s2");

  {
    ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));
    FlashcardSrsStore store;
    ASSERT_TRUE(store.load(srsPath.c_str(), walPath.c_str()));
    ASSERT_TRUE(store.recordRating(rec1, true));
    ASSERT_TRUE(store.recordRating(rec2, true));
    ASSERT_TRUE(store.markSessionComplete());
  }

  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  ASSERT_TRUE(session.isSessionComplete());
  EXPECT_EQ(session.sessionDay(), 1u);
  EXPECT_EQ(session.reviewsThisSession(), 2u);

  // From S1: next session is S2 (strict +1), preview reports 0 due, 0 new, nextScheduledDay == 5.
  EXPECT_EQ(session.computeNextSessionDay(), 2u);
  uint16_t nextDue = 99;
  uint16_t nextNew = 99;
  uint16_t nextScheduled = 0;
  session.previewNextSession(nextDue, nextNew, &nextScheduled);
  EXPECT_EQ(nextDue, 0u);
  EXPECT_EQ(nextNew, 0u);
  EXPECT_EQ(nextScheduled, 5u);

  // Step S1 -> S2: empty session enters SessionComplete immediately with 0 reviews.
  ASSERT_TRUE(session.startNextSession());
  EXPECT_EQ(session.sessionDay(), 2u);
  EXPECT_TRUE(session.isSessionComplete());
  EXPECT_EQ(session.reviewsThisSession(), 0u);
  EXPECT_EQ(session.remainingCount(), 0u);
  EXPECT_EQ(session.computeNextSessionDay(), 3u);
  session.previewNextSession(nextDue, nextNew, &nextScheduled);
  EXPECT_EQ(nextDue, 0u);
  EXPECT_EQ(nextNew, 0u);
  EXPECT_EQ(nextScheduled, 5u);

  // Practice Again on empty S2 loads the 2 studied cards via fallback without touching SRS or WAL.
  const std::vector<uint8_t> srsBeforeFree = flashcards_fs::getFileBytes(srsPath);
  const unsigned srsWritesBeforeFree = flashcards_fs::writeCount(srsPath);
  const unsigned walWritesBeforeFree = flashcards_fs::writeCount(walPath);
  EXPECT_EQ(session.countFreeReviewCards(), 2u);
  ASSERT_TRUE(session.startFreeReview());
  EXPECT_TRUE(session.isFreeReview());
  EXPECT_EQ(session.remainingCount(), 2u);
  ASSERT_TRUE(session.submitFreeReview(false));
  ASSERT_TRUE(session.submitFreeReview(false));
  EXPECT_FALSE(session.isFreeReview());
  EXPECT_TRUE(session.isSessionComplete());
  EXPECT_TRUE(session.lastPassWasFreeReview());
  EXPECT_EQ(session.sessionDay(), 2u);
  EXPECT_EQ(session.reviewsThisSession(), 0u);
  EXPECT_EQ(flashcards_fs::writeCount(srsPath), srsWritesBeforeFree);
  EXPECT_EQ(flashcards_fs::writeCount(walPath), walWritesBeforeFree);
  EXPECT_EQ(flashcards_fs::getFileBytes(srsPath), srsBeforeFree);
  session.close();

  // Verify inspectDeck() and open() on an uncompleted empty session (e.g. S3 before markSessionComplete) do not
  // fast-forward: both keep sessionDay == 3 and mark sessionCompleted == true.
  {
    FlashcardSrsStore store;
    ASSERT_TRUE(store.load(srsPath.c_str(), walPath.c_str()));
    ASSERT_TRUE(store.advanceSession(3));
    ASSERT_TRUE(store.compact());
  }
  FlashcardSession::DeckSummary sumS3{};
  ASSERT_TRUE(FlashcardSession::inspectDeck(deckPath.c_str(), "/.crosspoint/flashcards", sumS3));
  EXPECT_EQ(sumS3.sessionDay, 3u);
  EXPECT_EQ(sumS3.dueCount, 0u);
  EXPECT_EQ(sumS3.newCount, 0u);
  EXPECT_TRUE(sumS3.sessionCompleted);

  ASSERT_TRUE(session.open(deckPath.c_str(), "/.crosspoint/flashcards"));
  EXPECT_EQ(session.sessionDay(), 3u);
  EXPECT_TRUE(session.isSessionComplete());
  EXPECT_EQ(session.reviewsThisSession(), 0u);
  session.previewNextSession(nextDue, nextNew, &nextScheduled);
  EXPECT_EQ(nextDue, 0u);
  EXPECT_EQ(nextNew, 0u);
  EXPECT_EQ(nextScheduled, 5u);

  // Step S3 -> S4: S4 is still empty, while Up Next (S5) previews 2 due, 0 new (nextScheduled == 0).
  ASSERT_TRUE(session.startNextSession());
  EXPECT_EQ(session.sessionDay(), 4u);
  EXPECT_TRUE(session.isSessionComplete());
  EXPECT_EQ(session.reviewsThisSession(), 0u);
  session.previewNextSession(nextDue, nextNew, &nextScheduled);
  EXPECT_EQ(nextDue, 2u);
  EXPECT_EQ(nextNew, 0u);
  EXPECT_EQ(nextScheduled, 0u);

  // Step S4 -> S5: cards are now due on S5 with truthful elapsedDays == 4 (today=5 - lastReviewDay=1).
  ASSERT_TRUE(session.startNextSession());
  EXPECT_EQ(session.sessionDay(), 5u);
  EXPECT_FALSE(session.isSessionComplete());
  EXPECT_EQ(session.remainingCount(), 2u);

  FlashcardItem activeCard;
  CardSrsRecord activeSrs;
  IntervalPreview activePreview{};
  ASSERT_TRUE(session.currentCard(activeCard, activeSrs, activePreview));
  const IntervalPreview expectedPreview = FlashcardScheduler::previewIntervals(rec1, 5);
  EXPECT_EQ(activePreview.hardDays, expectedPreview.hardDays);
  EXPECT_EQ(activePreview.goodDays, expectedPreview.goodDays);
  EXPECT_EQ(activePreview.easyDays, expectedPreview.easyDays);

  const CardSrsRecord expectedRated = FlashcardScheduler::reviewCard(rec1, Rating::Good, 5);
  ASSERT_TRUE(session.submitRating(Rating::Good));
  ASSERT_TRUE(session.submitRating(Rating::Good));
  EXPECT_TRUE(session.isSessionComplete());
  EXPECT_EQ(session.reviewsThisSession(), 2u);

  const CardSrsRecord* persisted1 = session.srsStore().findCard(rec1.cardIdHash);
  ASSERT_NE(persisted1, nullptr);
  EXPECT_FLOAT_EQ(persisted1->stability, expectedRated.stability);
  EXPECT_EQ(persisted1->difficultyPermille, expectedRated.difficultyPermille);
  EXPECT_EQ(persisted1->dueDay, expectedRated.dueDay);
  EXPECT_EQ(persisted1->lastReviewDay, 5u);
  session.close();
}

}  // namespace
}  // namespace flashcards
