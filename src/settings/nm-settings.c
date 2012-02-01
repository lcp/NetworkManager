/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
 *
 * Søren Sandmann <sandmann@daimi.au.dk>
 * Dan Williams <dcbw@redhat.com>
 * Tambet Ingo <tambet@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * (C) Copyright 2007 - 2011 Red Hat, Inc.
 * (C) Copyright 2008 Novell, Inc.
 */

#include "config.h"

#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <gmodule.h>
#include <net/if_arp.h>
#include <pwd.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib-lowlevel.h>

#include <NetworkManager.h>
#include <nm-connection.h>
#include <nm-setting-8021x.h>
#include <nm-setting-bluetooth.h>
#include <nm-setting-cdma.h>
#include <nm-setting-connection.h>
#include <nm-setting-gsm.h>
#include <nm-setting-ip4-config.h>
#include <nm-setting-ip6-config.h>
#include <nm-setting-olpc-mesh.h>
#include <nm-setting-ppp.h>
#include <nm-setting-pppoe.h>
#include <nm-setting-serial.h>
#include <nm-setting-vpn.h>
#include <nm-setting-wired.h>
#include <nm-setting-wireless.h>
#include <nm-setting-wireless-security.h>
#include <nm-setting-bond.h>
#include <nm-utils.h>

#include "../nm-device-ethernet.h"
#include "../nm-device-wired.h"
#include "nm-dbus-glib-types.h"
#include "nm-settings.h"
#include "nm-settings-connection.h"
#include "nm-settings-error.h"
#include "nm-default-wired-connection.h"
#include "nm-logging.h"
#include "nm-dbus-manager.h"
#include "nm-manager-auth.h"
#include "nm-session-monitor.h"
#include "plugins/keyfile/plugin.h"
#include "nm-agent-manager.h"
#include "nm-settings-utils.h"

#define CONFIG_KEY_NO_AUTO_DEFAULT "no-auto-default"

/* LINKER CRACKROCK */
#define EXPORT(sym) void * __export_##sym = &sym;

#include "nm-inotify-helper.h"
EXPORT(nm_inotify_helper_get_type)
EXPORT(nm_inotify_helper_get)
EXPORT(nm_inotify_helper_add_watch)
EXPORT(nm_inotify_helper_remove_watch)

EXPORT(nm_settings_connection_get_type)
EXPORT(nm_settings_connection_replace_settings)
EXPORT(nm_settings_connection_replace_and_commit)
/* END LINKER CRACKROCK */

static void claim_connection (NMSettings *self,
                              NMSettingsConnection *connection,
                              gboolean do_export);

static gboolean impl_settings_list_connections (NMSettings *self,
                                                GPtrArray **connections,
                                                GError **error);

static gboolean impl_settings_get_connection_by_uuid (NMSettings *self,
                                                      const char *uuid,
                                                      char **out_object_path,
                                                      GError **error);

static void impl_settings_add_connection (NMSettings *self,
                                          GHashTable *settings,
                                          DBusGMethodInvocation *context);

static void impl_settings_save_hostname (NMSettings *self,
                                         const char *hostname,
                                         DBusGMethodInvocation *context);

#include "nm-settings-glue.h"

static void unmanaged_specs_changed (NMSystemConfigInterface *config, gpointer user_data);

typedef struct {
	NMDBusManager *dbus_mgr;
	DBusGConnection *bus;

	NMAgentManager *agent_mgr;

	char *config_file;

	NMSessionMonitor *session_monitor;
	GSList *auths;

	GSList *plugins;
	gboolean connections_loaded;
	GHashTable *connections;
	GSList *unmanaged_specs;
} NMSettingsPrivate;

G_DEFINE_TYPE (NMSettings, nm_settings, G_TYPE_OBJECT)

#define NM_SETTINGS_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_SETTINGS, NMSettingsPrivate))

enum {
	PROPERTIES_CHANGED,
	CONNECTION_ADDED,
	CONNECTION_UPDATED,
	CONNECTION_REMOVED,
	CONNECTION_VISIBILITY_CHANGED,
	CONNECTIONS_LOADED,
	AGENT_REGISTERED,

	NEW_CONNECTION, /* exported, not used internally */
	LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,
	PROP_UNMANAGED_SPECS,
	PROP_HOSTNAME,
	PROP_CAN_MODIFY,

	LAST_PROP
};

static void
load_connections (NMSettings *self)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	if (priv->connections_loaded)
		return;

	for (iter = priv->plugins; iter; iter = g_slist_next (iter)) {
		NMSystemConfigInterface *plugin = NM_SYSTEM_CONFIG_INTERFACE (iter->data);
		GSList *plugin_connections;
		GSList *elt;

		plugin_connections = nm_system_config_interface_get_connections (plugin);

		// FIXME: ensure connections from plugins loaded with a lower priority
		// get rejected when they conflict with connections from a higher
		// priority plugin.

		for (elt = plugin_connections; elt; elt = g_slist_next (elt))
			claim_connection (self, NM_SETTINGS_CONNECTION (elt->data), TRUE);

		g_slist_free (plugin_connections);
	}

	priv->connections_loaded = TRUE;

	/* FIXME: Bad hack */
	unmanaged_specs_changed (NULL, self);

	g_signal_emit (self, signals[CONNECTIONS_LOADED], 0);
}

void
nm_settings_for_each_connection (NMSettings *self,
                                 NMSettingsForEachFunc for_each_func,
                                 gpointer user_data)
{
	NMSettingsPrivate *priv;
	GHashTableIter iter;
	gpointer data;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_SETTINGS (self));
	g_return_if_fail (for_each_func != NULL);
	
	priv = NM_SETTINGS_GET_PRIVATE (self);

	load_connections (self);

	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, &data))
		for_each_func (self, NM_SETTINGS_CONNECTION (data), user_data);
}

static gboolean
impl_settings_list_connections (NMSettings *self,
                                GPtrArray **connections,
                                GError **error)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GHashTableIter iter;
	gpointer key;

	load_connections (self);

	*connections = g_ptr_array_sized_new (g_hash_table_size (priv->connections) + 1);
	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, &key, NULL))
		g_ptr_array_add (*connections, g_strdup ((const char *) key));
	return TRUE;
}

static gboolean
impl_settings_get_connection_by_uuid (NMSettings *self,
                                      const char *uuid,
                                      char **out_object_path,
                                      GError **error)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GHashTableIter iter;
	NMConnection *candidate = NULL;
	gboolean found = FALSE;

	load_connections (self);

	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer) &candidate)) {
		if (g_strcmp0 (uuid, nm_connection_get_uuid (candidate)) == 0) {
			*out_object_path = g_strdup (nm_connection_get_path (candidate));
			found = TRUE;
			break;
		}
	}

	if (!found) {
		g_set_error_literal (error,
		                     NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "No connection with the UUID was found.");
	}

	return found;
}

static int
connection_sort (gconstpointer pa, gconstpointer pb)
{
	NMConnection *a = NM_CONNECTION (pa);
	NMSettingConnection *con_a;
	NMConnection *b = NM_CONNECTION (pb);
	NMSettingConnection *con_b;
	guint64 ts_a, ts_b;

	con_a = nm_connection_get_setting_connection (a);
	g_assert (con_a);
	con_b = nm_connection_get_setting_connection (b);
	g_assert (con_b);

	if (nm_setting_connection_get_autoconnect (con_a) != nm_setting_connection_get_autoconnect (con_b)) {
		if (nm_setting_connection_get_autoconnect (con_a))
			return -1;
		return 1;
	}

	ts_a = nm_settings_connection_get_timestamp (NM_SETTINGS_CONNECTION (pa));
	ts_b = nm_settings_connection_get_timestamp (NM_SETTINGS_CONNECTION (pb));
	if (ts_a > ts_b)
		return -1;
	else if (ts_a == ts_b)
		return 0;
	return 1;
}

