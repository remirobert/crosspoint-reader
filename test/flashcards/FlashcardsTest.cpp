#include <HalStorage.h>
#include <Memory.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <set>
#include <string>
#include <vector>

#include "FlashcardDeckIndex.h"
#include "FlashcardSession.h"
#include "FlashcardTypes.h"

namespace flashcards {
namespace {

class FlashcardsStorageTest : public ::testing::Test {
 protected:
  void SetUp() override {
    flashcards_fs::reset();
    flashcards_test_mem::failAllocCountdown = -1;
  }

  static bool hasAnySrsOrWalFiles() {
    for (const auto& [path, _] : flashcards_fs::files) {
      if (path.ends_with(".srs") || path.ends_with(".wal")) {
        return true;
      }
    }
    return false;
  }
};

// ============================================================================
// FlashcardTypes Tests
// ============================================================================

TEST(FlashcardTypesTest, StructSizeAndLayoutInvariants) {
  EXPECT_EQ(DECK_INDEX_MAGIC, 0x31444346u);
  EXPECT_EQ(DECK_INDEX_VERSION, 2u);
  EXPECT_EQ(sizeof(DeckIndexHeader), 84u);
  EXPECT_EQ(sizeof(CardIndexRecord), 16u);
  EXPECT_EQ(MAX_JSONL_LINE_BYTES, 4096u);
  EXPECT_EQ(MAX_ACTIVE_QUEUE, 256u);

  const SessionProgress prog{};
  EXPECT_EQ(prog.cleared, 0u);
  EXPECT_EQ(prog.drilling, 0u);
  EXPECT_EQ(prog.unseen, 0u);

  const PassSummary pass{};
  EXPECT_EQ(pass.totalCards, 0u);
  EXPECT_EQ(pass.firstTryCount, 0u);
  EXPECT_EQ(pass.retriedCount, 0u);
  EXPECT_EQ(pass.totalReviews, 0u);
}

TEST(FlashcardTypesTest, Fnv1a64AndFormatDeckHashHex) {
  static_assert(fnv1a64("") == 0xcbf29ce484222325ULL);
  static_assert(fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
  static_assert(fnv1a64("foobar") == 0x85944171f73967e8ULL);

  EXPECT_EQ(fnv1a64(""), 0xcbf29ce484222325ULL);
  EXPECT_EQ(fnv1a64("foobar"), 0x85944171f73967e8ULL);
  EXPECT_NE(fnv1a64("食べる"), fnv1a64("飲む"));
  EXPECT_EQ(fnv1a64("食べる"), fnv1a64(std::string_view("食べる")));

  char hex[17];
  std::memset(hex, 'X', sizeof(hex));
  formatDeckHashHex("", hex);
  EXPECT_STREQ(hex, "cbf29ce484222325");
  EXPECT_EQ(std::strlen(hex), 16u);

  formatDeckHashHex("foobar", hex);
  EXPECT_STREQ(hex, "85944171f73967e8");
}

// ============================================================================
// FlashcardDeckIndex Tests
// ============================================================================

TEST_F(FlashcardsStorageTest, BuildIndexPathAndRemoveIndex) {
  std::string pathDefault;
  FlashcardDeckIndex::buildIndexPath(nullptr, "foobar", pathDefault);
  EXPECT_EQ(pathDefault, "/.crosspoint/flashcards/85944171f73967e8.idx");

  std::string pathTrailingSlash;
  FlashcardDeckIndex::buildIndexPath("/custom/cache/", "foobar", pathTrailingSlash);
  EXPECT_EQ(pathTrailingSlash, "/custom/cache/85944171f73967e8.idx");

  // Removing when neither .idx nor .idx.tmp exists returns true cleanly.
  EXPECT_TRUE(FlashcardDeckIndex::removeIndex("/custom/cache", "foobar"));

  ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/custom/cache"));
  flashcards_fs::setFile(pathTrailingSlash, "idx-bytes");
  flashcards_fs::setFile(pathTrailingSlash + ".tmp", "tmp-bytes");
  EXPECT_TRUE(Storage.exists(pathTrailingSlash.c_str()));
  EXPECT_TRUE(Storage.exists((pathTrailingSlash + ".tmp").c_str()));

  EXPECT_TRUE(FlashcardDeckIndex::removeIndex("/custom/cache/", "foobar"));
  EXPECT_FALSE(Storage.exists(pathTrailingSlash.c_str()));
  EXPECT_FALSE(Storage.exists((pathTrailingSlash + ".tmp").c_str()));
}

TEST_F(FlashcardsStorageTest, DeckIndexMetaLegacyKeysDuplicateIdsUtf8HtmlAndMalformedLines) {
  std::string overlongLine = R"({"id":"too-long","front":")";
  overlongLine.append(4100, 'x');
  overlongLine += R"(","back":"ignored"})";
  ASSERT_GT(overlongLine.size(), MAX_JSONL_LINE_BYTES);

