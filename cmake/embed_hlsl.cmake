# Pure CMake script to wrap shaders/ghost.hlsl into a C++ raw string literal (.inc)
if(NOT DEFINED SRC OR NOT DEFINED DST)
    message(FATAL_ERROR "Usage: cmake -DSRC=<input.hlsl> -DDST=<output.inc> -P embed_hlsl.cmake")
endif()

get_filename_component(DST_DIR "${DST}" DIRECTORY)
file(MAKE_DIRECTORY "${DST_DIR}")
file(READ "${SRC}" HLSL_CONTENT)
file(WRITE "${DST}" "R\"GHOST_HLSL(\n${HLSL_CONTENT})GHOST_HLSL\"\n")
