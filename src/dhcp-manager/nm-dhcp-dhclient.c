/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-dhcp-dhclient.c - dhclient specific hooks for NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
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
 * Copyright (C) 2005 - 2010 Red Hat, Inc.
 */

#define _XOPEN_SOURCE
#include <time.h>
#undef _XOPEN_SOURCE

#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#include <config.h>

#include "nm-dhcp-dhclient.h"
#include "nm-utils.h"
#include "nm-logging.h"
#include "nm-dhcp-dhclient-utils.h"

G_DEFINE_TYPE (NMDHCPDhclient, nm_dhcp_dhclient, NM_TYPE_DHCP_CLIENT)

#define NM_DHCP_DHCLIENT_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DHCP_DHCLIENT, NMDHCPDhclientPrivate))

#if defined(TARGET_DEBIAN) || defined(TARGET_SUSE) || defined(TARGET_MANDRIVA)
#if defined(DHCLIENT_V3)
#define NM_DHCLIENT_LEASE_DIR			LOCALSTATEDIR "/lib/dhcp3"
#else
#define NM_DHCLIENT_LEASE_DIR           LOCALSTATEDIR "/lib/dhcp"
#endif
#else
#define NM_DHCLIENT_LEASE_DIR           LOCALSTATEDIR "/lib/dhclient"
#endif

#define ACTION_SCRIPT_PATH	LIBEXECDIR "/nm-dhcp-client.action"

typedef struct {
	const char *path;
	char *conf_file;
	char *lease_file;
	char *pid_file;
} NMDHCPDhclientPrivate;

const char *
nm_dhcp_dhclient_get_path (const char *try_first)
{
	static const char *dhclient_paths[] = {
		"/sbin/dhclient",
		"/usr/sbin/dhclient",
		"/usr/pkg/sbin/dhclient",
		"/usr/local/sbin/dhclient",
		NULL
	};
	const char **path = dhclient_paths;

	if (strlen (try_first) && g_file_test (try_first, G_FILE_TEST_EXISTS))
		return try_first;

	while (*path != NULL) {
		if (g_file_test (*path, G_FILE_TEST_EXISTS))
			break;
		path++;
	}

	return *path;
}

static char *
get_leasefile_for_iface (const char * iface, const char *uuid, gboolean ipv6)
{
	return g_strdup_printf ("%s/dhclient%s-%s-%s.lease",
	                        NM_DHCLIENT_LEASE_DIR,
	                        ipv6 ? "6" : "",
	                        uuid,
	                        iface);
}

static void
add_lease_option (GHashTable *hash, char *line)
{
	char *spc;

	spc = strchr (line, ' ');
	if (!spc) {
		nm_log_warn (LOGD_DHCP, "DHCP lease file line '%s' did not contain a space", line);
		return;
	}

	/* If it's an 'option' line, split at second space */
	if (g_str_has_prefix (line, "option ")) {
		spc = strchr (spc + 1, ' ');
		if (!spc) {
			nm_log_warn (LOGD_DHCP, "DHCP lease file option line '%s' did not contain a second space",
			             line);
			return;
		}
	}

	/* Split the line at the space */
	*spc = '\0';
	spc++;

	/* Kill the ';' at the end of the line, if any */
	if (*(spc + strlen (spc) - 1) == ';')
		*(spc + strlen (spc) - 1) = '\0';

	/* Treat 'interface' specially */
	if (g_str_has_prefix (line, "interface")) {
		if (*(spc) == '"')
			spc++; /* Jump past the " */
		if (*(spc + strlen (spc) - 1) == '"')
			*(spc + strlen (spc) - 1) = '\0';  /* Kill trailing " */
	}

	g_hash_table_insert (hash, g_strdup (line), g_strdup (spc));
}

