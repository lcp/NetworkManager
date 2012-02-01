/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager system settings service
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
 * (C) Copyright 2008 Novell, Inc.
 * (C) Copyright 2008 - 2011 Red Hat, Inc.
 */

#include "config.h"

#include <string.h>
#include <netinet/ether.h>

#include <NetworkManager.h>
#include <dbus/dbus-glib-lowlevel.h>
#include <nm-setting-connection.h>
#include <nm-setting-vpn.h>
#include <nm-utils.h>

#include "nm-settings-connection.h"
#include "nm-session-monitor.h"
#include "nm-dbus-manager.h"
#include "nm-settings-error.h"
#include "nm-dbus-glib-types.h"
#include "nm-logging.h"
#include "nm-manager-auth.h"
#include "nm-marshal.h"
#include "nm-agent-manager.h"
#include "NetworkManagerUtils.h"

#define SETTINGS_TIMESTAMPS_FILE  LOCALSTATEDIR"/lib/NetworkManager/timestamps"
#define SETTINGS_SEEN_BSSIDS_FILE LOCALSTATEDIR"/lib/NetworkManager/seen-bssids"

static void impl_settings_connection_get_settings (NMSettingsConnection *connection,
                                                   DBusGMethodInvocation *context);

static void impl_settings_connection_update (NMSettingsConnection *connection,
                                             GHashTable *new_settings,
                                             DBusGMethodInvocation *context);

static void impl_settings_connection_delete (NMSettingsConnection *connection,
                                             DBusGMethodInvocation *context);

static void impl_settings_connection_get_secrets (NMSettingsConnection *connection,
                                                  const gchar *setting_name,
                                                  DBusGMethodInvocation *context);

#include "nm-settings-connection-glue.h"

G_DEFINE_TYPE (NMSettingsConnection, nm_settings_connection, NM_TYPE_CONNECTION)

#define NM_SETTINGS_CONNECTION_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), \
                                               NM_TYPE_SETTINGS_CONNECTION, \
                                               NMSettingsConnectionPrivate))

enum {
	PROP_0 = 0,
	PROP_VISIBLE,
};

enum {
	UPDATED,
	REMOVED,
	UNREGISTER,
	LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

typedef struct {
	gboolean disposed;

	NMDBusManager *dbus_mgr;
	NMAgentManager *agent_mgr;
	NMSessionMonitor *session_monitor;
	guint session_changed_id;

	GSList *pending_auths; /* List of pending authentication requests */
	gboolean visible; /* Is this connection is visible by some session? */
	GSList *reqs;  /* in-progress secrets requests */

	/* Caches secrets from on-disk connections; were they not cached any
	 * call to nm_connection_clear_secrets() wipes them out and we'd have
	 * to re-read them from disk which defeats the purpose of having the
	 * connection in-memory at all.
	 */
	NMConnection *system_secrets;

	/* Caches secrets from agents during the activation process; if new system
	 * secrets are returned from an agent, they get written out to disk,
	 * triggering a re-read of the connection, which reads only system
	 * secrets, and would wipe out any agent-owned or not-saved secrets the
	 * agent also returned.
	 */
	NMConnection *agent_secrets;

	guint64 timestamp;   /* Up-to-date timestamp of connection use */
	GHashTable *seen_bssids; /* Up-to-date BSSIDs that's been seen for the connection */
} NMSettingsConnectionPrivate;

/**************************************************************/

/* Return TRUE to continue, FALSE to stop */
typedef gboolean (*ForEachSecretFunc) (GHashTableIter *iter,
                                       NMSettingSecretFlags flags,
                                       gpointer user_data);

static void
for_each_secret (NMConnection *connection,
                 GHashTable *secrets,
                 ForEachSecretFunc callback,
                 gpointer callback_data)
{
	GHashTableIter iter;
	const char *setting_name;
	GHashTable *setting_hash;

	/* This function, given a hash of hashes representing new secrets of
	 * an NMConnection, walks through each toplevel hash (which represents a
	 * NMSetting), and for each setting, walks through that setting hash's
	 * properties.  For each property that's a secret, it will check that
	 * secret's flags in the backing NMConnection object, and call a supplied
	 * callback.
	 *
	 * The one complexity is that the VPN setting's 'secrets' property is
	 * *also* a hash table (since the key/value pairs are arbitrary and known
	 * only to the VPN plugin itself).  That means we have three levels of
	 * GHashTables that we potentially have to traverse here.  When we hit the
	 * VPN setting's 'secrets' property, we special-case that and iterate over
	 * each item in that 'secrets' hash table, calling the supplied callback
	 * each time.
	 */

	/* Walk through the list of setting hashes */
	g_hash_table_iter_init (&iter, secrets);
	while (g_hash_table_iter_next (&iter, (gpointer) &setting_name, (gpointer) &setting_hash)) {
		NMSetting *setting;
		GHashTableIter secret_iter;
		const char *secret_name;
		GValue *val;

		/* Get the actual NMSetting from the connection so we can get secret flags
		 * from the connection data, since flags aren't secrets.  What we're
		 * iterating here is just the secrets, not a whole connection.
		 */
		setting = nm_connection_get_setting_by_name (connection, setting_name);
		if (setting == NULL)
			continue;

		/* Walk through the list of keys in each setting hash */
		g_hash_table_iter_init (&secret_iter, setting_hash);
		while (g_hash_table_iter_next (&secret_iter, (gpointer) &secret_name, (gpointer) &val)) {
			NMSettingSecretFlags secret_flags = NM_SETTING_SECRET_FLAG_NONE;

			/* VPN secrets need slightly different treatment here since the
			 * "secrets" property is actually a hash table of secrets.
			 */
			if (NM_IS_SETTING_VPN (setting) && (g_strcmp0 (secret_name, NM_SETTING_VPN_SECRETS) == 0)) {
				GHashTableIter vpn_secrets_iter;

				/* Iterate through each secret from the VPN hash in the overall secrets hash */
				g_hash_table_iter_init (&vpn_secrets_iter, g_value_get_boxed (val));
				while (g_hash_table_iter_next (&vpn_secrets_iter, (gpointer) &secret_name, NULL)) {
					secret_flags = NM_SETTING_SECRET_FLAG_NONE;
					nm_setting_get_secret_flags (setting, secret_name, &secret_flags, NULL);
					if (callback (&vpn_secrets_iter, secret_flags, callback_data) == FALSE)
						return;
				}
			} else {
				nm_setting_get_secret_flags (setting, secret_name, &secret_flags, NULL);
				if (callback (&secret_iter, secret_flags, callback_data) == FALSE)
					return;
			}
		}
	}
}

/**************************************************************/

static void
set_visible (NMSettingsConnection *self, gboolean new_visible)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);

	if (new_visible == priv->visible)
		return;
	priv->visible = new_visible;
	g_object_notify (G_OBJECT (self), NM_SETTINGS_CONNECTION_VISIBLE);
}

gboolean
nm_settings_connection_is_visible (NMSettingsConnection *self)
{
	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_SETTINGS_CONNECTION (self), FALSE);

	return NM_SETTINGS_CONNECTION_GET_PRIVATE (self)->visible;
}

