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

#include "AnkiDeckIndex.h"
#include "AnkiScheduler.h"
#include "AnkiSession.h"
#include "AnkiSrsStore.h"
#include "AnkiTypes.h"

namespace anki {
namespace {

class AnkiStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    anki_fs::reset();
    anki_test_mem::failAllocCountdown = -1;
  }

  // Deep-copies the in-memory filesystem to simulate sudden 0V battery cutoff before close()/destructors run.
  static std::map<std::string, anki_fs::Node> snapshotFs() {
    std::map<std::string, anki_fs::Node> snap;
    for (const auto& [path, node] : anki_fs::files) {
      if (node) {
        snap.emplace(path, *node);
      }
    }
    return snap;
  }

  static void restoreFs(const std::map<std::string, anki_fs::Node>& snap) {
    anki_fs::reset();
    anki_fs::files.clear();
    for (const auto& [path, nodeCopy] : snap) {
      anki_fs::files.emplace(path, std::make_shared<anki_fs::Node>(nodeCopy));
    }
  }
};

// ============================================================================
// AnkiTypes Tests
// ============================================================================

TEST(AnkiTypesTest, StructSizeAndLayoutInvariants) {
  EXPECT_EQ(sizeof(DeckIndexHeader), 84u);
  EXPECT_EQ(sizeof(CardIndexRecord), 16u);
  EXPECT_EQ(sizeof(SrsFileHeader), 24u);
  EXPECT_EQ(sizeof(CardSrsRecord), 24u);
  EXPECT_EQ(sizeof(WalEntry), 32u);
  EXPECT_EQ(offsetof(WalEntry, crc32), 28u);
}

TEST(AnkiTypesTest, Fnv1a64KnownVectors) {
  static_assert(fnv1a64("") == 0xcbf29ce484222325ULL);
  static_assert(fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
  static_assert(fnv1a64("foobar") == 0x85944171f73967e8ULL);

  EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv1a64("foobar"), 0x85944171f73967e8ULL);
  EXPECT_NE(fnv1a64("食べる"), fnv1a64("飲む"));
  EXPECT_EQ(fnv1a64("食べる"), fnv1a64(std::string_view("食べる")));
}

TEST(AnkiTypesTest, Crc32KnownCheckVector) {
  constexpr const char kCheck[] = "123456789";
  EXPECT_EQ(crc32Anki(kCheck, 9), 0xCBF43926u);
  EXPECT_EQ(crc32Anki(nullptr, 0), 0u);
  EXPECT_EQ(crc32Anki(kCheck, 0), 0u);

  char flipped[10];
  std::memcpy(flipped, kCheck, sizeof(kCheck));
  flipped[4] ^= 0x01;
  EXPECT_NE(crc32Anki(flipped, 9), 0xCBF43926u);
}

TEST(AnkiTypesTest, FormatDeckHashHex) {
  char hex[17];
  std::memset(hex, 'X', sizeof(hex));
  formatDeckHashHex("", hex);
  EXPECT_STREQ(hex, "cbf29ce484222325");
  EXPECT_EQ(std::strlen(hex), 16u);

  formatDeckHashHex("foobar", hex);
  EXPECT_STREQ(hex, "85944171f73967e8");
}

// ============================================================================
// AnkiScheduler Tests (FSRS-5)
// ============================================================================