/* Returns a list of NMSettingsConnections.  Caller must free the list with
 * g_slist_free().
 */
GSList *
nm_settings_get_connections (NMSettings *self)
{
	GHashTableIter iter;
	gpointer data = NULL;
	GSList *list = NULL;

	g_return_val_if_fail (NM_IS_SETTINGS (self), NULL);

	g_hash_table_iter_init (&iter, NM_SETTINGS_GET_PRIVATE (self)->connections);
	while (g_hash_table_iter_next (&iter, NULL, &data))
		list = g_slist_insert_sorted (list, data, connection_sort);
	return list;
}

NMSettingsConnection *
nm_settings_get_connection_by_path (NMSettings *self, const char *path)
{
	NMSettingsPrivate *priv;

	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (NM_IS_SETTINGS (self), NULL);
	g_return_val_if_fail (path != NULL, NULL);

	priv = NM_SETTINGS_GET_PRIVATE (self);

	load_connections (self);

	return (NMSettingsConnection *) g_hash_table_lookup (priv->connections, path);
}

static void
clear_unmanaged_specs (NMSettings *self)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);

	g_slist_foreach (priv->unmanaged_specs, (GFunc) g_free, NULL);
	g_slist_free (priv->unmanaged_specs);
	priv->unmanaged_specs = NULL;
}

static char*
uscore_to_wincaps (const char *uscore)
{
	const char *p;
	GString *str;
	gboolean last_was_uscore;

	last_was_uscore = TRUE;
  
	str = g_string_new (NULL);
	p = uscore;
	while (p && *p) {
		if (*p == '-' || *p == '_')
			last_was_uscore = TRUE;
		else {
			if (last_was_uscore) {
				g_string_append_c (str, g_ascii_toupper (*p));
				last_was_uscore = FALSE;
			} else
				g_string_append_c (str, *p);
		}
		++p;
	}

	return g_string_free (str, FALSE);
}

static void
notify (GObject *object, GParamSpec *pspec)
{
	GValue *value;
	GHashTable *hash;

	value = g_slice_new0 (GValue);
	hash = g_hash_table_new_full (g_str_hash, g_str_equal, (GDestroyNotify) g_free, NULL);

	g_value_init (value, pspec->value_type);
	g_object_get_property (object, pspec->name, value);
	g_hash_table_insert (hash, uscore_to_wincaps (pspec->name), value);
	g_signal_emit (object, signals[PROPERTIES_CHANGED], 0, hash);
	g_hash_table_destroy (hash);
	g_value_unset (value);
	g_slice_free (GValue, value);
}

const GSList *
nm_settings_get_unmanaged_specs (NMSettings *self)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);

	load_connections (self);
	return priv->unmanaged_specs;
}

static NMSystemConfigInterface *
get_plugin (NMSettings *self, guint32 capability)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	g_return_val_if_fail (self != NULL, NULL);

	/* Do any of the plugins support setting the hostname? */
	for (iter = priv->plugins; iter; iter = iter->next) {
		NMSystemConfigInterfaceCapabilities caps = NM_SYSTEM_CONFIG_INTERFACE_CAP_NONE;

		g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_CAPABILITIES, &caps, NULL);
		if (caps & capability)
			return NM_SYSTEM_CONFIG_INTERFACE (iter->data);
	}

	return NULL;
}

/* Returns an allocated string which the caller owns and must eventually free */
char *
nm_settings_get_hostname (NMSettings *self)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *iter;
	char *hostname = NULL;

	/* Hostname returned is the hostname returned from the first plugin
	 * that provides one.
	 */
	for (iter = priv->plugins; iter; iter = iter->next) {
		NMSystemConfigInterfaceCapabilities caps = NM_SYSTEM_CONFIG_INTERFACE_CAP_NONE;

		g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_CAPABILITIES, &caps, NULL);
		if (caps & NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_HOSTNAME) {
			g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_HOSTNAME, &hostname, NULL);
			if (hostname && strlen (hostname))
				return hostname;
			g_free (hostname);
		}
	}

	return NULL;
}

static void
plugin_connection_added (NMSystemConfigInterface *config,
                         NMSettingsConnection *connection,
                         gpointer user_data)
{
	claim_connection (NM_SETTINGS (user_data), connection, TRUE);
}

static gboolean
find_unmanaged_device (NMSettings *self, const char *needle)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	for (iter = priv->unmanaged_specs; iter; iter = g_slist_next (iter)) {
		if (!strcmp ((const char *) iter->data, needle))
			return TRUE;
	}
	return FALSE;
}

static void
unmanaged_specs_changed (NMSystemConfigInterface *config,
                         gpointer user_data)
{
	NMSettings *self = NM_SETTINGS (user_data);
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	clear_unmanaged_specs (self);

	/* Ask all the plugins for their unmanaged specs */
	for (iter = priv->plugins; iter; iter = g_slist_next (iter)) {
		GSList *specs, *specs_iter;

		specs = nm_system_config_interface_get_unmanaged_specs (NM_SYSTEM_CONFIG_INTERFACE (iter->data));
		for (specs_iter = specs; specs_iter; specs_iter = specs_iter->next) {
			if (!find_unmanaged_device (self, (const char *) specs_iter->data)) {
				priv->unmanaged_specs = g_slist_prepend (priv->unmanaged_specs, specs_iter->data);
			} else
				g_free (specs_iter->data);
		}

		g_slist_free (specs);
	}

	g_object_notify (G_OBJECT (self), NM_SETTINGS_UNMANAGED_SPECS);
}

static void
hostname_changed (NMSystemConfigInterface *config,
                  GParamSpec *pspec,
                  gpointer user_data)
{
	g_object_notify (G_OBJECT (user_data), NM_SETTINGS_HOSTNAME);
}

static void
add_plugin (NMSettings *self, NMSystemConfigInterface *plugin)
{
	NMSettingsPrivate *priv;
	char *pname = NULL;
	char *pinfo = NULL;

	g_return_if_fail (NM_IS_SETTINGS (self));
	g_return_if_fail (NM_IS_SYSTEM_CONFIG_INTERFACE (plugin));

	priv = NM_SETTINGS_GET_PRIVATE (self);

	priv->plugins = g_slist_append (priv->plugins, g_object_ref (plugin));

	g_signal_connect (plugin, NM_SYSTEM_CONFIG_INTERFACE_CONNECTION_ADDED,
	                  G_CALLBACK (plugin_connection_added), self);
	g_signal_connect (plugin, "notify::hostname", G_CALLBACK (hostname_changed), self);

	nm_system_config_interface_init (plugin, NULL);

	g_object_get (G_OBJECT (plugin),
	              NM_SYSTEM_CONFIG_INTERFACE_NAME, &pname,
	              NM_SYSTEM_CONFIG_INTERFACE_INFO, &pinfo,
	              NULL);

	g_signal_connect (plugin, NM_SYSTEM_CONFIG_INTERFACE_UNMANAGED_SPECS_CHANGED,
	                  G_CALLBACK (unmanaged_specs_changed), self);

	nm_log_info (LOGD_SETTINGS, "Loaded plugin %s: %s", pname, pinfo);
	g_free (pname);
	g_free (pinfo);
}

