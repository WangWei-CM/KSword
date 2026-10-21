cmake_minimum_required(VERSION 3.16)
project(KSwordQtADS LANGUAGES CXX)

# Build the pinned upstream checkout with KSword's existing runtime filenames.
set(KSWORD_ADS_SOURCE_DIR "" CACHE PATH "Qt ADS v5.1.1 source checkout")
if(NOT EXISTS "${KSWORD_ADS_SOURCE_DIR}/src/CMakeLists.txt")
    message(FATAL_ERROR "Set KSWORD_ADS_SOURCE_DIR to the Qt ADS v5.1.1 checkout")
endif()
# Require the tracked KSword same-area parent patch before producing runtime DLLs.
file(SHA256 "${KSWORD_ADS_SOURCE_DIR}/src/DockAreaWidget.cpp" KSWORD_ADS_AREA_SHA256)
if(NOT KSWORD_ADS_AREA_SHA256 STREQUAL "0bc3a915640433e055b511f02760e955b82149c526a5c62ef617b84bdf4bbd01")
    message(FATAL_ERROR "Apply patches/0001-keep-inactive-dock-parent.patch to the pinned ADS source first")
endif()
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
set(QT_VERSION_MAJOR 6)
if(MSVC)
    add_compile_options(/MP)
endif()
add_subdirectory("${KSWORD_ADS_SOURCE_DIR}" upstream)
set_target_properties(qtadvanceddocking-qt6 PROPERTIES
    OUTPUT_NAME qtadvanceddocking
    DEBUG_POSTFIX d)
