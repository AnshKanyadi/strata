# Two INTERFACE "carrier" targets the rest of the build links against:
#
#   strata_warnings   -> warnings-as-errors, applied ONLY to our own code so
#                        third-party (FetchContent) sources are not held to it.
#   strata_sanitizers -> ASan+UBSan or TSan flags, selected by STRATA_SANITIZER.
#                        Propagated PUBLICly from strata_core so every consumer
#                        (tests, bench, cli) is instrumented and linked the same.

# --- Warnings ---------------------------------------------------------------
# Base set: held by ALL first-party code, tests included.
add_library(strata_warnings INTERFACE)
target_compile_options(strata_warnings INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Werror
  -Wshadow
  -Wnon-virtual-dtor
  -Woverloaded-virtual
  -Wnull-dereference
  -Wdouble-promotion)

# Strict numeric-conversion checking, applied to the ENGINE / hot-path code only
# (strata_core, strata CLI) per ADR 0002 — that is where a silent 64->32 narrow
# in a batch index is a real bug. Test code mixes int/size_t freely (loop bounds,
# literals) and is held to the base set above, not these.
add_library(strata_warnings_strict INTERFACE)
target_link_libraries(strata_warnings_strict INTERFACE strata_warnings)
target_compile_options(strata_warnings_strict INTERFACE
  -Wconversion
  -Wsign-conversion)

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