TEST(AnkiSchedulerTest, NewCardAllRatings) {
  CardSrsRecord card{};
  card.cardIdHash = 0x1234ULL;
  card.state = static_cast<uint8_t>(CardState::New);

  const IntervalPreview preview = AnkiScheduler::previewIntervals(card, 1);
  EXPECT_EQ(preview.hardDays, 1u);
  EXPECT_EQ(preview.goodDays, 3u);
  EXPECT_EQ(preview.easyDays, 16u);

  const CardSrsRecord onAgain = AnkiScheduler::reviewCard(card, Rating::Again, 1);
  EXPECT_EQ(onAgain.cardIdHash, 0x1234ULL);
  EXPECT_EQ(static_cast<CardState>(onAgain.state), CardState::Learning);
  EXPECT_EQ(onAgain.dueDay, 1u);
  EXPECT_EQ(onAgain.lastReviewDay, 1u);
  EXPECT_EQ(onAgain.lapses, 0u);
  EXPECT_NEAR(onAgain.stability, 0.40255f, 1e-4f);
  EXPECT_GE(onAgain.difficultyPermille, 1000u);
  EXPECT_LE(onAgain.difficultyPermille, 10000u);

  const CardSrsRecord onHard = AnkiScheduler::reviewCard(card, Rating::Hard, 1);
  EXPECT_EQ(static_cast<CardState>(onHard.state), CardState::Review);
  EXPECT_EQ(onHard.dueDay, static_cast<uint16_t>(1u + preview.hardDays));
  EXPECT_NEAR(onHard.stability, 1.18385f, 1e-4f);

  const CardSrsRecord onGood = AnkiScheduler::reviewCard(card, Rating::Good, 1);
  EXPECT_EQ(static_cast<CardState>(onGood.state), CardState::Review);
  EXPECT_EQ(onGood.dueDay, static_cast<uint16_t>(1u + preview.goodDays));
  EXPECT_NEAR(onGood.stability, 3.17300f, 1e-4f);

  const CardSrsRecord onEasy = AnkiScheduler::reviewCard(card, Rating::Easy, 1);
  EXPECT_EQ(static_cast<CardState>(onEasy.state), CardState::Review);
  EXPECT_EQ(onEasy.dueDay, static_cast<uint16_t>(1u + preview.easyDays));
  EXPECT_NEAR(onEasy.stability, 15.69105f, 1e-4f);

  EXPECT_GE(onAgain.difficultyPermille, onHard.difficultyPermille);
  EXPECT_GE(onHard.difficultyPermille, onGood.difficultyPermille);
  EXPECT_GE(onGood.difficultyPermille, onEasy.difficultyPermille);
}

TEST(AnkiSchedulerTest, SameSessionLearningStepElapsedZero) {
  CardSrsRecord card{};
  card.cardIdHash = 42ULL;
  const CardSrsRecord learning = AnkiScheduler::reviewCard(card, Rating::Again, 5);
  ASSERT_EQ(static_cast<CardState>(learning.state), CardState::Learning);
  ASSERT_EQ(learning.lastReviewDay, 5u);

  // Another Again in the same session keeps Learning state and difficultyPermille unchanged.
  const CardSrsRecord againStep = AnkiScheduler::reviewCard(learning, Rating::Again, 5);
  EXPECT_EQ(static_cast<CardState>(againStep.state), CardState::Learning);
  EXPECT_EQ(againStep.dueDay, 5u);
  EXPECT_EQ(againStep.lastReviewDay, 5u);
  EXPECT_EQ(againStep.lapses, 0u);
  EXPECT_EQ(againStep.difficultyPermille, learning.difficultyPermille);
  EXPECT_LT(againStep.stability, learning.stability);

  // Good in the same session (elapsed == 0) graduates to Review with short-term stability growth.
  const IntervalPreview stepPreview = AnkiScheduler::previewIntervals(learning, 5);
  const CardSrsRecord graduated = AnkiScheduler::reviewCard(learning, Rating::Good, 5);
  EXPECT_EQ(static_cast<CardState>(graduated.state), CardState::Review);
  EXPECT_EQ(graduated.dueDay, static_cast<uint16_t>(5u + stepPreview.goodDays));
  EXPECT_EQ(graduated.difficultyPermille, learning.difficultyPermille);
  EXPECT_GT(graduated.stability, learning.stability);
}