void
nm_settings_connection_recheck_visibility (NMSettingsConnection *self)
{
	NMSettingsConnectionPrivate *priv;
	NMSettingConnection *s_con;
	guint32 num, i;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_SETTINGS_CONNECTION (self));

	priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (self));
	g_assert (s_con);

	/* Check every user in the ACL for a session */
	num = nm_setting_connection_get_num_permissions (s_con);
	if (num == 0) {
		/* Visible to all */
		set_visible (self, TRUE);
		return;
	}

	for (i = 0; i < num; i++) {
		const char *puser;

		if (nm_setting_connection_get_permission (s_con, i, NULL, &puser, NULL)) {
			if (nm_session_monitor_user_has_session (priv->session_monitor, puser, NULL, NULL)) {
				set_visible (self, TRUE);
				return;
			}
		}
	}

	set_visible (self, FALSE);
}

static void
session_changed_cb (NMSessionMonitor *self, gpointer user_data)
{
	nm_settings_connection_recheck_visibility (NM_SETTINGS_CONNECTION (user_data));
}

/**************************************************************/

/* Return TRUE if any active user in the connection's ACL has the given
 * permission without having to authorize for it via PolicyKit.  Connections
 * visible to everyone automatically pass the check.
 */
gboolean
nm_settings_connection_check_permission (NMSettingsConnection *self,
                                         const char *permission)
{
	NMSettingsConnectionPrivate *priv;
	NMSettingConnection *s_con;
	guint32 num, i;
	const char *puser;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_SETTINGS_CONNECTION (self), FALSE);

	priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);

	if (priv->visible == FALSE)
		return FALSE;

	s_con = nm_connection_get_setting_connection (NM_CONNECTION (self));
	g_assert (s_con);

	/* Check every user in the ACL for a session */
	num = nm_setting_connection_get_num_permissions (s_con);
	if (num == 0) {
		/* Visible to all so it's OK to auto-activate */
		return TRUE;
	}

	for (i = 0; i < num; i++) {
		/* For each user get their secret agent and check if that agent has the
		 * required permission.
		 *
		 * FIXME: what if the user isn't running an agent?  PolKit needs a bus
		 * name or a PID but if the user isn't running an agent they won't have
		 * either.
		 */
		if (nm_setting_connection_get_permission (s_con, i, NULL, &puser, NULL)) {
			NMSecretAgent *agent = nm_agent_manager_get_agent_by_user (priv->agent_mgr, puser);

			if (agent && nm_secret_agent_has_permission (agent, permission))
				return TRUE;
		}
	}

	return FALSE;
}

/**************************************************************/

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
update_system_secrets_cache (NMSettingsConnection *self)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);

	if (priv->system_secrets)
		g_object_unref (priv->system_secrets);
	priv->system_secrets = nm_connection_duplicate (NM_CONNECTION (self));

	/* Clear out non-system-owned and not-saved secrets */
	nm_connection_clear_secrets_with_flags (priv->system_secrets,
	                                        secrets_filter_cb,
	                                        GUINT_TO_POINTER (NM_SETTING_SECRET_FLAG_NONE));
}

static void
update_agent_secrets_cache (NMSettingsConnection *self, NMConnection *new)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	NMSettingSecretFlags filter_flags = NM_SETTING_SECRET_FLAG_NOT_SAVED | NM_SETTING_SECRET_FLAG_AGENT_OWNED;

	if (priv->agent_secrets)
		g_object_unref (priv->agent_secrets);
	priv->agent_secrets = nm_connection_duplicate (new ? new : NM_CONNECTION (self));

	/* Clear out non-system-owned secrets */
	nm_connection_clear_secrets_with_flags (priv->agent_secrets,
	                                        secrets_filter_cb,
	                                        GUINT_TO_POINTER (filter_flags));
}

static void
secrets_cleared_cb (NMSettingsConnection *self)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);

	/* Clear agent secrets when connection's secrets are cleared since agent
	 * secrets are transient.
	 */
	if (priv->agent_secrets)
		g_object_unref (priv->agent_secrets);
	priv->agent_secrets = NULL;
}

/* Update the settings of this connection to match that of 'new', taking care to
 * make a private copy of secrets.
 */
gboolean
nm_settings_connection_replace_settings (NMSettingsConnection *self,
                                         NMConnection *new,
                                         GError **error)
{
	NMSettingsConnectionPrivate *priv;
	GHashTable *new_settings, *hash = NULL;
	gboolean success = FALSE;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_SETTINGS_CONNECTION (self), FALSE);
	g_return_val_if_fail (new != NULL, FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (new), FALSE);

	priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);

	new_settings = nm_connection_to_hash (new, NM_SETTING_HASH_FLAG_ALL);
	g_assert (new_settings);
	if (nm_connection_replace_settings (NM_CONNECTION (self), new_settings, error)) {
		/* Cache the just-updated system secrets in case something calls
		 * nm_connection_clear_secrets() and clears them.
		 */
		update_system_secrets_cache (self);
		success = TRUE;

		/* Add agent and always-ask secrets back; they won't necessarily be
		 * in the replacement connection data if it was eg reread from disk.
		 */
		if (priv->agent_secrets) {
			hash = nm_connection_to_hash (priv->agent_secrets, NM_SETTING_HASH_FLAG_ONLY_SECRETS);
			if (hash) {
				success = nm_connection_update_secrets (NM_CONNECTION (self), NULL, hash, error);
				g_hash_table_destroy (hash);
			}
		}

		nm_settings_connection_recheck_visibility (self);
	}
	g_hash_table_destroy (new_settings);
	return success;
}

static void
ignore_cb (NMSettingsConnection *connection,
           GError *error,
           gpointer user_data)
{
}

/* Replaces the settings in this connection with those in 'new'. If any changes
 * are made, commits them to permanent storage and to any other subsystems
 * watching this connection. Before returning, 'callback' is run with the given
 * 'user_data' along with any errors encountered.
 */
void
nm_settings_connection_replace_and_commit (NMSettingsConnection *self,
                                           NMConnection *new,
                                           NMSettingsConnectionCommitFunc callback,
                                           gpointer user_data)
{
	GError *error = NULL;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_SETTINGS_CONNECTION (self));
	g_return_if_fail (new != NULL);
	g_return_if_fail (NM_IS_CONNECTION (new));

	if (!callback)
		callback = ignore_cb;

	/* Do nothing if there's nothing to update */
	if (nm_connection_compare (NM_CONNECTION (self),
	                           NM_CONNECTION (new),
	                           NM_SETTING_COMPARE_FLAG_EXACT)) {
		callback (self, NULL, user_data);
		return;
	}

	if (nm_settings_connection_replace_settings (self, new, &error)) {
		nm_settings_connection_commit_changes (self, callback, user_data);
	} else {
		callback (self, error, user_data);
		g_clear_error (&error);
	}
}

void
nm_settings_connection_commit_changes (NMSettingsConnection *connection,
                                       NMSettingsConnectionCommitFunc callback,
                                       gpointer user_data)
{
	g_return_if_fail (connection != NULL);
	g_return_if_fail (NM_IS_SETTINGS_CONNECTION (connection));
	g_return_if_fail (callback != NULL);

	if (NM_SETTINGS_CONNECTION_GET_CLASS (connection)->commit_changes) {
		NM_SETTINGS_CONNECTION_GET_CLASS (connection)->commit_changes (connection,
		                                                               callback,
		                                                               user_data);
	} else {
		GError *error = g_error_new (NM_SETTINGS_ERROR,
		                             NM_SETTINGS_ERROR_INTERNAL_ERROR,
		                             "%s: %s:%d commit_changes() unimplemented", __func__, __FILE__, __LINE__);
		callback (connection, error, user_data);
		g_error_free (error);
	}
}