  std::string jsonl;
  // Legacy newPerDay and maxReviews keys in _meta must be ignored gracefully.
  jsonl += R"({"_meta":true,"title":"Japanese Core 2k","newPerDay":25,"maxReviews":150})"
           "\r\n";
  jsonl += "\r\n";
  jsonl += "   \t  \n";
  jsonl +=
      R"({"id":"n5-001","front":"\u98df\u3079\u308b","back":"to eat","hint":"たべる","extra":"毎日野菜を食べる。"})"
      "\r\n";
  // Duplicate explicit string ID ("n5-001") -> skipped, keeping first occurrence.
  jsonl += R"({"id":"n5-001","front":"Duplicate Front","back":"Duplicate Back"})"
           "\n";
  jsonl += "{not valid json at all\n";
  jsonl += R"({"front":"","back":"missing front"})"
           "\n";
  jsonl += R"({"front":"missing back"})"
           "\n";
  jsonl += overlongLine + "\r\n";
  jsonl += R"({"id":987654,"front":"<div>Line 1<br>Line 2</div>","back":"<p>A &amp; B &lt;C&gt;</p>",)"
           R"("hint":"&nbsp;<b>noun</b> ","extra":"&#39;quote&#39; &quot;dbl&quot;<br/>&#x41;"})"
           "\n";
  // Duplicate integer ID (987654) -> skipped.
  jsonl += R"({"id":987654,"front":"Second Int Dup","back":"Ignored"})"
           "\n";
  // Fallback ID hash from front when "id" is omitted, plus duplicate fallback front -> skipped.
  jsonl += R"({"front":"飲む","back":"to drink"})"
           "\n";
  jsonl += R"({"front":"飲む","back":"duplicate drink"})";

  const std::string deckPath = "/flashcards/jp.jsonl";
  flashcards_fs::setFile(deckPath, jsonl);
  ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));

  std::string idxPath;
  FlashcardDeckIndex::buildIndexPath("/.crosspoint/flashcards", deckPath, idxPath);

  FlashcardDeckIndex index;
  ASSERT_TRUE(index.openOrBuild(deckPath.c_str(), idxPath.c_str()));
  EXPECT_TRUE(index.isOpen());

  const DeckIndexHeader& hdr = index.header();
  EXPECT_EQ(hdr.magic, DECK_INDEX_MAGIC);
  EXPECT_EQ(hdr.version, DECK_INDEX_VERSION);
  EXPECT_EQ(hdr.deckFileSize, jsonl.size());
  EXPECT_EQ(hdr.cardCount, 3u);
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
  EXPECT_EQ(c1.extra, "'quote' \"dbl\"\nA");

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

