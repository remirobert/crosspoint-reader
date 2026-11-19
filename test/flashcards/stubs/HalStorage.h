#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using oflag_t = int;

inline constexpr oflag_t O_RDONLY = 0x00;
inline constexpr oflag_t O_READ = O_RDONLY;
inline constexpr oflag_t O_WRONLY = 0x01;
inline constexpr oflag_t O_WRITE = O_WRONLY;
inline constexpr oflag_t O_RDWR = 0x02;
inline constexpr oflag_t O_AT_END = 0x04;
inline constexpr oflag_t O_APPEND = 0x08;
inline constexpr oflag_t O_CREAT = 0x10;
inline constexpr oflag_t O_TRUNC = 0x20;
inline constexpr oflag_t O_EXCL = 0x40;

namespace flashcards_fs {

struct Node {
  bool directory = false;
  uint32_t mtime = 1;
  std::vector<uint8_t> bytes;
};

inline std::map<std::string, std::shared_ptr<Node>> files;
inline std::map<std::string, unsigned> writesByPath;
inline std::map<std::string, unsigned> opensByPath;

inline std::string normalizePath(std::string_view raw) {
  if (raw.empty()) return "/";
  std::string out(raw);
  if (out.front() != '/') out.insert(out.begin(), '/');
  while (out.size() > 1 && out.back() == '/') {
    out.pop_back();
  }
  return out;
}

inline std::string parentPath(const std::string& norm) {
  const size_t slash = norm.find_last_of('/');
  if (slash == std::string::npos || slash == 0) return "/";
  return norm.substr(0, slash);
}

inline void ensureRoot() {
  if (!files.count("/")) {
    auto root = std::make_shared<Node>();
    root->directory = true;
    files["/"] = std::move(root);
  }
}

inline void ensureParents(const std::string& norm) {
  ensureRoot();
  size_t pos = 1;
  while (true) {
    const size_t slash = norm.find('/', pos);
    if (slash == std::string::npos) break;
    const std::string dir = norm.substr(0, slash);
    if (!files.count(dir)) {
      auto node = std::make_shared<Node>();
      node->directory = true;
      files[dir] = std::move(node);
    }
    pos = slash + 1;
  }
}

inline void reset() {
  files.clear();
  writesByPath.clear();
  opensByPath.clear();
  ensureRoot();
}

inline void setFile(std::string_view path, std::string_view content, uint32_t mtime = 1) {
  const std::string norm = normalizePath(path);
  ensureParents(norm);
  auto node = std::make_shared<Node>();
  node->directory = false;
  node->mtime = mtime;
  node->bytes.assign(content.begin(), content.end());
  files[norm] = std::move(node);
}

inline void setFileBytes(std::string_view path, const std::vector<uint8_t>& data, uint32_t mtime = 1) {
  const std::string norm = normalizePath(path);
  ensureParents(norm);
  auto node = std::make_shared<Node>();
  node->directory = false;
  node->mtime = mtime;
  node->bytes = data;
  files[norm] = std::move(node);
}

inline std::vector<uint8_t> getFileBytes(std::string_view path) {
  const auto it = files.find(normalizePath(path));
  if (it == files.end() || it->second->directory) return {};
  return it->second->bytes;
}

inline std::string getFileString(std::string_view path) {
  const auto it = files.find(normalizePath(path));
  if (it == files.end() || it->second->directory) return {};
  return std::string(reinterpret_cast<const char*>(it->second->bytes.data()), it->second->bytes.size());
}

inline void appendRawBytes(std::string_view path, const void* data, size_t len) {
  const std::string norm = normalizePath(path);
  ensureParents(norm);
  auto& node = files[norm];
  if (!node) node = std::make_shared<Node>();
  const auto* ptr = static_cast<const uint8_t*>(data);
  node->bytes.insert(node->bytes.end(), ptr, ptr + len);
}

inline void truncateRawBytes(std::string_view path, size_t newSize) {
  const auto it = files.find(normalizePath(path));
  if (it != files.end() && !it->second->directory && newSize < it->second->bytes.size()) {
    it->second->bytes.resize(newSize);
  }
}

inline unsigned writeCount(std::string_view path) {
  const auto it = writesByPath.find(normalizePath(path));
  return it == writesByPath.end() ? 0u : it->second;
}

inline unsigned openCount(std::string_view path) {
  const auto it = opensByPath.find(normalizePath(path));
  return it == opensByPath.end() ? 0u : it->second;
}

}  // namespace flashcards_fs

class HalStorage;

class HalFile {
  friend class HalStorage;

 public:
  HalFile() = default;
  ~HalFile() = default;
  HalFile(HalFile&&) noexcept = default;
  HalFile& operator=(HalFile&&) noexcept = default;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush() {}

