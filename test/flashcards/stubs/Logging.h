#pragma once

namespace flashcards_test_log {
inline void sink(const char*, const char*, ...) {}
}  // namespace flashcards_test_log

#define LOG_DBG(origin, ...) flashcards_test_log::sink(origin, __VA_ARGS__)
#define LOG_ERR(origin, ...) flashcards_test_log::sink(origin, __VA_ARGS__)
#define LOG_INF(origin, ...) flashcards_test_log::sink(origin, __VA_ARGS__)