static GObject *
find_plugin (GSList *list, const char *pname)
{
	GSList *iter;
	GObject *obj = NULL;

	g_return_val_if_fail (pname != NULL, NULL);

	for (iter = list; iter && !obj; iter = g_slist_next (iter)) {
		NMSystemConfigInterface *plugin = NM_SYSTEM_CONFIG_INTERFACE (iter->data);
		char *list_pname = NULL;

		g_object_get (G_OBJECT (plugin),
		              NM_SYSTEM_CONFIG_INTERFACE_NAME,
		              &list_pname,
		              NULL);
		if (list_pname && !strcmp (pname, list_pname))
			obj = G_OBJECT (plugin);

		g_free (list_pname);
	}

	return obj;
}

static gboolean
load_plugins (NMSettings *self, const char **plugins, GError **error)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *list = NULL;
	const char **iter;
	gboolean success = TRUE;

	for (iter = plugins; *iter; iter++) {
		GModule *plugin;
		char *full_name, *path;
		const char *pname = *iter;
		GObject *obj;
		GObject * (*factory_func) (const char *);

		/* strip leading spaces */
		while (isblank (*pname))
			pname++;

		/* keyfile plugin built in now */
		if (!strcmp (pname, "keyfile"))
			continue;

		/* ifcfg-fedora was renamed ifcfg-rh; handle old configs here */
		if (!strcmp (pname, "ifcfg-fedora"))
			pname = "ifcfg-rh";

		obj = find_plugin (list, pname);
		if (obj)
			continue;

		full_name = g_strdup_printf ("nm-settings-plugin-%s", pname);
		path = g_module_build_path (PLUGINDIR, full_name);

		plugin = g_module_open (path, G_MODULE_BIND_LOCAL);
		if (!plugin) {
			g_set_error (error, 0, 0,
			             "Could not load plugin '%s': %s",
			             pname, g_module_error ());
			g_free (full_name);
			g_free (path);
			success = FALSE;
			break;
		}

		g_free (full_name);
		g_free (path);

		if (!g_module_symbol (plugin, "nm_system_config_factory", (gpointer) (&factory_func))) {
			g_set_error (error, 0, 0,
			             "Could not find plugin '%s' factory function.",
			             pname);
			success = FALSE;
			break;
		}

		obj = (*factory_func) (priv->config_file);
		if (!obj || !NM_IS_SYSTEM_CONFIG_INTERFACE (obj)) {
			g_set_error (error, 0, 0,
			             "Plugin '%s' returned invalid system config object.",
			             pname);
			success = FALSE;
			break;
		}

		g_module_make_resident (plugin);
		g_object_weak_ref (obj, (GWeakNotify) g_module_close, plugin);
		add_plugin (self, NM_SYSTEM_CONFIG_INTERFACE (obj));
		list = g_slist_append (list, obj);
	}

	g_slist_foreach (list, (GFunc) g_object_unref, NULL);
	g_slist_free (list);

	return success;
}

#define REMOVED_ID_TAG "removed-id-tag"
#define UPDATED_ID_TAG "updated-id-tag"
#define VISIBLE_ID_TAG "visible-id-tag"
#define UNREG_ID_TAG "unreg-id-tag"

static void
connection_removed (NMSettingsConnection *obj, gpointer user_data)
{
	GObject *connection = G_OBJECT (obj);
	guint id;

	g_object_ref (connection);

	/* Disconnect signal handlers, as plugins might still keep references
	 * to the connection (and thus the signal handlers would still be live)
	 * even after NMSettings has dropped all its references.
	 */

	id = GPOINTER_TO_UINT (g_object_get_data (connection, REMOVED_ID_TAG));
	if (id)
		g_signal_handler_disconnect (connection, id);

	id = GPOINTER_TO_UINT (g_object_get_data (connection, UPDATED_ID_TAG));
	if (id)
		g_signal_handler_disconnect (connection, id);

	id = GPOINTER_TO_UINT (g_object_get_data (connection, VISIBLE_ID_TAG));
	if (id)
		g_signal_handler_disconnect (connection, id);

	/* Forget about the connection internally */
	g_hash_table_remove (NM_SETTINGS_GET_PRIVATE (user_data)->connections,
	                     (gpointer) nm_connection_get_path (NM_CONNECTION (connection)));

	/* Re-emit for listeners like NMPolicy */
	g_signal_emit (NM_SETTINGS (user_data), signals[CONNECTION_REMOVED], 0, connection);

	g_object_unref (connection);
}

static void
connection_unregister (NMSettingsConnection *obj, gpointer user_data)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (user_data);
	GObject *connection = G_OBJECT (obj);
	guint id;

	/* Make sure it's unregistered from the bus now that's removed */
	dbus_g_connection_unregister_g_object (priv->bus, connection);

	id = GPOINTER_TO_UINT (g_object_get_data (connection, UNREG_ID_TAG));
	if (id)
		g_signal_handler_disconnect (connection, id);
}

static void
connection_updated (NMSettingsConnection *connection, gpointer user_data)
{
	/* Re-emit for listeners like NMPolicy */
	g_signal_emit (NM_SETTINGS (user_data),
	               signals[CONNECTION_UPDATED],
	               0,
	               connection);
}

static void
connection_visibility_changed (NMSettingsConnection *connection,
                               GParamSpec *pspec,
                               gpointer user_data)
{
	/* Re-emit for listeners like NMPolicy */
	g_signal_emit (NM_SETTINGS (user_data),
	               signals[CONNECTION_VISIBILITY_CHANGED],
	               0,
	               connection);
}

static void
secret_agent_registered (NMAgentManager *agent_mgr,
                         NMSecretAgent *agent,
                         gpointer user_data)
{
	/* Re-emit for listeners like NMPolicy */
	g_signal_emit (NM_SETTINGS (user_data),
	               signals[AGENT_REGISTERED],
	               0,
	               agent);
}

#define NM_DBUS_SERVICE_OPENCONNECT    "org.freedesktop.NetworkManager.openconnect"
#define NM_OPENCONNECT_KEY_GATEWAY "gateway"
#define NM_OPENCONNECT_KEY_COOKIE "cookie"
#define NM_OPENCONNECT_KEY_GWCERT "gwcert"
#define NM_OPENCONNECT_KEY_XMLCONFIG "xmlconfig"
#define NM_OPENCONNECT_KEY_LASTHOST "lasthost"
#define NM_OPENCONNECT_KEY_AUTOCONNECT "autoconnect"
#define NM_OPENCONNECT_KEY_CERTSIGS "certsigs"

static void
openconnect_migrate_hack (NMConnection *connection)
{
	NMSettingVPN *s_vpn;
	NMSettingSecretFlags flags = NM_SETTING_SECRET_FLAG_NOT_SAVED;

	/* Huge hack.  There were some openconnect changes that needed to happen
	 * pretty late, too late to get into distros.  Migration has already
	 * happened for many people, and their secret flags are wrong.  But we
	 * don't want to requrie re-migration, so we have to fix it up here. Ugh.
	 */

	s_vpn = nm_connection_get_setting_vpn (connection);
	if (s_vpn == NULL)
		return;

	if (g_strcmp0 (nm_setting_vpn_get_service_type (s_vpn), NM_DBUS_SERVICE_OPENCONNECT) == 0) {
		/* These are different for every login session, and should not be stored */
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_OPENCONNECT_KEY_GATEWAY, flags, NULL);
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_OPENCONNECT_KEY_COOKIE, flags, NULL);
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_OPENCONNECT_KEY_GWCERT, flags, NULL);

		/* These are purely internal data for the auth-dialog, and should be stored */
		flags = 0;
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_OPENCONNECT_KEY_XMLCONFIG, flags, NULL);
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_OPENCONNECT_KEY_LASTHOST, flags, NULL);
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_OPENCONNECT_KEY_AUTOCONNECT, flags, NULL);
		nm_setting_set_secret_flags (NM_SETTING (s_vpn), NM_OPENCONNECT_KEY_CERTSIGS, flags, NULL);
	}
}

