/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * NetworkManager VPN service plugin for Tailscale.
 *
 * tailscaled owns the tunnel, routing and DNS. This plugin only drives it
 * through the LocalAPI (switch profile, set WantRunning) and reports the
 * resulting tailscale0 addresses to NetworkManager, so that each Tailscale
 * profile can be represented as a NetworkManager VPN connection.
 */

#include <NetworkManager.h>
#include <arpa/inet.h>
#include <gio/gunixsocketaddress.h>
#include <ifaddrs.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "nm-tailscale.h"

#define KEY_PROFILE       NM_TAILSCALE_KEY_PROFILE
#define KEY_INTERFACE     NM_TAILSCALE_KEY_INTERFACE
#define DEFAULT_SOCKET    NM_TAILSCALE_DEFAULT_SOCKET
#define DEFAULT_INTERFACE NM_TAILSCALE_DEFAULT_INTERFACE
#define LOCALAPI_URL      "http://local-tailscaled.sock/localapi/v0/"

#define POLL_INTERVAL_MS   500
#define RUNNING_TIMEOUT_S  30

/*****************************************************************************/
/* LocalAPI */

static SoupSession *
localapi_session_new(void)
{
    const char *path = g_getenv(NM_TAILSCALE_SOCKET_ENV);
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(path ? path : DEFAULT_SOCKET);

    return soup_session_new_with_options("remote-connectable", addr, "timeout", 15, NULL);
}

/* Returns the decoded JSON body (a JSON null node for empty bodies). */
static JsonNode *
localapi(SoupSession *session, const char *method, const char *path, const char *body, GError **error)
{
    g_autofree char *uri = g_strconcat(LOCALAPI_URL, path, NULL);
    g_autoptr(SoupMessage) msg = soup_message_new(method, uri);
    g_autoptr(GBytes) resp = NULL;
    g_autoptr(JsonParser) parser = NULL;
    const char *data;
    gsize len;
    guint status;

    if (!msg) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS, "invalid LocalAPI path '%s'", path);
        return NULL;
    }
    if (body) {
        g_autoptr(GBytes) b = g_bytes_new(body, strlen(body));
        soup_message_set_request_body_from_bytes(msg, "application/json", b);
    }

    resp = soup_session_send_and_read(session, msg, NULL, error);
    if (!resp)
        return NULL;

    data = g_bytes_get_data(resp, &len);
    status = soup_message_get_status(msg);
    if (!SOUP_STATUS_IS_SUCCESSFUL(status)) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED, "tailscaled: %s %s: HTTP %u: %.*s",
                    method, path, status, (int) len, data ? data : "");
        return NULL;
    }
    if (len == 0)
        return json_node_new(JSON_NODE_NULL);

    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, data, (gssize) len, error))
        return NULL;
    return json_node_copy(json_parser_get_root(parser));
}

static JsonObject *
json_obj(JsonNode *node)
{
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static JsonObject *
json_member_obj(JsonObject *o, const char *key)
{
    return o && json_object_has_member(o, key) ? json_obj(json_object_get_member(o, key)) : NULL;
}

static const char *
json_member_str(JsonObject *o, const char *key)
{
    JsonNode *n = o && json_object_has_member(o, key) ? json_object_get_member(o, key) : NULL;

    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n)
                                                                                          : NULL;
}

/* Maps a user-facing profile selector to a LocalAPI profile ID. An exact ID
 * match wins; otherwise the selector must uniquely match a profile's name,
 * login name or tailnet. */
static char *
resolve_profile_id(SoupSession *session, const char *selector, GError **error)
{
    g_autoptr(JsonNode) root = localapi(session, "GET", "profiles/", NULL, error);
    const char *match = NULL;
    guint n_matches = 0;
    JsonArray *profiles;

    if (!root)
        return NULL;
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED, "tailscaled: unexpected profiles reply");
        return NULL;
    }

    profiles = json_node_get_array(root);
    for (guint i = 0; i < json_array_get_length(profiles); i++) {
        JsonObject *p = json_obj(json_array_get_element(profiles, i));
        const char *id = json_member_str(p, "ID");
        const char *names[] = {
            json_member_str(p, "Name"),
            json_member_str(json_member_obj(p, "UserProfile"), "LoginName"),
            json_member_str(json_member_obj(p, "NetworkProfile"), "DomainName"),
        };

        if (!id)
            continue;
        if (g_strcmp0(id, selector) == 0)
            return g_strdup(id);
        for (guint j = 0; j < G_N_ELEMENTS(names); j++) {
            if (names[j] && g_ascii_strcasecmp(names[j], selector) == 0) {
                if (g_strcmp0(match, id) != 0)
                    n_matches++;
                match = id;
                break;
            }
        }
    }

    if (n_matches == 1)
        return g_strdup(match);
    g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
                n_matches ? "Tailscale profile '%s' is ambiguous, use its ID (see 'tailscale switch --list')"
                          : "no Tailscale profile matches '%s' (see 'tailscale switch --list')",
                selector);
    return NULL;
}