TEST_F(FlashcardsStorageTest, DeckIndexCacheReuseVersionUpgradeAndSizeInvalidation) {
  const std::string deckPath = "/flashcards/vocab.jsonl";
  const std::string initialJsonl = R"({"front":"card1","back":"ans1"})"
                                   "\n"
                                   R"({"front":"card2","back":"ans2"})"
                                   "\n";
  flashcards_fs::setFile(deckPath, initialJsonl);
  ASSERT_TRUE(FlashcardDeckIndex::ensureDir("/.crosspoint/flashcards"));

  std::string idxPath;
  FlashcardDeckIndex::buildIndexPath("/.crosspoint/flashcards", deckPath, idxPath);
  const std::string tmpIdxPath = idxPath + ".tmp";

  {
    FlashcardDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild(deckPath.c_str(), idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 2u);
    EXPECT_STREQ(idx.header().title, "vocab");
    idx.close();
  }

  const unsigned writesAfterFirstBuild = flashcards_fs::writeCount(idxPath) + flashcards_fs::writeCount(tmpIdxPath);
  ASSERT_GT(writesAfterFirstBuild, 0u);

  // Re-opening unchanged .jsonl reuses cached .idx in O(1) with zero disk writes.
  {
    FlashcardDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild(deckPath.c_str(), idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 2u);
    idx.close();
  }
  const unsigned writesAfterReuse = flashcards_fs::writeCount(idxPath) + flashcards_fs::writeCount(tmpIdxPath);
  EXPECT_EQ(writesAfterReuse, writesAfterFirstBuild);

  // Legacy version=1 .idx header triggers automatic rebuild to version=2.
  {
    std::vector<uint8_t> oldIdx = flashcards_fs::getFileBytes(idxPath);
    ASSERT_GE(oldIdx.size(), sizeof(DeckIndexHeader));
    oldIdx[offsetof(DeckIndexHeader, version)] = 1;
    flashcards_fs::setFileBytes(idxPath, oldIdx);

    FlashcardDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild(deckPath.c_str(), idxPath.c_str()));
    EXPECT_EQ(idx.header().version, DECK_INDEX_VERSION);
    EXPECT_EQ(idx.header().cardCount, 2u);
    idx.close();
  }
  const unsigned writesAfterVersionRebuild = flashcards_fs::writeCount(idxPath) + flashcards_fs::writeCount(tmpIdxPath);
  EXPECT_GT(writesAfterVersionRebuild, writesAfterReuse);

  // Appending a card changes .jsonl size -> rebuilds .idx automatically.
  const std::string expandedJsonl = initialJsonl + R"({"front":"card3","back":"ans3"})"
                                                   "\n";
  flashcards_fs::setFile(deckPath, expandedJsonl);
  {
    FlashcardDeckIndex idx;
    ASSERT_TRUE(idx.openOrBuild(deckPath.c_str(), idxPath.c_str()));
    EXPECT_EQ(idx.header().cardCount, 3u);
    EXPECT_EQ(idx.header().deckFileSize, expandedJsonl.size());
    idx.close();
  }
  const unsigned writesAfterSizeRebuild = flashcards_fs::writeCount(idxPath) + flashcards_fs::writeCount(tmpIdxPath);
  EXPECT_GT(writesAfterSizeRebuild, writesAfterVersionRebuild);
}

TEST_F(FlashcardsStorageTest, DuplicateIdDedupScalesPastInline256Threshold) {
  const std::string deckPath = "/flashcards/dedup300.jsonl";
  std::string jsonl;
  for (int i = 0; i < 300; ++i) {
    jsonl += "{\"id\":\"id-" + std::to_string(i) + "\",\"front\":\"Q" + std::to_string(i) + "\",\"back\":\"A" +
             std::to_string(i) + "\"}\n";
  }
  // Append duplicates of both early (id-5) and post-256 (id-290) cards.
  jsonl += R"({"id":"id-5","front":"Dup5","back":"DupA5"})"
           "\n";
  jsonl += R"({"id":"id-290","front":"Dup290","back":"DupA290"})"
           "\n";
  flashcards_fs::setFile(deckPath, jsonl);

  FlashcardSession::DeckSummary sum{};
  ASSERT_TRUE(FlashcardSession::inspectDeck(deckPath.c_str(), "/.crosspoint/flashcards", sum));
  EXPECT_EQ(sum.totalCards, 300u);
  EXPECT_FALSE(hasAnySrsOrWalFiles());
}

// ============================================================================
// FlashcardSession Tests (Stateless Deck Practice)
// ============================================================================

