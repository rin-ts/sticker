cmake_minimum_required(VERSION 3.20)

project(rin_sticker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(sticker_enable_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall
    -Wextra
    -Wpedantic
    -Werror
  )
endfunction()

find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBP REQUIRED IMPORTED_TARGET
  libwebp
  libwebpmux
  libwebpdemux
)

add_library(sticker_native_core STATIC
  src/native/sticker_pipeline.cpp
)
target_include_directories(sticker_native_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/native"
)
target_link_libraries(sticker_native_core PUBLIC
  JPEG::JPEG
  PNG::PNG
  PkgConfig::WEBP
)
sticker_enable_warnings(sticker_native_core)

add_executable(sticker_worker
  src/native/sticker_worker.cpp
)
target_link_libraries(sticker_worker PRIVATE sticker_native_core)
sticker_enable_warnings(sticker_worker)

add_executable(sticker_native_tests
  tests/native_worker_test.cpp
)
target_link_libraries(sticker_native_tests PRIVATE sticker_native_core)
sticker_enable_warnings(sticker_native_tests)

set_target_properties(sticker_worker sticker_native_tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

enable_testing()
add_test(NAME sticker_native_tests COMMAND sticker_native_tests)
