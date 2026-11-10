# LZ4, the version Faster Archives builds with. Only the library sources, without
# LZ4's own CMake project, so none of its options reach the rest of the build.
include(FetchContent)
FetchContent_Declare(
    lz4
    GIT_REPOSITORY https://github.com/lz4/lz4.git
    GIT_TAG        v1.10.0
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  no-cmake-project-here
)
FetchContent_MakeAvailable(lz4)

enable_language(C)
add_library(ftx_lz4 STATIC
    "${lz4_SOURCE_DIR}/lib/lz4.c"
    "${lz4_SOURCE_DIR}/lib/lz4frame.c"
    "${lz4_SOURCE_DIR}/lib/lz4hc.c"
    "${lz4_SOURCE_DIR}/lib/xxhash.c"
)
target_include_directories(ftx_lz4 PUBLIC "${lz4_SOURCE_DIR}/lib")
# Direct memory access: safe on x86 and what Faster Archives measures with.
target_compile_definitions(ftx_lz4 PRIVATE LZ4_FORCE_MEMORY_ACCESS=2)
