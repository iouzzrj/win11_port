cmake_minimum_required(VERSION 3.20)

# Automatically target 32-bit x86 (Win32) when using Visual Studio generators
if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND NOT CMAKE_GENERATOR_PLATFORM)
    set(CMAKE_GENERATOR_PLATFORM "Win32" CACHE STRING "Target architecture (must be 32-bit x86)" FORCE)
endif()

project(GhostWin11
    VERSION 1.0.0
    DESCRIPTION "Original Xbox -> Windows 11 Native Compatibility Runtime (GhostWin11 / win11_port)"
    LANGUAGES CXX
)

# Enforce 32-bit x86 target (Ghost.exe is a 32-bit x86 Xbox image mapped at 0x00400000)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "GhostWin11 MUST be built as a 32-bit (x86 / Win32) executable.\n"
        "Please configure with: cmake -B build -A Win32 (or use --preset x86-release)."
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Generate C++ raw string literal include from pure shaders/ghost.hlsl (native CMake, zero Python dependency)
set(GHOST_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(GHOST_HLSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ghost.hlsl")
set(GHOST_HLSL_INC "${GHOST_GEN_DIR}/ghost_hlsl.inc")
set(GHOST_EMBED_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_hlsl.cmake")

add_custom_command(
    OUTPUT "${GHOST_HLSL_INC}"
    COMMAND "${CMAKE_COMMAND}"
        "-DSRC=${GHOST_HLSL_SRC}"
        "-DDST=${GHOST_HLSL_INC}"
        -P "${GHOST_EMBED_SCRIPT}"
    DEPENDS "${GHOST_HLSL_SRC}" "${GHOST_EMBED_SCRIPT}"
    COMMENT "Embedding shaders/ghost.hlsl into generated/ghost_hlsl.inc"
    VERBATIM
)

# Native C++ post-build PE verifier & LoadConfig patcher (uses <windows.h> PE structs, zero Python dependency)
add_executable(post_build_pe
    cmake/post_build_pe.cpp
)
target_compile_definitions(post_build_pe PRIVATE _CRT_SECURE_NO_WARNINGS)

# Define the GhostWin11 executable target
# NOTE: src/xbox_space.cpp MUST be the first source file so the linker places '.xbox'
# (0x00401000..0x00950FFF) as Section #1 before host '.text' (0x00951000+).
add_executable(GhostWin11 WIN32
    src/xbox_space.cpp
    src/main.cpp
    src/config.cpp
    src/launcher.cpp
    src/patch.cpp
    src/xbox_kernel.cpp
    src/xbox_xapi.cpp
    src/xbox_input.cpp
    src/xbox_video.cpp
    src/audio_adpcm.cpp
    src/audio_buffer.cpp
    src/audio_stream.cpp
    src/d3d11_backend.cpp
    src/d3d8_texture.cpp
    src/d3d8_shader.cpp
    src/d3d8_draw.cpp
    src/ghost_bot.cpp
    include/ghost_runtime.h
    include/ghost_addresses.h
    include/xbox_d3d8_types.h
    include/ghost_bot.h
    shaders/ghost.hlsl
    "${GHOST_HLSL_INC}"
)

set_source_files_properties(shaders/ghost.hlsl PROPERTIES HEADER_FILE_ONLY TRUE)

target_include_directories(GhostWin11 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${GHOST_GEN_DIR}"
)

target_compile_definitions(GhostWin11 PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    GHOST_PROJECT_NAME=${PROJECT_NAME}
)

if(MSVC)
    target_compile_options(GhostWin11 PRIVATE
        /utf-8
        /Gy
        /Zi
        /EHsc
        /W3
    )

    set(GHOST_MAP_FILE "${CMAKE_CURRENT_BINARY_DIR}/GhostWin11.map")
    set(GHOST_PDB_FILE "${CMAKE_CURRENT_BINARY_DIR}/GhostWin11.pdb")

    target_link_options(GhostWin11 PRIVATE
        /SUBSYSTEM:WINDOWS
        /INCREMENTAL:NO
        /SAFESEH:NO
        /BASE:0x00400000
        /DYNAMICBASE:NO
        /FIXED
        /LARGEADDRESSAWARE:NO
        /OPT:NOREF
        /OPT:NOICF
        /DEBUG
        "/PDB:${GHOST_PDB_FILE}"
        "/MAP:${GHOST_MAP_FILE}"
    )
endif()

target_link_libraries(GhostWin11 PRIVATE
    user32
    gdi32
    ole32
    d3d11
    dxgi
    d3dcompiler
    dsound
    winmm
    windowscodecs
)

# Output GhostWin11.exe into build/bin/GhostWin11.exe
set(GHOST_BIN_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
set_target_properties(GhostWin11 PROPERTIES
    OUTPUT_NAME "GhostWin11"
    RUNTIME_OUTPUT_DIRECTORY "${GHOST_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${GHOST_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${GHOST_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${GHOST_BIN_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${GHOST_BIN_DIR}"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    PDB_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_BINARY_DIR}"
    PDB_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_CURRENT_BINARY_DIR}"
    PDB_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_CURRENT_BINARY_DIR}"
    PDB_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_BINARY_DIR}"
)

# Post-build verification & PE LoadConfig directory clearing
add_dependencies(GhostWin11 post_build_pe)
add_custom_command(TARGET GhostWin11 POST_BUILD
    COMMAND "$<TARGET_FILE:post_build_pe>" "$<TARGET_FILE:GhostWin11>"
    COMMENT "Verifying .xbox section layout and clearing PE LoadConfig directory"
    VERBATIM
)
