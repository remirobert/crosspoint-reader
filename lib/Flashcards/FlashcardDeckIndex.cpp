#include "FlashcardDeckIndex.h"

#include <ArduinoJson.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>

namespace flashcards {

namespace {

constexpr size_t STREAM_READ_CHUNK_BYTES = 2048;

bool isAsciiSpace(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

bool hasNonWhitespace(std::string_view text) noexcept {
  for (char c : text) {
    if (!isAsciiSpace(c)) return true;
  }
  return false;
}

void copyUtf8Truncated(char* dst, size_t dstSize, std::string_view src) noexcept {
  if (!dst || dstSize == 0) return;
  size_t len = std::min(src.size(), dstSize - 1);
  // Step back over trailing UTF-8 continuation bytes if truncation split a multi-byte sequence.
  if (len < src.size() && (static_cast<uint8_t>(src[len]) & 0xC0u) == 0x80u) {
    while (len > 0 && (static_cast<uint8_t>(src[len - 1]) & 0xC0u) == 0x80u) {
      --len;
    }
    if (len > 0) {
      --len;
    }
  }
  if (len > 0) {
    std::memcpy(dst, src.data(), len);
  }
  dst[len] = '\0';
}

void extractStemTitle(std::string_view path, char outTitle[64]) noexcept {
  std::memset(outTitle, 0, 64);
  const size_t slashPos = path.find_last_of("/\\");
  const std::string_view filename = (slashPos == std::string_view::npos) ? path : path.substr(slashPos + 1);
  const size_t dotPos = filename.find_last_of('.');
  const std::string_view stem =
      (dotPos != std::string_view::npos && dotPos > 0) ? filename.substr(0, dotPos) : filename;
  copyUtf8Truncated(outTitle, 64, stem);
}

bool ensureParentDir(const char* filePath) {
  if (!filePath) return false;
  const std::string_view view(filePath);
  const size_t slash = view.find_last_of('/');
  if (slash == std::string_view::npos || slash == 0) return true;
  std::string parent(view.substr(0, slash));
  return FlashcardDeckIndex::ensureDir(parent.c_str());
}

uint64_t computeCardIdHash(JsonObjectConst obj, const char* rawFront) noexcept {
  JsonVariantConst idVar = obj["id"];
  if (idVar.is<const char*>()) {
    const char* idStr = idVar.as<const char*>();
    if (idStr && idStr[0] != '\0') {
      return fnv1a64(idStr);
    }
  } else if (idVar.is<int64_t>()) {
    char idBuf[24]{};
    const int n = std::snprintf(idBuf, sizeof(idBuf), "%lld", static_cast<long long>(idVar.as<int64_t>()));
    if (n > 0) {
      return fnv1a64(std::string_view(idBuf, std::min<size_t>(static_cast<size_t>(n), sizeof(idBuf) - 1)));
    }
  } else if (idVar.is<uint64_t>()) {
    char idBuf[24]{};
    const int n = std::snprintf(idBuf, sizeof(idBuf), "%llu", static_cast<unsigned long long>(idVar.as<uint64_t>()));
    if (n > 0) {
      return fnv1a64(std::string_view(idBuf, std::min<size_t>(static_cast<size_t>(n), sizeof(idBuf) - 1)));
    }
  }
  return fnv1a64(rawFront ? rawFront : "");
}

bool isHtmlTagStart(std::string_view text, size_t pos) noexcept {
  if (pos + 1 >= text.size()) return false;
  const unsigned char next = static_cast<unsigned char>(text[pos + 1]);
  return next == '/' || next == '!' || next == '?' || std::isalpha(next) != 0;
}

enum class HtmlTagAction : uint8_t { Strip, ForceNewline, BlockBoundary };

HtmlTagAction classifyHtmlTag(std::string_view tagBody) noexcept {
  size_t pos = 0;
  while (pos < tagBody.size() && (tagBody[pos] == '/' || isAsciiSpace(tagBody[pos]))) {
    ++pos;
  }
  const size_t nameStart = pos;
  while (pos < tagBody.size() && std::isalnum(static_cast<unsigned char>(tagBody[pos])) != 0) {
    ++pos;
  }
  const std::string_view name = tagBody.substr(nameStart, pos - nameStart);
  const auto iequals = [&](std::string_view target) noexcept {
    if (name.size() != target.size()) return false;
    for (size_t i = 0; i < name.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(name[i])) != target[i]) return false;
    }
    return true;
  };
  if (iequals("br")) return HtmlTagAction::ForceNewline;
  if (iequals("p") || iequals("div") || iequals("li") || iequals("tr") || iequals("hr")) {
    return HtmlTagAction::BlockBoundary;
  }
  return HtmlTagAction::Strip;
}

void appendUtf8Codepoint(std::string& out, uint32_t cp) {
  if (cp == 0 || cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) return;
  if (cp == 0xA0u) {
    out.push_back(' ');
  } else if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

bool decodeHtmlEntity(std::string_view entity, std::string& out) {
  if (entity.size() < 4 || entity.front() != '&' || entity.back() != ';') return false;
  const std::string_view body = entity.substr(1, entity.size() - 2);
  const auto iequals = [&](std::string_view target) noexcept {
    if (body.size() != target.size()) return false;
    for (size_t i = 0; i < body.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(body[i])) != target[i]) return false;
    }
    return true;
  };