static void
claim_connection (NMSettings *self,
                  NMSettingsConnection *connection,
                  gboolean do_export)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	static guint32 ec_counter = 0;
	GError *error = NULL;
	GHashTableIter iter;
	gpointer data;
	char *path;
	guint id;

	g_return_if_fail (NM_IS_SETTINGS_CONNECTION (connection));
	g_return_if_fail (nm_connection_get_path (NM_CONNECTION (connection)) == NULL);

	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, &data)) {
		/* prevent duplicates */
		if (data == connection)
			return;
	}

	if (!nm_connection_verify (NM_CONNECTION (connection), &error)) {
		nm_log_warn (LOGD_SETTINGS, "plugin provided invalid connection: '%s' / '%s' invalid: %d",
		             g_type_name (nm_connection_lookup_setting_type_by_quark (error->domain)),
		             error->message, error->code);
		g_error_free (error);
		return;
	}

	/* Read timestamp from look-aside file and put it into the connection's data */
	nm_settings_connection_read_and_fill_timestamp (connection);

	/* Read seen-bssids from look-aside file and put it into the connection's data */
	nm_settings_connection_read_and_fill_seen_bssids (connection);

	/* Ensure it's initial visibility is up-to-date */
	nm_settings_connection_recheck_visibility (connection);

	/* Evil openconnect migration hack */
	openconnect_migrate_hack (NM_CONNECTION (connection));

	id = g_signal_connect (connection, NM_SETTINGS_CONNECTION_REMOVED,
	                       G_CALLBACK (connection_removed),
	                       self);
	g_object_set_data (G_OBJECT (connection), REMOVED_ID_TAG, GUINT_TO_POINTER (id));

	id = g_signal_connect (connection, "unregister",
	                       G_CALLBACK (connection_unregister),
	                       self);
	g_object_set_data (G_OBJECT (connection), UNREG_ID_TAG, GUINT_TO_POINTER (id));

	id = g_signal_connect (connection, NM_SETTINGS_CONNECTION_UPDATED,
	                       G_CALLBACK (connection_updated),
	                       self);
	g_object_set_data (G_OBJECT (connection), UPDATED_ID_TAG, GUINT_TO_POINTER (id));

	id = g_signal_connect (connection, "notify::" NM_SETTINGS_CONNECTION_VISIBLE,
	                       G_CALLBACK (connection_visibility_changed),
	                       self);
	g_object_set_data (G_OBJECT (connection), VISIBLE_ID_TAG, GUINT_TO_POINTER (id));

	/* Export the connection over D-Bus */
	g_warn_if_fail (nm_connection_get_path (NM_CONNECTION (connection)) == NULL);
	path = g_strdup_printf ("%s/%u", NM_DBUS_PATH_SETTINGS, ec_counter++);
	nm_connection_set_path (NM_CONNECTION (connection), path);
	dbus_g_connection_register_g_object (priv->bus, path, G_OBJECT (connection));
	g_free (path);

	g_hash_table_insert (priv->connections,
	                     (gpointer) nm_connection_get_path (NM_CONNECTION (connection)),
	                     g_object_ref (connection));

	/* Only emit the individual connection-added signal after connections
	 * have been initially loaded.  While getting the first list of connections
	 * we suppress it, then send the connections-loaded signal after we're all
	 * done to minimize processing.
	 */
	if (priv->connections_loaded) {
		/* Internal added signal */
		g_signal_emit (self, signals[CONNECTION_ADDED], 0, connection);

		/* Exported D-Bus signal */
		g_signal_emit (self, signals[NEW_CONNECTION], 0, connection);
	}
}

static void
remove_default_wired_connection (NMSettings *self,
                                 NMSettingsConnection *connection,
                                 gboolean do_signal)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	const char *path = nm_connection_get_path (NM_CONNECTION (connection));

	if (g_hash_table_lookup (priv->connections, path)) {
		if (do_signal)
			g_signal_emit_by_name (G_OBJECT (connection), NM_SETTINGS_CONNECTION_REMOVED);
		g_hash_table_remove (priv->connections, path);
	}
}

static NMSettingsConnection *
add_new_connection (NMSettings *self,
                    NMConnection *connection,
                    GError **error)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *iter;
	NMSettingsConnection *added = NULL;
	GHashTableIter citer;
	NMConnection *candidate = NULL;

	/* Make sure a connection with this UUID doesn't already exist */
	g_hash_table_iter_init (&citer, priv->connections);
	while (g_hash_table_iter_next (&citer, NULL, (gpointer *) &candidate)) {
		if (g_strcmp0 (nm_connection_get_uuid (connection),
		               nm_connection_get_uuid (candidate)) == 0) {
			g_set_error_literal (error,
			                     NM_SETTINGS_ERROR,
			                     NM_SETTINGS_ERROR_UUID_EXISTS,
			                     "A connection with this UUID already exists.");
			return NULL;
		}
	}

	/* 1) plugin writes the NMConnection to disk
	 * 2) plugin creates a new NMSettingsConnection subclass with the settings
	 *     from the NMConnection and returns it to the settings service
	 * 3) settings service exports the new NMSettingsConnection subclass
	 * 4) plugin notices that something on the filesystem has changed
	 * 5) plugin reads the changes and ignores them because they will
	 *     contain the same data as the connection it already knows about
	 */
	for (iter = priv->plugins; iter; iter = g_slist_next (iter)) {
		NMSystemConfigInterface *plugin = NM_SYSTEM_CONFIG_INTERFACE (iter->data);
		GError *add_error = NULL;

		g_clear_error (error);
		added = nm_system_config_interface_add_connection (plugin, connection, &add_error);
		if (added) {
			claim_connection (self, added, TRUE);
			return added;
		}
		g_propagate_error (error, add_error);
	}
	return NULL;
}

static gboolean
secrets_filter_cb (NMSetting *setting,
                   const char *secret,
                   NMSettingSecretFlags flags,
                   gpointer user_data)
{
	NMSettingSecretFlags filter_flags = GPOINTER_TO_UINT (user_data);

	/* Returns TRUE to remove the secret */

	/* Can't use bitops with SECRET_FLAG_NONE so handle that specifically */
	if (   (flags == NM_SETTING_SECRET_FLAG_NONE)
	    && (filter_flags == NM_SETTING_SECRET_FLAG_NONE))
		return FALSE;

	/* Otherwise if the secret has at least one of the desired flags keep it */
	return (flags & filter_flags) ? FALSE : TRUE;
}

static void
send_agent_owned_secrets (NMSettings *self,
                          NMSettingsConnection *connection,
                          gulong caller_uid)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	NMConnection *for_agent;

	/* Dupe the connection so we can clear out non-agent-owned secrets,
	 * as agent-owned secrets are the only ones we send back to be saved.
	 * Only send secrets to agents of the same UID that called update too.
	 */
	for_agent = nm_connection_duplicate (NM_CONNECTION (connection));
	nm_connection_clear_secrets_with_flags (for_agent,
	                                        secrets_filter_cb,
	                                        GUINT_TO_POINTER (NM_SETTING_SECRET_FLAG_AGENT_OWNED));
	nm_agent_manager_save_secrets (priv->agent_mgr, for_agent, TRUE, caller_uid);
	g_object_unref (for_agent);
}

