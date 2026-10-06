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
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#define NM_DBUS_SERVICE_TAILSCALE "org.freedesktop.NetworkManager.tailscale"

/* vpn.data keys */
#define KEY_PROFILE   "profile"   /* profile ID, name, login name or tailnet */
#define KEY_INTERFACE "interface" /* defaults to DEFAULT_INTERFACE */

#define DEFAULT_SOCKET    "/var/run/tailscale/tailscaled.sock"
#define DEFAULT_INTERFACE "tailscale0"
#define LOCALAPI_URL      "http://local-tailscaled.sock/localapi/v0/"

#define POLL_INTERVAL_MS   500
#define RUNNING_TIMEOUT_S  30

/*****************************************************************************/
/* LocalAPI */

static SoupSession *
localapi_session_new(void)
{
    const char *path = g_getenv("TAILSCALED_SOCKET");
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

/* Switches to the profile (if needed) and asks tailscaled to come up. */
static gboolean
tailscale_up(SoupSession *session, const char *selector, GError **error)
{
    g_autofree char *id = resolve_profile_id(session, selector, error);
    g_autoptr(JsonNode) current = NULL;
    g_autoptr(JsonNode) prefs = NULL;

    if (!id)
        return FALSE;

    current = localapi(session, "GET", "profiles/current", NULL, error);
    if (!current)
        return FALSE;
    if (g_strcmp0(json_member_str(json_obj(current), "ID"), id) != 0) {
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

typedef struct {
    GVariant *config;
    GVariant *ip4;
    GVariant *ip6;
} VpnConfig;

static void
vpn_config_clear(VpnConfig *c)
{
    g_clear_pointer(&c->config, g_variant_unref);
    g_clear_pointer(&c->ip4, g_variant_unref);
    g_clear_pointer(&c->ip6, g_variant_unref);
}
G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(VpnConfig, vpn_config_clear)

/* Builds the configuration NetworkManager expects from a LocalAPI status
 * object. Only the tailnet addresses are reported: no routes, no DNS and
 * never-default, so NetworkManager does not compete with tailscaled. */
static gboolean
build_vpn_config(JsonObject *status, const char *iface, VpnConfig *out, GError **error)
{
    JsonNode *ips_node = json_object_has_member(status, "TailscaleIPs")
                             ? json_object_get_member(status, "TailscaleIPs")
                             : NULL;
    JsonArray *ips = ips_node && JSON_NODE_HOLDS_ARRAY(ips_node) ? json_node_get_array(ips_node) : NULL;
    GVariantBuilder config;

    if (if_nametoindex(iface) == 0) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED,
                    "interface %s not found (is tailscaled running in userspace-networking mode?)", iface);
        return FALSE;
    }

    for (guint i = 0; ips && i < json_array_get_length(ips); i++) {
        JsonNode *n = json_array_get_element(ips, i);
        const char *ip = JSON_NODE_HOLDS_VALUE(n) ? json_node_get_string(n) : NULL;
        struct in_addr a4;
        struct in6_addr a6;
        GVariantBuilder b;

        if (!ip)
            continue;
        if (!out->ip4 && inet_pton(AF_INET, ip, &a4) == 1) {
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_ADDRESS, g_variant_new_uint32(a4.s_addr));
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_PREFIX, g_variant_new_uint32(32));
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP4_CONFIG_NEVER_DEFAULT, g_variant_new_boolean(TRUE));
            out->ip4 = g_variant_ref_sink(g_variant_builder_end(&b));
        } else if (!out->ip6 && inet_pton(AF_INET6, ip, &a6) == 1) {
            g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP6_CONFIG_ADDRESS,
                                  g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, &a6, sizeof(a6), 1));
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP6_CONFIG_PREFIX, g_variant_new_uint32(128));
            g_variant_builder_add(&b, "{sv}", NM_VPN_PLUGIN_IP6_CONFIG_NEVER_DEFAULT, g_variant_new_boolean(TRUE));
            out->ip6 = g_variant_ref_sink(g_variant_builder_end(&b));
        }
    }

    if (!out->ip4 && !out->ip6) {
        g_set_error(error, NM_VPN_PLUGIN_ERROR, NM_VPN_PLUGIN_ERROR_FAILED, "tailscaled reports no Tailscale IPs");
        return FALSE;
    }

    g_variant_builder_init(&config, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_TUNDEV, g_variant_new_string(iface));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP4, g_variant_new_boolean(out->ip4 != NULL));
    g_variant_builder_add(&config, "{sv}", NM_VPN_PLUGIN_CONFIG_HAS_IP6, g_variant_new_boolean(out->ip6 != NULL));
    out->config = g_variant_ref_sink(g_variant_builder_end(&config));
    return TRUE;
}