TEST_F(FlashcardsStorageTest, SequentialPassAgainRequeueProgressSummaryAndRestart) {
  const std::string deckPath = "/flashcards/seq10.jsonl";
  std::string jsonl = R"({"_meta":true,"title":"Ten Deck","newPerDay":5,"maxReviews":50})"
                      "\n";
  for (int i = 0; i < 10; ++i) {
    jsonl += "{\"id\":\"c" + std::to_string(i) + "\",\"front\":\"Q" + std::to_string(i) + "\",\"back\":\"A" +
             std::to_string(i) + "\"}\n";
  }
  flashcards_fs::setFile(deckPath, jsonl);

  FlashcardSession::DeckSummary deckSum{};
  ASSERT_TRUE(FlashcardSession::inspectDeck(deckPath.c_str(), "/.crosspoint/flashcards", deckSum));
  EXPECT_EQ(deckSum.totalCards, 10u);
  EXPECT_STREQ(deckSum.title, "Ten Deck");
  EXPECT_FALSE(hasAnySrsOrWalFiles());

  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), /*shuffle=*/false, /*seed=*/0, "/.crosspoint/flashcards"));
  EXPECT_TRUE(session.isOpen());
  EXPECT_FALSE(session.isPassComplete());
  EXPECT_FALSE(session.isShuffled());
  EXPECT_EQ(session.totalDeckCards(), 10u);
  EXPECT_EQ(session.remainingCount(), 10u);
  EXPECT_STREQ(session.deckTitle(), "Ten Deck");

  SessionProgress prog = session.sessionProgress();
  EXPECT_EQ(prog.cleared, 0u);
  EXPECT_EQ(prog.drilling, 0u);
  EXPECT_EQ(prog.unseen, 10u);

  FlashcardItem card;
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q0");

  // Hit "Again" on Q0: re-queued 6 cards ahead (after Q1..Q6); remainingCount stays 10, drilling becomes 1.
  ASSERT_TRUE(session.submitChoice(true));
  EXPECT_EQ(session.remainingCount(), 10u);
  prog = session.sessionProgress();
  EXPECT_EQ(prog.cleared, 0u);
  EXPECT_EQ(prog.drilling, 1u);
  EXPECT_EQ(prog.unseen, 9u);

  // Clear Q1..Q6 on first try via "Got It".
  for (int expected = 1; expected <= 6; ++expected) {
    ASSERT_TRUE(session.currentCard(card));
    EXPECT_EQ(card.front, "Q" + std::to_string(expected));
    ASSERT_TRUE(session.submitChoice(false));
  }
  EXPECT_EQ(session.remainingCount(), 4u);
  prog = session.sessionProgress();
  EXPECT_EQ(prog.cleared, 6u);
  EXPECT_EQ(prog.drilling, 1u);
  EXPECT_EQ(prog.unseen, 3u);

  // Q0 reappears at slot 6; hit "Again" a second time on Q0 (now behind remaining Q7, Q8, Q9).
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q0");
  ASSERT_TRUE(session.submitChoice(true));
  prog = session.sessionProgress();
  EXPECT_EQ(prog.cleared, 6u);
  EXPECT_EQ(prog.drilling, 1u);
  EXPECT_EQ(prog.unseen, 3u);

  // Clear Q7, Q8 on first try; hit "Again" on Q9, then clear Q0 (retried), hit "Again" on last-card Q9, and clear Q9.
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q7");
  ASSERT_TRUE(session.submitChoice(false));

  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q8");
  ASSERT_TRUE(session.submitChoice(false));

  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q9");
  ASSERT_TRUE(session.submitChoice(true));
  prog = session.sessionProgress();
  EXPECT_EQ(prog.cleared, 8u);
  EXPECT_EQ(prog.drilling, 2u);
  EXPECT_EQ(prog.unseen, 0u);

  // Clear Q0 (had 2 Again hits -> counts once in retriedCount).
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q0");
  ASSERT_TRUE(session.submitChoice(false));
  EXPECT_EQ(session.remainingCount(), 1u);

  // Q9 is the sole remaining card: "Again" keeps it at front without completing the pass.
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q9");
  ASSERT_TRUE(session.submitChoice(true));
  EXPECT_FALSE(session.isPassComplete());
  EXPECT_EQ(session.remainingCount(), 1u);

  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q9");
  ASSERT_TRUE(session.submitChoice(false));

  // Pass is now complete!
  EXPECT_TRUE(session.isPassComplete());
  EXPECT_EQ(session.remainingCount(), 0u);
  EXPECT_FALSE(session.currentCard(card));
  EXPECT_FALSE(session.submitChoice(false));

  prog = session.sessionProgress();
  EXPECT_EQ(prog.cleared, 10u);
  EXPECT_EQ(prog.drilling, 0u);
  EXPECT_EQ(prog.unseen, 0u);

  const PassSummary pass = session.passSummary();
  EXPECT_EQ(pass.totalCards, 10u);
  EXPECT_EQ(pass.firstTryCount, 8u);
  EXPECT_EQ(pass.retriedCount, 2u);
  EXPECT_EQ(pass.firstTryCount + pass.retriedCount, pass.totalCards);
  EXPECT_EQ(pass.totalReviews, 14u);  // 10 Got It + 4 Again

  // Restart pass sequentially and verify clean state reset.
  ASSERT_TRUE(session.restartPass(/*shuffle=*/false));
  EXPECT_FALSE(session.isPassComplete());
  EXPECT_FALSE(session.isShuffled());
  EXPECT_EQ(session.remainingCount(), 10u);
  EXPECT_EQ(session.passSummary().totalReviews, 0u);
  EXPECT_EQ(session.passSummary().firstTryCount, 0u);
  EXPECT_EQ(session.passSummary().retriedCount, 0u);
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "Q0");

  EXPECT_FALSE(hasAnySrsOrWalFiles());
  session.close();
  EXPECT_FALSE(session.isOpen());
}