void
nm_settings_connection_delete (NMSettingsConnection *connection,
                               NMSettingsConnectionDeleteFunc callback,
                               gpointer user_data)
{
	g_return_if_fail (connection != NULL);
	g_return_if_fail (NM_IS_SETTINGS_CONNECTION (connection));
	g_return_if_fail (callback != NULL);

	if (NM_SETTINGS_CONNECTION_GET_CLASS (connection)->delete) {
		NM_SETTINGS_CONNECTION_GET_CLASS (connection)->delete (connection,
		                                                       callback,
		                                                       user_data);
	} else {
		GError *error = g_error_new (NM_SETTINGS_ERROR,
		                             NM_SETTINGS_ERROR_INTERNAL_ERROR,
		                             "%s: %s:%d delete() unimplemented", __func__, __FILE__, __LINE__);
		callback (connection, error, user_data);
		g_error_free (error);
	}
}

static void
commit_changes (NMSettingsConnection *connection,
                NMSettingsConnectionCommitFunc callback,
                gpointer user_data)
{
	g_object_ref (connection);
	g_signal_emit (connection, signals[UPDATED], 0);
	callback (connection, NULL, user_data);
	g_object_unref (connection);
}

static void
remove_entry_from_db (NMSettingsConnection *connection, const char* db_name)
{
	GKeyFile *key_file;
	const char *db_file;

	if (strcmp (db_name, "timestamps") == 0)
		db_file = SETTINGS_TIMESTAMPS_FILE;
	else if (strcmp (db_name, "seen-bssids") == 0)
		db_file = SETTINGS_SEEN_BSSIDS_FILE;
	else
		return;

	key_file = g_key_file_new ();
	if (g_key_file_load_from_file (key_file, db_file, G_KEY_FILE_KEEP_COMMENTS, NULL)) {
		const char *connection_uuid;
		char *data;
		gsize len;
		GError *error = NULL;

		connection_uuid = nm_connection_get_uuid (NM_CONNECTION (connection));

		g_key_file_remove_key (key_file, db_name, connection_uuid, NULL);
		data = g_key_file_to_data (key_file, &len, &error);
		if (data) {
			g_file_set_contents (db_file, data, len, &error);
			g_free (data);
		}
		if (error) {
			nm_log_warn (LOGD_SETTINGS, "error writing %s file '%s': %s", db_name, db_file, error->message);
			g_error_free (error);
		}
	}
	g_key_file_free (key_file);
}

static void
do_delete (NMSettingsConnection *connection,
           NMSettingsConnectionDeleteFunc callback,
           gpointer user_data)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (connection);
	NMConnection *for_agents;

	g_object_ref (connection);
	set_visible (connection, FALSE);

	/* Tell agents to remove secrets for this connection */
	for_agents = nm_connection_duplicate (NM_CONNECTION (connection));
	nm_connection_clear_secrets (for_agents);
	nm_agent_manager_delete_secrets (priv->agent_mgr, for_agents, FALSE, 0);
	g_object_unref (for_agents);

	/* Remove timestamp from timestamps database file */
	remove_entry_from_db (connection, "timestamps");

	/* Remove connection from seen-bssids database file */
	remove_entry_from_db (connection, "seen-bssids");

	/* Signal the connection is removed and deleted */
	g_signal_emit (connection, signals[REMOVED], 0);
	callback (connection, NULL, user_data);
	g_object_unref (connection);
}

/**************************************************************/

static gboolean
supports_secrets (NMSettingsConnection *connection, const char *setting_name)
{
	/* All secrets supported */
	return TRUE;
}

static gboolean
clear_nonagent_secrets (GHashTableIter *iter,
                        NMSettingSecretFlags flags,
                        gpointer user_data)
{
	if (flags != NM_SETTING_SECRET_FLAG_AGENT_OWNED)
		g_hash_table_iter_remove (iter);
	return TRUE;
}

static gboolean
clear_unsaved_secrets (GHashTableIter *iter,
                       NMSettingSecretFlags flags,
                       gpointer user_data)
{
	if (flags & (NM_SETTING_SECRET_FLAG_NOT_SAVED | NM_SETTING_SECRET_FLAG_NOT_REQUIRED))
		g_hash_table_iter_remove (iter);
	return TRUE;
}

static gboolean
has_system_owned_secrets (GHashTableIter *iter,
                          NMSettingSecretFlags flags,
                          gpointer user_data)
{
	gboolean *has_system_owned = user_data;

	if (flags == NM_SETTING_SECRET_FLAG_NONE) {
		*has_system_owned = TRUE;
		return FALSE;
	}
	return TRUE;
}

static void
new_secrets_commit_cb (NMSettingsConnection *connection,
                       GError *error,
                       gpointer user_data)
{
	if (error) {
		nm_log_warn (LOGD_SETTINGS, "Error saving new secrets to backing storage: (%d) %s",
		             error->code, error->message ? error->message : "(unknown)");
	}
}