GSList *
nm_dhcp_dhclient_get_lease_config (const char *iface, const char *uuid)
{
	GSList *parsed = NULL, *iter, *leases = NULL;
	char *contents = NULL;
	char *leasefile;
	char **line, **split = NULL;
	GHashTable *hash = NULL;

	leasefile = get_leasefile_for_iface (iface, uuid, FALSE);
	if (!leasefile)
		return NULL;

	if (!g_file_test (leasefile, G_FILE_TEST_EXISTS))
		goto out;

	if (!g_file_get_contents (leasefile, &contents, NULL, NULL))
		goto out;

	split = g_strsplit_set (contents, "\n\r", -1);
	g_free (contents);
	if (!split)
		goto out;

	for (line = split; line && *line; line++) {
		*line = g_strstrip (*line);

		if (!strcmp (*line, "}")) {
			/* Lease ends */
			parsed = g_slist_append (parsed, hash);
			hash = NULL;
		} else if (!strcmp (*line, "lease {")) {
			/* Beginning of a new lease */
			if (hash) {
				nm_log_warn (LOGD_DHCP, "DHCP lease file %s malformed; new lease started "
				             "without ending previous lease",
				             leasefile);
				g_hash_table_destroy (hash);
			}

			hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
		} else if (strlen (*line))
			add_lease_option (hash, *line);
	}
	g_strfreev (split);

	/* Check if the last lease in the file was properly ended */
	if (hash) {
		nm_log_warn (LOGD_DHCP, "DHCP lease file %s malformed; new lease started "
		             "without ending previous lease",
		             leasefile);
		g_hash_table_destroy (hash);
		hash = NULL;
	}

	for (iter = parsed; iter; iter = g_slist_next (iter)) {
		NMIP4Config *ip4;
		NMIP4Address *addr;
		const char *data;
		struct in_addr tmp;
		guint32 prefix;
		struct tm expire;

		hash = iter->data;

		/* Make sure this lease is for the interface we want */
		data = g_hash_table_lookup (hash, "interface");
		if (!data || strcmp (data, iface))
			continue;

		data = g_hash_table_lookup (hash, "expire");
		if (data) {
			time_t now_tt;
			struct tm *now;

			/* Read lease expiration (in UTC) */
			if (!strptime (data, "%w %Y/%m/%d %H:%M:%S", &expire)) {
				nm_log_warn (LOGD_DHCP, "couldn't parse DHCP lease file expire time '%s'",
				             data);
				continue;
			}

			now_tt = time (NULL);
			now = gmtime(&now_tt);

			/* Ignore this lease if it's already expired */
			if (expire.tm_year < now->tm_year)
				continue;
			else if (expire.tm_year == now->tm_year) {
				if (expire.tm_mon < now->tm_mon)
					continue;
				else if (expire.tm_mon == now->tm_mon) {
					if (expire.tm_mday < now->tm_mday)
						continue;
					else if (expire.tm_mday == now->tm_mday) {
						if (expire.tm_hour < now->tm_hour)
							continue;
						else if (expire.tm_hour == now->tm_hour) {
							if (expire.tm_min < now->tm_min)
								continue;
							else if (expire.tm_min == now->tm_min) {
								if (expire.tm_sec <= now->tm_sec)
									continue;
							}
						}
					}
				}
			}
			/* If we get this far, the lease hasn't expired */
		}

		data = g_hash_table_lookup (hash, "fixed-address");
		if (!data)
			continue;

		ip4 = nm_ip4_config_new ();
		addr = nm_ip4_address_new ();

		/* IP4 address */
		if (!inet_pton (AF_INET, data, &tmp)) {
			nm_log_warn (LOGD_DHCP, "couldn't parse DHCP lease file IP4 address '%s'", data);
			goto error;
		}
		nm_ip4_address_set_address (addr, tmp.s_addr);

		/* Netmask */
		data = g_hash_table_lookup (hash, "option subnet-mask");
		if (data) {
			if (!inet_pton (AF_INET, data, &tmp)) {
				nm_log_warn (LOGD_DHCP, "couldn't parse DHCP lease file IP4 subnet mask '%s'", data);
				goto error;
			}
			prefix = nm_utils_ip4_netmask_to_prefix (tmp.s_addr);
		} else {
			/* Get default netmask for the IP according to appropriate class. */
			prefix = nm_utils_ip4_get_default_prefix (nm_ip4_address_get_address (addr));
		}
		nm_ip4_address_set_prefix (addr, prefix);

		/* Gateway */
		data = g_hash_table_lookup (hash, "option routers");
		if (data) {
			if (!inet_pton (AF_INET, data, &tmp)) {
				nm_log_warn (LOGD_DHCP, "couldn't parse DHCP lease file IP4 gateway '%s'", data);
				goto error;
			}
			nm_ip4_address_set_gateway (addr, tmp.s_addr);
		}

		nm_ip4_config_take_address (ip4, addr);
		leases = g_slist_append (leases, ip4);
		continue;

	error:
		nm_ip4_address_unref (addr);
		g_object_unref (ip4);
	}

out:
	g_slist_foreach (parsed, (GFunc) g_hash_table_destroy, NULL);
	g_free (leasefile);
	return leases;
}