TEST_F(FlashcardsStorageTest, DeterministicAndAutoSeededShuffledPasses) {
  constexpr uint32_t kNumCards = 20;
  const std::string deckPath = "/flashcards/shuffle20.jsonl";
  std::string jsonl = R"({"_meta":true,"title":"Shuffle 20"})"
                      "\n";
  for (uint32_t i = 0; i < kNumCards; ++i) {
    jsonl += "{\"id\":\"s" + std::to_string(i) + "\",\"front\":\"Card" + std::to_string(i) + "\",\"back\":\"Ans" +
             std::to_string(i) + "\"}\n";
  }
  flashcards_fs::setFile(deckPath, jsonl);

  std::vector<std::string> sequentialOrder;
  for (uint32_t i = 0; i < kNumCards; ++i) {
    sequentialOrder.push_back("Card" + std::to_string(i));
  }

  auto collectPassFronts = [&](FlashcardSession& s) {
    std::vector<std::string> fronts;
    FlashcardItem item;
    while (!s.isPassComplete()) {
      EXPECT_TRUE(s.currentCard(item));
      fronts.push_back(item.front);
      EXPECT_TRUE(s.submitChoice(false));
    }
    return fronts;
  };

  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), /*shuffle=*/true, /*seed=*/42, "/.crosspoint/flashcards"));
  EXPECT_TRUE(session.isShuffled());
  const std::vector<std::string> orderSeed42 = collectPassFronts(session);
  ASSERT_EQ(orderSeed42.size(), kNumCards);
  EXPECT_NE(orderSeed42, sequentialOrder);

  const std::set<std::string> uniqueSeed42(orderSeed42.begin(), orderSeed42.end());
  EXPECT_EQ(uniqueSeed42.size(), kNumCards);

  // Re-running with the exact same seed=42 reproduces the exact same card sequence.
  ASSERT_TRUE(session.restartPass(/*shuffle=*/true, /*seed=*/42));
  EXPECT_EQ(collectPassFronts(session), orderSeed42);

  // Restarting with seed=99 produces a different valid permutation of all 20 cards.
  ASSERT_TRUE(session.restartPass(/*shuffle=*/true, /*seed=*/99));
  const std::vector<std::string> orderSeed99 = collectPassFronts(session);
  ASSERT_EQ(orderSeed99.size(), kNumCards);
  EXPECT_NE(orderSeed99, orderSeed42);
  EXPECT_NE(orderSeed99, sequentialOrder);
  EXPECT_EQ(std::set<std::string>(orderSeed99.begin(), orderSeed99.end()).size(), kNumCards);

  // Auto-seed (seed = 0) visits every card once without degenerating.
  ASSERT_TRUE(session.restartPass(/*shuffle=*/true, /*seed=*/0));
  const std::vector<std::string> orderAuto = collectPassFronts(session);
  ASSERT_EQ(orderAuto.size(), kNumCards);
  EXPECT_EQ(std::set<std::string>(orderAuto.begin(), orderAuto.end()).size(), kNumCards);
  session.close();
}