static gboolean
json_member_bool(JsonObject *o, const char *key)
{
    JsonNode *n = o && json_object_has_member(o, key) ? json_object_get_member(o, key) : NULL;

    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN && json_node_get_boolean(n);
}

static char *
current_profile_id(SoupSession *session, GError **error)
{
    g_autoptr(JsonNode) current = localapi(session, "GET", "profiles/current", NULL, error);

    return current ? g_strdup(json_member_str(json_obj(current), "ID")) : NULL;
}

/* Switches to the profile (if needed) and asks tailscaled to come up. */
static gboolean
tailscale_up(SoupSession *session, const char *id, GError **error)
{
    GError *local = NULL;
    g_autofree char *current = current_profile_id(session, &local);
    g_autoptr(JsonNode) prefs = NULL;

    if (local) {
        g_propagate_error(error, local);
        return FALSE;
    }
    if (g_strcmp0(current, id) != 0) {
        g_autofree char *path = g_strconcat("profiles/", id, NULL);
        g_autoptr(JsonNode) r = NULL;

        g_message("switching to Tailscale profile %s", id);
        r = localapi(session, "POST", path, NULL, error);
        if (!r)
            return FALSE;
    }

    prefs = localapi(session, "PATCH", "prefs", "{\"WantRunningSet\":true,\"WantRunning\":true}", error);
    return prefs != NULL;
}

static gboolean
tailscale_down(SoupSession *session, GError **error)
{
    g_autoptr(JsonNode) prefs =
        localapi(session, "PATCH", "prefs", "{\"WantRunningSet\":true,\"WantRunning\":false}", error);

    return prefs != NULL;
}

/*****************************************************************************/
/* NetworkManager configuration */

static gboolean
iface_has_address(const char *iface, int family, const void *addr)
{
    struct ifaddrs *ifas, *ifa;
    gboolean found = FALSE;

    if (getifaddrs(&ifas) != 0)
        return FALSE;
    for (ifa = ifas; ifa && !found; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != family || g_strcmp0(ifa->ifa_name, iface) != 0)
            continue;
        if (family == AF_INET)
            found = memcmp(&((struct sockaddr_in *) ifa->ifa_addr)->sin_addr, addr, sizeof(struct in_addr)) == 0;
        else
            found = memcmp(&((struct sockaddr_in6 *) ifa->ifa_addr)->sin6_addr, addr, sizeof(struct in6_addr)) == 0;
    }
    freeifaddrs(ifas);
    return found;
}

/* Builds the configuration NetworkManager expects from a LocalAPI status
 * object.
 *
 * No IP configuration is reported (has-ip4 and has-ip6 are FALSE): tailscaled
 * owns tailscale0's addresses, routes and DNS. If NetworkManager applied the
 * addresses too, it would remove them on disconnect while tailscaled stops,
 * and tailscaled would react to that link change by re-applying its tailnet
 * DNS configuration after stopping, leaving resolv.conf pointing at it.
 *
 * NetworkManager rejects a config without an external gateway and adds a host
 * route to it via the parent device. Tailscale has no single gateway, so the
 * node's own tailnet address is reported: any such route is shadowed by the
 * local routing table and never used. */
static GVariant *
build_vpn_config(JsonObject *status, const char *iface, GError **error)
{
    JsonNode *ips_node = json_object_has_member(status, "TailscaleIPs")
                             ? json_object_get_member(status, "TailscaleIPs")
                             : NULL;
    JsonArray *ips = ips_node && JSON_NODE_HOLDS_ARRAY(ips_node) ? json_node_get_array(ips_node) : NULL;
    GVariant *gateway = NULL;
    GVariantBuilder config;

    if (if_nametoindex(iface) == 0) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
                    "interface %s not found (is tailscaled running in userspace-networking mode?)", iface);
        return NULL;
    }

    /* "Running" is not enough: if tailscaled failed to configure the interface
     * (its router state can get out of sync with the kernel), nothing works.
     * Prefer the IPv4 address as gateway. */
    for (guint i = 0; ips && i < json_array_get_length(ips); i++) {
        JsonNode *n = json_array_get_element(ips, i);
        const char *ip = JSON_NODE_HOLDS_VALUE(n) ? json_node_get_string(n) : NULL;
        struct in_addr a4;
        struct in6_addr a6;
        gboolean is4;

        if (!ip)
            continue;
        is4 = inet_pton(AF_INET, ip, &a4) == 1;
        if (!is4 && inet_pton(AF_INET6, ip, &a6) != 1)
            continue;

        if (!iface_has_address(iface, is4 ? AF_INET : AF_INET6, is4 ? (void *) &a4 : (void *) &a6)) {
            g_clear_pointer(&gateway, g_variant_unref);
            g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
                        "tailscaled reports %s but it is not assigned to %s (restarting tailscaled may help)", ip,
                        iface);
            return NULL;
        }

        if (is4 && (!gateway || g_variant_is_of_type(gateway, G_VARIANT_TYPE_BYTESTRING))) {
            g_clear_pointer(&gateway, g_variant_unref);
            gateway = g_variant_ref_sink(g_variant_new_uint32(a4.s_addr));
        } else if (!is4 && !gateway) {
            gateway = g_variant_ref_sink(g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &a6, sizeof(a6), 1));
        }
    }

    if (!gateway) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED, "tailscaled reports no Tailscale IPs");
        return NULL;
    }

    g_variant_builder_init(&config, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_TUNDEV, g_variant_new_string(iface));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_EXT_GATEWAY, gateway);
    g_variant_unref(gateway);
    /* tailscaled survives link changes on its own, so vpn.persistent is safe */
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CAN_PERSIST, g_variant_new_boolean(TRUE));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP4, g_variant_new_boolean(FALSE));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP6, g_variant_new_boolean(FALSE));
    return g_variant_ref_sink(g_variant_builder_end(&config));
}