TEST(AnkiSchedulerTest, InterSessionReviewRecallAndLapseSaturation) {
  CardSrsRecord card{};
  card.cardIdHash = 99ULL;
  card = AnkiScheduler::reviewCard(card, Rating::Good, 1);
  ASSERT_EQ(static_cast<CardState>(card.state), CardState::Review);
  ASSERT_EQ(card.dueDay, 4u);

  const IntervalPreview previewDay4 = AnkiScheduler::previewIntervals(card, 4);
  EXPECT_LE(previewDay4.hardDays, previewDay4.goodDays);
  EXPECT_LT(previewDay4.goodDays, previewDay4.easyDays);

  const CardSrsRecord recalledHard = AnkiScheduler::reviewCard(card, Rating::Hard, 4);
  const CardSrsRecord recalledGood = AnkiScheduler::reviewCard(card, Rating::Good, 4);
  const CardSrsRecord recalledEasy = AnkiScheduler::reviewCard(card, Rating::Easy, 4);

  EXPECT_EQ(recalledHard.dueDay, static_cast<uint16_t>(4u + previewDay4.hardDays));
  EXPECT_EQ(recalledGood.dueDay, static_cast<uint16_t>(4u + previewDay4.goodDays));
  EXPECT_EQ(recalledEasy.dueDay, static_cast<uint16_t>(4u + previewDay4.easyDays));
  EXPECT_LT(recalledHard.stability, recalledGood.stability);
  EXPECT_LT(recalledGood.stability, recalledEasy.stability);
  EXPECT_GT(recalledGood.stability, card.stability);

  // Lapse transitions Review -> Relearning, increments lapses, sets dueDay = sessionDay.
  const CardSrsRecord lapsed = AnkiScheduler::reviewCard(recalledGood, Rating::Again, 20);
  EXPECT_EQ(static_cast<CardState>(lapsed.state), CardState::Relearning);
  EXPECT_EQ(lapsed.dueDay, 20u);
  EXPECT_EQ(lapsed.lastReviewDay, 20u);
  EXPECT_EQ(lapsed.lapses, 1u);
  EXPECT_GE(lapsed.stability, 0.1f);
  EXPECT_LE(lapsed.stability, recalledGood.stability);

  // Saturate uint8_t lapses at 255 without wrapping to 0.
  CardSrsRecord highLapse = recalledGood;
  highLapse.lapses = 254;
  highLapse = AnkiScheduler::reviewCard(highLapse, Rating::Again, 25);
  EXPECT_EQ(highLapse.lapses, 255u);
  highLapse = AnkiScheduler::reviewCard(highLapse, Rating::Good, 25);
  ASSERT_EQ(static_cast<CardState>(highLapse.state), CardState::Review);
  highLapse = AnkiScheduler::reviewCard(highLapse, Rating::Again, 30);
  EXPECT_EQ(highLapse.lapses, 255u);
}

