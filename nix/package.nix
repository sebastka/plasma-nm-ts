# Call with kdePackages.callPackage: the plasma-nm plugin must be built against
# the same plasma-nm as the one installed (private ABI).
{
  lib,
  stdenv,
  srcOnly,
  cmake,
  pkg-config,
  extra-cmake-modules,
  glib,
  json-glib,
  libsoup_3,
  networkmanager,
  qtbase,
  kcoreaddons,
  ki18n,
  kwidgetsaddons,
  networkmanager-qt,
  plasma-nm,
}:

stdenv.mkDerivation {
  pname = "plasma-nm-ts";
  version = "0.1.2";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [
    cmake
    pkg-config
    extra-cmake-modules
  ];

  buildInputs = [
    glib
    json-glib
    libsoup_3
    networkmanager
    qtbase
    kcoreaddons
    ki18n
    kwidgetsaddons
    networkmanager-qt
    plasma-nm
  ];

  cmakeFlags = [
    (lib.cmakeFeature "PLASMA_NM_SOURCE_DIR" "${srcOnly plasma-nm}")
    (lib.cmakeFeature "KDE_INSTALL_PLUGINDIR" "${placeholder "out"}/${qtbase.qtPluginPrefix}")
  ];

  # Only a Qt plugin, no executables to wrap
  dontWrapQtApps = true;

  # Consumed by networking.networkmanager.plugins
  passthru.networkManagerPlugin = "VPN/nm-tailscale-service.name";

  meta = {
    description = "Tailscale integration for NetworkManager and the KDE Plasma network applet";
    homepage = "https://github.com/sebastka/plasma-nm-ts";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
  };
}