  if (iequals("amp")) {
    out.push_back('&');
    return true;
  }
  if (iequals("lt")) {
    out.push_back('<');
    return true;
  }
  if (iequals("gt")) {
    out.push_back('>');
    return true;
  }
  if (iequals("quot")) {
    out.push_back('"');
    return true;
  }
  if (iequals("apos")) {
    out.push_back('\'');
    return true;
  }
  if (iequals("nbsp")) {
    out.push_back(' ');
    return true;
  }

  if (body.front() == '#' && body.size() >= 2) {
    size_t pos = 1;
    uint32_t base = 10;
    if (body[pos] == 'x' || body[pos] == 'X') {
      base = 16;
      ++pos;
    }
    if (pos >= body.size()) return false;
    uint32_t value = 0;
    for (; pos < body.size(); ++pos) {
      const unsigned char c = static_cast<unsigned char>(body[pos]);
      uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (base == 16 && c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (base == 16 && c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return false;
      }
      if (value > (0x10FFFFu - digit) / base) return false;
      value = value * base + digit;
    }
    appendUtf8Codepoint(out, value);
    return true;
  }

  return false;
}

void normalizeHtmlField(const char* raw, std::string& out) {
  out.clear();
  if (!raw || raw[0] == '\0') return;

  const std::string_view src(raw);
  out.reserve(src.size());

  const auto trimTrailingHorizontalSpaces = [&]() {
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
      out.pop_back();
    }
  };

  for (size_t i = 0; i < src.size();) {
    if (src[i] == '<' && isHtmlTagStart(src, i)) {
      const size_t close = src.find('>', i + 1);
      if (close != std::string_view::npos) {
        const HtmlTagAction action = classifyHtmlTag(src.substr(i + 1, close - i - 1));
        if (action == HtmlTagAction::ForceNewline) {
          trimTrailingHorizontalSpaces();
          out.push_back('\n');
        } else if (action == HtmlTagAction::BlockBoundary) {
          trimTrailingHorizontalSpaces();
          if (!out.empty() && out.back() != '\n') {
            out.push_back('\n');
          }
        }
        i = close + 1;
        continue;
      }
    }

    if (src[i] == '&') {
      const size_t semi = src.find(';', i + 1);
      if (semi != std::string_view::npos && semi - i <= 12) {
        if (decodeHtmlEntity(src.substr(i, semi - i + 1), out)) {
          i = semi + 1;
          continue;
        }
      }
    }

    const char c = src[i++];
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      trimTrailingHorizontalSpaces();
      out.push_back('\n');
    } else {
      out.push_back(c);
    }
  }

  while (!out.empty() && isAsciiSpace(out.back())) {
    out.pop_back();
  }
  size_t lead = 0;
  while (lead < out.size() && isAsciiSpace(out[lead])) {
    ++lead;
  }
  if (lead > 0) {
    out.erase(0, lead);
  }
}

struct IndexBuildWorkspace {
  DeckIndexHeader newHead{};
  HalFile tmpFile;
  JsonDocument doc;
  std::string tmpPath;
};