  size_t getName(char* name, size_t len) {
    if (!node) {
      if (name && len > 0) name[0] = '\0';
      return 0;
    }
    const size_t slash = path.find_last_of('/');
    const std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    if (name && len > 0) {
      const size_t copyLen = std::min(base.size(), len - 1);
      std::memcpy(name, base.data(), copyLen);
      name[copyLen] = '\0';
    }
    return base.size();
  }

  size_t size() { return node ? node->bytes.size() : 0; }
  size_t fileSize() { return size(); }
  uint64_t fileSize64() { return node ? static_cast<uint64_t>(node->bytes.size()) : 0; }
  uint32_t modificationTime() { return node ? node->mtime : 0; }

  bool seekSet(size_t offset) {
    if (!node || node->directory || offset > node->bytes.size()) return false;
    filePos = offset;
    return true;
  }

  bool seek(size_t pos) { return seekSet(pos); }
  bool seek64(uint64_t pos) { return seekSet(static_cast<size_t>(pos)); }

  bool seekCur(int64_t offset) {
    if (!node || node->directory) return false;
    const int64_t target = static_cast<int64_t>(filePos) + offset;
    if (target < 0) return false;
    return seekSet(static_cast<size_t>(target));
  }

  bool truncate(uint64_t length) {
    if (!node || !canWrite || node->directory || length > node->bytes.size()) return false;
    node->bytes.resize(static_cast<size_t>(length));
    filePos = std::min(filePos, node->bytes.size());
    return true;
  }

  int available() const {
    if (!node || node->directory) return 0;
    const size_t sz = node->bytes.size();
    return filePos < sz ? static_cast<int>(sz - filePos) : 0;
  }

  size_t position() const { return node ? filePos : 0; }

  int read(void* buf, size_t count) {
    if (!node || !canRead || node->directory) return -1;
    if (count == 0 || filePos >= node->bytes.size()) return 0;
    const size_t n = std::min(count, node->bytes.size() - filePos);
    std::memcpy(buf, node->bytes.data() + filePos, n);
    filePos += n;
    return static_cast<int>(n);
  }

  int read() {
    if (!node || !canRead || node->directory || filePos >= node->bytes.size()) return -1;
    return static_cast<int>(node->bytes[filePos++]);
  }

  size_t write(const uint8_t* buf, size_t count) {
    if (!node || !canWrite || node->directory) return 0;
    if (count == 0) return 0;
    flashcards_fs::writesByPath[path]++;
    if (appendMode) {
      filePos = node->bytes.size();
    }
    if (filePos + count > node->bytes.size()) {
      node->bytes.resize(filePos + count);
    }
    std::memcpy(node->bytes.data() + filePos, buf, count);
    filePos += count;
    return count;
  }

  size_t write(const void* buf, size_t count) { return write(static_cast<const uint8_t*>(buf), count); }

  size_t write(uint8_t b) { return write(&b, 1); }

  bool isDirectory() const { return node && node->directory; }

  void rewindDirectory() { filePos = 0; }

  bool close() {
    const bool wasOpen = static_cast<bool>(node);
    node.reset();
    path.clear();
    filePos = 0;
    canRead = false;
    canWrite = false;
    appendMode = false;
    return wasOpen;
  }

  HalFile openNextFile() {
    if (!node || !node->directory) return {};
    std::vector<std::string> children;
    for (const auto& [entryPath, entryNode] : flashcards_fs::files) {
      if (entryPath != path && flashcards_fs::parentPath(entryPath) == path) {
        children.push_back(entryPath);
      }
    }
    if (filePos >= children.size()) return {};
    HalFile child;
    child.path = children[filePos++];
    child.node = flashcards_fs::files[child.path];
    child.canRead = true;
    return child;
  }

  bool isOpen() const { return static_cast<bool>(node); }
  operator bool() const { return isOpen(); }

 private:
  std::shared_ptr<flashcards_fs::Node> node;
  std::string path;
  size_t filePos = 0;
  bool canRead = false;
  bool canWrite = false;
  bool appendMode = false;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool ready() const { return true; }

  bool exists(const char* path) {
    if (!path || path[0] == '\0') return false;
    flashcards_fs::ensureRoot();
    return flashcards_fs::files.count(flashcards_fs::normalizePath(path)) != 0;
  }

  bool mkdir(const char* path, const bool pFlag = true) {
    if (!path || path[0] == '\0') return false;
    flashcards_fs::ensureRoot();
    const std::string norm = flashcards_fs::normalizePath(path);
    const auto existing = flashcards_fs::files.find(norm);
    if (existing != flashcards_fs::files.end()) {
      return existing->second->directory;
    }
    if (pFlag) {
      flashcards_fs::ensureParents(norm);
    } else {
      const auto parent = flashcards_fs::files.find(flashcards_fs::parentPath(norm));
      if (parent == flashcards_fs::files.end() || !parent->second->directory) return false;
    }
    auto dir = std::make_shared<flashcards_fs::Node>();
    dir->directory = true;
    flashcards_fs::files[norm] = std::move(dir);
    return true;
  }