static void
pk_add_cb (NMAuthChain *chain,
           GError *chain_error,
           DBusGMethodInvocation *context,
           gpointer user_data)
{
	NMSettings *self = NM_SETTINGS (user_data);
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	NMAuthCallResult result;
	GError *error = NULL, *add_error = NULL;
	NMConnection *connection;
	NMSettingsConnection *added = NULL;
	NMSettingsAddCallback callback;
	gpointer callback_data;
	gulong caller_uid;
	const char *perm;

	priv->auths = g_slist_remove (priv->auths, chain);

	if (chain_error) {
		error = g_error_new (NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_GENERAL,
		                     "Error checking authorization: %s",
		                     chain_error->message ? chain_error->message : "(unknown)");
		goto done;
	}

	perm = nm_auth_chain_get_data (chain, "perm");
	g_assert (perm);
	result = nm_auth_chain_get_result (chain, perm);

	/* Caller didn't successfully authenticate */
	if (result != NM_AUTH_CALL_RESULT_YES) {
		error = g_error_new_literal (NM_SETTINGS_ERROR,
		                             NM_SETTINGS_ERROR_NOT_PRIVILEGED,
		                             "Insufficient privileges.");
		goto done;
	}

	connection = nm_auth_chain_get_data (chain, "connection");
	g_assert (connection);
	added = add_new_connection (self, connection, &add_error);
	if (!added) {
		error = g_error_new (NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_ADD_FAILED,
		                     "Saving connection failed: (%d) %s",
		                     add_error ? add_error->code : -1,
		                     (add_error && add_error->message) ? add_error->message : "(unknown)");
		g_error_free (add_error);
	}

done:
	callback = nm_auth_chain_get_data (chain, "callback");
	callback_data = nm_auth_chain_get_data (chain, "callback-data");
	caller_uid = nm_auth_chain_get_data_ulong (chain, "caller-uid");

	callback (self, added, error, context, callback_data);

	/* Send agent-owned secrets to the agents */
	if (!error && added)
		send_agent_owned_secrets (self, added, caller_uid);

	g_clear_error (&error);
	nm_auth_chain_unref (chain);
}

static void
add_cb (NMSettings *self,
        NMSettingsConnection *connection,
        GError *error,
        DBusGMethodInvocation *context,
        gpointer user_data)
{
	if (error)
		dbus_g_method_return_error (context, error);
	else
		dbus_g_method_return (context, nm_connection_get_path (NM_CONNECTION (connection)));
}

void
nm_settings_add_connection (NMSettings *self,
                            NMConnection *connection,
                            DBusGMethodInvocation *context,
                            NMSettingsAddCallback callback,
                            gpointer user_data)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	NMSettingConnection *s_con;
	NMAuthChain *chain;
	GError *error = NULL, *tmp_error = NULL;
	gulong caller_uid = G_MAXULONG;
	char *error_desc = NULL;
	const char *perm;

	/* Connection must be valid, of course */
	if (!nm_connection_verify (connection, &tmp_error)) {
		error = g_error_new (NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "The connection was invalid: %s",
		                     tmp_error ? tmp_error->message : "(unknown)");
		g_error_free (tmp_error);
		callback (self, NULL, error, context, user_data);
		g_error_free (error);
		return;
	}

	/* Do any of the plugins support adding? */
	if (!get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS)) {
		error = g_error_new_literal (NM_SETTINGS_ERROR,
		                             NM_SETTINGS_ERROR_ADD_NOT_SUPPORTED,
		                             "None of the registered plugins support add.");
		callback (self, NULL, error, context, user_data);
		g_error_free (error);
		return;
	}

	/* Get the caller's UID */
	if (!nm_auth_get_caller_uid (context, priv->dbus_mgr, &caller_uid, &error_desc)) {
		error = g_error_new (NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_NOT_PRIVILEGED,
		                     "Unable to determine UID of request: %s.",
		                     error_desc ? error_desc : "(unknown)");
		g_free (error_desc);
		callback (self, NULL, error, context, user_data);
		g_error_free (error);
		return;
	}

	/* Ensure the caller's username exists in the connection's permissions,
	 * or that the permissions is empty (ie, visible by everyone).
	 */
	if (0 != caller_uid) {
		if (!nm_auth_uid_in_acl (connection, priv->session_monitor, caller_uid, &error_desc)) {
			error = g_error_new_literal (NM_SETTINGS_ERROR,
			                             NM_SETTINGS_ERROR_NOT_PRIVILEGED,
			                             error_desc);
			g_free (error_desc);
			callback (self, NULL, error, context, user_data);
			g_error_free (error);
			return;
		}

		/* Caller is allowed to add this connection */
	}

	/* If the caller is the only user in the connection's permissions, then
	 * we use the 'modify.own' permission instead of 'modify.system'.  If the
	 * request affects more than just the caller, require 'modify.system'.
	 */
	s_con = nm_connection_get_setting_connection (connection);
	g_assert (s_con);
	if (nm_setting_connection_get_num_permissions (s_con) == 1)
		perm = NM_AUTH_PERMISSION_SETTINGS_MODIFY_OWN;
	else
		perm = NM_AUTH_PERMISSION_SETTINGS_MODIFY_SYSTEM;

	/* Otherwise validate the user request */
	chain = nm_auth_chain_new (context, NULL, pk_add_cb, self);
	g_assert (chain);
	priv->auths = g_slist_append (priv->auths, chain);
	nm_auth_chain_add_call (chain, perm, TRUE);
	nm_auth_chain_set_data (chain, "perm", (gpointer) perm, NULL);
	nm_auth_chain_set_data (chain, "connection", g_object_ref (connection), g_object_unref);
	nm_auth_chain_set_data (chain, "callback", callback, NULL);
	nm_auth_chain_set_data (chain, "callback-data", user_data, NULL);
	nm_auth_chain_set_data_ulong (chain, "caller-uid", caller_uid);
}

static void
impl_settings_add_connection (NMSettings *self,
                              GHashTable *settings,
                              DBusGMethodInvocation *context)
{
	NMConnection *connection;
	GError *error = NULL;

	connection = nm_connection_new_from_hash (settings, &error);
	if (connection) {
		nm_settings_add_connection (self, connection, context, add_cb, NULL);
		g_object_unref (connection);
	} else {
		g_assert (error);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
	}
}

static void
pk_hostname_cb (NMAuthChain *chain,
                GError *chain_error,
                DBusGMethodInvocation *context,
                gpointer user_data)
{
	NMSettings *self = NM_SETTINGS (user_data);
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	NMAuthCallResult result;
	gboolean success = FALSE;
	GError *error = NULL;
	GSList *iter;
	const char *hostname;

	priv->auths = g_slist_remove (priv->auths, chain);

	/* If our NMSettingsConnection is already gone, do nothing */
	if (chain_error) {
		error = g_error_new (NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_GENERAL,
		                     "Error checking authorization: %s",
		                     chain_error->message ? chain_error->message : "(unknown)");
		goto done;
	}

	result = nm_auth_chain_get_result (chain, NM_AUTH_PERMISSION_SETTINGS_MODIFY_HOSTNAME);

	/* Caller didn't successfully authenticate */
	if (result != NM_AUTH_CALL_RESULT_YES) {
		error = g_error_new_literal (NM_SETTINGS_ERROR,
		                             NM_SETTINGS_ERROR_NOT_PRIVILEGED,
		                             "Insufficient privileges.");
		goto done;
	}

	/* Set the hostname in all plugins */
	hostname = nm_auth_chain_get_data (chain, "hostname");
	for (iter = priv->plugins; iter; iter = iter->next) {
		NMSystemConfigInterfaceCapabilities caps = NM_SYSTEM_CONFIG_INTERFACE_CAP_NONE;

		g_object_get (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_CAPABILITIES, &caps, NULL);
		if (caps & NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_HOSTNAME) {
			g_object_set (G_OBJECT (iter->data), NM_SYSTEM_CONFIG_INTERFACE_HOSTNAME, hostname, NULL);
			success = TRUE;
		}
	}

	if (!success) {
		error = g_error_new_literal (NM_SETTINGS_ERROR,
		                             NM_SETTINGS_ERROR_SAVE_HOSTNAME_FAILED,
		                             "Saving the hostname failed.");
	}

done:
	if (error)
		dbus_g_method_return_error (context, error);
	else
		dbus_g_method_return (context);

	g_clear_error (&error);
	nm_auth_chain_unref (chain);
}