TEST_F(FlashcardsStorageTest, StreamsDecksLargerThan256InSequentialAndShuffledModes) {
  constexpr uint32_t kTotalCards = 300;
  static_assert(kTotalCards > MAX_ACTIVE_QUEUE);

  const std::string deckPath = "/flashcards/stream300.jsonl";
  std::string jsonl = R"({"_meta":true,"title":"Stream 300"})"
                      "\n";
  for (uint32_t i = 0; i < kTotalCards; ++i) {
    jsonl += "{\"id\":\"p" + std::to_string(i) + "\",\"front\":\"P" + std::to_string(i) + "\",\"back\":\"B" +
             std::to_string(i) + "\"}\n";
  }
  flashcards_fs::setFile(deckPath, jsonl);

  auto parseOrdinal = [](const std::string& front) -> uint32_t {
    return static_cast<uint32_t>(std::stoul(front.substr(1)));
  };

  // 1. Sequential 300-card pass with mid-stream and tail "Again" re-queues.
  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), /*shuffle=*/false, /*seed=*/0, "/.crosspoint/flashcards"));
  EXPECT_EQ(session.remainingCount(), kTotalCards);

  SessionProgress p = session.sessionProgress();
  EXPECT_EQ(p.cleared, 0u);
  EXPECT_EQ(p.drilling, 0u);
  EXPECT_EQ(p.unseen, kTotalCards);

  FlashcardItem card;
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "P0");
  ASSERT_TRUE(session.submitChoice(true));  // P0 Again -> drilling=1, unseen=299

  std::vector<uint32_t> clearedCounts(kTotalCards, 0u);
  while (session.remainingCount() > 2u) {
    ASSERT_TRUE(session.currentCard(card));
    ++clearedCounts[parseOrdinal(card.front)];
    ASSERT_TRUE(session.submitChoice(false));
    p = session.sessionProgress();
    EXPECT_EQ(static_cast<uint32_t>(p.cleared) + p.drilling + p.unseen, kTotalCards);
    EXPECT_EQ(session.remainingCount(), static_cast<size_t>(p.drilling + p.unseen));
  }

  // Hit Again near the tail after all 300 cards have streamed in.
  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "P298");
  ASSERT_TRUE(session.submitChoice(true));

  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "P299");
  ++clearedCounts[parseOrdinal(card.front)];
  ASSERT_TRUE(session.submitChoice(false));

  ASSERT_TRUE(session.currentCard(card));
  EXPECT_EQ(card.front, "P298");
  ++clearedCounts[parseOrdinal(card.front)];
  ASSERT_TRUE(session.submitChoice(false));

  EXPECT_TRUE(session.isPassComplete());
  for (uint32_t ord = 0; ord < kTotalCards; ++ord) {
    EXPECT_EQ(clearedCounts[ord], 1u) << "sequential ordinal " << ord;
  }
  PassSummary sumSeq = session.passSummary();
  EXPECT_EQ(sumSeq.totalCards, kTotalCards);
  EXPECT_EQ(sumSeq.firstTryCount, kTotalCards - 2u);
  EXPECT_EQ(sumSeq.retriedCount, 2u);
  EXPECT_EQ(sumSeq.totalReviews, kTotalCards + 2u);

  // 2. Shuffled 300-card pass (streams from 300-entry uint16_t orderBuf across the 256-slot active queue).
  ASSERT_TRUE(session.restartPass(/*shuffle=*/true, /*seed=*/12345));
  EXPECT_TRUE(session.isShuffled());
  EXPECT_EQ(session.remainingCount(), kTotalCards);

  std::fill(clearedCounts.begin(), clearedCounts.end(), 0u);
  std::vector<uint32_t> firstSeenOrder;
  firstSeenOrder.reserve(kTotalCards);

  // Hit Again on the first 3 shuffled cards, then clear everything.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(session.currentCard(card));
    firstSeenOrder.push_back(parseOrdinal(card.front));
    ASSERT_TRUE(session.submitChoice(true));
  }

  while (!session.isPassComplete()) {
    ASSERT_TRUE(session.currentCard(card));
    const uint32_t ord = parseOrdinal(card.front);
    ++clearedCounts[ord];
    ASSERT_TRUE(session.submitChoice(false));
    p = session.sessionProgress();
    EXPECT_EQ(static_cast<uint32_t>(p.cleared) + p.drilling + p.unseen, kTotalCards);
  }

  for (uint32_t ord = 0; ord < kTotalCards; ++ord) {
    EXPECT_EQ(clearedCounts[ord], 1u) << "shuffled ordinal " << ord;
  }
  EXPECT_FALSE(firstSeenOrder[0] == 0u && firstSeenOrder[1] == 1u && firstSeenOrder[2] == 2u);

  const PassSummary sumShuf = session.passSummary();
  EXPECT_EQ(sumShuf.totalCards, kTotalCards);
  EXPECT_EQ(sumShuf.firstTryCount, kTotalCards - 3u);
  EXPECT_EQ(sumShuf.retriedCount, 3u);
  EXPECT_EQ(sumShuf.totalReviews, kTotalCards + 3u);
  EXPECT_FALSE(hasAnySrsOrWalFiles());
  session.close();
}