/*****************************************************************************/
/* Plugin */

typedef struct {
    NMVpnServicePlugin parent;

    SoupSession *session;
    char *interface;
    guint poll_id;
    gint64 deadline;
} NMTailscalePlugin;

typedef struct {
    NMVpnServicePluginClass parent;
} NMTailscalePluginClass;

GType nm_tailscale_plugin_get_type(void);
G_DEFINE_TYPE(NMTailscalePlugin, nm_tailscale_plugin, NM_TYPE_VPN_SERVICE_PLUGIN)

#define NM_TAILSCALE_PLUGIN(o) (G_TYPE_CHECK_INSTANCE_CAST((o), nm_tailscale_plugin_get_type(), NMTailscalePlugin))

static gboolean
poll_running(gpointer user_data)
{
    NMTailscalePlugin *self = user_data;
    NMVpnServicePlugin *plugin = NM_VPN_SERVICE_PLUGIN(self);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) status = localapi(self->session, "GET", "status?peers=false", NULL, &error);
    const char *state = json_member_str(json_obj(status), "BackendState");
    g_auto(VpnConfig) cfg = {0};

    if (g_strcmp0(state, "Running") == 0 && build_vpn_config(json_obj(status), self->interface, &cfg, &error)) {
        g_message("Tailscale is running, reporting %s to NetworkManager", self->interface);
        self->poll_id = 0;
        nm_vpn_service_plugin_set_config(plugin, cfg.config);
        if (cfg.ip4)
            nm_vpn_service_plugin_set_ip4_config(plugin, cfg.ip4);
        if (cfg.ip6)
            nm_vpn_service_plugin_set_ip6_config(plugin, cfg.ip6);
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

    if (!tailscale_up(self->session, profile, error))
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

    g_clear_handle_id(&self->poll_id, g_source_remove);
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
    g_clear_object(&self->session);
    g_clear_pointer(&self->interface, g_free);
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
/* Standalone test mode: exercises the LocalAPI path without NetworkManager. */

static int
run_test(const char *profile, gboolean down)
{
    g_autoptr(SoupSession) session = localapi_session_new();
    g_autoptr(GError) error = NULL;
    gint64 deadline = g_get_monotonic_time() + RUNNING_TIMEOUT_S * G_USEC_PER_SEC;

    if (down) {
        if (!tailscale_down(session, &error))
            goto fail;
        printf("Tailscale is down\n");
        return 0;
    }

    if (!tailscale_up(session, profile, &error))
        goto fail;

    for (;;) {
        g_autoptr(JsonNode) status = localapi(session, "GET", "status?peers=false", NULL, &error);
        const char *state = json_member_str(json_obj(status), "BackendState");
        g_auto(VpnConfig) cfg = {0};

        if (!status)
            goto fail;
        if (g_strcmp0(state, "Running") == 0) {
            g_autofree char *c = NULL, *ip4 = NULL, *ip6 = NULL;

            if (!build_vpn_config(json_obj(status), DEFAULT_INTERFACE, &cfg, &error))
                goto fail;
            c = g_variant_print(cfg.config, TRUE);
            ip4 = cfg.ip4 ? g_variant_print(cfg.ip4, TRUE) : NULL;
            ip6 = cfg.ip6 ? g_variant_print(cfg.ip6, TRUE) : NULL;
            printf("config: %s\nip4:    %s\nip6:    %s\n", c, ip4 ? ip4 : "-", ip6 ? ip6 : "-");
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
    gboolean persist = FALSE;
    gboolean debug = FALSE;
    gboolean test_down = FALSE;
    GOptionEntry entries[] = {
        {"persist", 0, 0, G_OPTION_ARG_NONE, &persist, "Don't quit when the VPN connection terminates", NULL},
        {"debug", 0, 0, G_OPTION_ARG_NONE, &debug, "Enable verbose debug logging", NULL},
        {"bus-name", 0, 0, G_OPTION_ARG_STRING, &bus_name, "D-Bus name to use for this instance", "NAME"},
        {"test-up", 0, 0, G_OPTION_ARG_STRING, &test_profile, "Bring PROFILE up without NetworkManager and print the "
                                                              "config that would be reported", "PROFILE"},
        {"test-down", 0, 0, G_OPTION_ARG_NONE, &test_down, "Bring Tailscale down without NetworkManager", NULL},
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

    if (test_profile || test_down)
        return run_test(test_profile, test_down);

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