static void
impl_settings_save_hostname (NMSettings *self,
                             const char *hostname,
                             DBusGMethodInvocation *context)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	NMAuthChain *chain;
	GError *error = NULL;

	/* Do any of the plugins support setting the hostname? */
	if (!get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_HOSTNAME)) {
		error = g_error_new_literal (NM_SETTINGS_ERROR,
		                             NM_SETTINGS_ERROR_SAVE_HOSTNAME_NOT_SUPPORTED,
		                             "None of the registered plugins support setting the hostname.");
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	/* Otherwise validate the user request */
	chain = nm_auth_chain_new (context, NULL, pk_hostname_cb, self);
	g_assert (chain);
	priv->auths = g_slist_append (priv->auths, chain);
	nm_auth_chain_add_call (chain, NM_AUTH_PERMISSION_SETTINGS_MODIFY_HOSTNAME, TRUE);
	nm_auth_chain_set_data (chain, "hostname", g_strdup (hostname), g_free);
}

static gboolean
have_connection_for_device (NMSettings *self, GByteArray *mac, NMDevice *device)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GHashTableIter iter;
	gpointer data;
	NMSettingConnection *s_con;
	NMSettingWired *s_wired;
	NMSettingInfiniband *s_infiniband;
	const GByteArray *setting_mac;
	gboolean ret = FALSE;

	g_return_val_if_fail (NM_IS_SETTINGS (self), FALSE);
	g_return_val_if_fail (mac != NULL, FALSE);

	/* Find a wired connection locked to the given MAC address, if any */
	g_hash_table_iter_init (&iter, priv->connections);
	while (g_hash_table_iter_next (&iter, NULL, &data)) {
		NMConnection *connection = NM_CONNECTION (data);
		const char *ctype, *iface;

		s_con = nm_connection_get_setting_connection (connection);
		ctype = nm_setting_connection_get_connection_type (s_con);

		iface = nm_connection_get_virtual_iface_name (connection);
		if (iface) {
			if (!strcmp (iface, nm_device_get_iface (device))) {
				ret = TRUE;
				break;
			} else
				continue;
		}

		if (   strcmp (ctype, NM_SETTING_WIRED_SETTING_NAME)
		    && strcmp (ctype, NM_SETTING_INFINIBAND_SETTING_NAME)
		    && strcmp (ctype, NM_SETTING_PPPOE_SETTING_NAME))
			continue;

		s_wired = nm_connection_get_setting_wired (connection);
		s_infiniband = nm_connection_get_setting_infiniband (connection);

		/* No wired setting; therefore the PPPoE connection applies to any device */
		if (!s_wired && !strcmp (ctype, NM_SETTING_PPPOE_SETTING_NAME)) {
			ret = TRUE;
			break;
		}

		g_assert (s_wired != NULL || s_infiniband != NULL);

		setting_mac = s_wired ?
			nm_setting_wired_get_mac_address (s_wired) :
			nm_setting_infiniband_get_mac_address (s_infiniband);
		if (setting_mac) {
			/* A connection mac-locked to this device */
			if (mac->len == setting_mac->len &&
				!memcmp (setting_mac->data, mac->data, mac->len)) {
				ret = TRUE;
				break;
			}
		} else {
			/* A connection that applies to any wired device */
			ret = TRUE;
			break;
		}
	}

	return ret;
}

/* Search through the list of blacklisted MAC addresses in the config file. */
static gboolean
is_mac_auto_wired_blacklisted (NMSettings *self, const GByteArray *mac, int hwaddr_type)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GKeyFile *config;
	char **list, **iter;
	gboolean found = FALSE;

	g_return_val_if_fail (mac != NULL, FALSE);

	if (!priv->config_file)
		return FALSE;

	config = g_key_file_new ();
	if (!config) {
		nm_log_warn (LOGD_SETTINGS, "not enough memory to load config file.");
		return FALSE;
	}

	g_key_file_set_list_separator (config, ',');
	if (!g_key_file_load_from_file (config, priv->config_file, G_KEY_FILE_NONE, NULL))
		goto out;

	list = g_key_file_get_string_list (config, "main", CONFIG_KEY_NO_AUTO_DEFAULT, NULL, NULL);
	for (iter = list; iter && *iter; iter++) {
		guint8 *candidate, buffer[NM_UTILS_HWADDR_LEN_MAX];

		if (strcmp (g_strstrip (*iter), "*") == 0) {
			found = TRUE;
			break;
		}

		candidate = nm_utils_hwaddr_aton (*iter, hwaddr_type, buffer);
		if (candidate && !memcmp (mac->data, candidate, mac->len)) {
			found = TRUE;
			break;
		}
	}

	if (list)
		g_strfreev (list);

out:
	g_key_file_free (config);
	return found;
}

#define DEFAULT_WIRED_TAG "default-wired"

static void
default_wired_deleted (NMDefaultWiredConnection *wired,
                       const GByteArray *mac,
                       NMSettings *self)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	NMSettingConnection *s_con;
	int hwaddr_type;
	char *tmp;
	GKeyFile *config;
	char **list, **iter, **updated;
	gboolean found = FALSE;
	gsize len = 0, i;
	char *data;

	/* If there was no config file specified, there's nothing to do */
	if (!priv->config_file)
		goto cleanup;

	/* When the default wired connection is removed (either deleted or saved
	 * to a new persistent connection by a plugin), write the MAC address of
	 * the wired device to the config file and don't create a new default wired
	 * connection for that device again.
	 */

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (wired));
	g_assert (s_con);

	/* Ignore removals of read-only connections, since they couldn't have
	 * been removed by the user.
	 */
	if (nm_setting_connection_get_read_only (s_con))
		goto cleanup;

	config = g_key_file_new ();
	if (!config)
		goto cleanup;

	if (nm_connection_get_setting_wired (NM_CONNECTION (wired)))
		hwaddr_type = ARPHRD_ETHER;
	else if (nm_connection_get_setting_infiniband (NM_CONNECTION (wired)))
		hwaddr_type = ARPHRD_INFINIBAND;
	else
		goto cleanup;

	g_key_file_set_list_separator (config, ',');
	g_key_file_load_from_file (config, priv->config_file, G_KEY_FILE_KEEP_COMMENTS, NULL);

	list = g_key_file_get_string_list (config, "main", CONFIG_KEY_NO_AUTO_DEFAULT, &len, NULL);
	for (iter = list; iter && *iter; iter++) {
		guint8 *candidate, buffer[NM_UTILS_HWADDR_LEN_MAX];

		if (strcmp (g_strstrip (*iter), "*") == 0) {
			found = TRUE;
			break;
		}

		candidate = nm_utils_hwaddr_aton (*iter, hwaddr_type, buffer);
		if (candidate && !memcmp (mac->data, candidate, mac->len)) {
			found = TRUE;
			break;
		}
	}

	/* Add this device's MAC to the list */
	if (!found) {
		tmp = nm_utils_hwaddr_ntoa (mac->data, hwaddr_type);

		/* New list; size + 1 for the new element, + 1 again for ending NULL */
		updated = g_malloc0 (sizeof (char*) * (len + 2));

		/* Copy original list and add new MAC */
		for (i = 0; list && list[i]; i++)
			updated[i] = list[i];
		updated[i++] = tmp;
		updated[i] = NULL;

		g_key_file_set_string_list (config,
		                            "main", CONFIG_KEY_NO_AUTO_DEFAULT,
		                            (const char **) updated,
		                            len + 2);
		/* g_free() not g_strfreev() since 'updated' isn't a deep-copy */
		g_free (updated);
		g_free (tmp);

		data = g_key_file_to_data (config, &len, NULL);
		if (data) {
			g_file_set_contents (priv->config_file, data, len, NULL);
			g_free (data);
		}
	}

	if (list)
		g_strfreev (list);
	g_key_file_free (config);