TEST(AnkiSchedulerTest, MonotonicPreviewsAndUint16DaySaturation) {
  for (uint16_t diff : {1000u, 5000u, 10000u}) {
    for (float stab : {0.1f, 0.4f, 1.2f, 10.0f, 500.0f, 50000.0f}) {
      CardSrsRecord rec{};
      rec.stability = stab;
      rec.difficultyPermille = diff;
      rec.lastReviewDay = 1;
      rec.dueDay = 10;
      rec.state = static_cast<uint8_t>(CardState::Review);

      const IntervalPreview p = AnkiScheduler::previewIntervals(rec, 10);
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

  const IntervalPreview nearMax = AnkiScheduler::previewIntervals(huge, 65530);
  EXPECT_GE(nearMax.hardDays, 1u);
  EXPECT_LE(nearMax.hardDays, nearMax.goodDays);
  EXPECT_LT(nearMax.goodDays, nearMax.easyDays);
  EXPECT_EQ(AnkiScheduler::reviewCard(huge, Rating::Easy, 65530).dueDay, 65535u);
  EXPECT_EQ(AnkiScheduler::reviewCard(huge, Rating::Easy, 65535).dueDay, 65535u);
  EXPECT_EQ(AnkiScheduler::reviewCard(huge, Rating::Good, 65535).dueDay, 65535u);
}

// ============================================================================
// AnkiDeckIndex Tests
// ============================================================================

TEST_F(AnkiStorageTest, DeckIndexMetaUtf8IdsHtmlMalformedAndLongLines) {
  std::string overlongLine = R"({"id":"too-long","front":")";
  overlongLine.append(4100, 'x');
  overlongLine += R"(","back":"ignored"})";
  ASSERT_GT(overlongLine.size(), MAX_JSONL_LINE_BYTES);

  std::string jsonl;
  jsonl += R"({"_meta":true,"title":"Japanese Core 2k","newPerDay":25,"maxReviews":150})" "\r\n";
  jsonl += "\r\n";
  jsonl += "   \t  \n";
  jsonl += R"({"id":"n5-001","front":"食べる","back":"to eat","hint":"たべる","extra":"毎日野菜を食べる。"})" "\r\n";
  jsonl += "{not valid json at all\n";
  jsonl += R"({"front":"","back":"missing front"})" "\n";
  jsonl += R"({"front":"missing back"})" "\n";
  jsonl += overlongLine + "\r\n";
  jsonl +=
      R"({"id":987654,"front":"<div>Line 1<br>Line 2</div>","back":"<p>A &amp; B &lt;C&gt;</p>",)"
      R"("hint":"&nbsp;<b>noun</b> ","extra":"&#39;quote&#39; &quot;dbl&quot;<br/>next"})"
      "\n";
  // Final line without trailing newline and without "id" (uses fallback hash of front).
  jsonl += R"({"front":"飲む","back":"to drink"})";

  anki_fs::setFile("/anki/jp.jsonl", jsonl);
  ASSERT_TRUE(AnkiDeckIndex::ensureDir("/.crosspoint/anki"));
  EXPECT_TRUE(Storage.exists("/.crosspoint"));
  EXPECT_TRUE(Storage.exists("/.crosspoint/anki"));

  AnkiDeckIndex index;
  ASSERT_TRUE(index.openOrBuild("/anki/jp.jsonl", "/.crosspoint/anki/jp.idx"));
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

  AnkiCard c0;
  ASSERT_TRUE(index.readCard(0, scratch.get(), MAX_JSONL_LINE_BYTES, c0));
  EXPECT_EQ(c0.cardIdHash, fnv1a64("n5-001"));
  EXPECT_EQ(c0.front, "食べる");
  EXPECT_EQ(c0.back, "to eat");
  EXPECT_EQ(c0.hint, "たべる");
  EXPECT_EQ(c0.extra, "毎日野菜を食べる。");

  AnkiCard c1;
  ASSERT_TRUE(index.readCard(1, scratch.get(), MAX_JSONL_LINE_BYTES, c1));
  EXPECT_EQ(c1.cardIdHash, fnv1a64("987654"));
  EXPECT_EQ(c1.front, "Line 1\nLine 2");
  EXPECT_EQ(c1.back, "A & B <C>");
  EXPECT_EQ(c1.hint, "noun");
  EXPECT_EQ(c1.extra, "'quote' \"dbl\"\nnext");

  AnkiCard c2;
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

TEST_F(AnkiStorageTest, DeckIndexCacheReuseAndRebuildOnSizeChange) {
  const std::string initialJsonl =
      R"({"front":"card1","back":"ans1"})" "\n"
      R"({"front":"card2","back":"ans2"})" "\n";
  anki_fs::setFile("/anki/vocab.jsonl", initialJsonl);
  ASSERT_TRUE(AnkiDeckIndex::ensureDir("/.crosspoint/anki"));

  const std::string idxPath = "/.crosspoint/anki/vocab.idx";
  const std::string tmpIdxPath = "/.crosspoint/anki/vocab.idx.tmp";

  {
    AnkiDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild("/anki/vocab.jsonl", idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 2u);
    EXPECT_STREQ(idx.header().title, "vocab");
    EXPECT_EQ(idx.header().newPerDay, DEFAULT_NEW_PER_DAY);
    EXPECT_EQ(idx.header().maxReviews, DEFAULT_MAX_REVIEWS);
    idx.close();
  }

  const unsigned writesAfterFirstBuild = anki_fs::writeCount(idxPath) + anki_fs::writeCount(tmpIdxPath);
  ASSERT_GT(writesAfterFirstBuild, 0u);

  // Re-opening unchanged .jsonl reuses the cached .idx without writing a new index.
  {
    AnkiDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild("/anki/vocab.jsonl", idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 2u);
    idx.close();
  }
  const unsigned writesAfterReuse = anki_fs::writeCount(idxPath) + anki_fs::writeCount(tmpIdxPath);
  EXPECT_EQ(writesAfterReuse, writesAfterFirstBuild);

  // Appending a card changes .jsonl size -> rebuilds .idx cleanly (even when .idx already exists on SdFat).
  const std::string expandedJsonl = initialJsonl + R"({"front":"card3","back":"ans3"})" "\n";
  anki_fs::setFile("/anki/vocab.jsonl", expandedJsonl);
  {
    AnkiDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild("/anki/vocab.jsonl", idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 3u);
    EXPECT_EQ(idx.header().deckFileSize, expandedJsonl.size());
    idx.close();
  }
  const unsigned writesAfterRebuild = anki_fs::writeCount(idxPath) + anki_fs::writeCount(tmpIdxPath);
  EXPECT_GT(writesAfterRebuild, writesAfterReuse);
}