TEST_F(FlashcardsStorageTest, EmptyDeckOpensAsImmediatelyComplete) {
  const std::string deckPath = "/flashcards/empty.jsonl";
  flashcards_fs::setFile(deckPath, R"({"_meta":true,"title":"Empty Deck"})"
                                   "\n   \n");

  FlashcardSession::DeckSummary deckSum{};
  ASSERT_TRUE(FlashcardSession::inspectDeck(deckPath.c_str(), "/.crosspoint/flashcards", deckSum));
  EXPECT_EQ(deckSum.totalCards, 0u);
  EXPECT_STREQ(deckSum.title, "Empty Deck");

  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), /*shuffle=*/true, /*seed=*/42, "/.crosspoint/flashcards"));
  EXPECT_TRUE(session.isOpen());
  EXPECT_TRUE(session.isPassComplete());
  EXPECT_EQ(session.totalDeckCards(), 0u);
  EXPECT_EQ(session.remainingCount(), 0u);
  EXPECT_STREQ(session.deckTitle(), "Empty Deck");

  const SessionProgress prog = session.sessionProgress();
  EXPECT_EQ(prog.cleared, 0u);
  EXPECT_EQ(prog.drilling, 0u);
  EXPECT_EQ(prog.unseen, 0u);

  const PassSummary pass = session.passSummary();
  EXPECT_EQ(pass.totalCards, 0u);
  EXPECT_EQ(pass.firstTryCount, 0u);
  EXPECT_EQ(pass.retriedCount, 0u);
  EXPECT_EQ(pass.totalReviews, 0u);

  FlashcardItem dummy;
  EXPECT_FALSE(session.currentCard(dummy));
  EXPECT_FALSE(session.submitChoice(false));
  EXPECT_FALSE(session.submitChoice(true));

  ASSERT_TRUE(session.restartPass(/*shuffle=*/false));
  EXPECT_TRUE(session.isPassComplete());
  EXPECT_EQ(session.totalDeckCards(), 0u);
  session.close();
}

