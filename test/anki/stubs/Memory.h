#pragma once

#include <cstddef>
#include <iterator>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace anki_test_mem {
inline int failAllocCountdown = -1;

inline bool shouldFailAlloc() {
  if (failAllocCountdown < 0) return false;
  if (failAllocCountdown == 0) {
    failAllocCountdown = -1;
    return true;
  }
  --failAllocCountdown;
  return false;
}
}  // namespace anki_test_mem

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (anki_test_mem::shouldFailAlloc()) return nullptr;
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(size_t count) {
  if (anki_test_mem::shouldFailAlloc()) return nullptr;
  using Elem = std::remove_extent_t<T>;
  return std::unique_ptr<T>(new (std::nothrow) Elem[count]());
}

template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F f) : fn{std::move(f)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;

template <typename T, typename A>
void shrinkToFit(std::vector<T, A>& v) {
  if (v.capacity() == v.size()) return;
  std::vector<T, A>(std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()), v.get_allocator()).swap(v);
}
