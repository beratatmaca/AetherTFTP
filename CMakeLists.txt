cmake_minimum_required(VERSION 3.16)

project(AetherTFTP
    VERSION 0.1.0
    DESCRIPTION "Cross-platform TFTP GUI & CLI"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Network Widgets)

add_library(aether_core STATIC
    src/core/tftp_protocol.cpp
    src/core/tftp_protocol.h
    src/core/tftp_session.cpp
    src/core/tftp_session.h
    src/core/tftp_server.cpp
    src/core/tftp_server.h
    src/core/metrics_exporter.cpp
    src/core/metrics_exporter.h
    src/core/tftp_client.cpp
    src/core/tftp_client.h
)
target_link_libraries(aether_core PUBLIC Qt6::Core Qt6::Network)
target_include_directories(aether_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(aether_cli STATIC
    src/cli/cli_runner.cpp
    src/cli/cli_runner.h
)
target_link_libraries(aether_cli PUBLIC aether_core Qt6::Core)

set(CMAKE_AUTORCC ON)
add_library(aether_gui STATIC
    src/gui/transfer_model.cpp
    src/gui/transfer_model.h
    src/gui/server_config_dialog.cpp
    src/gui/server_config_dialog.h
    src/gui/mainwindow.cpp
    src/gui/mainwindow.h
    src/gui/resources.qrc
)
target_link_libraries(aether_gui PUBLIC aether_core Qt6::Core Qt6::Widgets)

add_executable(aethertftp src/main.cpp)
target_link_libraries(aethertftp PRIVATE aether_cli aether_gui)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    add_executable(tftp_test tests/tftp_test.cpp)
    target_link_libraries(tftp_test PRIVATE aether_core Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME tftp_test COMMAND tftp_test)
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/DevTools.cmake)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(GNUInstallDirs)
    install(TARGETS aether_core aether_cli aethertftp
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
    install(DIRECTORY src/core/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aether/core
        FILES_MATCHING PATTERN "*.h"
    )
    install(FILES dist/aethertftp.service
        DESTINATION /lib/systemd/system
    )
endif()

set(CPACK_PACKAGE_NAME "AetherTFTP")
set(CPACK_PACKAGE_VENDOR "AetherTFTP Project")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Modern lightweight TFTP client and server")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

# Debian packaging details
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "AetherTFTP Maintainers <maintainers@aethertftp.example>")
set(CPACK_DEBIAN_PACKAGE_SECTION "net")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libqt6core6, libqt6network6, libqt6widgets6, libqt6gui6")

# WiX Windows Installer details
set(CPACK_WIX_UPGRADE_GUID "D9C5F2A8-A29F-4D2A-B9C0-8B94F3C8F3A2")
set(CPACK_WIX_PRODUCT_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.ico") # placeholder or fallback in wix if exists

include(CPack)


