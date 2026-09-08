cmake_minimum_required(VERSION 3.20)
project(patchouli_all_min LANGUAGES CXX)

add_executable(patchouli src/main.cpp)

target_compile_features(patchouli PRIVATE cxx_std_20)

target_include_directories(
  patchouli PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)

if(WIN32)
  target_link_libraries(patchouli PRIVATE psapi)
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(patchouli_tests tests/patchouli_tests.cpp)
  target_compile_features(patchouli_tests PRIVATE cxx_std_20)
  target_include_directories(patchouli_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
  add_test(NAME patchouli_tests COMMAND patchouli_tests)
endif()