static gboolean
merge_dhclient_config (const char *iface,
                       const char *conf_file,
                       NMSettingIP4Config *s_ip4,
                       guint8 *anycast_addr,
                       const char *hostname,
                       const char *orig_path,
                       GError **error)
{
	char *orig = NULL, *new;
	gboolean success = FALSE;

	g_return_val_if_fail (iface != NULL, FALSE);
	g_return_val_if_fail (conf_file != NULL, FALSE);

	if (g_file_test (orig_path, G_FILE_TEST_EXISTS)) {
		GError *read_error = NULL;

		if (!g_file_get_contents (orig_path, &orig, NULL, &read_error)) {
			nm_log_warn (LOGD_DHCP, "(%s): error reading dhclient configuration %s: %s",
			             iface, orig_path, read_error->message);
			g_error_free (read_error);
		}
	}

	new = nm_dhcp_dhclient_create_config (iface, s_ip4, anycast_addr, hostname, orig_path, orig);
	g_assert (new);
	success = g_file_set_contents (conf_file, new, -1, error);
	g_free (new);

	return success;
}

/* NM provides interface-specific options; thus the same dhclient config
 * file cannot be used since DHCP transactions can happen in parallel.
 * Since some distros don't have default per-interface dhclient config files,
 * read their single config file and merge that into a custom per-interface
 * config file along with the NM options.
 */
static char *
create_dhclient_config (const char *iface,
                        NMSettingIP4Config *s_ip4,
                        guint8 *dhcp_anycast_addr,
                        const char *hostname)
{
	char *orig = NULL, *tmp, *conf_file = NULL;
	GError *error = NULL;
	gboolean success = FALSE;

	g_return_val_if_fail (iface != NULL, NULL);

#if defined(TARGET_SUSE)
	orig = g_strdup (SYSCONFDIR "/dhclient.conf");
#elif defined(TARGET_DEBIAN) || defined(TARGET_GENTOO)
#if defined(DHCLIENT_V3)
	orig = g_strdup (SYSCONFDIR "/dhcp3/dhclient.conf");
#else
	orig = g_strdup (SYSCONFDIR "/dhcp/dhclient.conf");
#endif
#else
	orig = g_strdup_printf (SYSCONFDIR "/dhclient-%s.conf", iface);
#endif

	if (!orig) {
		nm_log_warn (LOGD_DHCP, "(%s): not enough memory for dhclient options.", iface);
		return NULL;
	}

#if !defined(TARGET_SUSE) && !defined(TARGET_DEBIAN) && !defined(TARGET_GENTOO)
	/* Try /etc/dhcp/ too (rh #607759) */
	if (!g_file_test (orig, G_FILE_TEST_EXISTS)) {
		g_free (orig);
		orig = g_strdup_printf (SYSCONFDIR "/dhcp/dhclient-%s.conf", iface);
		if (!orig) {
			nm_log_warn (LOGD_DHCP, "(%s): not enough memory for dhclient options.", iface);
			return NULL;
		}
	}
#endif

	tmp = g_strdup_printf ("nm-dhclient-%s.conf", iface);
	conf_file = g_build_filename ("/var", "run", tmp, NULL);
	g_free (tmp);

	error = NULL;
	success = merge_dhclient_config (iface, conf_file, s_ip4, dhcp_anycast_addr, hostname, orig, &error);
	if (!success) {
		nm_log_warn (LOGD_DHCP, "(%s): error creating dhclient configuration: %s",
		             iface, error->message);
		g_error_free (error);
	}

	g_free (orig);
	return conf_file;
}