static void
agent_secrets_done_cb (NMAgentManager *manager,
                       guint32 call_id,
                       const char *agent_dbus_owner,
                       const char *agent_username,
                       gboolean agent_has_modify,
                       const char *setting_name,
                       NMSettingsGetSecretsFlags flags,
                       GHashTable *secrets,
                       GError *error,
                       gpointer user_data,
                       gpointer other_data2,
                       gpointer other_data3)
{
	NMSettingsConnection *self = NM_SETTINGS_CONNECTION (user_data);
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	NMSettingsConnectionSecretsFunc callback = other_data2;
	gpointer callback_data = other_data3;
	GError *local = NULL;
	GHashTable *hash;
	gboolean agent_had_system = FALSE;

	if (error) {
		nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) secrets request error: (%d) %s",
		            nm_connection_get_uuid (NM_CONNECTION (self)),
		            setting_name,
		            call_id,
		            error->code,
		            error->message ? error->message : "(unknown)");

		callback (self, call_id, NULL, setting_name, error, callback_data);
		return;
	}

	if (!nm_connection_get_setting_by_name (NM_CONNECTION (self), setting_name)) {
		local = g_error_new (NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_SETTING,
		                     "%s.%d - Connection didn't have requested setting '%s'.",
		                     __FILE__, __LINE__, setting_name);
		callback (self, call_id, NULL, setting_name, local, callback_data);
		g_clear_error (&local);
		return;
	}

	g_assert (secrets);
	if (agent_dbus_owner) {
		nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) secrets returned from agent %s",
		            nm_connection_get_uuid (NM_CONNECTION (self)),
		            setting_name,
		            call_id,
		            agent_dbus_owner);

		/* If the agent returned any system-owned secrets (initial connect and no
		 * secrets given when the connection was created, or something like that)
		 * make sure the agent's UID has the 'modify' permission before we use or
		 * save those system-owned secrets.  If not, discard them and use the
		 * existing secrets, or fail the connection.
		 */
		for_each_secret (NM_CONNECTION (self), secrets, has_system_owned_secrets, &agent_had_system);
		if (agent_had_system) {
			if (flags == NM_SETTINGS_GET_SECRETS_FLAG_NONE) {
				/* No user interaction was allowed when requesting secrets; the
				 * agent is being bad.  Remove system-owned secrets.
				 */
				nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) interaction forbidden but agent %s returned system secrets",
				            nm_connection_get_uuid (NM_CONNECTION (self)),
				            setting_name,
				            call_id,
				            agent_dbus_owner);

				for_each_secret (NM_CONNECTION (self), secrets, clear_nonagent_secrets, NULL);
			} else if (agent_has_modify == FALSE) {
				/* Agent didn't successfully authenticate; clear system-owned secrets
				 * from the secrets the agent returned.
				 */
				nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) agent failed to authenticate but provided system secrets",
				            nm_connection_get_uuid (NM_CONNECTION (self)),
				            setting_name,
				            call_id);

				for_each_secret (NM_CONNECTION (self), secrets, clear_nonagent_secrets, NULL);
			}
		}
	} else {
		nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) existing secrets returned",
		            nm_connection_get_uuid (NM_CONNECTION (self)),
		            setting_name,
		            call_id);
	}

	nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) secrets request completed",
	            nm_connection_get_uuid (NM_CONNECTION (self)),
	            setting_name,
	            call_id);

	/* If no user interaction was allowed, make sure that no "unsaved" secrets
	 * came back.  Unsaved secrets by definition require user interaction.
	 */
	if (flags == NM_SETTINGS_GET_SECRETS_FLAG_NONE)
		for_each_secret (NM_CONNECTION (self), secrets, clear_unsaved_secrets, NULL);

	/* Update the connection with our existing secrets from backing storage */
	nm_connection_clear_secrets (NM_CONNECTION (self));
	hash = nm_connection_to_hash (priv->system_secrets, NM_SETTING_HASH_FLAG_ONLY_SECRETS);
	if (!hash || nm_connection_update_secrets (NM_CONNECTION (self), setting_name, hash, &local)) {
		/* Update the connection with the agent's secrets; by this point if any
		 * system-owned secrets exist in 'secrets' the agent that provided them
		 * will have been authenticated, so those secrets can replace the existing
		 * system secrets.
		 */
		if (nm_connection_update_secrets (NM_CONNECTION (self), setting_name, secrets, &local)) {
			/* Now that all secrets are updated, copy and cache new secrets, 
			 * then save them to backing storage.
			 */
			update_system_secrets_cache (self);
			update_agent_secrets_cache (self, NULL);

			/* Only save secrets to backing storage if the agent returned any
			 * new system secrets.  If it didn't, then the secrets are agent-
			 * owned and there's no point to writing out the connection when
			 * nothing has changed, since agent-owned secrets don't get saved here.
			 */
			if (agent_had_system) {
				nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) saving new secrets to backing storage",
						    nm_connection_get_uuid (NM_CONNECTION (self)),
						    setting_name,
						    call_id);

				nm_settings_connection_commit_changes (self, new_secrets_commit_cb, NULL);
			} else {
				nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) new agent secrets processed",
						    nm_connection_get_uuid (NM_CONNECTION (self)),
						    setting_name,
						    call_id);
			}
		} else {
			nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) failed to update with agent secrets: (%d) %s",
			            nm_connection_get_uuid (NM_CONNECTION (self)),
			            setting_name,
			            call_id,
			            local ? local->code : -1,
			            (local && local->message) ? local->message : "(unknown)");
		}
	} else {
		nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) failed to update with existing secrets: (%d) %s",
		            nm_connection_get_uuid (NM_CONNECTION (self)),
		            setting_name,
		            call_id,
		            local ? local->code : -1,
		            (local && local->message) ? local->message : "(unknown)");
	}

	callback (self, call_id, agent_username, setting_name, local, callback_data);
	g_clear_error (&local);
	if (hash)
		g_hash_table_destroy (hash);
}

/**
 * nm_settings_connection_get_secrets:
 * @connection: the #NMSettingsConnection
 * @filter_by_uid: if TRUE, only request secrets from agents registered by the
 * same UID as @uid.
 * @uid: when @filter_by_uid is TRUE, only request secrets from agents belonging
 * to this UID
 * @setting_name: the setting to return secrets for
 * @flags: flags to modify the secrets request
 * @hint: the name of a key in @setting_name for which a secret may be required
 * @callback: the function to call with returned secrets
 * @callback_data: user data to pass to @callback
 *
 * Retrieves secrets from persistent storage and queries any secret agents for
 * additional secrets.
 *
 * Returns: a call ID which may be used to cancel the ongoing secrets request
 **/
guint32 
nm_settings_connection_get_secrets (NMSettingsConnection *self,
                                    gboolean filter_by_uid,
                                    gulong uid,
                                    const char *setting_name,
                                    NMSettingsGetSecretsFlags flags,
                                    const char *hint,
                                    NMSettingsConnectionSecretsFunc callback,
                                    gpointer callback_data,
                                    GError **error)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	GHashTable *existing_secrets;
	guint32 call_id = 0;

	/* Use priv->secrets to work around the fact that nm_connection_clear_secrets()
	 * will clear secrets on this object's settings.
	 */
	if (!priv->system_secrets) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_CONNECTION,
		             "%s.%d - Internal error; secrets cache invalid.",
		             __FILE__, __LINE__);
		return 0;
	}

	/* Make sure the request actually requests something we can return */
	if (!nm_connection_get_setting_by_name (NM_CONNECTION (self), setting_name)) {
		g_set_error (error, NM_SETTINGS_ERROR, NM_SETTINGS_ERROR_INVALID_SETTING,
		             "%s.%d - Connection didn't have requested setting '%s'.",
		             __FILE__, __LINE__, setting_name);
		return 0;
	}

	existing_secrets = nm_connection_to_hash (priv->system_secrets, NM_SETTING_HASH_FLAG_ONLY_SECRETS);
	call_id = nm_agent_manager_get_secrets (priv->agent_mgr,
	                                        NM_CONNECTION (self),
	                                        filter_by_uid,
	                                        uid,
	                                        existing_secrets,
	                                        setting_name,
	                                        flags,
	                                        hint,
	                                        agent_secrets_done_cb,
	                                        self,
	                                        callback,
	                                        callback_data);
	if (existing_secrets)
		g_hash_table_unref (existing_secrets);

	nm_log_dbg (LOGD_SETTINGS, "(%s/%s:%u) secrets requested flags 0x%X hint '%s'",
	            nm_connection_get_uuid (NM_CONNECTION (self)),
	            setting_name,
	            call_id,
	            flags,
	            hint);

	return call_id;
}

void
nm_settings_connection_cancel_secrets (NMSettingsConnection *self,
                                       guint32 call_id)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);

	nm_log_dbg (LOGD_SETTINGS, "(%s:%u) secrets canceled",
	            nm_connection_get_uuid (NM_CONNECTION (self)),
	            call_id);

	priv->reqs = g_slist_remove (priv->reqs, GUINT_TO_POINTER (call_id));
	nm_agent_manager_cancel_secrets (priv->agent_mgr, call_id);
}

/**** User authorization **************************************/

typedef void (*AuthCallback) (NMSettingsConnection *connection, 
                              DBusGMethodInvocation *context,
                              gulong sender_uid,
                              GError *error,
                              gpointer data);

