# Defines miniz as a static library via FetchContent. Idempotent.
#
# miniz is a single-file-style C zlib/ZIP implementation: the trainers feature
# needs a ZIP reader with no extra runtime deps, and this keeps the DLL free of
# a second copy of zlib (protobuf already carries its own).
#
# 3.0.2 predates the MINIZ_-prefixed options: it has no MINIZ_BUILD_TESTS, and
# it gates examples on BUILD_EXAMPLES, which defaults off because as a
# FetchContent subproject it is not MINIZ_STANDALONE_PROJECT. The MINIZ_-
# prefixed names are still pinned so a newer tag keeps honouring them.
if(TARGET miniz)
    return()
endif()

include(FetchCache)
include(FetchContent)

set(MINIZ_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(MINIZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(miniz)