/*****************************************************************************/
/* BusWatch: follows tailscaled's IPN bus and calls back, debounced, whenever
 * the state, profile or prefs may have changed. Bus messages are only used as
 * triggers: callers always decide from fresh LocalAPI state, so missed or
 * unknown messages cannot cause a wrong decision. While tailscaled is
 * unreachable, the callback still fires on every reconnection attempt. */

#define WATCH_DEBOUNCE_MS  300
#define WATCH_RETRY_MS     2000
#define WATCH_MAX_FAILURES 15 /* tailscaled unreachable for ~30s */

typedef void (*BusWatchFunc)(gpointer user_data);

typedef struct {
    SoupSession *stream; /* without timeout: the bus is idle for long */
    GCancellable *cancellable;
    GDataInputStream *lines;
    guint changed_id;
    guint reopen_id;
    BusWatchFunc changed;
    gpointer user_data;
} BusWatch;

static void bus_watch_open(BusWatch *w);

static void
bus_watch_close_stream(BusWatch *w)
{
    g_cancellable_cancel(w->cancellable);
    g_clear_object(&w->cancellable);
    g_clear_object(&w->lines);
}

static void
bus_watch_free(BusWatch *w)
{
    bus_watch_close_stream(w);
    g_clear_handle_id(&w->changed_id, g_source_remove);
    g_clear_handle_id(&w->reopen_id, g_source_remove);
    g_clear_object(&w->stream);
    g_free(w);
}

static gboolean
bus_watch_changed_cb(gpointer user_data)
{
    BusWatch *w = user_data;

    w->changed_id = 0;
    /* May free the watch: do not touch it afterwards */
    w->changed(w->user_data);
    return G_SOURCE_REMOVE;
}

static void
bus_watch_trigger(BusWatch *w, guint delay_ms)
{
    if (!w->changed_id)
        w->changed_id = g_timeout_add(delay_ms, bus_watch_changed_cb, w);
}

static gboolean
bus_watch_reopen_cb(gpointer user_data)
{
    BusWatch *w = user_data;

    w->reopen_id = 0;
    bus_watch_open(w);
    /* Things may have changed while the bus was closed */
    bus_watch_trigger(w, 0);
    return G_SOURCE_REMOVE;
}

static void
bus_watch_lost(BusWatch *w, const GError *error)
{
    g_debug("watch: IPN bus unavailable%s%s", error ? ": " : "", error ? error->message : "");
    bus_watch_close_stream(w);
    if (!w->reopen_id)
        w->reopen_id = g_timeout_add(WATCH_RETRY_MS, bus_watch_reopen_cb, w);
}

static void
bus_watch_line_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), result, NULL, &error);
    BusWatch *w = user_data;

    /* Cancelled means the watch may already be freed */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    if (!line) {
        /* tailscaled closed the stream (restart?) */
        bus_watch_lost(w, error);
        return;
    }

    /* State, profile and prefs changes all arrive as such messages */
    if (strstr(line, "\"State\"") || strstr(line, "\"Prefs\"") || strstr(line, "\"ErrMessage\""))
        bus_watch_trigger(w, WATCH_DEBOUNCE_MS);

    g_data_input_stream_read_line_async(w->lines, G_PRIORITY_DEFAULT, w->cancellable, bus_watch_line_cb, w);
}

static void
bus_watch_opened_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GInputStream) body = soup_session_send_finish(SOUP_SESSION(source), result, &error);
    BusWatch *w = user_data;

    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    if (body) {
        SoupMessage *msg = soup_session_get_async_result_message(SOUP_SESSION(source), result);

        if (!SOUP_STATUS_IS_SUCCESSFUL(soup_message_get_status(msg)))
            g_set_error(&error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED, "HTTP %u",
                        soup_message_get_status(msg));
    }
    if (error) {
        bus_watch_lost(w, error);
        return;
    }

    w->lines = g_data_input_stream_new(body);
    g_data_input_stream_read_line_async(w->lines, G_PRIORITY_DEFAULT, w->cancellable, bus_watch_line_cb, w);
}