TEST_F(FlashcardsStorageTest, OomProbeFailurePathsAndShuffleOrderFallbackFailSoft) {
  const std::string deckPath = "/flashcards/oom.jsonl";
  std::string jsonl = R"({"_meta":true,"title":"OOM Deck"})"
                      "\n";
  for (int i = 0; i < 12; ++i) {
    jsonl += "{\"id\":\"o" + std::to_string(i) + "\",\"front\":\"Q" + std::to_string(i) + "\",\"back\":\"A" +
             std::to_string(i) + "\"}\n";
  }
  flashcards_fs::setFile(deckPath, jsonl);

  // 1. inspectDeck OOM fails soft.
  for (int step = 0; step < 4; ++step) {
    flashcards_fs::reset();
    flashcards_fs::setFile(deckPath, jsonl);
    FlashcardSession::DeckSummary out{};
    flashcards_test_mem::failAllocCountdown = step;
    const bool ok = FlashcardSession::inspectDeck(deckPath.c_str(), "/.crosspoint/flashcards", out);
    if (!ok) {
      EXPECT_EQ(out.totalCards, 0u);
    }
    flashcards_test_mem::failAllocCountdown = -1;
  }

  // 2. Cold open() OOM at every allocation point either fails soft (isOpen == false) or falls back to window shuffle!
  for (int step = 0; step < 6; ++step) {
    flashcards_fs::reset();
    flashcards_fs::setFile(deckPath, jsonl);
    FlashcardSession session;
    flashcards_test_mem::failAllocCountdown = step;
    const bool ok = session.open(deckPath.c_str(), /*shuffle=*/true, /*seed=*/42, "/.crosspoint/flashcards");
    flashcards_test_mem::failAllocCountdown = -1;
    if (!ok) {
      EXPECT_FALSE(session.isOpen());
      EXPECT_EQ(session.remainingCount(), 0u);
    } else {
      // When step hits orderBuf specifically, open() succeeds via in-place 256-slot window shuffle fallback!
      EXPECT_TRUE(session.isOpen());
      EXPECT_EQ(session.remainingCount(), 12u);
      session.close();
    }
  }

  // 3. Explicit verification of orderBuf OOM fallback on restartPass(true, 77) when queue is already reserved:
  FlashcardSession healthy;
  ASSERT_TRUE(healthy.open(deckPath.c_str(), /*shuffle=*/false, /*seed=*/0, "/.crosspoint/flashcards"));
  ASSERT_EQ(healthy.remainingCount(), 12u);

  // Queue capacity is already >= 12, so the ONLY allocation in restartPass(true, 77) is orderBuf: fail it!
  flashcards_test_mem::failAllocCountdown = 0;
  ASSERT_TRUE(healthy.restartPass(/*shuffle=*/true, /*seed=*/77));
  flashcards_test_mem::failAllocCountdown = -1;
  EXPECT_TRUE(healthy.isShuffled());
  EXPECT_EQ(healthy.remainingCount(), 12u);

  std::set<std::string> visited;
  FlashcardItem item;
  while (!healthy.isPassComplete()) {
    ASSERT_TRUE(healthy.currentCard(item));
    visited.insert(item.front);
    ASSERT_TRUE(healthy.submitChoice(false));
  }
  EXPECT_EQ(visited.size(), 12u);
  healthy.close();
}

TEST_F(FlashcardsStorageTest, ShuffledDeckOver4096CardsUsesWindowFallbackAndVisitsAllCards) {
  constexpr uint32_t kHugeCards = 4100;
  const std::string deckPath = "/flashcards/huge4100.jsonl";
  std::string jsonl;
  jsonl.reserve(kHugeCards * 36);
  for (uint32_t i = 0; i < kHugeCards; ++i) {
    jsonl += "{\"id\":";
    jsonl += std::to_string(i);
    jsonl += ",\"front\":\"H";
    jsonl += std::to_string(i);
    jsonl += "\",\"back\":\"B\"}\n";
  }
  flashcards_fs::setFile(deckPath, jsonl);

  FlashcardSession session;
  ASSERT_TRUE(session.open(deckPath.c_str(), /*shuffle=*/true, /*seed=*/314159, "/.crosspoint/flashcards"));
  EXPECT_TRUE(session.isShuffled());
  EXPECT_EQ(session.totalDeckCards(), kHugeCards);
  EXPECT_EQ(session.remainingCount(), kHugeCards);

  std::vector<uint8_t> seen(kHugeCards, 0);
  FlashcardItem item;
  uint32_t firstCardOrd = 0;
  bool capturedFirst = false;
  while (!session.isPassComplete()) {
    ASSERT_TRUE(session.currentCard(item));
    const uint32_t ord = static_cast<uint32_t>(std::stoul(item.front.substr(1)));
    ASSERT_LT(ord, kHugeCards);
    if (!capturedFirst) {
      firstCardOrd = ord;
      capturedFirst = true;
    }
    ++seen[ord];
    ASSERT_TRUE(session.submitChoice(false));
  }
  EXPECT_NE(firstCardOrd, 0u);
  for (uint32_t i = 0; i < kHugeCards; ++i) {
    EXPECT_EQ(seen[i], 1u);
  }
  const PassSummary pass = session.passSummary();
  EXPECT_EQ(pass.totalCards, kHugeCards);
  EXPECT_EQ(pass.firstTryCount, kHugeCards);
  EXPECT_EQ(pass.retriedCount, 0u);
  EXPECT_EQ(pass.totalReviews, kHugeCards);
  session.close();
}

}  // namespace
}  // namespace flashcards
