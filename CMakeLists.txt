cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Version.cmake)

project(AetherTFTP
    VERSION ${AETHER_VERSION}
    DESCRIPTION "Cross-platform TFTP GUI & CLI"
    LANGUAGES CXX)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.h.in
    ${CMAKE_BINARY_DIR}/generated/aether/version.h
    @ONLY)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_COVERAGE "Enable coverage instrumentation" OFF)
if(BUILD_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(--coverage)
        add_link_options(--coverage)
    endif()
endif()

# Sanitizer instrumentation. Accepts: address, undefined, address+undefined,
# or thread. Applied to all targets, so configure must run before any target.
set(AETHER_SANITIZER "" CACHE STRING
    "Sanitizer(s) to enable: address, undefined, address+undefined, thread")
if(AETHER_SANITIZER)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
       CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        string(REPLACE "+" "," _aether_san "${AETHER_SANITIZER}")
        add_compile_options(-fsanitize=${_aether_san} -fno-omit-frame-pointer -g)
        add_link_options(-fsanitize=${_aether_san})
        message(STATUS "AetherTFTP sanitizer enabled: ${_aether_san}")
    else()
        message(WARNING "AETHER_SANITIZER ignored: unsupported compiler")
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

# ---------------------------------------------------------------------------
# Optional: native systemd journal integration (Linux only)
# Enables sd_journal_send() structured log entries and sd_notify() readiness
# signalling.  Requires libsystemd-dev on the build host.  Set
# -DAETHER_USE_SYSTEMD=OFF to disable even when the library is present.
# ---------------------------------------------------------------------------
option(AETHER_USE_SYSTEMD "Use libsystemd for sd_journal / sd_notify" ON)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND AETHER_USE_SYSTEMD)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(SYSTEMD QUIET IMPORTED_TARGET libsystemd)
    endif()
    if(TARGET PkgConfig::SYSTEMD)
        message(STATUS "AetherTFTP: libsystemd found — enabling journald integration")
    else()
        message(STATUS "AetherTFTP: libsystemd not found — falling back to syslog")
    endif()
endif()

# Prebuilt Qt 6 for macOS can emit a spurious link to Apple's long-removed AGL
# framework (pulled in transitively via Qt6::Gui / WrapOpenGL), which breaks
# linking on current macOS SDKs with: "ld: framework 'AGL' not found". We never
# use AGL, so handle it two ways:
#   1. Best-effort: rewrite any reachable AGL reference in Qt's imported targets
#      to the still-present OpenGL framework.
#   2. Guaranteed: provide a stub AGL.framework on the framework search path so
#      the stray "-framework AGL" directive links regardless of its origin. The
#      stub's install_name points at the real (still-present) OpenGL framework,
#      so the recorded runtime dependency resolves on any macOS — no dangling
#      @rpath load command. We reference no AGL symbols, so the empty stub is
#      sufficient.
if(APPLE)
    foreach(_qt_target Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network WrapOpenGL::WrapOpenGL Qt6::OpenGL)
        if(TARGET ${_qt_target})
            get_target_property(_qt_link_libs ${_qt_target} INTERFACE_LINK_LIBRARIES)
            if(_qt_link_libs)
                list(TRANSFORM _qt_link_libs REPLACE "^AGL$" "-framework OpenGL")
                list(TRANSFORM _qt_link_libs REPLACE "^.*[/ ]AGL(\\.framework)?$" "-framework OpenGL")
                set_target_properties(${_qt_target} PROPERTIES INTERFACE_LINK_LIBRARIES "${_qt_link_libs}")
            endif()
        endif()
    endforeach()

    set(_aether_compat_fw "${CMAKE_BINARY_DIR}/compat-frameworks")
    set(_aether_opengl_id "/System/Library/Frameworks/OpenGL.framework/Versions/A/OpenGL")
    if(NOT EXISTS "${_aether_compat_fw}/AGL.framework/AGL")
        file(MAKE_DIRECTORY "${_aether_compat_fw}/AGL.framework")
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -dynamiclib -x c /dev/null
                    -install_name "${_aether_opengl_id}"
                    -o "${_aether_compat_fw}/AGL.framework/AGL"
            RESULT_VARIABLE _aether_agl_rc OUTPUT_QUIET ERROR_QUIET)
    endif()
    if(EXISTS "${_aether_compat_fw}/AGL.framework/AGL")
        add_link_options("-F${_aether_compat_fw}")
        message(STATUS "AetherTFTP: AGL link-compat stub at ${_aether_compat_fw}")
    endif()
