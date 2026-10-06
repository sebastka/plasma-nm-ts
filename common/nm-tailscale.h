/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Shared between the NetworkManager service plugin and the plasma-nm UI plugin.
 */

#ifndef NM_TAILSCALE_H
#define NM_TAILSCALE_H

#define NM_DBUS_SERVICE_TAILSCALE "org.freedesktop.NetworkManager.tailscale"

/* vpn.data keys */
#define NM_TAILSCALE_KEY_PROFILE   "profile"   /* profile ID, name, login name or tailnet */
#define NM_TAILSCALE_KEY_INTERFACE "interface" /* defaults to NM_TAILSCALE_DEFAULT_INTERFACE */

#define NM_TAILSCALE_DEFAULT_SOCKET    "/var/run/tailscale/tailscaled.sock"
#define NM_TAILSCALE_DEFAULT_INTERFACE "tailscale0"
#define NM_TAILSCALE_SOCKET_ENV        "TAILSCALED_SOCKET"

#endif /* NM_TAILSCALE_H */