static void
bus_watch_open(BusWatch *w)
{
    g_autoptr(SoupMessage) msg = soup_message_new("GET", LOCALAPI_URL "watch-ipn-bus?mask=0");

    bus_watch_close_stream(w);
    w->cancellable = g_cancellable_new();
    soup_session_send_async(w->stream, msg, G_PRIORITY_DEFAULT, w->cancellable, bus_watch_opened_cb, w);
}

/* Calls @changed once right away: something may have changed before the bus
 * was open. */
static BusWatch *
bus_watch_new(BusWatchFunc changed, gpointer user_data)
{
    const char *path = g_getenv(NM_TAILSCALE_SOCKET_ENV);
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(path ? path : DEFAULT_SOCKET);
    BusWatch *w = g_new0(BusWatch, 1);

    w->stream = soup_session_new_with_options("remote-connectable", addr, NULL);
    w->changed = changed;
    w->user_data = user_data;
    bus_watch_open(w);
    bus_watch_trigger(w, 0);
    return w;
}

/*****************************************************************************/
/* Whether a connected profile still reflects tailscaled's state. A connection
 * is released when another profile became active (another connection, or
 * 'tailscale switch'), Tailscale was stopped ('tailscale down'), the profile
 * needs to log in again, or tailscaled stays unreachable. */

typedef enum {
    PROFILE_CURRENT,
    PROFILE_UNKNOWN,          /* tailscaled unreachable, for now */
    PROFILE_RELEASE_EXPECTED, /* tailscaled moved on: a normal disconnect */
    PROFILE_RELEASE_LOGIN,
    PROFILE_RELEASE_FAILED,
} ProfileVerdict;

static ProfileVerdict
check_profile(SoupSession *api, const char *profile_id, guint *failures, const char **reason)
{
    g_autoptr(GError) error = NULL;
    g_autofree char *current = current_profile_id(api, &error);
    g_autoptr(JsonNode) prefs = NULL;
    g_autoptr(JsonNode) status = NULL;
    const char *state;

    if (!error)
        prefs = localapi(api, "GET", "prefs", NULL, &error);
    if (!error)
        status = localapi(api, "GET", "status?peers=false", NULL, &error);

    if (error) {
        g_debug("watch: %s", error->message);
        if (++*failures < WATCH_MAX_FAILURES)
            return PROFILE_UNKNOWN;
        *reason = "tailscaled is unreachable";
        return PROFILE_RELEASE_FAILED;
    }

    *failures = 0;
    state = json_member_str(json_obj(status), "BackendState");
    if (g_strcmp0(current, profile_id) != 0) {
        *reason = "another Tailscale profile became active";
        return PROFILE_RELEASE_EXPECTED;
    }
    if (!json_member_bool(json_obj(prefs), "WantRunning")) {
        *reason = "Tailscale was stopped";
        return PROFILE_RELEASE_EXPECTED;
    }
    if (g_strcmp0(state, "NeedsLogin") == 0 || g_strcmp0(state, "NeedsMachineAuth") == 0) {
        *reason = "the Tailscale profile needs to log in again";
        return PROFILE_RELEASE_LOGIN;
    }
    return PROFILE_CURRENT;
}

/*****************************************************************************/
/* Plugin */

typedef struct {
    NMVpnServicePlugin parent;

    SoupSession *session;
    char *interface;
    char *profile_id;
    char *uuid; /* of the NetworkManager connection */
    guint poll_id;
    gint64 deadline;
    BusWatch *watch;
    guint watch_failures;
    gboolean released; /* tailscaled moved on: disconnect must leave it alone */
} NMTailscalePlugin;

typedef struct {
    NMVpnServicePluginClass parent;
} NMTailscalePluginClass;

GType nm_tailscale_plugin_get_type(void);
G_DEFINE_TYPE(NMTailscalePlugin, nm_tailscale_plugin, NM_TYPE_VPN_SERVICE_PLUGIN)

#define NM_TAILSCALE_PLUGIN(o) (G_TYPE_CHECK_INSTANCE_CAST((o), nm_tailscale_plugin_get_type(), NMTailscalePlugin))

static GVariant *
nm_dbus_get_property(GDBusConnection *bus, const char *path, const char *iface, const char *name, GError **error)
{
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(bus, NM_DBUS_SERVICE, path, "org.freedesktop.DBus.Properties",
                                                            "Get", g_variant_new("(ss)", iface, name),
                                                            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                                                            error);
    GVariant *value = NULL;

    if (reply)
        g_variant_get(reply, "(v)", &value);
    return value;
}

/* Object path of the active connection with this UUID, or NULL. */
static char *
find_active_connection(GDBusConnection *bus, const char *uuid, GError **error)
{
    g_autoptr(GVariant) actives =
        nm_dbus_get_property(bus, NM_DBUS_PATH, NM_DBUS_INTERFACE, "ActiveConnections", error);
    GVariantIter iter;
    const char *path;

    if (!actives)
        return NULL;
    g_variant_iter_init(&iter, actives);
    while (g_variant_iter_next(&iter, "&o", &path)) {
        g_autoptr(GVariant) value =
            nm_dbus_get_property(bus, path, NM_DBUS_INTERFACE_ACTIVE_CONNECTION, "Uuid", NULL);

        if (value && g_strcmp0(g_variant_get_string(value, NULL), uuid) == 0)
            return g_strdup(path);
    }
    return NULL;
}