endif()

add_library(aether_core STATIC
    src/core/tftp_protocol.cpp
    src/core/tftp_protocol.h
    src/core/tftp_session.cpp
    src/core/tftp_session.h
    src/core/tftp_server.cpp
    src/core/tftp_server.h
    src/core/pxe_proxy_dhcp.cpp
    src/core/pxe_proxy_dhcp.h
    src/core/metrics_exporter.cpp
    src/core/metrics_exporter.h
    src/core/tftp_client.cpp
    src/core/tftp_client.h
    src/core/qlog.cpp
    src/core/qlog.h
)
target_link_libraries(aether_core PUBLIC Qt6::Core Qt6::Network)
target_include_directories(aether_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated)
if(TARGET PkgConfig::SYSTEMD)
    target_compile_definitions(aether_core PUBLIC AETHER_HAVE_SYSTEMD)
    target_link_libraries(aether_core PUBLIC PkgConfig::SYSTEMD)
endif()

add_library(aether_cli STATIC
    src/cli/cli_runner.cpp
    src/cli/cli_runner.h
)
target_link_libraries(aether_cli PUBLIC aether_core Qt6::Core)

set(CMAKE_AUTORCC ON)
add_library(aether_gui STATIC
    src/gui/transfer_model.cpp
    src/gui/transfer_model.h
    src/gui/theme_controller.cpp
    src/gui/theme_controller.h
    src/gui/mainwindow.cpp
    src/gui/mainwindow.h
    src/gui/speed_chart_widget.cpp
    src/gui/speed_chart_widget.h
    src/gui/resources.qrc
)
target_link_libraries(aether_gui PUBLIC aether_core Qt6::Core Qt6::Widgets)

if(APPLE)
    add_executable(aethertftp MACOSX_BUNDLE src/main.cpp)
else()
    add_executable(aethertftp src/main.cpp)
endif()
target_link_libraries(aethertftp PRIVATE aether_cli aether_gui)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    add_executable(tftp_test tests/tftp_test.cpp)
    target_link_libraries(tftp_test PRIVATE aether_core aether_cli Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME tftp_test COMMAND tftp_test)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DevTools.cmake)

include(GNUInstallDirs)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/dist/aethertftp.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/aethertftp.service
        @ONLY
    )

    install(TARGETS aether_core aether_cli aethertftp
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY src/core/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aether/core
        FILES_MATCHING PATTERN "*.h"
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/aethertftp.service
        DESTINATION /lib/systemd/system
    )
elseif(WIN32)
    install(TARGETS aethertftp RUNTIME DESTINATION bin)

    # Locate windeployqt
    if(TARGET Qt6::windeployqt)
        get_target_property(WINDEPLOYQT_EXECUTABLE Qt6::windeployqt IMPORTED_LOCATION)
    else()
        find_program(WINDEPLOYQT_EXECUTABLE windeployqt
            HINTS "${QT6_INSTALL_PREFIX}/bin" "${Qt6_DIR}/../../../bin"
        )
    endif()

    install(CODE "
        execute_process(COMMAND \"${WINDEPLOYQT_EXECUTABLE}\" \"\${CMAKE_INSTALL_PREFIX}/bin/aethertftp.exe\")
    ")
endif()

set(CPACK_PACKAGE_NAME "AetherTFTP")
set(CPACK_PACKAGE_VENDOR "AetherTFTP Project")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_VERSION "${AETHER_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modern lightweight TFTP client and server")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt")

# Debian packaging details
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "AetherTFTP Maintainers <beratatmaca@live.com>")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# WiX Windows Installer details
set(CPACK_WIX_UPGRADE_GUID "D9C5F2A8-A29F-4D2A-B9C0-8B94F3C8F3A2")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico")

include(CPack)