bool indexSingleLine(char* line, size_t lineLen, uint32_t lineOffset, IndexBuildWorkspace& ws) {
  if (!hasNonWhitespace(std::string_view(line, lineLen))) {
    return true;
  }

  const DeserializationError err = deserializeJson(ws.doc, line, lineLen);
  if (err != DeserializationError::Ok || ws.doc.overflowed() || !ws.doc.is<JsonObjectConst>()) {
    LOG_ERR("FCARD", "Skipping malformed JSON line at offset %u", static_cast<unsigned>(lineOffset));
    return true;
  }

  const JsonObjectConst obj = ws.doc.as<JsonObjectConst>();
  if (obj["_meta"].is<bool>() && obj["_meta"].as<bool>()) {
    const char* metaTitle = obj["title"] | "";
    if (metaTitle[0] != '\0') {
      std::memset(ws.newHead.title, 0, sizeof(ws.newHead.title));
      copyUtf8Truncated(ws.newHead.title, sizeof(ws.newHead.title), metaTitle);
    }
    if (!obj["newPerDay"].isNull()) {
      const int32_t rawNew = obj["newPerDay"] | static_cast<int32_t>(DEFAULT_NEW_PER_DAY);
      ws.newHead.newPerDay = static_cast<uint16_t>(std::clamp<int32_t>(rawNew, 0, MAX_NEW_PER_DAY));
    }
    if (!obj["maxReviews"].isNull()) {
      const int32_t rawMax = obj["maxReviews"] | static_cast<int32_t>(DEFAULT_MAX_REVIEWS);
      ws.newHead.maxReviews = static_cast<uint16_t>(std::clamp<int32_t>(rawMax, 1, MAX_REVIEWS_CAP));
    }
    return true;
  }

  const char* rawFront = obj["front"] | "";
  const char* rawBack = obj["back"] | "";
  if (!hasNonWhitespace(rawFront) || !hasNonWhitespace(rawBack)) {
    LOG_ERR("FCARD", "Skipping card with missing/empty front or back at offset %u", static_cast<unsigned>(lineOffset));
    return true;
  }

  if (ws.newHead.cardCount == std::numeric_limits<uint32_t>::max()) {
    LOG_ERR("FCARD", "Card count overflow at offset %u", static_cast<unsigned>(lineOffset));
    return false;
  }

  CardIndexRecord rec{};
  rec.cardIdHash = computeCardIdHash(obj, rawFront);
  rec.byteOffset = lineOffset;
  rec.byteLength = static_cast<uint16_t>(lineLen);
  rec.flags = 0;

  if (!serialization::tryWritePod(ws.tmpFile, rec)) {
    LOG_ERR("FCARD", "Failed writing index record to %s", ws.tmpPath.c_str());
    return false;
  }

  ++ws.newHead.cardCount;
  return true;
}

}  // namespace

FlashcardDeckIndex::~FlashcardDeckIndex() { close(); }

bool FlashcardDeckIndex::ensureDir(const char* dirPath) {
  if (!dirPath || dirPath[0] == '\0') {
    LOG_ERR("FCARD", "Cannot create empty directory path");
    return false;
  }

  const std::string_view full(dirPath);
  if (full == "/") return true;

  std::string current;
  current.reserve(full.size());

  size_t pos = 0;
  if (full[0] == '/') {
    current.push_back('/');
    pos = 1;
  }

  while (pos < full.size()) {
    const size_t nextSlash = full.find('/', pos);
    const size_t segEnd = (nextSlash == std::string_view::npos) ? full.size() : nextSlash;
    if (segEnd > pos) {
      if (current.size() > 1 && current.back() != '/') {
        current.push_back('/');
      }
      current.append(full.data() + pos, segEnd - pos);
      if (!Storage.exists(current.c_str()) && !Storage.mkdir(current.c_str())) {
        LOG_ERR("FCARD", "Failed to create directory %s", current.c_str());
        return false;
      }
    }
    if (nextSlash == std::string_view::npos) break;
    pos = nextSlash + 1;
  }

  return true;
}

