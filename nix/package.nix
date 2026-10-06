{
  lib,
  stdenv,
  cmake,
  pkg-config,
  glib,
  json-glib,
  libsoup_3,
  networkmanager,
}:

stdenv.mkDerivation {
  pname = "plasma-nm-ts";
  version = "0.1.0";

  src = lib.cleanSource ../.;

  nativeBuildInputs = [
    cmake
    pkg-config
  ];

  buildInputs = [
    glib
    json-glib
    libsoup_3
    networkmanager
  ];

  # Consumed by networking.networkmanager.plugins
  passthru.networkManagerPlugin = "VPN/nm-tailscale-service.name";

  meta = {
    description = "Tailscale integration for NetworkManager and the KDE Plasma network applet";
    homepage = "https://github.com/sebastka/plasma-nm-ts";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.linux;
  };
}