// ============================================================================
// AnkiSrsStore & AnkiSession Tests
// ============================================================================

TEST_F(AnkiStorageTest, FullSessionFlowAndAgainIntraSessionRequeue) {
  std::string jsonl = R"({"_meta":true,"title":"Ten Deck","newPerDay":10,"maxReviews":100})" "\n";
  for (int i = 0; i < 12; ++i) {
    jsonl += "{\"id\":\"c" + std::to_string(i) + "\",\"front\":\"Q" + std::to_string(i) + "\",\"back\":\"A" +
             std::to_string(i) + "\"}\n";
  }
  anki_fs::setFile("/anki/ten.jsonl", jsonl);

  AnkiSession::DeckSummary summary{};
  ASSERT_TRUE(AnkiSession::inspectDeck("/anki/ten.jsonl", "/.crosspoint/anki", summary));
  EXPECT_EQ(summary.totalCards, 12u);
  EXPECT_EQ(summary.dueCount, 0u);
  EXPECT_EQ(summary.newCount, 10u);
  EXPECT_EQ(summary.sessionDay, 1u);
  EXPECT_FALSE(summary.sessionCompleted);
  EXPECT_STREQ(summary.title, "Ten Deck");

  AnkiSession session;
  ASSERT_TRUE(session.open("/anki/ten.jsonl", "/.crosspoint/anki"));
  EXPECT_FALSE(session.isSessionComplete());
  EXPECT_EQ(session.totalDeckCards(), 12u);
  EXPECT_EQ(session.sessionDay(), 1u);
  EXPECT_EQ(session.remainingCount(), 10u);
  EXPECT_STREQ(session.deckTitle(), "Ten Deck");

  AnkiCard card;
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
  AnkiSession reopened;
  ASSERT_TRUE(reopened.open("/anki/ten.jsonl", "/.crosspoint/anki"));
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

TEST_F(AnkiStorageTest, WalReplayOnPowerCutAndTornWalRecovery) {
  const std::string deckPath = "/anki/power.jsonl";
  std::string jsonl = R"({"_meta":true,"title":"Power Deck","newPerDay":5,"maxReviews":50})" "\n";
  for (int i = 0; i < 6; ++i) {
    jsonl += "{\"id\":\"p" + std::to_string(i) + "\",\"front\":\"Front" + std::to_string(i) + "\",\"back\":\"Back" +
             std::to_string(i) + "\"}\n";
  }
  anki_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  AnkiSrsStore::buildPaths("/.crosspoint/anki", deckPath, idxPath, srsPath, walPath);

  std::map<std::string, anki_fs::Node> midSessionPowerCut;
  {
    AnkiSession session;
    ASSERT_TRUE(session.open(deckPath.c_str(), "/.crosspoint/anki"));
    ASSERT_EQ(session.remainingCount(), 5u);

    // Rate p0 Good (graduated), p1 Again (Learning, due today), p2 Easy (graduated).
    ASSERT_TRUE(session.submitRating(Rating::Good));
    ASSERT_TRUE(session.submitRating(Rating::Again));
    ASSERT_TRUE(session.submitRating(Rating::Easy));

    // Confirm WAL exists on disk without .srs compaction yet (< 4096 B).
    EXPECT_TRUE(Storage.exists(walPath.c_str()));
    EXPECT_EQ(anki_fs::getFileBytes(walPath).size(), 3u * sizeof(WalEntry));

    // Capture exact SD card state mid-session before close() or destructor runs.
    midSessionPowerCut = snapshotFs();
    session.close();
  }

  // Restore mid-session power-cut snapshot and append 13 torn bytes to .wal (simulating mid-write brownout).
  restoreFs(midSessionPowerCut);
  const uint8_t tornTail[13] = {0xAA, 0x55, 0x01, 0x02, 0x03, 0xFF, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70};
  anki_fs::appendRawBytes(walPath, tornTail, sizeof(tornTail));

  {
    AnkiSession recovered;
    ASSERT_TRUE(recovered.open(deckPath.c_str(), "/.crosspoint/anki"));
    EXPECT_EQ(recovered.sessionDay(), 1u);
    EXPECT_FALSE(recovered.isSessionComplete());
    EXPECT_EQ(recovered.reviewsThisSession(), 3u);
    // Remaining in Session 1: p1 (due today in Learning) + 2 remaining New budget (p3, p4) = 3 cards.
    EXPECT_EQ(recovered.remainingCount(), 3u);

    AnkiCard c;
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
  anki_fs::appendRawBytes(walPath, &badFrame, sizeof(badFrame));

  {
    AnkiSrsStore store;
    ASSERT_TRUE(store.load(srsPath.c_str(), walPath.c_str()));
    EXPECT_EQ(store.header().reviewsThisSession, 3u);
    EXPECT_EQ(store.header().newThisSession, 3u);
    EXPECT_NE(store.findCard(fnv1a64("p0")), nullptr);
    EXPECT_NE(store.findCard(fnv1a64("p1")), nullptr);
    EXPECT_NE(store.findCard(fnv1a64("p2")), nullptr);
    EXPECT_EQ(store.findCard(fnv1a64("p3")), nullptr);
  }
}

TEST_F(AnkiStorageTest, EmptyDayFastForwardNewPerDayOverrideAndResetDeck) {
  const std::string deckPath = "/anki/mastered.jsonl";
  const std::string jsonl =
      R"({"_meta":true,"title":"Short Deck","newPerDay":20,"maxReviews":100})" "\n"
      R"({"id":"m1","front":"Alpha","back":"One"})" "\n"
      R"({"id":"m2","front":"Beta","back":"Two"})" "\n"
      R"({"id":"m3","front":"Gamma","back":"Three"})" "\n";
  anki_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  AnkiSrsStore::buildPaths("/.crosspoint/anki", deckPath, idxPath, srsPath, walPath);

  // Override newPerDay to 2 before opening session.
  {
    ASSERT_TRUE(AnkiDeckIndex::ensureDir("/.crosspoint/anki"));
    AnkiSrsStore store;
    ASSERT_TRUE(store.load(srsPath.c_str(), walPath.c_str()));
    ASSERT_TRUE(store.setNewPerDayOverride(2));
    EXPECT_EQ(store.effectiveNewPerDay(20), 2u);
  }

  // Session 1 introduces only 2 cards (m1, m2) because of newPerDayOverride == 2.
  {
    AnkiSession s1;
    ASSERT_TRUE(s1.open(deckPath.c_str(), "/.crosspoint/anki"));
    EXPECT_EQ(s1.remainingCount(), 2u);
    ASSERT_TRUE(s1.submitRating(Rating::Easy));  // dueDay = 17
    ASSERT_TRUE(s1.submitRating(Rating::Easy));  // dueDay = 17
    EXPECT_TRUE(s1.isSessionComplete());

    // 1 unseen New card (m3) still remains, so next session day is 2 (no fast-forward yet).
    EXPECT_EQ(s1.computeNextSessionDay(), 2u);
    ASSERT_TRUE(s1.startNextSession());
    EXPECT_EQ(s1.sessionDay(), 2u);
    EXPECT_EQ(s1.remainingCount(), 1u);

    // Rate m3 Easy on day 2 -> dueDay = 18. Now 0 unseen New cards remain; earliest due is day 17!
    ASSERT_TRUE(s1.submitRating(Rating::Easy));
    EXPECT_TRUE(s1.isSessionComplete());
    EXPECT_EQ(s1.computeNextSessionDay(), 17u);

    uint16_t duePreview = 0;
    uint16_t newPreview = 0;
    s1.previewNextSession(duePreview, newPreview);
    EXPECT_EQ(duePreview, 2u);
    EXPECT_EQ(newPreview, 0u);

    // Advancing jumps directly from day 2 to day 17, skipping empty days 3..16!
    ASSERT_TRUE(s1.startNextSession());
    EXPECT_EQ(s1.sessionDay(), 17u);
    EXPECT_FALSE(s1.isSessionComplete());
    EXPECT_EQ(s1.remainingCount(), 2u);
    s1.close();
  }

  // Reset deck removes .srs and .wal while preserving .idx.
  EXPECT_TRUE(Storage.exists(idxPath.c_str()));
  EXPECT_TRUE(Storage.exists(srsPath.c_str()));
  ASSERT_TRUE(AnkiSrsStore::resetDeck("/.crosspoint/anki", deckPath));
  EXPECT_TRUE(Storage.exists(idxPath.c_str()));
  EXPECT_FALSE(Storage.exists(srsPath.c_str()));
  EXPECT_FALSE(Storage.exists(walPath.c_str()));

  AnkiSession afterReset;
  ASSERT_TRUE(afterReset.open(deckPath.c_str(), "/.crosspoint/anki"));
  EXPECT_EQ(afterReset.sessionDay(), 1u);
  EXPECT_EQ(afterReset.remainingCount(), 3u);
  afterReset.close();
}

TEST_F(AnkiStorageTest, WalThresholdAutoCompactionAndSingleCardAgainLoop) {
  const std::string deckPath = "/anki/single.jsonl";
  const std::string jsonl =
      R"({"_meta":true,"title":"Clamp Deck","newPerDay":999,"maxReviews":999})" "\n"
      R"({"id":"only","front":"SoloQ","back":"SoloA"})" "\n";
  anki_fs::setFile(deckPath, jsonl);

  std::string idxPath;
  std::string srsPath;
  std::string walPath;
  AnkiSrsStore::buildPaths("/.crosspoint/anki", deckPath, idxPath, srsPath, walPath);

  AnkiSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), "/.crosspoint/anki"));
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