static void
pk_auth_cb (NMAuthChain *chain,
            GError *chain_error,
            DBusGMethodInvocation *context,
            gpointer user_data)
{
	NMSettingsConnection *self = NM_SETTINGS_CONNECTION (user_data);
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	GError *error = NULL;
	NMAuthCallResult result;
	const char *perm;
	AuthCallback callback;
	gpointer callback_data;
	gulong sender_uid;

	priv->pending_auths = g_slist_remove (priv->pending_auths, chain);

	/* If our NMSettingsConnection is already gone, do nothing */
	if (chain_error) {
		error = g_error_new (NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_GENERAL,
		                     "Error checking authorization: %s",
		                     chain_error->message ? chain_error->message : "(unknown)");
	} else {
		perm = nm_auth_chain_get_data (chain, "perm");
		g_assert (perm);
		result = nm_auth_chain_get_result (chain, perm);

		/* Caller didn't successfully authenticate */
		if (result != NM_AUTH_CALL_RESULT_YES) {
			error = g_error_new_literal (NM_SETTINGS_ERROR,
			                             NM_SETTINGS_ERROR_NOT_PRIVILEGED,
			                             "Insufficient privileges.");
		}
	}

	callback = nm_auth_chain_get_data (chain, "callback");
	callback_data = nm_auth_chain_get_data (chain, "callback-data");
	sender_uid = nm_auth_chain_get_data_ulong (chain, "sender-uid");
	callback (self, context, sender_uid, error, callback_data);

	g_clear_error (&error);
	nm_auth_chain_unref (chain);
}

static gboolean
check_user_in_acl (NMConnection *connection,
                   DBusGMethodInvocation *context,
                   NMDBusManager *dbus_mgr,
                   NMSessionMonitor *session_monitor,
                   gulong *out_sender_uid,
                   GError **error)
{
	gulong sender_uid = G_MAXULONG;
	char *error_desc = NULL;

	g_return_val_if_fail (connection != NULL, FALSE);
	g_return_val_if_fail (context != NULL, FALSE);
	g_return_val_if_fail (session_monitor != NULL, FALSE);

	/* Get the caller's UID */
	if (!nm_auth_get_caller_uid (context, dbus_mgr, &sender_uid, &error_desc)) {
		g_set_error_literal (error,
		                     NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_PERMISSION_DENIED,
		                     error_desc);
		g_free (error_desc);
		return FALSE;
	}

	/* Make sure the UID can view this connection */
	if (0 != sender_uid) {
		if (!nm_auth_uid_in_acl (connection, session_monitor, sender_uid, &error_desc)) {
			g_set_error_literal (error,
			                     NM_SETTINGS_ERROR,
			                     NM_SETTINGS_ERROR_PERMISSION_DENIED,
			                     error_desc);
			g_free (error_desc);
			return FALSE;
		}
	}

	if (out_sender_uid)
		*out_sender_uid = sender_uid;
	return TRUE;
}

static void
auth_start (NMSettingsConnection *self,
            DBusGMethodInvocation *context,
            const char *check_permission,
            AuthCallback callback,
            gpointer callback_data)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	NMAuthChain *chain;
	gulong sender_uid = G_MAXULONG;
	GError *error = NULL;

	if (!check_user_in_acl (NM_CONNECTION (self),
	                        context,
	                        priv->dbus_mgr,
	                        priv->session_monitor,
	                        &sender_uid,
	                        &error)) {
		callback (self, context, G_MAXULONG, error, callback_data);
		g_clear_error (&error);
		return;
	}

	if (check_permission) {
		chain = nm_auth_chain_new (context, NULL, pk_auth_cb, self);
		g_assert (chain);
		nm_auth_chain_set_data (chain, "perm", (gpointer) check_permission, NULL);
		nm_auth_chain_set_data (chain, "callback", callback, NULL);
		nm_auth_chain_set_data (chain, "callback-data", callback_data, NULL);
		nm_auth_chain_set_data_ulong (chain, "sender-uid", sender_uid);

		nm_auth_chain_add_call (chain, check_permission, TRUE);
		priv->pending_auths = g_slist_append (priv->pending_auths, chain);
	} else {
		/* Don't need polkit auth, automatic success */
		callback (self, context, sender_uid, NULL, callback_data);
	}
}

/**** DBus method handlers ************************************/

static gboolean
check_writable (NMConnection *connection, GError **error)
{
	NMSettingConnection *s_con;

	g_return_val_if_fail (connection != NULL, FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);

	s_con = nm_connection_get_setting_connection (connection);
	if (!s_con) {
		g_set_error_literal (error,
		                     NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_INVALID_CONNECTION,
		                     "Connection did not have required 'connection' setting");
		return FALSE;
	}

	/* If the connection is read-only, that has to be changed at the source of
	 * the problem (ex a system settings plugin that can't write connections out)
	 * instead of over D-Bus.
	 */
	if (nm_setting_connection_get_read_only (s_con)) {
		g_set_error_literal (error,
		                     NM_SETTINGS_ERROR,
		                     NM_SETTINGS_ERROR_READ_ONLY_CONNECTION,
		                     "Connection is read-only");
		return FALSE;
	}

	return TRUE;
}

static void
get_settings_auth_cb (NMSettingsConnection *self, 
                      DBusGMethodInvocation *context,
                      gulong sender_uid,
                      GError *error,
                      gpointer data)
{
	if (error)
		dbus_g_method_return_error (context, error);
	else {
		GHashTable *settings;
		NMConnection *dupl_con;
		NMSettingConnection *s_con;
		guint64 timestamp;

		dupl_con = nm_connection_duplicate (NM_CONNECTION (self));
		g_assert (dupl_con);

		/* Timestamp is not updated in connection's 'timestamp' property,
		 * because it would force updating the connection and in turn
		 * writing to /etc periodically, which we want to avoid. Rather real
		 * timestamps are kept track of in a private variable. So, substitute
		 * timestamp property with the real one here before returning the settings.
		 */
		timestamp = nm_settings_connection_get_timestamp (self);
		if (timestamp) {
			s_con = nm_connection_get_setting_connection (NM_CONNECTION (dupl_con));
			g_assert (s_con);
			g_object_set (s_con, NM_SETTING_CONNECTION_TIMESTAMP, timestamp, NULL);
		}

		/* Secrets should *never* be returned by the GetSettings method, they
		 * get returned by the GetSecrets method which can be better
		 * protected against leakage of secrets to unprivileged callers.
		 */
		settings = nm_connection_to_hash (NM_CONNECTION (dupl_con), NM_SETTING_HASH_FLAG_NO_SECRETS);
		g_assert (settings);
		dbus_g_method_return (context, settings);
		g_hash_table_destroy (settings);
		g_object_unref (dupl_con);
	}
}

static void
impl_settings_connection_get_settings (NMSettingsConnection *self,
                                       DBusGMethodInvocation *context)
{
	auth_start (self, context, NULL, get_settings_auth_cb, NULL);
}

typedef struct {
	DBusGMethodInvocation *context;
	NMAgentManager *agent_mgr;
	gulong sender_uid;
} UpdateInfo;

static void
con_update_cb (NMSettingsConnection *self,
               GError *error,
               gpointer user_data)
{
	UpdateInfo *info = user_data;
	NMConnection *for_agent;

	if (error)
		dbus_g_method_return_error (info->context, error);
	else {
		/* Dupe the connection so we can clear out non-agent-owned secrets,
		 * as agent-owned secrets are the only ones we send back be saved.
		 * Only send secrets to agents of the same UID that called update too.
		 */
		for_agent = nm_connection_duplicate (NM_CONNECTION (self));
		nm_connection_clear_secrets_with_flags (for_agent,
		                                        secrets_filter_cb,
		                                        GUINT_TO_POINTER (NM_SETTING_SECRET_FLAG_AGENT_OWNED));
		nm_agent_manager_save_secrets (info->agent_mgr, for_agent, TRUE, info->sender_uid);
		g_object_unref (for_agent);

		dbus_g_method_return (info->context);
	}

	g_object_unref (info->agent_mgr);
	memset (info, 0, sizeof (*info));
	g_free (info);
}

