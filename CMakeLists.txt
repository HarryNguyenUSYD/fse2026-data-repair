cmake_minimum_required(VERSION 3.20)
project(fse2026_new_test LANGUAGES CXX)
include(CTest)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
foreach(configuration Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${configuration} configuration_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${configuration_upper} ${CMAKE_BINARY_DIR})
endforeach()

set(PATCHOULI_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sources/patchouli")
add_subdirectory(${PATCHOULI_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/patchouli-build)
add_executable(betamax-old sources/betamax/main.cpp)
target_compile_features(betamax-old PRIVATE cxx_std_20)

add_executable(epsilonrepair sources/epsilonrepair/erepair2.cpp)
target_compile_features(epsilonrepair PRIVATE cxx_std_17)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(epsilonrepair PRIVATE -include algorithm)
endif()

foreach(format date time url isbn ipv4 ipv6)
  add_executable(validate_${format} validators/validator.cpp)
  target_compile_features(validate_${format} PRIVATE cxx_std_20)

  add_executable(validate_old_${format} validators/validator.cpp)
  target_compile_features(validate_old_${format} PRIVATE cxx_std_20)
  target_compile_definitions(validate_old_${format} PRIVATE VALIDATOR_FILE_INPUT=1)

  add_executable(boundary_validate_${format} validators/boundary_validator.cpp)
  target_compile_features(boundary_validate_${format} PRIVATE cxx_std_20)
endforeach()