TEST_F(AnkiStorageTest, MaxSrsRecordsCapAndZeroMidSessionReallocation) {
  ASSERT_TRUE(AnkiDeckIndex::ensureDir("/.crosspoint/anki"));
  const char* srsPath = "/.crosspoint/anki/cap.srs";
  const char* walPath = "/.crosspoint/anki/cap.wal";

  // 1. Oversized .srs file (MAX_SRS_RECORDS + 1) is rejected cleanly without allocating.
  {
    SrsFileHeader overHead{};
    overHead.recordCount = MAX_SRS_RECORDS + 1;
    std::vector<uint8_t> raw(sizeof(SrsFileHeader) + static_cast<size_t>(overHead.recordCount) * sizeof(CardSrsRecord),
                             0);
    std::memcpy(raw.data(), &overHead, sizeof(overHead));
    anki_fs::setFileBytes(srsPath, raw);

    AnkiSrsStore store;
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
    anki_fs::setFileBytes(srsPath, raw);

    AnkiSrsStore store;
    ASSERT_TRUE(store.load(srsPath, walPath, MAX_NEW_PER_DAY));
    ASSERT_EQ(store.records().size(), MAX_SRS_RECORDS);

    CardSrsRecord existingUpdate = *store.findCard(2000ULL);
    existingUpdate.dueDay = 12;
    ASSERT_TRUE(store.recordRating(existingUpdate, false));
    EXPECT_EQ(store.records().size(), MAX_SRS_RECORDS);
    EXPECT_EQ(store.findCard(2000ULL)->dueDay, 12u);
    const size_t walAfterValidUpdate = anki_fs::getFileBytes(walPath).size();
    EXPECT_EQ(walAfterValidUpdate, sizeof(WalEntry));

    CardSrsRecord overflowNew{};
    overflowNew.cardIdHash = static_cast<uint64_t>(MAX_SRS_RECORDS + 99);
    overflowNew.state = static_cast<uint8_t>(CardState::Review);
    EXPECT_FALSE(store.recordRating(overflowNew, true));
    EXPECT_EQ(store.records().size(), MAX_SRS_RECORDS);
    EXPECT_EQ(anki_fs::getFileBytes(walPath).size(), walAfterValidUpdate);
  }

  // 3. AnkiSession::open pre-reserves sortedRecords upfront so rating new cards causes zero mid-session reallocations.
  {
    const std::string deckPath = "/anki/prerev.jsonl";
    const std::string jsonl =
        R"({"id":"r1","front":"Q1","back":"A1"})" "\n"
        R"({"id":"r2","front":"Q2","back":"A2"})" "\n"
        R"({"id":"r3","front":"Q3","back":"A3"})" "\n";
    anki_fs::setFile(deckPath, jsonl);

    AnkiSession s1;
    ASSERT_TRUE(s1.open(deckPath.c_str(), "/.crosspoint/anki"));
    ASSERT_GE(s1.srsStore().records().capacity(), static_cast<size_t>(MAX_NEW_PER_DAY));
    const CardSrsRecord* initialBufPtr = s1.srsStore().records().data();
    ASSERT_TRUE(s1.submitRating(Rating::Good));
    ASSERT_TRUE(s1.submitRating(Rating::Good));
    EXPECT_EQ(s1.srsStore().records().data(), initialBufPtr);
    s1.close();
  }
}