static void
update_auth_cb (NMSettingsConnection *self,
                DBusGMethodInvocation *context,
                gulong sender_uid,
                GError *error,
                gpointer data)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	NMConnection *new_settings = data;
	UpdateInfo *info;

	if (error)
		dbus_g_method_return_error (context, error);
	else {
		info = g_malloc0 (sizeof (*info));
		info->context = context;
		info->agent_mgr = g_object_ref (priv->agent_mgr);
		info->sender_uid = sender_uid;

		/* Cache the new secrets from the agent, as stuff like inotify-triggered
		 * changes to connection's backing config files will blow them away if
		 * they're in the main connection.
		 */
		update_agent_secrets_cache (self, new_settings);

		/* Update and commit our settings. */
		nm_settings_connection_replace_and_commit (self,
			                                       new_settings,
			                                       con_update_cb,
			                                       info);
	}

	g_object_unref (new_settings);
}

static const char *
get_modify_permission_update (NMConnection *old, NMConnection *new)
{
	NMSettingConnection *s_con;
	guint32 orig_num = 0, new_num = 0;

	s_con = nm_connection_get_setting_connection (old);
	g_assert (s_con);
	orig_num = nm_setting_connection_get_num_permissions (s_con);

	s_con = nm_connection_get_setting_connection (new);
	g_assert (s_con);
	new_num = nm_setting_connection_get_num_permissions (s_con);

	/* If the caller is the only user in either connection's permissions, then
	 * we use the 'modify.own' permission instead of 'modify.system'.
	 */
	if (orig_num == 1 && new_num == 1)
		return NM_AUTH_PERMISSION_SETTINGS_MODIFY_OWN;

	/* If the update request affects more than just the caller (ie if the old
	 * settings were system-wide, or the new ones are), require 'modify.system'.
	 */
	return NM_AUTH_PERMISSION_SETTINGS_MODIFY_SYSTEM;
}

static void
impl_settings_connection_update (NMSettingsConnection *self,
                                 GHashTable *new_settings,
                                 DBusGMethodInvocation *context)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	NMConnection *tmp;
	GError *error = NULL;

	/* If the connection is read-only, that has to be changed at the source of
	 * the problem (ex a system settings plugin that can't write connections out)
	 * instead of over D-Bus.
	 */
	if (!check_writable (NM_CONNECTION (self), &error)) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	/* Check if the settings are valid first */
	tmp = nm_connection_new_from_hash (new_settings, &error);
	if (!tmp) {
		g_assert (error);
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	/* And that the new connection settings will be visible to the user
	 * that's sending the update request.  You can't make a connection
	 * invisible to yourself.
	 */
	if (!check_user_in_acl (tmp,
	                        context,
	                        priv->dbus_mgr,
	                        priv->session_monitor,
	                        NULL,
	                        &error)) {
		dbus_g_method_return_error (context, error);
		g_clear_error (&error);
		g_object_unref (tmp);
		return;
	}

	auth_start (self,
	            context,
	            get_modify_permission_update (NM_CONNECTION (self), tmp),
	            update_auth_cb,
	            tmp);
}

static void
con_delete_cb (NMSettingsConnection *connection,
               GError *error,
               gpointer user_data)
{
	DBusGMethodInvocation *context = user_data;

	if (error)
		dbus_g_method_return_error (context, error);
	else
		dbus_g_method_return (context);
}

static void
delete_auth_cb (NMSettingsConnection *self, 
                DBusGMethodInvocation *context,
                gulong sender_uid,
                GError *error,
                gpointer data)
{
	if (error) {
		dbus_g_method_return_error (context, error);
		return;
	}

	nm_settings_connection_delete (self, con_delete_cb, context);
}

static const char *
get_modify_permission_basic (NMSettingsConnection *connection)
{
	NMSettingConnection *s_con;

	/* If the caller is the only user in the connection's permissions, then
	 * we use the 'modify.own' permission instead of 'modify.system'.  If the
	 * request affects more than just the caller, require 'modify.system'.
	 */
	s_con = nm_connection_get_setting_connection (NM_CONNECTION (connection));
	g_assert (s_con);
	if (nm_setting_connection_get_num_permissions (s_con) == 1)
		return NM_AUTH_PERMISSION_SETTINGS_MODIFY_OWN;

	return NM_AUTH_PERMISSION_SETTINGS_MODIFY_SYSTEM;
}

static void
impl_settings_connection_delete (NMSettingsConnection *self,
                                 DBusGMethodInvocation *context)
{
	GError *error = NULL;
	
	if (!check_writable (NM_CONNECTION (self), &error)) {
		dbus_g_method_return_error (context, error);
		g_error_free (error);
		return;
	}

	auth_start (self, context, get_modify_permission_basic (self), delete_auth_cb, NULL);
}

/**************************************************************/

static void
dbus_get_agent_secrets_cb (NMSettingsConnection *self,
                           guint32 call_id,
                           const char *agent_username,
                           const char *setting_name,
                           GError *error,
                           gpointer user_data)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	DBusGMethodInvocation *context = user_data;
	GHashTable *hash;

	priv->reqs = g_slist_remove (priv->reqs, GUINT_TO_POINTER (call_id));

	if (error)
		dbus_g_method_return_error (context, error);
	else {
		/* Return secrets from agent and backing storage to the D-Bus caller;
		 * nm_settings_connection_get_secrets() will have updated itself with
		 * secrets from backing storage and those returned from the agent
		 * by the time we get here.
		 */
		hash = nm_connection_to_hash (NM_CONNECTION (self), NM_SETTING_HASH_FLAG_ONLY_SECRETS);
		if (!hash)
			hash = g_hash_table_new (NULL, NULL);
		dbus_g_method_return (context, hash);
		g_hash_table_destroy (hash);
	}
}

static void
dbus_secrets_auth_cb (NMSettingsConnection *self, 
                      DBusGMethodInvocation *context,
                      gulong sender_uid,
                      GError *error,
                      gpointer user_data)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	char *setting_name = user_data;
	guint32 call_id = 0;
	GError *local = NULL;

	if (!error) {
		call_id = nm_settings_connection_get_secrets (self,
			                                          TRUE,
			                                          sender_uid,
			                                          setting_name,
			                                          NM_SETTINGS_GET_SECRETS_FLAG_NONE,
			                                          NULL,
			                                          dbus_get_agent_secrets_cb,
			                                          context,
			                                          &local);
		if (call_id > 0) {
			/* track the request and wait for the callback */
			priv->reqs = g_slist_append (priv->reqs, GUINT_TO_POINTER (call_id));
		}
	}

	if (error || local) {
		dbus_g_method_return_error (context, error ? error : local);
		g_clear_error (&local);
	}

	g_free (setting_name);
}

static void
impl_settings_connection_get_secrets (NMSettingsConnection *self,
                                      const gchar *setting_name,
                                      DBusGMethodInvocation *context)
{
	auth_start (self,
	            context,
	            get_modify_permission_basic (self),
	            dbus_secrets_auth_cb,
	            g_strdup (setting_name));
}

/**************************************************************/

void
nm_settings_connection_signal_remove (NMSettingsConnection *self)
{
	/* Emit removed first */
	g_signal_emit_by_name (self, NM_SETTINGS_CONNECTION_REMOVED);

	/* And unregistered last to ensure the removed signal goes out before
	 * we take the connection off the bus.
	 */
	g_signal_emit_by_name (self, "unregister");
}