static void
dhclient_child_setup (gpointer user_data G_GNUC_UNUSED)
{
	/* We are in the child process at this point */
	pid_t pid = getpid ();
	setpgid (pid, pid);
}

static GPid
dhclient_start (NMDHCPClient *client,
                const char *mode_opt,
                gboolean release)
{
	NMDHCPDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE (client);
	GPtrArray *argv = NULL;
	GPid pid = -1;
	GError *error = NULL;
	const char *iface, *uuid, *system_bus_address;
	char *binary_name, *cmd_str, *pid_file = NULL, *system_bus_address_env = NULL;
	gboolean ipv6;
	guint log_domain;

	g_return_val_if_fail (priv->pid_file == NULL, -1);

	iface = nm_dhcp_client_get_iface (client);
	uuid = nm_dhcp_client_get_uuid (client);
	ipv6 = nm_dhcp_client_get_ipv6 (client);

	log_domain = ipv6 ? LOGD_DHCP6 : LOGD_DHCP4;

#if defined(DHCLIENT_V3)
	if (ipv6) {
		nm_log_warn (log_domain, "(%s): ISC dhcp3 does not support IPv6", iface);
		return -1;
	}
#endif

	if (!g_file_test (priv->path, G_FILE_TEST_EXISTS)) {
		nm_log_warn (log_domain, "%s does not exist.", priv->path);
		return -1;
	}

	pid_file = g_strdup_printf (LOCALSTATEDIR "/run/dhclient%s-%s.pid",
		                        ipv6 ? "6" : "",
		                        iface);
	if (!pid_file) {
		nm_log_warn (log_domain, "(%s): not enough memory for dhcpcd options.", iface);
		return -1;
	}

	/* Kill any existing dhclient from the pidfile */
	binary_name = g_path_get_basename (priv->path);
	nm_dhcp_client_stop_existing (pid_file, binary_name);
	g_free (binary_name);

	if (release) {
		/* release doesn't use the pidfile after killing an old client */
		g_free (pid_file);
		pid_file = NULL;
	}

	g_free (priv->lease_file);
	priv->lease_file = get_leasefile_for_iface (iface, uuid, ipv6);
	if (!priv->lease_file) {
		nm_log_warn (log_domain, "(%s): not enough memory for dhclient options.", iface);
		return -1;
	}

	argv = g_ptr_array_new ();
	g_ptr_array_add (argv, (gpointer) priv->path);

	g_ptr_array_add (argv, (gpointer) "-d");

	if (release)
		g_ptr_array_add (argv, (gpointer) "-r");

#if !defined(DHCLIENT_V3)
	if (ipv6) {
		g_ptr_array_add (argv, (gpointer) "-6");
		if (mode_opt)
			g_ptr_array_add (argv, (gpointer) mode_opt);
	} else {
		g_ptr_array_add (argv, (gpointer) "-4");
	}
#endif

	g_ptr_array_add (argv, (gpointer) "-sf");	/* Set script file */
	g_ptr_array_add (argv, (gpointer) ACTION_SCRIPT_PATH );

	if (pid_file) {
		g_ptr_array_add (argv, (gpointer) "-pf");	/* Set pid file */
		g_ptr_array_add (argv, (gpointer) pid_file);
	}

	g_ptr_array_add (argv, (gpointer) "-lf");	/* Set lease file */
	g_ptr_array_add (argv, (gpointer) priv->lease_file);

	if (priv->conf_file) {
		g_ptr_array_add (argv, (gpointer) "-cf");	/* Set interface config file */
		g_ptr_array_add (argv, (gpointer) priv->conf_file);
	}

	/* Usually the system bus address is well-known; but if it's supposed
	 * to be something else, we need to push it to dhclient, since dhclient
	 * sanitizes the environment it gives the action scripts.
	 */
	system_bus_address = getenv ("DBUS_SYSTEM_BUS_ADDRESS");
	if (system_bus_address) {
		system_bus_address_env = g_strdup_printf ("DBUS_SYSTEM_BUS_ADDRESS=%s", system_bus_address);
		g_ptr_array_add (argv, (gpointer) "-e");
		g_ptr_array_add (argv, (gpointer) system_bus_address_env);
	}


	g_ptr_array_add (argv, (gpointer) iface);
	g_ptr_array_add (argv, NULL);

	cmd_str = g_strjoinv (" ", (gchar **) argv->pdata);
	nm_log_dbg (log_domain, "running: %s", cmd_str);
	g_free (cmd_str);

	if (!g_spawn_async (NULL, (char **) argv->pdata, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                    &dhclient_child_setup, NULL, &pid, &error)) {
		nm_log_warn (log_domain, "dhclient failed to start: '%s'", error->message);
		g_error_free (error);
		pid = -1;
	} else {
		nm_log_info (log_domain, "dhclient started with pid %d", pid);
		priv->pid_file = pid_file;
	}

	g_ptr_array_free (argv, TRUE);
	g_free (system_bus_address_env);
	return pid;
}

