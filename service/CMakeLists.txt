pkg_check_modules(SERVICE_DEPS REQUIRED IMPORTED_TARGET
    libnm>=1.40
    libsoup-3.0
    json-glib-1.0
    gio-unix-2.0
)

add_executable(nm-tailscale-service nm-tailscale-service.c)
target_include_directories(nm-tailscale-service PRIVATE ${PROJECT_SOURCE_DIR}/common)
target_link_libraries(nm-tailscale-service PRIVATE PkgConfig::SERVICE_DEPS)
target_compile_options(nm-tailscale-service PRIVATE -Wall -Wextra -Wno-unused-parameter)
install(TARGETS nm-tailscale-service DESTINATION ${CMAKE_INSTALL_LIBEXECDIR})

# NetworkManager looks for VPN plugins in <prefix>/lib/NetworkManager/VPN, never lib64
configure_file(nm-tailscale-service.name.in nm-tailscale-service.name @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/nm-tailscale-service.name DESTINATION lib/NetworkManager/VPN)
install(FILES nm-tailscale-service.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/system.d)
