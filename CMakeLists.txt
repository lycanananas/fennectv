cmake_minimum_required(VERSION 3.20)
# The version lives in VERSION, one line (packaging and releases read it too).
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FENNECTV_VERSION LIMIT_COUNT 1)
string(STRIP "${FENNECTV_VERSION}" FENNECTV_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
project(FennecTV VERSION ${FENNECTV_VERSION} LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FENNECTV_BUILD_TESTS "Build the tests" ON)
option(FENNECTV_BUILD_APPS "Build fennectv-tx and fennectv-rx (needs Qt 6)" ON)

function(fennectv_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# Code copied from Ragtime (vendor/README.md).
add_library(ragtime_vendored STATIC
    vendor/ragtime/src/link/Data2GLink.cpp)
target_include_directories(ragtime_vendored PUBLIC vendor/ragtime/include)

# libfennectv: what goes on the air and the burst budget. No Qt, no
# FFmpeg.
add_library(fennectv_core STATIC
    core/src/protocol.cpp
    core/src/budget.cpp)
target_include_directories(fennectv_core PUBLIC core/include)
target_link_libraries(fennectv_core PUBLIC ragtime_vendored)
fennectv_warnings(fennectv_core)

find_package(PkgConfig REQUIRED)

# The codecs, each straight through its library: AV1 with libaom, VVC
# with VVenC and VVdeC (AUR on Arch: vvenc, vvdec).
pkg_check_modules(AOM REQUIRED IMPORTED_TARGET aom)
pkg_check_modules(VVC REQUIRED IMPORTED_TARGET libvvenc libvvdec)
add_library(fennectv_codec STATIC codec/src/codec.cpp codec/src/av1.cpp codec/src/vvc.cpp)
target_include_directories(fennectv_codec PUBLIC codec/include)
target_link_libraries(fennectv_codec PUBLIC fennectv_core PRIVATE PkgConfig::AOM PkgConfig::VVC)
fennectv_warnings(fennectv_codec)

# Pictures in through the FFmpeg libraries: sources, scaling, frame rate,
# pixel format. Nothing else of FFmpeg is used.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavfilter libavdevice libavutil)
add_library(fennectv_media STATIC media/src/media.cpp)
target_include_directories(fennectv_media PUBLIC media/include)
target_link_libraries(fennectv_media PUBLIC fennectv_codec PRIVATE PkgConfig::FFMPEG)
fennectv_warnings(fennectv_media)

if(FENNECTV_BUILD_APPS)
    find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)
    add_library(fennectv_link STATIC link/src/host_client.cpp)
    target_include_directories(fennectv_link PUBLIC link/include)
    target_link_libraries(fennectv_link PUBLIC fennectv_core Qt6::Core Qt6::Network)
    target_compile_definitions(fennectv_link PUBLIC QT_NO_KEYWORDS)
    fennectv_warnings(fennectv_link)

    # Windows: a manifest with UTF-8 as the code page (file names).
    set(_win_resources "")
    if(WIN32)
        enable_language(RC)
        set(_win_resources apps/common/windows/fennectv.rc)
    endif()

    add_executable(fennectv-tx apps/tx/main.cpp ${_win_resources})
    target_link_libraries(fennectv-tx PRIVATE fennectv_link fennectv_media)
    fennectv_warnings(fennectv-tx)

    add_executable(fennectv-rx apps/rx/main.cpp ${_win_resources})
    target_link_libraries(fennectv-rx PRIVATE fennectv_link fennectv_codec Qt6::Widgets)
    fennectv_warnings(fennectv-rx)

    foreach(app fennectv-tx fennectv-rx)
        target_compile_definitions(${app} PRIVATE FENNECTV_VERSION="${PROJECT_VERSION}")
    endforeach()
    install(TARGETS fennectv-tx fennectv-rx RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/fennectv.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES packaging/linux/fennectv.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
endif()

if(FENNECTV_BUILD_TESTS)
    enable_testing()
    function(fennectv_test name label)
        add_executable(${name} ${ARGN})
        target_link_libraries(${name} PRIVATE fennectv_core)
        target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/vendor/ragtime/tests)
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES LABELS ${label})
    endfunction()

    fennectv_test(vendored_data2g_link_test unit vendor/ragtime/tests/Data2GLinkTest.cpp)
    fennectv_test(fennectv_protocol_test unit tests/unit/ProtocolTest.cpp)
    fennectv_test(fennectv_budget_test unit tests/unit/BudgetTest.cpp)

    # The test film (not in the repository): FENNECTV_TEST_VIDEO, or the
    # first .mp4 in ../video beside this checkout.
    file(GLOB _videos "${PROJECT_SOURCE_DIR}/../video/*.mp4")
    set(_video_default "")
    if(_videos)
        list(SORT _videos)
        list(GET _videos 0 _video_default)
    endif()
    set(FENNECTV_TEST_VIDEO "${_video_default}" CACHE FILEPATH "Video for the media tests")
    add_executable(fennectv_media_test tests/media/MediaTest.cpp)
    target_link_libraries(fennectv_media_test PRIVATE fennectv_media fennectv_codec)
    target_include_directories(fennectv_media_test PRIVATE ${PROJECT_SOURCE_DIR}/vendor/ragtime/tests)
    add_test(NAME fennectv_media_test
             COMMAND fennectv_media_test "${FENNECTV_TEST_VIDEO}" 120)
    set_tests_properties(fennectv_media_test PROPERTIES LABELS media SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
