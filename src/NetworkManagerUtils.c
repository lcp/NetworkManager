/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* NetworkManager -- Network link manager
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
 * Copyright (C) 2004 - 2011 Red Hat, Inc.
 * Copyright (C) 2005 - 2008 Novell, Inc.
 */

#include <glib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <stdlib.h>

#include "NetworkManagerUtils.h"
#include "nm-utils.h"
#include "nm-logging.h"
#include "nm-device.h"
#include "nm-dbus-manager.h"
#include "nm-dispatcher-action.h"
#include "nm-dbus-glib-types.h"
#include "nm-setting-connection.h"
#include "nm-setting-ip4-config.h"
#include "nm-setting-ip6-config.h"
#include "nm-setting-wireless.h"
#include "nm-setting-wireless-security.h"
#include "nm-manager-auth.h"

/*
 * nm_ethernet_address_is_valid
 *
 * Compares an Ethernet address against known invalid addresses.
 *
 */
gboolean
nm_ethernet_address_is_valid (const struct ether_addr *test_addr)
{
	guint8 invalid_addr1[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	guint8 invalid_addr2[ETH_ALEN] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	guint8 invalid_addr3[ETH_ALEN] = {0x44, 0x44, 0x44, 0x44, 0x44, 0x44};
	guint8 invalid_addr4[ETH_ALEN] = {0x00, 0x30, 0xb4, 0x00, 0x00, 0x00}; /* prism54 dummy MAC */

	g_return_val_if_fail (test_addr != NULL, FALSE);

	/* Compare the AP address the card has with invalid ethernet MAC addresses. */
	if (!memcmp (test_addr->ether_addr_octet, &invalid_addr1, ETH_ALEN))
		return FALSE;

	if (!memcmp (test_addr->ether_addr_octet, &invalid_addr2, ETH_ALEN))
		return FALSE;

	if (!memcmp (test_addr->ether_addr_octet, &invalid_addr3, ETH_ALEN))
		return FALSE;

	if (!memcmp (test_addr->ether_addr_octet, &invalid_addr4, ETH_ALEN))
		return FALSE;

	if (test_addr->ether_addr_octet[0] & 1)			/* Multicast addresses */
		return FALSE;
	
	return TRUE;
}


int
nm_spawn_process (const char *args)
{
	gint num_args;
	char **argv = NULL;
	int status = -1;
	GError *error = NULL;

	g_return_val_if_fail (args != NULL, -1);

	if (!g_shell_parse_argv (args, &num_args, &argv, &error)) {
		nm_log_warn (LOGD_CORE, "could not parse arguments for '%s': %s", args, error->message);
		g_error_free (error);
		return -1;
	}

	if (!g_spawn_sync ("/", argv, NULL, 0, NULL, NULL, NULL, NULL, &status, &error)) {
		nm_log_warn (LOGD_CORE, "could not spawn process '%s': %s", args, error->message);
		g_error_free (error);
	}

	g_strfreev (argv);
	return status;
}

/*
 * nm_utils_ip4_netmask_to_prefix
 *
 * Figure out the network prefix from a netmask.  Netmask
 * MUST be in network byte order.
 *
 */
guint32
nm_utils_ip4_netmask_to_prefix (guint32 netmask)
{
	guchar *p, *end;
	guint32 prefix = 0;

	p = (guchar *) &netmask;
	end = p + sizeof (guint32);

	while ((*p == 0xFF) && p < end) {
		prefix += 8;
		p++;
	}

	if (p < end) {
		guchar v = *p;

		while (v) {
			prefix++;
			v <<= 1;
		}
	}

	return prefix;
}

/*
 * nm_utils_ip4_prefix_to_netmask
 *
 * Figure out the netmask from a prefix.
 *
 */
guint32
nm_utils_ip4_prefix_to_netmask (guint32 prefix)
{
	guint32 msk = 0x80000000;
	guint32 netmask = 0;

	while (prefix > 0) {
		netmask |= msk;
		msk >>= 1;
		prefix--;
	}

	return (guint32) htonl (netmask);
}

void
nm_utils_merge_ip4_config (NMIP4Config *ip4_config, NMSettingIP4Config *setting)
{
	int i, j;

	if (!setting)
		return; /* Defaults are just fine */

	if (nm_setting_ip4_config_get_ignore_auto_dns (setting)) {
		nm_ip4_config_reset_nameservers (ip4_config);
		nm_ip4_config_reset_domains (ip4_config);
		nm_ip4_config_reset_searches (ip4_config);
	}

	if (nm_setting_ip4_config_get_ignore_auto_routes (setting))
		nm_ip4_config_reset_routes (ip4_config);

	for (i = 0; i < nm_setting_ip4_config_get_num_dns (setting); i++) {
		guint32 ns;
		gboolean found = FALSE;

		/* Avoid dupes */
		ns = nm_setting_ip4_config_get_dns (setting, i);
		for (j = 0; j < nm_ip4_config_get_num_nameservers (ip4_config); j++) {
			if (nm_ip4_config_get_nameserver (ip4_config, j) == ns) {
				found = TRUE;
				break;
			}
		}

		if (!found)
			nm_ip4_config_add_nameserver (ip4_config, ns);
	}

	/* DNS search domains */
	for (i = 0; i < nm_setting_ip4_config_get_num_dns_searches (setting); i++) {
		const char *search = nm_setting_ip4_config_get_dns_search (setting, i);
		gboolean found = FALSE;

		/* Avoid dupes */
		for (j = 0; j < nm_ip4_config_get_num_searches (ip4_config); j++) {
			if (!strcmp (search, nm_ip4_config_get_search (ip4_config, j))) {
				found = TRUE;
				break;
			}
		}

		if (!found)
			nm_ip4_config_add_search (ip4_config, search);
	}

	/* IPv4 addresses */
	for (i = 0; i < nm_setting_ip4_config_get_num_addresses (setting); i++) {
		NMIP4Address *setting_addr = nm_setting_ip4_config_get_address (setting, i);
		guint32 num;

		num = nm_ip4_config_get_num_addresses (ip4_config);
		for (j = 0; j < num; j++) {
			NMIP4Address *cfg_addr = nm_ip4_config_get_address (ip4_config, j);

			/* Dupe, override with user-specified address */
			if (nm_ip4_address_get_address (cfg_addr) == nm_ip4_address_get_address (setting_addr)) {
				nm_ip4_config_replace_address (ip4_config, j, setting_addr);
				break;
			}
		}

		if (j == num)
			nm_ip4_config_add_address (ip4_config, setting_addr);
	}

	/* IPv4 routes */
	for (i = 0; i < nm_setting_ip4_config_get_num_routes (setting); i++) {
		NMIP4Route *setting_route = nm_setting_ip4_config_get_route (setting, i);
		guint32 num;

		num = nm_ip4_config_get_num_routes (ip4_config);
		for (j = 0; j < num; j++) {
			NMIP4Route *cfg_route = nm_ip4_config_get_route (ip4_config, j);

			/* Dupe, override with user-specified route */
			if (   (nm_ip4_route_get_dest (cfg_route) == nm_ip4_route_get_dest (setting_route))
			    && (nm_ip4_route_get_prefix (cfg_route) == nm_ip4_route_get_prefix (setting_route))
			    && (nm_ip4_route_get_next_hop (cfg_route) == nm_ip4_route_get_next_hop (setting_route))) {
				nm_ip4_config_replace_route (ip4_config, j, setting_route);
				break;
			}
		}

		if (j == num)
			nm_ip4_config_add_route (ip4_config, setting_route);
	}

	if (nm_setting_ip4_config_get_never_default (setting))
		nm_ip4_config_set_never_default (ip4_config, TRUE);
}

static inline gboolean
ip6_addresses_equal (const struct in6_addr *a, const struct in6_addr *b)
{
	return memcmp (a, b, sizeof (struct in6_addr)) == 0;
}

/* This is exactly identical to nm_utils_merge_ip4_config, with s/4/6/,
 * except that we can't compare addresses with ==.
 */
void
nm_utils_merge_ip6_config (NMIP6Config *ip6_config, NMSettingIP6Config *setting)
{
	int i, j;

	if (!setting)
		return; /* Defaults are just fine */

	if (nm_setting_ip6_config_get_ignore_auto_dns (setting)) {
		nm_ip6_config_reset_nameservers (ip6_config);
		nm_ip6_config_reset_domains (ip6_config);
		nm_ip6_config_reset_searches (ip6_config);
	}

	if (nm_setting_ip6_config_get_ignore_auto_routes (setting))
		nm_ip6_config_reset_routes (ip6_config);

	for (i = 0; i < nm_setting_ip6_config_get_num_dns (setting); i++) {
		const struct in6_addr *ns;
		gboolean found = FALSE;

		/* Avoid dupes */
		ns = nm_setting_ip6_config_get_dns (setting, i);
		for (j = 0; j < nm_ip6_config_get_num_nameservers (ip6_config); j++) {
			if (ip6_addresses_equal (nm_ip6_config_get_nameserver (ip6_config, j), ns)) {
				found = TRUE;
				break;
			}
		}

		if (!found)
			nm_ip6_config_add_nameserver (ip6_config, ns);
	}

	/* DNS search domains */
	for (i = 0; i < nm_setting_ip6_config_get_num_dns_searches (setting); i++) {
		const char *search = nm_setting_ip6_config_get_dns_search (setting, i);
		gboolean found = FALSE;

		/* Avoid dupes */
		for (j = 0; j < nm_ip6_config_get_num_searches (ip6_config); j++) {
			if (!strcmp (search, nm_ip6_config_get_search (ip6_config, j))) {
				found = TRUE;
				break;
			}
		}

		if (!found)
			nm_ip6_config_add_search (ip6_config, search);
	}

	/* IPv6 addresses */
	for (i = 0; i < nm_setting_ip6_config_get_num_addresses (setting); i++) {
		NMIP6Address *setting_addr = nm_setting_ip6_config_get_address (setting, i);
		guint32 num;

		num = nm_ip6_config_get_num_addresses (ip6_config);
		for (j = 0; j < num; j++) {
			NMIP6Address *cfg_addr = nm_ip6_config_get_address (ip6_config, j);

			/* Dupe, override with user-specified address */
			if (ip6_addresses_equal (nm_ip6_address_get_address (cfg_addr), nm_ip6_address_get_address (setting_addr))) {
				nm_ip6_config_replace_address (ip6_config, j, setting_addr);
				break;
			}
		}

		if (j == num)
			nm_ip6_config_add_address (ip6_config, setting_addr);
	}

	/* IPv6 routes */
	for (i = 0; i < nm_setting_ip6_config_get_num_routes (setting); i++) {
		NMIP6Route *setting_route = nm_setting_ip6_config_get_route (setting, i);
		guint32 num;

		num = nm_ip6_config_get_num_routes (ip6_config);
		for (j = 0; j < num; j++) {
			NMIP6Route *cfg_route = nm_ip6_config_get_route (ip6_config, j);

			/* Dupe, override with user-specified route */
			if (   ip6_addresses_equal (nm_ip6_route_get_dest (cfg_route), nm_ip6_route_get_dest (setting_route))
			    && (nm_ip6_route_get_prefix (cfg_route) == nm_ip6_route_get_prefix (setting_route))
				&& ip6_addresses_equal (nm_ip6_route_get_next_hop (cfg_route), nm_ip6_route_get_next_hop (setting_route))) {
				nm_ip6_config_replace_route (ip6_config, j, setting_route);
				break;
			}
		}

		if (j == num)
			nm_ip6_config_add_route (ip6_config, setting_route);
	}

	if (nm_setting_ip6_config_get_never_default (setting))
		nm_ip6_config_set_never_default (ip6_config, TRUE);
}

static void
dump_object_to_props (GObject *object, GHashTable *hash)
{
	GParamSpec **pspecs;
	guint len = 0, i;

	pspecs = g_object_class_list_properties (G_OBJECT_GET_CLASS (object), &len);
	for (i = 0; i < len; i++) {
		value_hash_add_object_property (hash,
		                                pspecs[i]->name,
		                                object,
		                                pspecs[i]->name,
		                                pspecs[i]->value_type);
	}
	g_free (pspecs);
}

static void
dump_dhcp4_to_props (NMDHCP4Config *config, GHashTable *hash)
{
	GSList *options, *iter;

	options = nm_dhcp4_config_list_options (config);
	for (iter = options; iter; iter = g_slist_next (iter)) {
		const char *option = (const char *) iter->data;
		const char *val;

		val = nm_dhcp4_config_get_option (config, option);
		value_hash_add_str (hash, option, val);
	}
	g_slist_free (options);
}

static void
dump_dhcp6_to_props (NMDHCP6Config *config, GHashTable *hash)
{
	GSList *options, *iter;

	options = nm_dhcp6_config_list_options (config);
	for (iter = options; iter; iter = g_slist_next (iter)) {
		const char *option = (const char *) iter->data;
		const char *val;

		val = nm_dhcp6_config_get_option (config, option);
		value_hash_add_str (hash, option, val);
	}
	g_slist_free (options);
}

static void
fill_device_props (NMDevice *device,
                   GHashTable *dev_hash,
                   GHashTable *ip4_hash,
                   GHashTable *ip6_hash,
                   GHashTable *dhcp4_hash,
                   GHashTable *dhcp6_hash)
{
	NMIP4Config *ip4_config;
	NMIP6Config *ip6_config;
	NMDHCP4Config *dhcp4_config;
	NMDHCP6Config *dhcp6_config;

	/* If the action is for a VPN, send the VPN's IP interface instead of the device's */
	value_hash_add_str (dev_hash, NMD_DEVICE_PROPS_IP_INTERFACE, nm_device_get_ip_iface (device));
	value_hash_add_str (dev_hash, NMD_DEVICE_PROPS_INTERFACE, nm_device_get_iface (device));
	value_hash_add_uint (dev_hash, NMD_DEVICE_PROPS_TYPE, nm_device_get_device_type (device));
	value_hash_add_uint (dev_hash, NMD_DEVICE_PROPS_STATE, nm_device_get_state (device));
	value_hash_add_object_path (dev_hash, NMD_DEVICE_PROPS_PATH, nm_device_get_path (device));

	ip4_config = nm_device_get_ip4_config (device);
	if (ip4_config)
		dump_object_to_props (G_OBJECT (ip4_config), ip4_hash);

	ip6_config = nm_device_get_ip6_config (device);
	if (ip6_config)
		dump_object_to_props (G_OBJECT (ip6_config), ip6_hash);

	dhcp4_config = nm_device_get_dhcp4_config (device);
	if (dhcp4_config)
		dump_dhcp4_to_props (dhcp4_config, dhcp4_hash);

	dhcp6_config = nm_device_get_dhcp6_config (device);
	if (dhcp6_config)
		dump_dhcp6_to_props (dhcp6_config, dhcp6_hash);
}

static void
fill_vpn_props (NMIP4Config *ip4_config,
                NMIP6Config *ip6_config,
                GHashTable *ip4_hash,
                GHashTable *ip6_hash)
{
	if (ip4_config)
		dump_object_to_props (G_OBJECT (ip4_config), ip4_hash);
	if (ip6_config)
		dump_object_to_props (G_OBJECT (ip6_config), ip6_hash);
}

static void
dispatcher_done_cb (DBusGProxy *proxy, DBusGProxyCall *call, gpointer user_data)
{
	dbus_g_proxy_end_call (proxy, call, NULL, G_TYPE_INVALID);
	g_object_unref (proxy);
}

void
nm_utils_call_dispatcher (const char *action,
                          NMConnection *connection,
                          NMDevice *device,
                          const char *vpn_iface,
                          NMIP4Config *vpn_ip4_config,
                          NMIP6Config *vpn_ip6_config)
{
	NMDBusManager *dbus_mgr;
	DBusGProxy *proxy;
	DBusGConnection *g_connection;
	GHashTable *connection_hash;
	GHashTable *connection_props;
	GHashTable *device_props;
	GHashTable *device_ip4_props;
	GHashTable *device_ip6_props;
	GHashTable *device_dhcp4_props;
	GHashTable *device_dhcp6_props;
	GHashTable *vpn_ip4_props;
	GHashTable *vpn_ip6_props;

	g_return_if_fail (action != NULL);

	/* All actions except 'hostname' require a device */
	if (strcmp (action, "hostname") != 0)
		g_return_if_fail (NM_IS_DEVICE (device));
	/* VPN actions require at least an IPv4 config (for now) */
	if (strcmp (action, "vpn-up") == 0)
		g_return_if_fail (vpn_ip4_config != NULL);

	dbus_mgr = nm_dbus_manager_get ();
	g_connection = nm_dbus_manager_get_connection (dbus_mgr);
	proxy = dbus_g_proxy_new_for_name (g_connection,
	                                   NM_DISPATCHER_DBUS_SERVICE,
	                                   NM_DISPATCHER_DBUS_PATH,
	                                   NM_DISPATCHER_DBUS_IFACE);
	if (!proxy) {
		nm_log_err (LOGD_CORE, "could not get dispatcher proxy!");
		g_object_unref (dbus_mgr);
		return;
	}

	if (connection) {
		connection_hash = nm_connection_to_hash (connection, NM_SETTING_HASH_FLAG_NO_SECRETS);

		connection_props = value_hash_create ();

		/* path */
		value_hash_add_object_path (connection_props,
									NMD_CONNECTION_PROPS_PATH,
									nm_connection_get_path (connection));
	} else {
		connection_hash = value_hash_create ();
		connection_props = value_hash_create ();
	}

	device_props = value_hash_create ();
	device_ip4_props = value_hash_create ();
	device_ip6_props = value_hash_create ();
	device_dhcp4_props = value_hash_create ();
	device_dhcp6_props = value_hash_create ();
	vpn_ip4_props = value_hash_create ();
	vpn_ip6_props = value_hash_create ();

	/* hostname actions only send the hostname */
	if (strcmp (action, "hostname") != 0) {
		fill_device_props (device,
			               device_props,
			               device_ip4_props,
			               device_ip6_props,
			               device_dhcp4_props,
			               device_dhcp6_props);
		if (vpn_iface)
			fill_vpn_props (vpn_ip4_config, NULL, vpn_ip4_props, vpn_ip6_props);
	}

	/* Do a non-blocking call, but wait for the reply, because dbus-glib
	 * sometimes needs time to complete internal housekeeping.  If we use
	 * dbus_g_proxy_call_no_reply(), that housekeeping (specifically the
	 * GetNameOwner response) doesn't complete and we run into an assert
	 * on unreffing the proxy.
	 */
	dbus_g_proxy_begin_call_with_timeout (proxy, "Action",
	                                      dispatcher_done_cb,
	                                      dbus_mgr,       /* automatically unref the dbus mgr when call is done */
	                                      g_object_unref,
	                                      5000,
	                                      G_TYPE_STRING, action,
	                                      DBUS_TYPE_G_MAP_OF_MAP_OF_VARIANT, connection_hash,
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, connection_props,
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_props,
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_ip4_props,
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_ip6_props,
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_dhcp4_props,
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, device_dhcp6_props,
	                                      G_TYPE_STRING, vpn_iface ? vpn_iface : "",
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, vpn_ip4_props,
	                                      DBUS_TYPE_G_MAP_OF_VARIANT, vpn_ip6_props,
	                                      G_TYPE_INVALID);
	g_hash_table_destroy (connection_hash);
	g_hash_table_destroy (connection_props);
	g_hash_table_destroy (device_props);
	g_hash_table_destroy (device_ip4_props);
	g_hash_table_destroy (device_ip6_props);
	g_hash_table_destroy (device_dhcp4_props);
	g_hash_table_destroy (device_dhcp6_props);
	g_hash_table_destroy (vpn_ip4_props);
	g_hash_table_destroy (vpn_ip6_props);
}

gboolean
nm_match_spec_hwaddr (const GSList *specs, const char *hwaddr)
{
	const GSList *iter;
	char *hwaddr_match, *p;

	g_return_val_if_fail (hwaddr != NULL, FALSE);

	p = hwaddr_match = g_strdup_printf ("mac:%s", hwaddr);

	while (*p) {
		*p = g_ascii_tolower (*p);
		p++;
	}

	for (iter = specs; iter; iter = g_slist_next (iter)) {
		if (!strcmp ((const char *) iter->data, hwaddr_match)) {
			g_free (hwaddr_match);
			return TRUE;
		}
	}

	g_free (hwaddr_match);
	return FALSE;
}

#define BUFSIZE 10

static gboolean
parse_subchannels (const char *subchannels, guint32 *a, guint32 *b, guint32 *c)
{
	long unsigned int tmp;
	char buf[BUFSIZE + 1];
	const char *p = subchannels;
	int i = 0;
	char *pa = NULL, *pb = NULL, *pc = NULL;

	g_return_val_if_fail (subchannels != NULL, FALSE);
	g_return_val_if_fail (a != NULL, FALSE);
	g_return_val_if_fail (*a == 0, FALSE);
	g_return_val_if_fail (b != NULL, FALSE);
	g_return_val_if_fail (*b == 0, FALSE);
	g_return_val_if_fail (c != NULL, FALSE);
	g_return_val_if_fail (*c == 0, FALSE);

	/* sanity check */
	if (!isxdigit (subchannels[0]))
		return FALSE;

	/* Get the first channel */
	while (*p && (*p != ',')) {
		if (!isxdigit (*p) && (*p != '.'))
			return FALSE;  /* Invalid chars */
		if (i >= BUFSIZE)
			return FALSE;  /* Too long to be a subchannel */
		buf[i++] = *p++;
	}
	buf[i] = '\0';

	/* and grab each of its elements, there should be 3 */
	pa = &buf[0];
	pb = strchr (buf, '.');
	if (pb)
		pc = strchr (pb + 1, '.');
	if (!pa || !pb || !pc)
		return FALSE;

	/* Split the string */
	*pb++ = '\0';
	*pc++ = '\0';

	errno = 0;
	tmp = strtoul (pa, NULL, 16);
	if (errno)
		return FALSE;
	*a = (guint32) tmp;

	errno = 0;
	tmp = strtoul (pb, NULL, 16);
	if (errno)
		return FALSE;
	*b = (guint32) tmp;

	errno = 0;
	tmp = strtoul (pc, NULL, 16);
	if (errno)
		return FALSE;
	*c = (guint32) tmp;

	return TRUE;
}

#define SUBCHAN_TAG "s390-subchannels:"

gboolean
nm_match_spec_s390_subchannels (const GSList *specs, const char *subchannels)
{
	const GSList *iter;
	guint32 a = 0, b = 0, c = 0;
	guint32 spec_a = 0, spec_b = 0, spec_c = 0;

	g_return_val_if_fail (subchannels != NULL, FALSE);

	if (!parse_subchannels (subchannels, &a, &b, &c))
		return FALSE;

	for (iter = specs; iter; iter = g_slist_next (iter)) {
		const char *spec = iter->data;

		if (!strncmp (spec, SUBCHAN_TAG, strlen (SUBCHAN_TAG))) {
			spec += strlen (SUBCHAN_TAG);
			if (parse_subchannels (spec, &spec_a, &spec_b, &spec_c)) {
				if (a == spec_a && b == spec_b && c == spec_c)
					return TRUE;
			}
		}
	}

	return FALSE;
}

const char *
nm_utils_get_shared_wifi_permission (NMConnection *connection)
{
	NMSettingWireless *s_wifi;
	NMSettingWirelessSecurity *s_wsec;
	NMSettingIP4Config *s_ip4;
	const char *method = NULL;

	s_ip4 = nm_connection_get_setting_ip4_config (connection);
	if (s_ip4)
		method = nm_setting_ip4_config_get_method (s_ip4);

	if (g_strcmp0 (method, NM_SETTING_IP4_CONFIG_METHOD_SHARED) != 0)
		return NULL;  /* Not shared */

	s_wifi = nm_connection_get_setting_wireless (connection);
	if (s_wifi) {
		s_wsec = nm_connection_get_setting_wireless_security (connection);
		if (nm_setting_wireless_get_security (s_wifi) || s_wsec)
			return NM_AUTH_PERMISSION_WIFI_SHARE_PROTECTED;
		else
			return NM_AUTH_PERMISSION_WIFI_SHARE_OPEN;
	}

	return NULL;
}

/*********************************/

static void
nm_gvalue_destroy (gpointer data)
{
	GValue *value = (GValue *) data;

	g_value_unset (value);
	g_slice_free (GValue, value);
}

GHashTable *
value_hash_create (void)
{
	return g_hash_table_new_full (g_str_hash, g_str_equal, g_free, nm_gvalue_destroy);
}

void
value_hash_add (GHashTable *hash,
				const char *key,
				GValue *value)
{
	g_hash_table_insert (hash, g_strdup (key), value);
}

void
value_hash_add_str (GHashTable *hash,
					const char *key,
					const char *str)
{
	GValue *value;

	value = g_slice_new0 (GValue);
	g_value_init (value, G_TYPE_STRING);
	g_value_set_string (value, str);

	value_hash_add (hash, key, value);
}

void
value_hash_add_object_path (GHashTable *hash,
							const char *key,
							const char *op)
{
	GValue *value;

	value = g_slice_new0 (GValue);
	g_value_init (value, DBUS_TYPE_G_OBJECT_PATH);
	g_value_set_boxed (value, op);

	value_hash_add (hash, key, value);
}

void
value_hash_add_uint (GHashTable *hash,
					 const char *key,
					 guint32 val)
{
	GValue *value;

	value = g_slice_new0 (GValue);
	g_value_init (value, G_TYPE_UINT);
	g_value_set_uint (value, val);

	value_hash_add (hash, key, value);
}

void
value_hash_add_bool (GHashTable *hash,
					 const char *key,
					 gboolean val)
{
	GValue *value;

	value = g_slice_new0 (GValue);
	g_value_init (value, G_TYPE_BOOLEAN);
	g_value_set_boolean (value, val);

	value_hash_add (hash, key, value);
}

void
value_hash_add_object_property (GHashTable *hash,
                                const char *key,
                                GObject *object,
                                const char *prop,
                                GType val_type)
{
	GValue *value;

	value = g_slice_new0 (GValue);
	g_value_init (value, val_type);
	g_object_get_property (object, prop, value);
	value_hash_add (hash, key, value);
}

gboolean
nm_utils_do_sysctl (const char *path, const char *value)
{
	int fd, len, nwrote, total;

	fd = open (path, O_WRONLY | O_TRUNC);
	if (fd == -1)
		return FALSE;

	len = strlen (value);
	total = 0;
	do {
		nwrote = write (fd, value + total, len - total);
		if (nwrote == -1) {
			if (errno == EINTR)
				continue;
			close (fd);
			return FALSE;
		}
		total += nwrote;
	} while (total < len);

	close (fd);
	return TRUE;
}

gboolean
nm_utils_get_proc_sys_net_value (const char *path,
                                 const char *iface,
                                 guint32 *out_value)
{
	GError *error = NULL;
	char *contents = NULL;
	gboolean success = FALSE;
	long int tmp;

	if (!g_file_get_contents (path, &contents, NULL, &error)) {
		nm_log_dbg (LOGD_DEVICE, "(%s): error reading %s: (%d) %s",
		            iface, path,
		            error ? error->code : -1,
		            error && error->message ? error->message : "(unknown)");
		g_clear_error (&error);
	} else {
		errno = 0;
		tmp = strtol (contents, NULL, 10);
		if ((errno == 0) && (tmp == 0 || tmp == 1)) {
			*out_value = (guint32) tmp;
			success = TRUE;
		}
		g_free (contents);
	}

	return success;
}

static char *
get_new_connection_name (const GSList *existing,
                         const char *format,
                         const char *preferred)
{
	GSList *names = NULL;
	const GSList *iter;
	char *cname = NULL;
	int i = 0;
	gboolean preferred_found = FALSE;

	for (iter = existing; iter; iter = g_slist_next (iter)) {
		NMConnection *candidate = NM_CONNECTION (iter->data);
		const char *id;

		id = nm_connection_get_id (candidate);
		g_assert (id);
		names = g_slist_append (names, (gpointer) id);

		if (preferred && !preferred_found && (strcmp (preferred, id) == 0))
			preferred_found = TRUE;
	}

	/* Return the preferred name if it was unique */
	if (preferred && !preferred_found) {
		g_slist_free (names);
		return g_strdup (preferred);
	}

	/* Otherwise find the next available unique connection name using the given
	 * connection name template.
	 */
	while (!cname && (i++ < 10000)) {
		char *temp;
		gboolean found = FALSE;

		temp = g_strdup_printf (format, i);
		for (iter = names; iter; iter = g_slist_next (iter)) {
			if (!strcmp (iter->data, temp)) {
				found = TRUE;
				break;
			}
		}
		if (!found)
			cname = temp;
		else
			g_free (temp);
	}

	g_slist_free (names);
	return cname;
}

void
nm_utils_complete_generic (NMConnection *connection,
                           const char *ctype,
                           const GSList *existing,
                           const char *format,
                           const char *preferred,
                           gboolean default_enable_ipv6)
{
	NMSettingConnection *s_con;
	NMSettingIP4Config *s_ip4;
	NMSettingIP6Config *s_ip6;
	const char *method;
	char *id, *uuid;

	s_con = nm_connection_get_setting_connection (connection);
	if (!s_con) {
		s_con = (NMSettingConnection *) nm_setting_connection_new ();
		nm_connection_add_setting (connection, NM_SETTING (s_con));
	}
	g_object_set (G_OBJECT (s_con), NM_SETTING_CONNECTION_TYPE, ctype, NULL);

	if (!nm_setting_connection_get_uuid (s_con)) {
		uuid = nm_utils_uuid_generate ();
		g_object_set (G_OBJECT (s_con), NM_SETTING_CONNECTION_UUID, uuid, NULL);
		g_free (uuid);
	}

	/* Add a connection ID if absent */
	if (!nm_setting_connection_get_id (s_con)) {
		id = get_new_connection_name (existing, format, preferred);
		g_object_set (G_OBJECT (s_con), NM_SETTING_CONNECTION_ID, id, NULL);
		g_free (id);
	}

	/* Add an 'auto' IPv4 connection if present */
	s_ip4 = nm_connection_get_setting_ip4_config (connection);
	if (!s_ip4) {
		s_ip4 = (NMSettingIP4Config *) nm_setting_ip4_config_new ();
		nm_connection_add_setting (connection, NM_SETTING (s_ip4));
	}
	method = nm_setting_ip4_config_get_method (s_ip4);
	if (!method) {
		g_object_set (G_OBJECT (s_ip4),
		              NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO,
		              NULL);
	}

	/* Add an 'auto' IPv6 setting if allowed and not preset */
	s_ip6 = nm_connection_get_setting_ip6_config (connection);
	if (!s_ip6 && default_enable_ipv6) {
		s_ip6 = (NMSettingIP6Config *) nm_setting_ip6_config_new ();
		nm_connection_add_setting (connection, NM_SETTING (s_ip6));
	}
	if (s_ip6 && !nm_setting_ip6_config_get_method (s_ip6)) {
		g_object_set (G_OBJECT (s_ip6),
		              NM_SETTING_IP6_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO,
		              NM_SETTING_IP6_CONFIG_MAY_FAIL, TRUE,
		              NULL);
	}
}