cleanup:
	g_object_set_data (G_OBJECT (nm_default_wired_connection_get_device (wired)),
	                   DEFAULT_WIRED_TAG,
	                   NULL);
}

static void
delete_cb (NMSettingsConnection *connection, GError *error, gpointer user_data)
{
}

static void
default_wired_try_update (NMDefaultWiredConnection *wired,
                          NMSettings *self)
{
	GError *error = NULL;
	const char *id;
	NMSettingsConnection *added;

	/* Try to move this default wired conneciton to a plugin so that it has
	 * persistent storage.
	 */

	/* Keep it alive over removal so we can re-add it if we need to */
	g_object_ref (wired);

	id = nm_connection_get_id (NM_CONNECTION (wired));
	g_assert (id);

	remove_default_wired_connection (self, NM_SETTINGS_CONNECTION (wired), FALSE);
	added = add_new_connection (self, NM_CONNECTION (wired), &error);
	if (added) {
		nm_settings_connection_delete (NM_SETTINGS_CONNECTION (wired), delete_cb, NULL);

		g_object_set_data (G_OBJECT (nm_default_wired_connection_get_device (wired)),
		                   DEFAULT_WIRED_TAG,
		                   NULL);
		nm_log_info (LOGD_SETTINGS, "Saved default wired connection '%s' to persistent storage", id);
	} else {
		nm_log_warn (LOGD_SETTINGS, "couldn't save default wired connection '%s': %d / %s",
			         id,
			         error ? error->code : -1,
			         (error && error->message) ? error->message : "(unknown)");
		g_clear_error (&error);

		/* If there was an error, don't destroy the default wired connection,
		 * but add it back to the system settings service. Connection is already
		 * exported on the bus, don't export it again, thus do_export == FALSE.
		 */
		claim_connection (self, NM_SETTINGS_CONNECTION (wired), FALSE);
	}

	g_object_unref (wired);
}

void
nm_settings_device_added (NMSettings *self, NMDevice *device)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GByteArray *mac = NULL;
	guint8 *hwaddr;
	int hwaddr_type;
	NMDefaultWiredConnection *wired;
	gboolean read_only = TRUE;
	const char *id;
	char *defname;

	if (!NM_IS_DEVICE_WIRED (device))
		return;

	/* If the device isn't managed or it already has a default wired connection,
	 * ignore it.
	 */
	if (   !nm_device_get_managed (device)
	    || g_object_get_data (G_OBJECT (device), DEFAULT_WIRED_TAG))
		return;

	hwaddr = nm_device_wired_get_hwaddr (NM_DEVICE_WIRED (device));
	hwaddr_type = nm_device_wired_get_hwaddr_type (NM_DEVICE_WIRED (device));

	mac = g_byte_array_new ();
	g_byte_array_append (mac, hwaddr, nm_utils_hwaddr_len (hwaddr_type));

	if (   have_connection_for_device (self, mac, device)
		|| is_mac_auto_wired_blacklisted (self, mac, hwaddr_type))
		goto ignore;

	if (get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS))
		read_only = FALSE;

	defname = nm_settings_utils_get_default_wired_name (priv->connections);
	wired = nm_default_wired_connection_new (mac, device, defname, read_only);
	g_free (defname);
	if (!wired)
		goto ignore;

	id = nm_connection_get_id (NM_CONNECTION (wired));
	g_assert (id);

	nm_log_info (LOGD_SETTINGS, "Added default wired connection '%s' for %s",
	             id, nm_device_get_udi (device));

	g_signal_connect (wired, "try-update", (GCallback) default_wired_try_update, self);
	g_signal_connect (wired, "deleted", (GCallback) default_wired_deleted, self);
	claim_connection (self, NM_SETTINGS_CONNECTION (wired), TRUE);
	g_object_unref (wired);

	g_object_set_data (G_OBJECT (device), DEFAULT_WIRED_TAG, wired);

ignore:
	g_byte_array_free (mac, TRUE);
}

void
nm_settings_device_removed (NMSettings *self, NMDevice *device)
{
	NMDefaultWiredConnection *connection;

	if (!NM_IS_DEVICE_WIRED (device))
		return;

	connection = (NMDefaultWiredConnection *) g_object_get_data (G_OBJECT (device), DEFAULT_WIRED_TAG);
	if (connection)
		remove_default_wired_connection (self, NM_SETTINGS_CONNECTION (connection), TRUE);
}

/***************************************************************/

NMSettings *
nm_settings_new (const char *config_file,
                 const char **plugins,
                 GError **error)
{
	NMSettings *self;
	NMSettingsPrivate *priv;
	GObject *keyfile_plugin;

	self = g_object_new (NM_TYPE_SETTINGS, NULL);
	if (!self)
		return NULL;

	priv = NM_SETTINGS_GET_PRIVATE (self);

	priv->config_file = g_strdup (config_file);
	priv->dbus_mgr = nm_dbus_manager_get ();
	priv->bus = nm_dbus_manager_get_connection (priv->dbus_mgr);

	if (plugins) {
		/* Load the plugins; fail if a plugin is not found. */
		if (!load_plugins (self, plugins, error)) {
			g_object_unref (self);
			return NULL;
		}
	}

	/* Add the keyfile plugin last */
	keyfile_plugin = nm_settings_keyfile_plugin_new (config_file);
	g_assert (keyfile_plugin);
	add_plugin (self, NM_SYSTEM_CONFIG_INTERFACE (keyfile_plugin));

	unmanaged_specs_changed (NULL, self);

	dbus_g_connection_register_g_object (priv->bus, NM_DBUS_PATH_SETTINGS, G_OBJECT (self));
	return self;
}

static void
nm_settings_init (NMSettings *self)
{
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);

	priv->connections = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_object_unref);

	priv->session_monitor = nm_session_monitor_get ();

	/* Hold a reference to the agent manager so it stays alive; the only
	 * other holders are NMSettingsConnection objects which are often
	 * transient, and we don't want the agent manager to get destroyed and
	 * recreated often.
	 */
	priv->agent_mgr = nm_agent_manager_get ();

	g_signal_connect (priv->agent_mgr, "agent-registered", G_CALLBACK (secret_agent_registered), self);
}