TEST_F(AnkiStorageTest, OomProbeFailurePathsFailSoft) {
  const std::string deckPath = "/anki/oom.jsonl";
  const std::string jsonl =
      R"({"id":"o1","front":"Front1","back":"Back1"})" "\n"
      R"({"id":"o2","front":"Front2","back":"Back2"})" "\n";
  anki_fs::setFile(deckPath, jsonl);
  ASSERT_TRUE(AnkiDeckIndex::ensureDir("/.crosspoint/anki"));

  // Direct AnkiSrsStore::load OOM on extraCapacityHint probe.
  {
    AnkiSrsStore store;
    anki_test_mem::failAllocCountdown = 0;
    EXPECT_FALSE(store.load("/.crosspoint/anki/oom_direct.srs", "/.crosspoint/anki/oom_direct.wal", MAX_NEW_PER_DAY));
    anki_test_mem::failAllocCountdown = -1;
  }

  // Direct AnkiSrsStore::recordRating OOM when growing from 0 capacity without hint.
  {
    AnkiSrsStore store;
    ASSERT_TRUE(store.load("/.crosspoint/anki/oom_grow.srs", "/.crosspoint/anki/oom_grow.wal", 0));
    CardSrsRecord card{};
    card.cardIdHash = 123ULL;
    card.state = static_cast<uint8_t>(CardState::Review);
    anki_test_mem::failAllocCountdown = 0;
    EXPECT_FALSE(store.recordRating(card, true));
    EXPECT_FALSE(Storage.exists("/.crosspoint/anki/oom_grow.wal"));
    anki_test_mem::failAllocCountdown = -1;
  }

  // Every heap allocation point in AnkiSession::open fails soft without crashing or leaving opened == true.
  for (int failStep = 0; failStep < 5; ++failStep) {
    anki_fs::reset();
    anki_fs::setFile(deckPath, jsonl);
    AnkiSession session;
    anki_test_mem::failAllocCountdown = failStep;
    const bool ok = session.open(deckPath.c_str(), "/.crosspoint/anki");
    if (anki_test_mem::failAllocCountdown == -1 && !ok) {
      EXPECT_FALSE(session.isOpen());
    } else {
      session.close();
    }
    anki_test_mem::failAllocCountdown = -1;
  }

  // Subsequent open with normal heap succeeds cleanly.
  AnkiSession healthy;
  ASSERT_TRUE(healthy.open(deckPath.c_str(), "/.crosspoint/anki"));
  EXPECT_EQ(healthy.remainingCount(), 2u);
  healthy.close();
}

}  // namespace
}  // namespace anki