void FlashcardDeckIndex::close() {
  if (idxFile.isOpen()) {
    idxFile.close();
  }
  if (jsonlFile.isOpen()) {
    jsonlFile.close();
  }
  head = DeckIndexHeader{};
  opened = false;
}

bool FlashcardDeckIndex::tryOpenValidIndex(const char* idxPath, uint32_t actualDeckSize) {
  if (!Storage.exists(idxPath)) {
    return false;
  }
  if (!Storage.openFileForRead("FCARD", idxPath, idxFile)) {
    return false;
  }

  DeckIndexHeader candidate{};
  if (!serialization::tryReadPod(idxFile, candidate)) {
    idxFile.close();
    return false;
  }

  const uint64_t expectedBytes =
      sizeof(DeckIndexHeader) + static_cast<uint64_t>(candidate.cardCount) * sizeof(CardIndexRecord);
  if (candidate.magic != DECK_INDEX_MAGIC || candidate.version != DECK_INDEX_VERSION ||
      candidate.deckFileSize != actualDeckSize || expectedBytes != static_cast<uint64_t>(idxFile.size())) {
    idxFile.close();
    return false;
  }

  candidate.title[sizeof(candidate.title) - 1] = '\0';
  candidate.newPerDay = std::min<uint16_t>(candidate.newPerDay, MAX_NEW_PER_DAY);
  candidate.maxReviews = std::clamp<uint16_t>(candidate.maxReviews, 1, MAX_REVIEWS_CAP);
  head = candidate;
  opened = true;
  return true;
}

