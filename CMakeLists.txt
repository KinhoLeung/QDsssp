cmake_minimum_required(VERSION 3.16)
project(QDsssp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
include(GNUInstallDirs)
find_package(Qt5 5.15 REQUIRED COMPONENTS Widgets)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
option(QDSSSP_BUILD_TESTS "Build numerical and interaction tests" ON)
option(QDSSSP_BUILD_DEMO "Build the standalone demo application" ON)
option(QDSSSP_ENABLE_PIPEWIRE "Build the PipeWire audio-to-spectrum source when available" ON)

set(QDSSSP_HAS_PIPEWIRE OFF)
if(QDSSSP_ENABLE_PIPEWIRE)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    endif()
    if(TARGET PkgConfig::PIPEWIRE)
        set(QDSSSP_HAS_PIPEWIRE ON)
    else()
        find_path(QDSSSP_PIPEWIRE_INCLUDE_DIR pipewire/pipewire.h
            PATHS "${CMAKE_SYSROOT}/usr/include/pipewire-0.3")
        find_path(QDSSSP_SPA_INCLUDE_DIR spa/param/audio/format-utils.h
            PATHS "${CMAKE_SYSROOT}/usr/include/spa-0.2")
        find_library(QDSSSP_PIPEWIRE_LIBRARY NAMES pipewire-0.3
            PATHS "${CMAKE_SYSROOT}/usr/lib" "${CMAKE_SYSROOT}/lib")
        if(QDSSSP_PIPEWIRE_INCLUDE_DIR AND QDSSSP_SPA_INCLUDE_DIR AND QDSSSP_PIPEWIRE_LIBRARY)
            set(QDSSSP_HAS_PIPEWIRE ON)
        endif()
    endif()
endif()

add_library(qdsssp STATIC
    src/eq_response.cpp src/spectrum.cpp src/audio_spectrum.cpp src/spectrum_fft.h src/eq_widget.cpp src/response_path.h src/overlay_region.h src/filter_icon.cpp assets/dsssp.qrc
    include/qdsssp/eq_response.h include/qdsssp/spectrum.h include/qdsssp/audio_spectrum.h
    include/qdsssp/eq_widget.h include/qdsssp/filter_icon.h)
target_include_directories(qdsssp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(qdsssp PUBLIC Qt5::Widgets Threads::Threads)
target_compile_options(qdsssp PRIVATE -Wall -Wextra -Wpedantic)
if(QDSSSP_HAS_PIPEWIRE)
    target_compile_definitions(qdsssp PRIVATE QDSSSP_HAS_PIPEWIRE=1)
    if(TARGET PkgConfig::PIPEWIRE)
        target_link_libraries(qdsssp PUBLIC PkgConfig::PIPEWIRE)
    else()
        target_include_directories(qdsssp SYSTEM PRIVATE ${QDSSSP_PIPEWIRE_INCLUDE_DIR} ${QDSSSP_SPA_INCLUDE_DIR})
        target_link_libraries(qdsssp PUBLIC ${QDSSSP_PIPEWIRE_LIBRARY})
    endif()
endif()
install(TARGETS qdsssp
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(QDSSSP_BUILD_DEMO)
    add_executable(qdsssp_demo demo/main.cpp)
    target_link_libraries(qdsssp_demo PRIVATE qdsssp)

    # Qt Creator derives the remote executable path from this install rule.
    # Keep this as a string so callers can use either the board's absolute
    # path or a prefix-relative path in a normal host installation.
    set(QDSSSP_DEPLOY_DIR "/opt/qdsssp/bin" CACHE STRING "Demo executable directory on the device")
    install(TARGETS qdsssp_demo RUNTIME DESTINATION "${QDSSSP_DEPLOY_DIR}" COMPONENT Runtime)
    install(FILES assets/DOGFIGHT.pcm DESTINATION "${QDSSSP_DEPLOY_DIR}/../assets" COMPONENT Runtime)
endif()

if(QDSSSP_BUILD_TESTS)
    enable_testing()
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    add_executable(qdsssp_tests tests/test_qdsssp.cpp)
    target_link_libraries(qdsssp_tests PRIVATE qdsssp Qt5::Test)
    add_test(NAME qdsssp_tests COMMAND qdsssp_tests -platform offscreen)
endif()