static void
deactivate_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    NMTailscalePlugin *self = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

    if (!reply) {
        g_warning("could not deactivate the connection through NetworkManager: %s", error->message);
        nm_vpn_service_plugin_disconnect(NM_VPN_SERVICE_PLUGIN(self), NULL);
    }
    g_object_unref(self);
}

/* Deactivates the connection as a user would, so that NetworkManager and
 * plasma-nm report a normal disconnection rather than a failure. Asynchronous:
 * NetworkManager calls back into this plugin's Disconnect before replying. */
static gboolean
deactivate_through_nm(NMTailscalePlugin *self, GError **error)
{
    g_autoptr(GDBusConnection) bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, error);
    g_autofree char *path = NULL;

    if (!bus)
        return FALSE;
    path = find_active_connection(bus, self->uuid, error);
    if (!path) {
        if (error && !*error)
            g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED, "active connection not found");
        return FALSE;
    }

    g_dbus_connection_call(bus, NM_DBUS_SERVICE, NM_DBUS_PATH, NM_DBUS_INTERFACE, "DeactivateConnection",
                           g_variant_new("(o)", path), NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, deactivate_cb,
                           g_object_ref(self));
    return TRUE;
}

static void
on_tailscale_changed(gpointer user_data)
{
    NMTailscalePlugin *self = user_data;
    NMVpnServicePlugin *plugin = NM_VPN_SERVICE_PLUGIN(self);
    g_autoptr(GError) error = NULL;
    const char *reason = NULL;
    ProfileVerdict verdict = check_profile(self->session, self->profile_id, &self->watch_failures, &reason);

    if (verdict == PROFILE_CURRENT || verdict == PROFILE_UNKNOWN)
        return;

    g_message("%s, disconnecting", reason);
    self->released = TRUE;
    g_clear_pointer(&self->watch, bus_watch_free);

    if (verdict == PROFILE_RELEASE_EXPECTED) {
        if (deactivate_through_nm(self, &error))
            return;
        g_warning("could not deactivate the connection through NetworkManager: %s", error->message);
    } else if (verdict == PROFILE_RELEASE_LOGIN) {
        nm_vpn_service_plugin_failure(plugin, NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED);
    }
    nm_vpn_service_plugin_disconnect(plugin, NULL);
}

static gboolean
poll_running(gpointer user_data)
{
    NMTailscalePlugin *self = user_data;
    NMVpnServicePlugin *plugin = NM_VPN_SERVICE_PLUGIN(self);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) status = localapi(self->session, "GET", "status?peers=false", NULL, &error);
    const char *state = json_member_str(json_obj(status), "BackendState");
    g_autoptr(GVariant) config = NULL;

    if (g_strcmp0(state, "Running") == 0
        && (config = build_vpn_config(json_obj(status), self->interface, &error))) {
        g_message("Tailscale is running, reporting %s to NetworkManager", self->interface);
        self->poll_id = 0;
        nm_vpn_service_plugin_set_config(plugin, config);
        self->watch_failures = 0;
        self->watch = bus_watch_new(on_tailscale_changed, self);
        return G_SOURCE_REMOVE;
    }

    if (g_strcmp0(state, "NeedsLogin") == 0 || g_strcmp0(state, "NeedsMachineAuth") == 0) {
        g_warning("Tailscale profile is in state %s, log in with 'tailscale login' first", state);
        self->poll_id = 0;
        nm_vpn_service_plugin_failure(plugin, NM_VPN_PLUGIN_FAILURE_LOGIN_FAILED);
        return G_SOURCE_REMOVE;
    }

    if (g_get_monotonic_time() > self->deadline) {
        g_warning("timed out waiting for Tailscale (state %s): %s", state ? state : "unknown",
                  error ? error->message : "not running");
        self->poll_id = 0;
        nm_vpn_service_plugin_failure(plugin, NM_VPN_PLUGIN_FAILURE_CONNECT_FAILED);
        return G_SOURCE_REMOVE;
    }

    if (error)
        g_debug("still waiting for Tailscale: %s", error->message);
    return G_SOURCE_CONTINUE;
}

