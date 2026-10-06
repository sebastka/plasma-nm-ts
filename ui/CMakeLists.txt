# plasma-nm VPN UI plugin. plasma-nm installs libplasmanm_editor but not its
# headers, so this builds against the plasma-nm source tree of the exact
# installed version: the plugin uses plasma-nm's private ABI.

set(PLASMA_NM_SOURCE_DIR "" CACHE PATH "plasma-nm source tree matching the installed plasma-nm")
if(NOT EXISTS "${PLASMA_NM_SOURCE_DIR}/libs/editor/vpnuiplugin.h")
    message(FATAL_ERROR "Set PLASMA_NM_SOURCE_DIR to the plasma-nm source of the installed version "
                        "(see scripts/fetch-plasma-nm-source.sh), or configure with -DBUILD_PLASMA_NM_PLUGIN=OFF")
endif()

find_package(ECM 6.0 REQUIRED NO_MODULE)
list(APPEND CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.6 REQUIRED COMPONENTS Core DBus Network Widgets)
find_package(KF6 6.0 REQUIRED COMPONENTS CoreAddons I18n NetworkManagerQt WidgetsAddons)
pkg_check_modules(LIBNM REQUIRED IMPORTED_TARGET libnm)
find_library(PLASMANM_EDITOR_LIBRARY plasmanm_editor REQUIRED)

add_library(plasmanetworkmanagement_tailscaleui MODULE)
target_sources(plasmanetworkmanagement_tailscaleui PRIVATE
    tailscaleui.cpp
    tailscaleui.h
    tailscalewidget.cpp
    tailscalewidget.h
    tailscaleprofiles.cpp
    tailscaleprofiles.h
)
target_compile_definitions(plasmanetworkmanagement_tailscaleui PRIVATE
    TRANSLATION_DOMAIN="plasmanetworkmanagement_tailscaleui"
)
target_include_directories(plasmanetworkmanagement_tailscaleui PRIVATE
    ${PROJECT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/compat
    ${PLASMA_NM_SOURCE_DIR}/libs/editor
    ${PLASMA_NM_SOURCE_DIR}/libs/editor/widgets
)
target_link_libraries(plasmanetworkmanagement_tailscaleui PRIVATE
    ${PLASMANM_EDITOR_LIBRARY}
    PkgConfig::LIBNM
    Qt6::DBus
    Qt6::Network
    Qt6::Widgets
    KF6::CoreAddons
    KF6::I18n
    KF6::NetworkManagerQt
    KF6::WidgetsAddons
)

install(TARGETS plasmanetworkmanagement_tailscaleui DESTINATION ${KDE_INSTALL_PLUGINDIR}/plasma/network/vpn)
