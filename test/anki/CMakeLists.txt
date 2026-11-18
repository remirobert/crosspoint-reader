if(NOT TARGET bleturner_arduinojson)
  include(FetchContent)
  FetchContent_Declare(
    arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
    SOURCE_SUBDIR no-build
  )
  FetchContent_MakeAvailable(arduinojson)
  add_library(bleturner_arduinojson INTERFACE)
  target_include_directories(bleturner_arduinojson SYSTEM INTERFACE ${arduinojson_SOURCE_DIR}/src)
endif()

add_executable(AnkiTest
  AnkiTest.cpp
  ${REPO_ROOT}/lib/Anki/AnkiScheduler.cpp
  ${REPO_ROOT}/lib/Anki/AnkiDeckIndex.cpp
  ${REPO_ROOT}/lib/Anki/AnkiSrsStore.cpp
  ${REPO_ROOT}/lib/Anki/AnkiSession.cpp
)

target_include_directories(AnkiTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Anki
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(AnkiTest PRIVATE
  crosspoint_test_common
  bleturner_arduinojson
  GTest::gtest_main
)

gtest_discover_tests(AnkiTest)
