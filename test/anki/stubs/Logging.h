#pragma once

namespace anki_test_log {
inline void sink(const char*, const char*, ...) {}
}  // namespace anki_test_log

#define LOG_DBG(origin, ...) anki_test_log::sink(origin, __VA_ARGS__)
#define LOG_ERR(origin, ...) anki_test_log::sink(origin, __VA_ARGS__)
#define LOG_INF(origin, ...) anki_test_log::sink(origin, __VA_ARGS__)
