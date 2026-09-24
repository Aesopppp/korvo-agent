cmake_minimum_required(VERSION 3.16)

# ESP-IDF's bundled Git may not include patch.exe. Component discovery runs
# in a child CMake process, so make Git for Windows' tools available there too.
if(CMAKE_HOST_WIN32)
    find_program(KORVO_PATCH_EXECUTABLE NAMES patch.exe patch
        HINTS "$ENV{ProgramFiles}/Git/usr/bin"
              "$ENV{LOCALAPPDATA}/Programs/Git/usr/bin")
    if(KORVO_PATCH_EXECUTABLE)
        get_filename_component(_korvo_patch_dir "${KORVO_PATCH_EXECUTABLE}" DIRECTORY)
        set(ENV{PATH} "${_korvo_patch_dir};$ENV{PATH}")
    else()
        message(FATAL_ERROR "Install Git for Windows with patch.exe, or add its usr/bin directory to PATH.")
    endif()
endif()

include("$ENV{IDF_PATH}/tools/cmake/project.cmake")
set(COMPONENTS main)

project(korvo_agent)

# Xtensa GCC 14.2.0 ICEs in register allocation for this ESP-DSP 1.8.0
# image-convolution fallback at -O2. Keep performance optimization elsewhere.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_EQUAL "14.2.0")
    idf_component_get_property(korvo_dsp_lib espressif__esp-dsp COMPONENT_LIB)
    idf_component_get_property(korvo_dsp_dir espressif__esp-dsp COMPONENT_DIR)
    set_source_files_properties("${korvo_dsp_dir}/modules/conv/float/dspi_conv_f32_ansi.c"
        TARGET_DIRECTORY ${korvo_dsp_lib} PROPERTIES COMPILE_OPTIONS "-O1")
endif()