/**
 * nm_settings_connection_get_timestamp:
 * @connection: the #NMSettingsConnection
 *
 * Returns current connection's timestamp.
 *
 * Returns: timestamp of the last connection use (0 when it's not used)
 **/
guint64
nm_settings_connection_get_timestamp (NMSettingsConnection *connection)
{
	g_return_val_if_fail (connection != NULL, 0);
	g_return_val_if_fail (NM_IS_SETTINGS_CONNECTION (connection), 0);

	return NM_SETTINGS_CONNECTION_GET_PRIVATE (connection)->timestamp;
}

/**
 * nm_settings_connection_update_timestamp:
 * @connection: the #NMSettingsConnection
 * @timestamp: timestamp to set into the connection and to store into
 * the timestamps database
 *
 * Updates the connection and timestamps database with the provided timestamp.
 **/
void
nm_settings_connection_update_timestamp (NMSettingsConnection *connection, guint64 timestamp)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (connection);
	const char *connection_uuid;
	GKeyFile *timestamps_file;
	char *data, *tmp;
	gsize len;
	GError *error = NULL;

	/* Update timestamp in private storage */
	priv->timestamp = timestamp;

	/* Save timestamp to timestamps database file */
	timestamps_file = g_key_file_new ();
	if (!g_key_file_load_from_file (timestamps_file, SETTINGS_TIMESTAMPS_FILE, G_KEY_FILE_KEEP_COMMENTS, &error)) {
		if (!(error->domain == G_FILE_ERROR && error->code == G_FILE_ERROR_NOENT))
			nm_log_warn (LOGD_SETTINGS, "error parsing timestamps file '%s': %s", SETTINGS_TIMESTAMPS_FILE, error->message);
		g_clear_error (&error);
	}

	connection_uuid = nm_connection_get_uuid (NM_CONNECTION (connection));
	tmp = g_strdup_printf ("%" G_GUINT64_FORMAT, timestamp);
	g_key_file_set_value (timestamps_file, "timestamps", connection_uuid, tmp);
	g_free (tmp);
 
	data = g_key_file_to_data (timestamps_file, &len, &error);
	if (data) {
		g_file_set_contents (SETTINGS_TIMESTAMPS_FILE, data, len, &error);
		g_free (data);
	}
	if (error) {
		nm_log_warn (LOGD_SETTINGS, "error saving timestamp to file '%s': %s", SETTINGS_TIMESTAMPS_FILE, error->message);
		g_error_free (error);
	}
	g_key_file_free (timestamps_file);
}

/**
 * nm_settings_connection_read_and_fill_timestamp:
 * @connection: the #NMSettingsConnection
 *
 * Retrieves timestamp of the connection's last usage from database file and
 * stores it into the connection private data.
 **/
void
nm_settings_connection_read_and_fill_timestamp (NMSettingsConnection *connection)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (connection);
	const char *connection_uuid;
	guint64 timestamp = 0;
	GKeyFile *timestamps_file;
	GError *err = NULL;
	char *tmp_str;

	/* Get timestamp from database file */
	timestamps_file = g_key_file_new ();
	g_key_file_load_from_file (timestamps_file, SETTINGS_TIMESTAMPS_FILE, G_KEY_FILE_KEEP_COMMENTS, NULL);
	connection_uuid = nm_connection_get_uuid (NM_CONNECTION (connection));
	tmp_str = g_key_file_get_value (timestamps_file, "timestamps", connection_uuid, &err);
	if (tmp_str) {
		timestamp = g_ascii_strtoull (tmp_str, NULL, 10);
		g_free (tmp_str);
	}

	/* Update connection's timestamp */
	if (!err)
		priv->timestamp = timestamp;
	else {
		nm_log_dbg (LOGD_SETTINGS, "failed to read connection timestamp for '%s': (%d) %s",
		            connection_uuid, err->code, err->message);
		g_clear_error (&err);
	}
	g_key_file_free (timestamps_file);
}

static guint
mac_hash (gconstpointer v)
{
	const guint8 *p = v;
	guint32 i, h = 5381;

	for (i = 0; i < ETH_ALEN; i++)
		h = (h << 5) + h + p[i];
	return h;
}

static gboolean
mac_equal (gconstpointer a, gconstpointer b)
{
	return memcmp (a, b, ETH_ALEN) == 0;
}

static guint8 *
mac_dup (const struct ether_addr *old)
{
	guint8 *new;

	g_return_val_if_fail (old != NULL, NULL);

	new = g_malloc0 (ETH_ALEN);
	memcpy (new, old, ETH_ALEN);
	return new;
}

/**
 * nm_settings_connection_has_seen_bssid:
 * @connection: the #NMSettingsConnection
 * @bssid: the BSSID to check the seen BSSID list for
 *
 * Returns: TRUE if the given @bssid is in the seen BSSIDs list
 **/
gboolean
nm_settings_connection_has_seen_bssid (NMSettingsConnection *connection,
                                       const struct ether_addr *bssid)
{
	g_return_val_if_fail (connection != NULL, FALSE);
	g_return_val_if_fail (NM_IS_SETTINGS_CONNECTION (connection), FALSE);
	g_return_val_if_fail (bssid != NULL, FALSE);

	return !!g_hash_table_lookup (NM_SETTINGS_CONNECTION_GET_PRIVATE (connection)->seen_bssids, bssid);
}

/**
 * nm_settings_connection_add_seen_bssid:
 * @connection: the #NMSettingsConnection
 * @seen_bssid: BSSID to set into the connection and to store into
 * the seen-bssids database
 *
 * Updates the connection and seen-bssids database with the provided BSSID.
 **/
void
nm_settings_connection_add_seen_bssid (NMSettingsConnection *connection,
                                       const struct ether_addr *seen_bssid)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (connection);
	const char *connection_uuid;
	GKeyFile *seen_bssids_file;
	char *data, *bssid_str;
	const char **list;
	gsize len;
	GError *error = NULL;
	GHashTableIter iter;
	guint n;

	g_return_if_fail (seen_bssid != NULL);

	if (g_hash_table_lookup (priv->seen_bssids, seen_bssid))
		return;  /* Already in the list */

	/* Add the new BSSID; let the hash take ownership of the allocated BSSID string */
	bssid_str = nm_utils_hwaddr_ntoa (seen_bssid, ARPHRD_ETHER);
	g_return_if_fail (bssid_str != NULL);
	g_hash_table_insert (priv->seen_bssids, mac_dup (seen_bssid), bssid_str);

	/* Build up a list of all the BSSIDs in string form */
	n = 0;
	list = g_malloc0 (g_hash_table_size (priv->seen_bssids) * sizeof (char *));
	g_hash_table_iter_init (&iter, priv->seen_bssids);
	while (g_hash_table_iter_next (&iter, NULL, (gpointer) &bssid_str))
		list[n++] = bssid_str;

	/* Save BSSID to seen-bssids file */
	seen_bssids_file = g_key_file_new ();
	g_key_file_set_list_separator (seen_bssids_file, ',');
	if (!g_key_file_load_from_file (seen_bssids_file, SETTINGS_SEEN_BSSIDS_FILE, G_KEY_FILE_KEEP_COMMENTS, &error)) {
		if (!g_error_matches (error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
			nm_log_warn (LOGD_SETTINGS, "error parsing seen-bssids file '%s': %s",
			             SETTINGS_SEEN_BSSIDS_FILE, error->message);
		}
		g_clear_error (&error);
	}

	connection_uuid = nm_connection_get_uuid (NM_CONNECTION (connection));
	g_key_file_set_string_list (seen_bssids_file, "seen-bssids", connection_uuid, list, n);
	g_free (list);

	data = g_key_file_to_data (seen_bssids_file, &len, &error);
	if (data) {
		g_file_set_contents (SETTINGS_SEEN_BSSIDS_FILE, data, len, &error);
		g_free (data);
	}
	g_key_file_free (seen_bssids_file);

	if (error) {
		nm_log_warn (LOGD_SETTINGS, "error saving seen-bssids to file '%s': %s",
		             SETTINGS_SEEN_BSSIDS_FILE, error->message);
		g_error_free (error);
	}
}