static GPid
real_ip4_start (NMDHCPClient *client,
                NMSettingIP4Config *s_ip4,
                guint8 *dhcp_anycast_addr,
                const char *hostname)
{
	NMDHCPDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE (client);
	const char *iface;

	iface = nm_dhcp_client_get_iface (client);

	priv->conf_file = create_dhclient_config (iface, s_ip4, dhcp_anycast_addr, hostname);
	if (!priv->conf_file) {
		nm_log_warn (LOGD_DHCP4, "(%s): error creating dhclient configuration file.", iface);
		return -1;
	}

	return dhclient_start (client, NULL, FALSE);
}

static GPid
real_ip6_start (NMDHCPClient *client,
                NMSettingIP6Config *s_ip6,
                guint8 *dhcp_anycast_addr,
                const char *hostname,
                gboolean info_only)
{
	return dhclient_start (client, info_only ? "-S" : "-N", FALSE);
}

static void
real_stop (NMDHCPClient *client, gboolean release)
{
	NMDHCPDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE (client);

	/* Chain up to parent */
	NM_DHCP_CLIENT_CLASS (nm_dhcp_dhclient_parent_class)->stop (client, release);

	if (priv->conf_file)
		remove (priv->conf_file);
	if (priv->pid_file) {
		remove (priv->pid_file);
		g_free (priv->pid_file);
		priv->pid_file = NULL;
	}

	if (release) {
		GPid rpid;

		rpid = dhclient_start (client, NULL, TRUE);
		if (rpid > 0) {
			/* Wait a few seconds for the release to happen */
			nm_dhcp_client_stop_pid (rpid, nm_dhcp_client_get_iface (client), 5);
		}
	}
}

/***************************************************/

static void
nm_dhcp_dhclient_init (NMDHCPDhclient *self)
{
	NMDHCPDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE (self);

	priv->path = nm_dhcp_dhclient_get_path (DHCLIENT_PATH);
}

static void
dispose (GObject *object)
{
	NMDHCPDhclientPrivate *priv = NM_DHCP_DHCLIENT_GET_PRIVATE (object);

	g_free (priv->pid_file);
	g_free (priv->conf_file);
	g_free (priv->lease_file);

	G_OBJECT_CLASS (nm_dhcp_dhclient_parent_class)->dispose (object);
}

static void
nm_dhcp_dhclient_class_init (NMDHCPDhclientClass *dhclient_class)
{
	NMDHCPClientClass *client_class = NM_DHCP_CLIENT_CLASS (dhclient_class);
	GObjectClass *object_class = G_OBJECT_CLASS (dhclient_class);

	g_type_class_add_private (dhclient_class, sizeof (NMDHCPDhclientPrivate));

	/* virtual methods */
	object_class->dispose = dispose;

	client_class->ip4_start = real_ip4_start;
	client_class->ip6_start = real_ip6_start;
	client_class->stop = real_stop;
}

