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