static void
add_seen_bssid_string (NMSettingsConnection *self, const char *bssid)
{
	struct ether_addr mac;

	g_return_if_fail (bssid != NULL);
	if (ether_aton_r (bssid, &mac)) {
		g_hash_table_insert (NM_SETTINGS_CONNECTION_GET_PRIVATE (self)->seen_bssids,
		                     mac_dup (&mac),
		                     g_strdup (bssid));
	}
}

/**
 * nm_settings_connection_read_and_fill_seen_bssids:
 * @connection: the #NMSettingsConnection
 *
 * Retrieves seen BSSIDs of the connection from database file and stores then into the
 * connection private data.
 **/
void
nm_settings_connection_read_and_fill_seen_bssids (NMSettingsConnection *connection)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (connection);
	const char *connection_uuid;
	GKeyFile *seen_bssids_file;
	char **tmp_strv = NULL;
	gsize i, len = 0;
	NMSettingWireless *s_wifi;

	/* Get seen BSSIDs from database file */
	seen_bssids_file = g_key_file_new ();
	g_key_file_set_list_separator (seen_bssids_file, ',');
	if (g_key_file_load_from_file (seen_bssids_file, SETTINGS_SEEN_BSSIDS_FILE, G_KEY_FILE_KEEP_COMMENTS, NULL)) {
		connection_uuid = nm_connection_get_uuid (NM_CONNECTION (connection));
		tmp_strv = g_key_file_get_string_list (seen_bssids_file, "seen-bssids", connection_uuid, &len, NULL);
	}
	g_key_file_free (seen_bssids_file);

	/* Update connection's seen-bssids */
	if (tmp_strv) {
		g_hash_table_remove_all (priv->seen_bssids);
		for (i = 0; i < len; i++)
			add_seen_bssid_string (connection, tmp_strv[i]);
		g_strfreev (tmp_strv);
	} else {
		/* If this connection didn't have an entry in the seen-bssids database,
		 * maybe this is the first time we've read it in, so populate the
		 * seen-bssids list from the deprecated seen-bssids property of the
		 * wifi setting.
		 */
		s_wifi = nm_connection_get_setting_wireless (NM_CONNECTION (connection));
		if (s_wifi) {
			len = nm_setting_wireless_get_num_seen_bssids (s_wifi);
			for (i = 0; i < len; i++)
				add_seen_bssid_string (connection, nm_setting_wireless_get_seen_bssid (s_wifi, i));
		}
	}
}

/**************************************************************/

static void
nm_settings_connection_init (NMSettingsConnection *self)
{
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	static guint32 dbus_counter = 0;
	char *dbus_path;

	priv->dbus_mgr = nm_dbus_manager_get ();

	dbus_path = g_strdup_printf ("%s/%u", NM_DBUS_PATH_SETTINGS, dbus_counter++);
	nm_connection_set_path (NM_CONNECTION (self), dbus_path);
	g_free (dbus_path);
	priv->visible = FALSE;

	priv->session_monitor = nm_session_monitor_get ();
	priv->session_changed_id = g_signal_connect (priv->session_monitor,
	                                             NM_SESSION_MONITOR_CHANGED,
	                                             G_CALLBACK (session_changed_cb),
	                                             self);

	priv->agent_mgr = nm_agent_manager_get ();

	priv->seen_bssids = g_hash_table_new_full (mac_hash, mac_equal, g_free, g_free);

	g_signal_connect (self, "secrets-cleared", G_CALLBACK (secrets_cleared_cb), NULL);
}

static void
dispose (GObject *object)
{
	NMSettingsConnection *self = NM_SETTINGS_CONNECTION (object);
	NMSettingsConnectionPrivate *priv = NM_SETTINGS_CONNECTION_GET_PRIVATE (self);
	GSList *iter;

	if (priv->disposed)
		goto out;
	priv->disposed = TRUE;

	if (priv->system_secrets)
		g_object_unref (priv->system_secrets);
	if (priv->agent_secrets)
		g_object_unref (priv->agent_secrets);

	/* Cancel PolicyKit requests */
	for (iter = priv->pending_auths; iter; iter = g_slist_next (iter))
		nm_auth_chain_unref ((NMAuthChain *) iter->data);
	g_slist_free (priv->pending_auths);
	priv->pending_auths = NULL;

	/* Cancel in-progress secrets requests */
	for (iter = priv->reqs; iter; iter = g_slist_next (iter))
		nm_agent_manager_cancel_secrets (priv->agent_mgr, GPOINTER_TO_UINT (iter->data));
	g_slist_free (priv->reqs);

	g_hash_table_destroy (priv->seen_bssids);

	set_visible (self, FALSE);

	if (priv->session_changed_id)
		g_signal_handler_disconnect (priv->session_monitor, priv->session_changed_id);
	g_object_unref (priv->session_monitor);
	g_object_unref (priv->agent_mgr);
	g_object_unref (priv->dbus_mgr);

out:
	G_OBJECT_CLASS (nm_settings_connection_parent_class)->dispose (object);
}

static void
get_property (GObject *object, guint prop_id,
              GValue *value, GParamSpec *pspec)
{
	switch (prop_id) {
	case PROP_VISIBLE:
		g_value_set_boolean (value, NM_SETTINGS_CONNECTION_GET_PRIVATE (object)->visible);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
set_property (GObject *object, guint prop_id,
              const GValue *value, GParamSpec *pspec)
{
	G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
}

static void
nm_settings_connection_class_init (NMSettingsConnectionClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	g_type_class_add_private (class, sizeof (NMSettingsConnectionPrivate));

	/* Virtual methods */
	object_class->dispose = dispose;
	object_class->get_property = get_property;
	object_class->set_property = set_property;

	class->commit_changes = commit_changes;
	class->delete = do_delete;
	class->supports_secrets = supports_secrets;

	/* Properties */
	g_object_class_install_property
		(object_class, PROP_VISIBLE,
		 g_param_spec_boolean (NM_SETTINGS_CONNECTION_VISIBLE,
		                       "Visible",
		                       "Visible",
		                       FALSE,
		                       G_PARAM_READABLE));

	/* Signals */
	signals[UPDATED] = 
		g_signal_new (NM_SETTINGS_CONNECTION_UPDATED,
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_FIRST,
		              0,
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);

	signals[REMOVED] = 
		g_signal_new (NM_SETTINGS_CONNECTION_REMOVED,
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_FIRST,
		              0,
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);

	/* Not exported */
	signals[UNREGISTER] = 
		g_signal_new ("unregister",
		              G_TYPE_FROM_CLASS (class),
		              G_SIGNAL_RUN_FIRST,
		              0,
		              NULL, NULL,
		              g_cclosure_marshal_VOID__VOID,
		              G_TYPE_NONE, 0);

	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (class),
	                                 &dbus_glib_nm_settings_connection_object_info);
}
