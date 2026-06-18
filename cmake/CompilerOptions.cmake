# Two INTERFACE "carrier" targets the rest of the build links against:
#
#   strata_warnings   -> warnings-as-errors, applied ONLY to our own code so
#                        third-party (FetchContent) sources are not held to it.
#   strata_sanitizers -> ASan+UBSan or TSan flags, selected by STRATA_SANITIZER.
#                        Propagated PUBLICly from strata_core so every consumer
#                        (tests, bench, cli) is instrumented and linked the same.

# --- Warnings ---------------------------------------------------------------
add_library(strata_warnings INTERFACE)
target_compile_options(strata_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wnull-dereference
  -Wdouble-promotion)

# --- Sanitizers -------------------------------------------------------------
set(STRATA_SANITIZER "OFF" CACHE STRING "Sanitizer set: OFF | ASAN_UBSAN | TSAN")
set_property(CACHE STRATA_SANITIZER PROPERTY STRINGS OFF ASAN_UBSAN TSAN)

add_library(strata_sanitizers INTERFACE)

set(_strata_san_flags "")
if(STRATA_SANITIZER STREQUAL "ASAN_UBSAN")
  set(_strata_san_flags
      -fsanitize=address,undefined
      -fno-omit-frame-pointer
      -fno-sanitize-recover=all) # turn UBSan "warnings" into hard failures.
elseif(STRATA_SANITIZER STREQUAL "TSAN")
  set(_strata_san_flags
      -fsanitize=thread
      -fno-omit-frame-pointer)
elseif(NOT STRATA_SANITIZER STREQUAL "OFF")
  message(FATAL_ERROR "STRATA_SANITIZER must be OFF, ASAN_UBSAN, or TSAN (got '${STRATA_SANITIZER}')")
endif()

if(_strata_san_flags)
  target_compile_options(strata_sanitizers INTERFACE ${_strata_san_flags})
  target_link_options(strata_sanitizers INTERFACE ${_strata_san_flags})
endif()
