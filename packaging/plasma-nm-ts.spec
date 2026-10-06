# Fedora package for plasma-nm-ts, built by packaging/build-rpm.sh. The
# plasma-nm plugin uses plasma-nm's private ABI, so it is built against the
# source of the installed plasma-nm, passed as plasma_nm_source together with
# its version (plasma_nm_version).
%{!?plasma_nm_version:%global plasma_nm_version 0}

Name:           plasma-nm-ts
Version:        0.1.2
Release:        1%{?dist}
Summary:        Tailscale profiles as NetworkManager VPN connections in KDE Plasma
License:        GPL-2.0-or-later
URL:            https://github.com/sebastka/plasma-nm-ts
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.22
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  extra-cmake-modules
BuildRequires:  systemd-rpm-macros
BuildRequires:  pkgconfig(gio-unix-2.0)
BuildRequires:  pkgconfig(json-glib-1.0)
BuildRequires:  pkgconfig(libnm)
BuildRequires:  pkgconfig(libsoup-3.0)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  cmake(Qt6Widgets)
BuildRequires:  cmake(KF6CoreAddons)
BuildRequires:  cmake(KF6I18n)
BuildRequires:  cmake(KF6NetworkManagerQt)
BuildRequires:  cmake(KF6WidgetsAddons)
BuildRequires:  plasma-nm

Requires:       NetworkManager
Requires:       plasma-nm >= %{plasma_nm_version}
Recommends:     tailscale

%description
Each Tailscale profile (account) becomes a VPN connection in NetworkManager
and the KDE Plasma network applet. tailscaled keeps managing the tunnel,
routes and DNS; connecting switches tailscaled to the profile, and changes
made with the tailscale command are reflected in NetworkManager.

%prep
%autosetup

%build
# A shell check: rpm expands macros such as %%{error:} when parsing the spec,
# which dnf builddep does before plasma_nm_source can be known
if [ -z "%{?plasma_nm_source}" ]; then
    echo "define plasma_nm_source, see packaging/build-rpm.sh" >&2
    exit 1
fi
%cmake -DPLASMA_NM_SOURCE_DIR=%{?plasma_nm_source}
%cmake_build

%install
%cmake_install

%post
%systemd_post nm-tailscale-sync.service
# Not enabled through presets: Fedora's default preset disables unknown services
if [ $1 -eq 1 ]; then
    systemctl enable --now nm-tailscale-sync.service >/dev/null 2>&1 || :
fi

%preun
%systemd_preun nm-tailscale-sync.service

%postun
%systemd_postun_with_restart nm-tailscale-sync.service

%files
%license LICENSE
%doc README.md
%{_libexecdir}/nm-tailscale-service
%{_prefix}/lib/NetworkManager/VPN/nm-tailscale-service.name
%{_datadir}/dbus-1/system.d/nm-tailscale-service.conf
%{_unitdir}/nm-tailscale-sync.service
%{_qt6_plugindir}/plasma/network/vpn/plasmanetworkmanagement_tailscaleui.so

%changelog
* Wed Oct 07 2026 Sebastian Karlsen <sebastian@karlsen.fr> - 0.1.2-1
- Attestation bundle published with each release

* Wed Oct 07 2026 Sebastian Karlsen <sebastian@karlsen.fr> - 0.1.1-1
- Packages for Ubuntu LTS; build provenance attestations

* Wed Oct 07 2026 Sebastian Karlsen <sebastian@karlsen.fr> - 0.1.0-1
- First package