bool FlashcardDeckIndex::buildIndex(const char* jsonlPath, const char* idxPath, uint32_t actualDeckSize) {
  auto ws = makeUniqueNoThrow<IndexBuildWorkspace>();
  if (!ws) {
    LOG_ERR("FCARD", "OOM allocating %u-byte index workspace", static_cast<unsigned>(sizeof(IndexBuildWorkspace)));
    return false;
  }

  auto readBuf = makeUniqueNoThrow<char[]>(STREAM_READ_CHUNK_BYTES);
  if (!readBuf) {
    LOG_ERR("FCARD", "OOM allocating %u-byte JSONL read chunk", static_cast<unsigned>(STREAM_READ_CHUNK_BYTES));
    return false;
  }

  auto lineBuf = makeUniqueNoThrow<char[]>(MAX_JSONL_LINE_BYTES);
  if (!lineBuf) {
    LOG_ERR("FCARD", "OOM allocating %u-byte JSONL line buffer", static_cast<unsigned>(MAX_JSONL_LINE_BYTES));
    return false;
  }

  ws->newHead.magic = DECK_INDEX_MAGIC;
  ws->newHead.version = DECK_INDEX_VERSION;
  ws->newHead.deckFileSize = actualDeckSize;
  ws->newHead.cardCount = 0;
  ws->newHead.newPerDay = DEFAULT_NEW_PER_DAY;
  ws->newHead.maxReviews = DEFAULT_MAX_REVIEWS;
  extractStemTitle(jsonlPath, ws->newHead.title);

  if (!ensureParentDir(idxPath)) {
    return false;
  }

  ws->tmpPath.assign(idxPath);
  ws->tmpPath.append(".tmp");
  if (Storage.exists(ws->tmpPath.c_str())) {
    Storage.remove(ws->tmpPath.c_str());
  }

  if (!Storage.openFileForWrite("FCARD", ws->tmpPath, ws->tmpFile)) {
    LOG_ERR("FCARD", "Failed to open %s for write", ws->tmpPath.c_str());
    return false;
  }

  const auto abortTmp = [&]() {
    if (ws->tmpFile.isOpen()) {
      ws->tmpFile.close();
    }
    Storage.remove(ws->tmpPath.c_str());
    return false;
  };

  if (!serialization::tryWritePod(ws->tmpFile, ws->newHead)) {
    LOG_ERR("FCARD", "Failed writing placeholder header to %s", ws->tmpPath.c_str());
    return abortTmp();
  }

  if (!jsonlFile.seek(0)) {
    LOG_ERR("FCARD", "Failed seeking %s to start", jsonlPath);
    return abortTmp();
  }

  uint32_t streamOffset = 0;
  uint32_t lineStartOffset = 0;
  size_t lineLen = 0;
  bool lineOverflowed = false;
  bool pendingCr = false;

  while (streamOffset < actualDeckSize) {
    const size_t toRead = std::min<size_t>(STREAM_READ_CHUNK_BYTES, actualDeckSize - streamOffset);
    const int bytesRead = jsonlFile.read(readBuf.get(), toRead);
    if (bytesRead <= 0) {
      LOG_ERR("FCARD", "Short read while indexing %s at offset %u", jsonlPath, static_cast<unsigned>(streamOffset));
      return abortTmp();
    }

    for (int i = 0; i < bytesRead; ++i) {
      const char c = readBuf[static_cast<size_t>(i)];
      ++streamOffset;

      if (c == '\n') {
        pendingCr = false;
        if (lineOverflowed) {
          LOG_ERR("FCARD", "Skipping overlong JSONL line (> %u bytes) at offset %u",
                  static_cast<unsigned>(MAX_JSONL_LINE_BYTES - 1), static_cast<unsigned>(lineStartOffset));
        } else if (lineLen > 0) {
          lineBuf[lineLen] = '\0';
          if (!indexSingleLine(lineBuf.get(), lineLen, lineStartOffset, *ws)) {
            return abortTmp();
          }
        }
        lineLen = 0;
        lineOverflowed = false;
        lineStartOffset = streamOffset;
        continue;
      }

      if (pendingCr) {
        if (!lineOverflowed) {
          if (lineLen < MAX_JSONL_LINE_BYTES - 1) {
            lineBuf[lineLen++] = '\r';
          } else {
            lineOverflowed = true;
          }
        }
        pendingCr = false;
      }

      if (c == '\r') {
        pendingCr = true;
      } else if (!lineOverflowed) {
        if (lineLen < MAX_JSONL_LINE_BYTES - 1) {
          lineBuf[lineLen++] = c;
        } else {
          lineOverflowed = true;
        }
      }
    }
  }

  pendingCr = false;
  if (lineOverflowed) {
    LOG_ERR("FCARD", "Skipping overlong trailing JSONL line (> %u bytes) at offset %u",
            static_cast<unsigned>(MAX_JSONL_LINE_BYTES - 1), static_cast<unsigned>(lineStartOffset));
  } else if (lineLen > 0) {
    lineBuf[lineLen] = '\0';
    if (!indexSingleLine(lineBuf.get(), lineLen, lineStartOffset, *ws)) {
      return abortTmp();
    }
  }

  if (!ws->tmpFile.seek(0) || !serialization::tryWritePod(ws->tmpFile, ws->newHead)) {
    LOG_ERR("FCARD", "Failed finalizing index header in %s", ws->tmpPath.c_str());
    return abortTmp();
  }
  ws->tmpFile.flush();
  if (!ws->tmpFile.close()) {
    LOG_ERR("FCARD", "Failed closing %s", ws->tmpPath.c_str());
    Storage.remove(ws->tmpPath.c_str());
    return false;
  }

  if (Storage.exists(idxPath) && !Storage.remove(idxPath)) {
    LOG_ERR("FCARD", "Failed removing outdated index %s", idxPath);
    Storage.remove(ws->tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(ws->tmpPath.c_str(), idxPath)) {
    LOG_ERR("FCARD", "Failed renaming %s to %s", ws->tmpPath.c_str(), idxPath);
    Storage.remove(ws->tmpPath.c_str());
    return false;
  }

  if (!Storage.openFileForRead("FCARD", idxPath, idxFile)) {
    LOG_ERR("FCARD", "Failed reopening built index %s", idxPath);
    return false;
  }

  head = ws->newHead;
  opened = true;
  return true;
}

bool FlashcardDeckIndex::openOrBuild(const char* jsonlPath, const char* idxPath) {
  close();
  if (!jsonlPath || jsonlPath[0] == '\0' || !idxPath || idxPath[0] == '\0') {
    LOG_ERR("FCARD", "Invalid deck or index path");
    return false;
  }

  if (!Storage.openFileForRead("FCARD", jsonlPath, jsonlFile)) {
    LOG_ERR("FCARD", "Failed to open deck %s", jsonlPath);
    return false;
  }

  const uint64_t rawSize = static_cast<uint64_t>(jsonlFile.size());
  if (rawSize > std::numeric_limits<uint32_t>::max()) {
    LOG_ERR("FCARD", "Deck file %s exceeds 4 GB limit", jsonlPath);
    close();
    return false;
  }
  const uint32_t actualDeckSize = static_cast<uint32_t>(rawSize);

  if (tryOpenValidIndex(idxPath, actualDeckSize)) {
    return true;
  }

  if (!buildIndex(jsonlPath, idxPath, actualDeckSize)) {
    close();
    return false;
  }

  return true;
}

bool FlashcardDeckIndex::readIndexRecord(uint32_t ordinal, CardIndexRecord& out) {
  return readIndexRecords(ordinal, &out, 1);
}

bool FlashcardDeckIndex::readIndexRecords(uint32_t startOrdinal, CardIndexRecord* outBuf, size_t count) {
  if (!opened || !outBuf || count == 0) {
    return false;
  }
  if (startOrdinal >= head.cardCount || count > head.cardCount - startOrdinal) {
    return false;
  }

  const uint64_t offset = sizeof(DeckIndexHeader) + static_cast<uint64_t>(startOrdinal) * sizeof(CardIndexRecord);
  const size_t totalBytes = count * sizeof(CardIndexRecord);
  if (offset > std::numeric_limits<size_t>::max()) {
    return false;
  }

  if (!idxFile.seek(static_cast<size_t>(offset)) || idxFile.read(outBuf, totalBytes) != static_cast<int>(totalBytes)) {
    LOG_ERR("FCARD", "Failed reading %u index record(s) at ordinal %u", static_cast<unsigned>(count),
            static_cast<unsigned>(startOrdinal));
    return false;
  }

  for (size_t i = 0; i < count; ++i) {
    const CardIndexRecord& rec = outBuf[i];
    if (rec.byteLength == 0 || rec.byteLength >= MAX_JSONL_LINE_BYTES ||
        static_cast<uint64_t>(rec.byteOffset) + rec.byteLength > head.deckFileSize) {
      LOG_ERR("FCARD", "Corrupt index record bounds at ordinal %u", static_cast<unsigned>(startOrdinal + i));
      return false;
    }
  }

  return true;
}

bool FlashcardDeckIndex::readCard(uint32_t ordinal, char* lineScratch, size_t scratchSize, FlashcardItem& outCard) {
  if (!opened || !lineScratch || scratchSize == 0) {
    LOG_ERR("FCARD", "Cannot read card: index closed or invalid scratch buffer");
    return false;
  }

  CardIndexRecord rec{};
  if (!readIndexRecord(ordinal, rec)) {
    return false;
  }

  if (static_cast<size_t>(rec.byteLength) + 1 > scratchSize) {
    LOG_ERR("FCARD", "Scratch buffer (%u B) too small for %u-byte card line", static_cast<unsigned>(scratchSize),
            static_cast<unsigned>(rec.byteLength));
    return false;
  }

  if (!jsonlFile.seek(rec.byteOffset) ||
      jsonlFile.read(lineScratch, rec.byteLength) != static_cast<int>(rec.byteLength)) {
    LOG_ERR("FCARD", "Failed reading card JSONL at offset %u", static_cast<unsigned>(rec.byteOffset));
    return false;
  }
  lineScratch[rec.byteLength] = '\0';

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, lineScratch, rec.byteLength);
  if (err != DeserializationError::Ok || doc.overflowed() || !doc.is<JsonObjectConst>()) {
    LOG_ERR("FCARD", "Malformed card JSON at ordinal %u", static_cast<unsigned>(ordinal));
    return false;
  }

  const JsonObjectConst obj = doc.as<JsonObjectConst>();
  normalizeHtmlField(obj["front"] | "", outCard.front);
  normalizeHtmlField(obj["back"] | "", outCard.back);
  normalizeHtmlField(obj["hint"] | "", outCard.hint);
  normalizeHtmlField(obj["extra"] | "", outCard.extra);
  outCard.cardIdHash = rec.cardIdHash;
  return true;
}

}  // namespace flashcards
