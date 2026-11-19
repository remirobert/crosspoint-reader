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

add_executable(FlashcardsTest
  FlashcardsTest.cpp
  ${REPO_ROOT}/lib/Flashcards/FlashcardScheduler.cpp
  ${REPO_ROOT}/lib/Flashcards/FlashcardDeckIndex.cpp
  ${REPO_ROOT}/lib/Flashcards/FlashcardSrsStore.cpp
  ${REPO_ROOT}/lib/Flashcards/FlashcardSession.cpp
)

target_include_directories(FlashcardsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Flashcards
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(FlashcardsTest PRIVATE
  crosspoint_test_common
  bleturner_arduinojson
  GTest::gtest_main
)

gtest_discover_tests(FlashcardsTest)