static gboolean
real_connect(NMVpnServicePlugin *plugin, NMConnection *connection, GError **error)
{
    NMTailscalePlugin *self = NM_TAILSCALE_PLUGIN(plugin);
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);
    const char *profile = s_vpn ? nm_setting_vpn_get_data_item(s_vpn, KEY_PROFILE) : NULL;
    const char *iface = s_vpn ? nm_setting_vpn_get_data_item(s_vpn, KEY_INTERFACE) : NULL;

    if (!profile || !*profile) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_BAD_ARGUMENTS,
                    "missing '" KEY_PROFILE "' in vpn.data");
        return FALSE;
    }

    g_free(self->interface);
    self->interface = g_strdup(iface && *iface ? iface : DEFAULT_INTERFACE);
    self->released = FALSE;
    g_clear_pointer(&self->watch, bus_watch_free);
    g_free(self->uuid);
    self->uuid = g_strdup(nm_connection_get_uuid(connection));

    g_free(self->profile_id);
    self->profile_id = resolve_profile_id(self->session, profile, error);
    if (!self->profile_id)
        return FALSE;
    if (!tailscale_up(self->session, self->profile_id, error))
        return FALSE;

    g_clear_handle_id(&self->poll_id, g_source_remove);
    self->deadline = g_get_monotonic_time() + RUNNING_TIMEOUT_S * G_USEC_PER_SEC;
    self->poll_id = g_timeout_add(POLL_INTERVAL_MS, poll_running, self);
    return TRUE;
}

static gboolean
real_need_secrets(NMVpnServicePlugin *plugin, NMConnection *connection, const char **setting_name, GError **error)
{
    return FALSE;
}

static gboolean
real_disconnect(NMVpnServicePlugin *plugin, GError **error)
{
    NMTailscalePlugin *self = NM_TAILSCALE_PLUGIN(plugin);
    g_autofree char *current = NULL;
    GError *local = NULL;

    g_clear_handle_id(&self->poll_id, g_source_remove);
    g_clear_pointer(&self->watch, bus_watch_free);

    if (self->released || !self->profile_id)
        return TRUE;

    /* Never stop Tailscale on behalf of another connection's profile */
    current = current_profile_id(self->session, &local);
    if (local) {
        g_propagate_error(error, local);
        return FALSE;
    }
    if (g_strcmp0(current, self->profile_id) != 0) {
        g_message("another Tailscale profile is active, leaving Tailscale running");
        return TRUE;
    }
    return tailscale_down(self->session, error);
}

static void
nm_tailscale_plugin_init(NMTailscalePlugin *self)
{
    self->session = localapi_session_new();
}

static void
nm_tailscale_plugin_dispose(GObject *object)
{
    NMTailscalePlugin *self = NM_TAILSCALE_PLUGIN(object);

    g_clear_handle_id(&self->poll_id, g_source_remove);
    g_clear_pointer(&self->watch, bus_watch_free);
    g_clear_object(&self->session);
    g_clear_pointer(&self->interface, g_free);
    g_clear_pointer(&self->profile_id, g_free);
    g_clear_pointer(&self->uuid, g_free);
    G_OBJECT_CLASS(nm_tailscale_plugin_parent_class)->dispose(object);
}

static void
nm_tailscale_plugin_class_init(NMTailscalePluginClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    NMVpnServicePluginClass *parent_class = NM_VPN_SERVICE_PLUGIN_CLASS(klass);

    object_class->dispose = nm_tailscale_plugin_dispose;
    parent_class->connect = real_connect;
    parent_class->need_secrets = real_need_secrets;
    parent_class->disconnect = real_disconnect;
}

/*****************************************************************************/
/* Sync mode: a long-running service activating the NetworkManager connection
 * of the profile tailscaled runs, when Tailscale was started outside
 * NetworkManager ('tailscale up', 'tailscale switch', at boot). The plugin
 * instances handle the opposite direction, so this never deactivates. */

#define SYNC_FAILED_BACKOFF_S 30

typedef struct {
    SoupSession *api;
    NMClient *nm;
    BusWatch *watch;
    gboolean activating;         /* activation requested, no reply yet */
    NMActiveConnection *pending; /* our activation, until it settles */
    char *pending_profile;
    char *backoff_profile;       /* last activation failed for it... */
    gint64 backoff_until;        /* ...so do not retry before */
} Sync;

static gboolean
is_tailscale_connection(NMConnection *connection)
{
    NMSettingVpn *s_vpn = nm_connection_get_setting_vpn(connection);

    return s_vpn && g_strcmp0(nm_setting_vpn_get_service_type(s_vpn), NM_DBUS_SERVICE_TAILSCALE) == 0;
}

static void
sync_settle(Sync *s, gboolean failed)
{
    if (failed) {
        g_free(s->backoff_profile);
        s->backoff_profile = g_steal_pointer(&s->pending_profile);
        s->backoff_until = g_get_monotonic_time() + SYNC_FAILED_BACKOFF_S * G_USEC_PER_SEC;
    }
    g_clear_pointer(&s->pending_profile, g_free);
    if (s->pending) {
        g_signal_handlers_disconnect_by_data(s->pending, s);
        g_clear_object(&s->pending);
    }
    /* Tailscale may have moved on during the activation */
    bus_watch_trigger(s->watch, 0);
}

static void
sync_pending_state_cb(NMActiveConnection *ac, GParamSpec *pspec, gpointer user_data)
{
    Sync *s = user_data;
    NMActiveConnectionState state = nm_active_connection_get_state(ac);

    if (state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
        sync_settle(s, FALSE);
    } else if (state >= NM_ACTIVE_CONNECTION_STATE_DEACTIVATING) {
        g_warning("sync: activating '%s' failed", nm_active_connection_get_id(ac));
        sync_settle(s, TRUE);
    }
}