static void
dispose (GObject *object)
{
	NMSettings *self = NM_SETTINGS (object);
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);
	GSList *iter;

	for (iter = priv->auths; iter; iter = g_slist_next (iter))
		nm_auth_chain_unref ((NMAuthChain *) iter->data);
	g_slist_free (priv->auths);

	g_object_unref (priv->dbus_mgr);

	g_object_unref (priv->session_monitor);
	g_object_unref (priv->agent_mgr);

	G_OBJECT_CLASS (nm_settings_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMSettings *self = NM_SETTINGS (object);
	NMSettingsPrivate *priv = NM_SETTINGS_GET_PRIVATE (self);

	g_hash_table_destroy (priv->connections);

	clear_unmanaged_specs (self);

	g_slist_foreach (priv->plugins, (GFunc) g_object_unref, NULL);
	g_slist_free (priv->plugins);

	g_free (priv->config_file);

	G_OBJECT_CLASS (nm_settings_parent_class)->finalize (object);
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	NMSettings *self = NM_SETTINGS (object);
	const GSList *specs, *iter;
	GSList *copy = NULL;

	switch (prop_id) {
	case PROP_UNMANAGED_SPECS:
		specs = nm_settings_get_unmanaged_specs (self);
		for (iter = specs; iter; iter = g_slist_next (iter))
			copy = g_slist_append (copy, g_strdup (iter->data));
		g_value_take_boxed (value, copy);
		break;
	case PROP_HOSTNAME:
		g_value_take_string (value, nm_settings_get_hostname (self));

		/* Don't ever pass NULL through D-Bus */
		if (!g_value_get_string (value))
			g_value_set_static_string (value, "");
		break;
	case PROP_CAN_MODIFY:
		g_value_set_boolean (value, !!get_plugin (self, NM_SYSTEM_CONFIG_INTERFACE_CAP_MODIFY_CONNECTIONS));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
nm_settings_class_init (NMSettingsClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);
	
	g_type_class_add_private (class, sizeof (NMSettingsPrivate));

	/* virtual methods */
	object_class->notify = notify;
	object_class->get_property = get_property;
	object_class->dispose = dispose;
	object_class->finalize = finalize;

	/* properties */

	g_object_class_install_property
		(object_class, PROP_UNMANAGED_SPECS,
		 g_param_spec_boxed (NM_SETTINGS_UNMANAGED_SPECS,
							 "Unamanged device specs",
							 "Unmanaged device specs",
							 DBUS_TYPE_G_LIST_OF_STRING,
							 G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_HOSTNAME,
		 g_param_spec_string (NM_SETTINGS_HOSTNAME,
		                      "Hostname",
		                      "Persistent hostname",
		                      NULL,
		                      G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_CAN_MODIFY,
		 g_param_spec_boolean (NM_SETTINGS_CAN_MODIFY,
		                       "CanModify",
		                       "Can modify anything (hostname, connections, etc)",
		                       FALSE,
		                       G_PARAM_READABLE));

	/* signals */
	signals[PROPERTIES_CHANGED] = 
	                g_signal_new ("properties-changed",
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              G_STRUCT_OFFSET (NMSettingsClass, properties_changed),
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__BOXED,
	                              G_TYPE_NONE, 1, DBUS_TYPE_G_MAP_OF_VARIANT);
	signals[CONNECTION_ADDED] = 
	                g_signal_new (NM_SETTINGS_SIGNAL_CONNECTION_ADDED,
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              G_STRUCT_OFFSET (NMSettingsClass, connection_added),
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__OBJECT,
	                              G_TYPE_NONE, 1, G_TYPE_OBJECT);

	signals[CONNECTION_UPDATED] = 
	                g_signal_new (NM_SETTINGS_SIGNAL_CONNECTION_UPDATED,
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              G_STRUCT_OFFSET (NMSettingsClass, connection_updated),
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__OBJECT,
	                              G_TYPE_NONE, 1, G_TYPE_OBJECT);

	signals[CONNECTION_REMOVED] = 
	                g_signal_new (NM_SETTINGS_SIGNAL_CONNECTION_REMOVED,
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              G_STRUCT_OFFSET (NMSettingsClass, connection_removed),
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__OBJECT,
	                              G_TYPE_NONE, 1, G_TYPE_OBJECT);

	signals[CONNECTION_VISIBILITY_CHANGED] = 
	                g_signal_new (NM_SETTINGS_SIGNAL_CONNECTION_VISIBILITY_CHANGED,
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              G_STRUCT_OFFSET (NMSettingsClass, connection_visibility_changed),
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__OBJECT,
	                              G_TYPE_NONE, 1, G_TYPE_OBJECT);

	signals[CONNECTIONS_LOADED] = 
	                g_signal_new (NM_SETTINGS_SIGNAL_CONNECTIONS_LOADED,
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST,
	                              G_STRUCT_OFFSET (NMSettingsClass, connections_loaded),
	                              NULL, NULL,
	                              g_cclosure_marshal_VOID__VOID,
	                              G_TYPE_NONE, 0);

	signals[AGENT_REGISTERED] =
		g_signal_new (NM_SETTINGS_SIGNAL_AGENT_REGISTERED,
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              G_STRUCT_OFFSET (NMSettingsClass, agent_registered),
		              NULL, NULL,
		              g_cclosure_marshal_VOID__OBJECT,
		              G_TYPE_NONE, 1, G_TYPE_OBJECT);


	signals[NEW_CONNECTION] = 
	                g_signal_new ("new-connection",
	                              G_OBJECT_CLASS_TYPE (object_class),
	                              G_SIGNAL_RUN_FIRST, 0, NULL, NULL,
	                              g_cclosure_marshal_VOID__OBJECT,
	                              G_TYPE_NONE, 1, G_TYPE_OBJECT);

	dbus_g_error_domain_register (NM_SETTINGS_ERROR,
	                              NM_DBUS_IFACE_SETTINGS,
	                              NM_TYPE_SETTINGS_ERROR);

	/* And register all the settings errors with D-Bus */
	dbus_g_error_domain_register (NM_CONNECTION_ERROR, NULL, NM_TYPE_CONNECTION_ERROR);
	dbus_g_error_domain_register (NM_SETTING_802_1X_ERROR, NULL, NM_TYPE_SETTING_802_1X_ERROR);
	dbus_g_error_domain_register (NM_SETTING_BLUETOOTH_ERROR, NULL, NM_TYPE_SETTING_BLUETOOTH_ERROR);
	dbus_g_error_domain_register (NM_SETTING_CDMA_ERROR, NULL, NM_TYPE_SETTING_CDMA_ERROR);
	dbus_g_error_domain_register (NM_SETTING_CONNECTION_ERROR, NULL, NM_TYPE_SETTING_CONNECTION_ERROR);
	dbus_g_error_domain_register (NM_SETTING_GSM_ERROR, NULL, NM_TYPE_SETTING_GSM_ERROR);
	dbus_g_error_domain_register (NM_SETTING_IP4_CONFIG_ERROR, NULL, NM_TYPE_SETTING_IP4_CONFIG_ERROR);
	dbus_g_error_domain_register (NM_SETTING_IP6_CONFIG_ERROR, NULL, NM_TYPE_SETTING_IP6_CONFIG_ERROR);
	dbus_g_error_domain_register (NM_SETTING_OLPC_MESH_ERROR, NULL, NM_TYPE_SETTING_OLPC_MESH_ERROR);
	dbus_g_error_domain_register (NM_SETTING_PPP_ERROR, NULL, NM_TYPE_SETTING_PPP_ERROR);
	dbus_g_error_domain_register (NM_SETTING_PPPOE_ERROR, NULL, NM_TYPE_SETTING_PPPOE_ERROR);
	dbus_g_error_domain_register (NM_SETTING_SERIAL_ERROR, NULL, NM_TYPE_SETTING_SERIAL_ERROR);
	dbus_g_error_domain_register (NM_SETTING_VPN_ERROR, NULL, NM_TYPE_SETTING_VPN_ERROR);
	dbus_g_error_domain_register (NM_SETTING_WIRED_ERROR, NULL, NM_TYPE_SETTING_WIRED_ERROR);
	dbus_g_error_domain_register (NM_SETTING_WIRELESS_SECURITY_ERROR, NULL, NM_TYPE_SETTING_WIRELESS_SECURITY_ERROR);
	dbus_g_error_domain_register (NM_SETTING_WIRELESS_ERROR, NULL, NM_TYPE_SETTING_WIRELESS_ERROR);
	dbus_g_error_domain_register (NM_SETTING_ERROR, NULL, NM_TYPE_SETTING_ERROR);

	dbus_g_object_type_install_info (NM_TYPE_SETTINGS, &dbus_glib_nm_settings_object_info);

}