  bool ensureDirectoryExists(const char* path) { return mkdir(path, true); }

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY) {
    if (!path || path[0] == '\0') return {};
    flashcards_fs::ensureRoot();
    const std::string norm = flashcards_fs::normalizePath(path);
    const int accMode = oflag & 0x03;
    const bool canRead = (accMode == O_RDONLY) || ((oflag & O_RDWR) != 0);
    const bool canWrite = ((oflag & O_WRONLY) != 0) || ((oflag & O_RDWR) != 0);

    auto it = flashcards_fs::files.find(norm);
    if (it == flashcards_fs::files.end()) {
      if (!(oflag & O_CREAT) || !canWrite) return {};
      const auto parent = flashcards_fs::files.find(flashcards_fs::parentPath(norm));
      if (parent == flashcards_fs::files.end() || !parent->second->directory) return {};
      auto created = std::make_shared<flashcards_fs::Node>();
      it = flashcards_fs::files.emplace(norm, std::move(created)).first;
    } else {
      if ((oflag & O_CREAT) && (oflag & O_EXCL)) return {};
      if (it->second->directory && canWrite) return {};
    }

    if ((oflag & O_TRUNC) && canWrite) {
      it->second->bytes.clear();
    }

    flashcards_fs::opensByPath[norm]++;
    HalFile f;
    f.node = it->second;
    f.path = norm;
    f.canRead = canRead;
    f.canWrite = canWrite;
    f.appendMode = (oflag & O_APPEND) != 0;
    f.filePos = (oflag & (O_APPEND | O_AT_END)) ? it->second->bytes.size() : 0;
    return f;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file = open(path, O_RDONLY);
    return static_cast<bool>(file);
  }

  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForRead(moduleName, path.c_str(), file);
  }

  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file = open(path, O_RDWR | O_CREAT | O_TRUNC);
    return static_cast<bool>(file);
  }

  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
    return openFileForWrite(moduleName, path.c_str(), file);
  }

  bool remove(const char* path) {
    if (!path || path[0] == '\0') return false;
    const std::string norm = flashcards_fs::normalizePath(path);
    const auto it = flashcards_fs::files.find(norm);
    if (it == flashcards_fs::files.end() || it->second->directory) return false;
    flashcards_fs::files.erase(it);
    return true;
  }

  // Matches SdFat vol().rename(): fails if oldPath is missing or newPath already exists.
  bool rename(const char* oldPath, const char* newPath) {
    if (!oldPath || !newPath) return false;
    const std::string src = flashcards_fs::normalizePath(oldPath);
    const std::string dst = flashcards_fs::normalizePath(newPath);
    if (!flashcards_fs::files.count(src) || flashcards_fs::files.count(dst)) return false;
    const auto dstParent = flashcards_fs::files.find(flashcards_fs::parentPath(dst));
    if (dstParent == flashcards_fs::files.end() || !dstParent->second->directory) return false;

    std::vector<std::pair<std::string, std::shared_ptr<flashcards_fs::Node>>> moved;
    const std::string prefix = src + "/";
    for (auto it = flashcards_fs::files.begin(); it != flashcards_fs::files.end();) {
      if (it->first == src || it->first.starts_with(prefix)) {
        moved.emplace_back(dst + it->first.substr(src.size()), it->second);
        it = flashcards_fs::files.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& [p, node] : moved) {
      flashcards_fs::files[p] = std::move(node);
    }
    return true;
  }

  bool replaceFile(const char* tmpPath, const char* path) {
    return (!exists(path) || remove(path)) && rename(tmpPath, path);
  }

  bool rmdir(const char* path) {
    if (!path || path[0] == '\0') return false;
    const std::string norm = flashcards_fs::normalizePath(path);
    if (norm == "/") return false;
    const auto it = flashcards_fs::files.find(norm);
    if (it == flashcards_fs::files.end() || !it->second->directory) return false;
    const std::string prefix = norm + "/";
    for (const auto& [k, _] : flashcards_fs::files) {
      if (k.starts_with(prefix)) return false;
    }
    flashcards_fs::files.erase(it);
    return true;
  }

  bool removeDir(const char* path) {
    if (!path || path[0] == '\0') return false;
    const std::string norm = flashcards_fs::normalizePath(path);
    if (norm == "/") return false;
    const auto it = flashcards_fs::files.find(norm);
    if (it == flashcards_fs::files.end() || !it->second->directory) return false;
    const std::string prefix = norm + "/";
    for (auto cur = flashcards_fs::files.begin(); cur != flashcards_fs::files.end();) {
      if (cur->first == norm || cur->first.starts_with(prefix)) {
        cur = flashcards_fs::files.erase(cur);
      } else {
        ++cur;
      }
    }
    return true;
  }
};

#define Storage HalStorage::getInstance()