static void
sync_activated_cb(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Sync *s = user_data;
    g_autoptr(GError) error = NULL;
    NMActiveConnection *ac = nm_client_activate_connection_finish(NM_CLIENT(source), result, &error);

    s->activating = FALSE;
    if (!ac) {
        g_warning("sync: activation failed: %s", error->message);
        sync_settle(s, TRUE);
        return;
    }

    s->pending = ac;
    g_signal_connect(ac, "notify::" NM_ACTIVE_CONNECTION_STATE, G_CALLBACK(sync_pending_state_cb), s);
    sync_pending_state_cb(ac, NULL, s);
}

static void
sync_changed(gpointer user_data)
{
    Sync *s = user_data;
    g_autoptr(GError) error = NULL;
    g_autofree char *current = NULL;
    g_autoptr(JsonNode) prefs = NULL;
    g_autoptr(JsonNode) status = NULL;
    const GPtrArray *connections;
    const GPtrArray *actives;
    NMRemoteConnection *candidate = NULL;
    const char *state;

    if (s->activating || s->pending)
        return; /* re-checked once it settles */

    current = current_profile_id(s->api, &error);
    if (!error)
        prefs = localapi(s->api, "GET", "prefs", NULL, &error);
    if (!error)
        status = localapi(s->api, "GET", "status?peers=false", NULL, &error);
    if (error) {
        g_debug("sync: %s", error->message); /* the bus watch retries */
        return;
    }

    state = json_member_str(json_obj(status), "BackendState");
    if (!current || !json_member_bool(json_obj(prefs), "WantRunning")
        || !(g_strcmp0(state, "Running") == 0 || g_strcmp0(state, "Starting") == 0))
        return;

    /* Leave any Tailscale connection being activated or already active alone:
     * either it is this profile's, or its plugin is switching profiles */
    actives = nm_client_get_active_connections(s->nm);
    for (guint i = 0; i < actives->len; i++) {
        NMActiveConnection *ac = actives->pdata[i];
        NMRemoteConnection *rc = nm_active_connection_get_connection(ac);
        NMActiveConnectionState ac_state = nm_active_connection_get_state(ac);

        if (rc && is_tailscale_connection(NM_CONNECTION(rc))
            && (ac_state == NM_ACTIVE_CONNECTION_STATE_ACTIVATING
                || ac_state == NM_ACTIVE_CONNECTION_STATE_ACTIVATED))
            return;
    }

    connections = nm_client_get_connections(s->nm);
    for (guint i = 0; i < connections->len && !candidate; i++) {
        NMConnection *c = connections->pdata[i];
        const char *selector;
        g_autofree char *id = NULL;

        if (!is_tailscale_connection(c))
            continue;
        selector = nm_setting_vpn_get_data_item(nm_connection_get_setting_vpn(c), KEY_PROFILE);
        id = selector ? resolve_profile_id(s->api, selector, NULL) : NULL;
        if (g_strcmp0(id, current) == 0)
            candidate = connections->pdata[i];
    }

    if (!candidate) {
        g_debug("sync: no NetworkManager connection for Tailscale profile %s", current);
        return;
    }
    if (g_strcmp0(s->backoff_profile, current) == 0 && g_get_monotonic_time() < s->backoff_until)
        return;

    g_message("sync: Tailscale runs profile %s, activating '%s'", current,
              nm_connection_get_id(NM_CONNECTION(candidate)));
    s->activating = TRUE;
    s->pending_profile = g_strdup(current);
    nm_client_activate_connection_async(s->nm, NM_CONNECTION(candidate), NULL, NULL, NULL, sync_activated_cb, s);
}

static int
run_sync(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
    Sync s = {0};

    s.nm = nm_client_new(NULL, &error);
    if (!s.nm) {
        g_warning("sync: cannot connect to NetworkManager: %s", error->message);
        return 1;
    }
    s.api = localapi_session_new();
    s.watch = bus_watch_new(sync_changed, &s);
    g_message("sync: following tailscaled");
    g_main_loop_run(loop);
    return 0;
}

/*****************************************************************************/
/* Standalone test mode: exercises the LocalAPI path without NetworkManager. */

typedef struct {
    SoupSession *api;
    const char *profile_id;
    guint failures;
    GMainLoop *loop;
} TestWatch;

static void
test_watch_changed(gpointer user_data)
{
    TestWatch *t = user_data;
    const char *reason = NULL;
    ProfileVerdict verdict = check_profile(t->api, t->profile_id, &t->failures, &reason);

    if (verdict == PROFILE_CURRENT || verdict == PROFILE_UNKNOWN)
        return;
    printf("would disconnect: %s (%s)\n", reason,
           verdict == PROFILE_RELEASE_EXPECTED ? "deactivate"
           : verdict == PROFILE_RELEASE_LOGIN  ? "login failure"
                                               : "failure");
    fflush(stdout);
    g_main_loop_quit(t->loop);
}

static int
run_test(const char *profile, gboolean down, gboolean watch)
{
    g_autoptr(SoupSession) session = localapi_session_new();
    g_autoptr(GError) error = NULL;
    g_autofree char *id = NULL;
    gint64 deadline = g_get_monotonic_time() + RUNNING_TIMEOUT_S * G_USEC_PER_SEC;

    if (down) {
        if (!tailscale_down(session, &error))
            goto fail;
        printf("Tailscale is down\n");
        return 0;
    }

    id = resolve_profile_id(session, profile, &error);
    if (!id)
        goto fail;

    if (watch) {
        g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
        TestWatch t = {.api = session, .profile_id = id, .loop = loop};
        BusWatch *w = bus_watch_new(test_watch_changed, &t);

        printf("watching profile %s\n", id);
        fflush(stdout);
        g_main_loop_run(loop);
        bus_watch_free(w);
        return 0;
    }

    if (!tailscale_up(session, id, &error))
        goto fail;

    for (;;) {
        g_autoptr(JsonNode) status = localapi(session, "GET", "status?peers=false", NULL, &error);
        const char *state = json_member_str(json_obj(status), "BackendState");
        g_autoptr(GVariant) config = NULL;

        if (!status)
            goto fail;
        if (g_strcmp0(state, "Running") == 0) {
            g_autofree char *c = NULL;

            config = build_vpn_config(json_obj(status), DEFAULT_INTERFACE, &error);
            if (!config)
                goto fail;
            c = g_variant_print(config, TRUE);
            printf("config: %s\n", c);
            return 0;
        }
        if (g_get_monotonic_time() > deadline) {
            fprintf(stderr, "timed out, state %s\n", state ? state : "unknown");
            return 1;
        }
        g_usleep(POLL_INTERVAL_MS * 1000);
    }

fail:
    fprintf(stderr, "error: %s\n", error->message);
    return 1;
}

/*****************************************************************************/

static void
quit_mainloop(NMVpnServicePlugin *plugin, gpointer user_data)
{
    g_main_loop_quit(user_data);
}

int
main(int argc, char *argv[])
{
    g_autoptr(GOptionContext) opt_ctx = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    NMVpnServicePlugin *plugin;
    g_autofree char *bus_name = g_strdup(NM_DBUS_SERVICE_TAILSCALE);
    g_autofree char *test_profile = NULL;
    g_autofree char *test_watch = NULL;
    gboolean persist = FALSE;
    gboolean debug = FALSE;
    gboolean test_down = FALSE;
    gboolean sync = FALSE;
    GOptionEntry entries[] = {
        {"persist", 0, 0, G_OPTION_ARG_NONE, &persist, "Don't quit when the VPN connection terminates", NULL},
        {"debug", 0, 0, G_OPTION_ARG_NONE, &debug, "Enable verbose debug logging", NULL},
        {"bus-name", 0, 0, G_OPTION_ARG_STRING, &bus_name, "D-Bus name to use for this instance", "NAME"},
        {"test-up", 0, 0, G_OPTION_ARG_STRING, &test_profile, "Bring PROFILE up without NetworkManager and print the "
                                                              "config that would be reported", "PROFILE"},
        {"test-down", 0, 0, G_OPTION_ARG_NONE, &test_down, "Bring Tailscale down without NetworkManager", NULL},
        {"test-watch", 0, 0, G_OPTION_ARG_STRING, &test_watch, "Watch tailscaled as a connection to PROFILE would, "
                                                               "and report when it would disconnect", "PROFILE"},
        {"sync", 0, 0, G_OPTION_ARG_NONE, &sync, "Run as a service activating the NetworkManager connection of the "
                                                  "profile Tailscale runs, when started outside NetworkManager", NULL},
        {NULL},
    };

    opt_ctx = g_option_context_new(NULL);
    g_option_context_set_summary(opt_ctx, "NetworkManager VPN service plugin for Tailscale.");
    g_option_context_add_main_entries(opt_ctx, entries, NULL);
    if (!g_option_context_parse(opt_ctx, &argc, &argv, &error)) {
        g_printerr("%s\n", error->message);
        return 1;
    }

    if (debug)
        g_setenv("G_MESSAGES_DEBUG", "all", TRUE);

    if (sync)
        return run_sync();
    if (test_watch)
        return run_test(test_watch, FALSE, TRUE);
    if (test_profile || test_down)
        return run_test(test_profile, test_down, FALSE);

    plugin = g_initable_new(nm_tailscale_plugin_get_type(), NULL, &error,
                            NM_VPN_SERVICE_PLUGIN_DBUS_SERVICE_NAME, bus_name,
                            NM_VPN_SERVICE_PLUGIN_DBUS_WATCH_PEER, !debug,
                            NULL);
    if (!plugin) {
        g_warning("failed to initialize the plugin: %s", error->message);
        return 1;
    }

    loop = g_main_loop_new(NULL, FALSE);
    if (!persist)
        g_signal_connect(plugin, "quit", G_CALLBACK(quit_mainloop), loop);
    g_main_loop_run(loop);
    g_object_unref(plugin);
    return 0;
}
