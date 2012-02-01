/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-tool - information tool for NetworkManager
 *
 * Dan Williams <dcbw@redhat.com>
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
 * (C) Copyright 2005 - 2011 Red Hat, Inc.
 * (C) Copyright 2007 Novell, Inc.
 */

#include <config.h>
#include <glib.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <nm-client.h>
#include <nm-device.h>
#include <nm-device-ethernet.h>
#include <nm-device-wifi.h>
#include <nm-device-modem.h>
#include <nm-device-bt.h>
#include <nm-device-wimax.h>
#include <nm-utils.h>
#include <nm-setting-ip4-config.h>
#include <nm-setting-ip6-config.h>
#include <nm-vpn-connection.h>
#include <nm-setting-connection.h>

/* Don't use nm-dbus-glib-types.h so that we can keep nm-tool
 * building standalone outside of the NM tree.
 */
#define DBUS_TYPE_G_MAP_OF_VARIANT          (dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, G_TYPE_VALUE))
#define DBUS_TYPE_G_MAP_OF_MAP_OF_VARIANT   (dbus_g_type_get_map ("GHashTable", G_TYPE_STRING, DBUS_TYPE_G_MAP_OF_VARIANT))
#define DBUS_TYPE_G_ARRAY_OF_OBJECT_PATH    (dbus_g_type_get_collection ("GPtrArray", DBUS_TYPE_G_OBJECT_PATH))

static GHashTable *connections = NULL;

static gboolean
get_nm_state (NMClient *client)
{
	NMState state;
	char *state_string;
	gboolean success = TRUE;

	state = nm_client_get_state (client);

	switch (state) {
	case NM_STATE_ASLEEP:
		state_string = "asleep";
		break;
	case NM_STATE_CONNECTING:
		state_string = "connecting";
		break;
	case NM_STATE_CONNECTED_LOCAL:
		state_string = "connected (local only)";
		break;
	case NM_STATE_CONNECTED_SITE:
		state_string = "connected (site only)";
		break;
	case NM_STATE_CONNECTED_GLOBAL:
		state_string = "connected (global)";
		break;
	case NM_STATE_DISCONNECTED:
		state_string = "disconnected";
		break;
	case NM_STATE_UNKNOWN:
	default:
		state_string = "unknown";
		success = FALSE;
		break;
	}

	printf ("State: %s\n\n", state_string);

	return success;
}

static void
print_header (const char *label, const char *iface, const char *connection)
{
	GString *string;

	string = g_string_sized_new (79);
	g_string_append_printf (string, "- %s: ", label);
	if (iface)
		g_string_append_printf (string, "%s ", iface);
	if (connection)
		g_string_append_printf (string, " [%s] ", connection);

	while (string->len < 80)
		g_string_append_c (string, '-');

	printf ("%s\n", string->str);

	g_string_free (string, TRUE);
}

static void
print_string (const char *label, const char *data)
{
#define SPACING 18
	int label_len = 0;
	char spaces[50];
	int i;

	g_return_if_fail (label != NULL);
	g_return_if_fail (data != NULL);

	label_len = strlen (label);
	if (label_len > SPACING)
		label_len = SPACING - 1;
	for (i = 0; i < (SPACING - label_len); i++)
		spaces[i] = 0x20;
	spaces[i] = 0x00;

	printf ("  %s:%s%s\n", label, &spaces[0], data);
}


static void
detail_access_point (gpointer data, gpointer user_data)
{
	NMAccessPoint *ap = NM_ACCESS_POINT (data);
	const char *active_bssid = (const char *) user_data;
	GString *str;
	gboolean active = FALSE;
	guint32 flags, wpa_flags, rsn_flags;
	const GByteArray * ssid;
	char *tmp;

	flags = nm_access_point_get_flags (ap);
	wpa_flags = nm_access_point_get_wpa_flags (ap);
	rsn_flags = nm_access_point_get_rsn_flags (ap);

	if (active_bssid) {
		const char *current_bssid = nm_access_point_get_hw_address (ap);
		if (current_bssid && !strcmp (current_bssid, active_bssid))
			active = TRUE;
	}

	str = g_string_new (NULL);
	g_string_append_printf (str,
	                        "%s, %s, Freq %d MHz, Rate %d Mb/s, Strength %d",
	                        (nm_access_point_get_mode (ap) == NM_802_11_MODE_INFRA) ? "Infra" : "Ad-Hoc",
	                        nm_access_point_get_hw_address (ap),
	                        nm_access_point_get_frequency (ap),
	                        nm_access_point_get_max_bitrate (ap) / 1000,
	                        nm_access_point_get_strength (ap));

	if (   !(flags & NM_802_11_AP_FLAGS_PRIVACY)
	    &&  (wpa_flags != NM_802_11_AP_SEC_NONE)
	    &&  (rsn_flags != NM_802_11_AP_SEC_NONE))
		g_string_append (str, ", Encrypted: ");

	if (   (flags & NM_802_11_AP_FLAGS_PRIVACY)
	    && (wpa_flags == NM_802_11_AP_SEC_NONE)
	    && (rsn_flags == NM_802_11_AP_SEC_NONE))
		g_string_append (str, " WEP");
	if (wpa_flags != NM_802_11_AP_SEC_NONE)
		g_string_append (str, " WPA");
	if (rsn_flags != NM_802_11_AP_SEC_NONE)
		g_string_append (str, " WPA2");
	if (   (wpa_flags & NM_802_11_AP_SEC_KEY_MGMT_802_1X)
	    || (rsn_flags & NM_802_11_AP_SEC_KEY_MGMT_802_1X))
		g_string_append (str, " Enterprise");

	/* FIXME: broadcast/hidden */

	ssid = nm_access_point_get_ssid (ap);
	tmp = g_strdup_printf ("  %s%s", active ? "*" : "",
	                       ssid ? nm_utils_escape_ssid (ssid->data, ssid->len) : "(none)");

	print_string (tmp, str->str);

	g_string_free (str, TRUE);
	g_free (tmp);
}

static const char *
wimax_network_type_to_str (NMWimaxNspNetworkType type)
{
	switch (type) {
	case NM_WIMAX_NSP_NETWORK_TYPE_HOME:
		return "Home";
	case NM_WIMAX_NSP_NETWORK_TYPE_PARTNER:
		return "Partner";
	case NM_WIMAX_NSP_NETWORK_TYPE_ROAMING_PARTNER:
		return "Roaming";
	default:
		return "Unknown";
	}
}

static void
detail_nsp (gpointer data, gpointer user_data)
{
	NMWimaxNsp *nsp = NM_WIMAX_NSP (data);
	const char *active_name = (const char *) user_data;
	const char *name;
	char *label;
	char *data_str;
	gboolean active = FALSE;

	name = nm_wimax_nsp_get_name (nsp);

	if (active_name)
		active = g_strcmp0 (active_name, name) == 0;

	label = g_strdup_printf ("  %s%s", active ? "*" : "", name);
	data_str = g_strdup_printf ("%d%% (%s)",
				    nm_wimax_nsp_get_signal_quality (nsp),
				    wimax_network_type_to_str (nm_wimax_nsp_get_network_type (nsp)));

	print_string (label, data_str);
	g_free (label);
	g_free (data_str);
}

static gchar *
ip4_address_as_string (guint32 ip)
{
	struct in_addr tmp_addr;
	char buf[INET_ADDRSTRLEN+1];

	memset (&buf, '\0', sizeof (buf));
	tmp_addr.s_addr = ip;

	if (inet_ntop (AF_INET, &tmp_addr, buf, INET_ADDRSTRLEN)) {
		return g_strdup (buf);
	} else {
		g_warning ("%s: error converting IP4 address 0x%X",
		           __func__, ntohl (tmp_addr.s_addr));
		return NULL;
	}
}

static gchar *
ip6_address_as_string (const struct in6_addr *ip)
{
	char buf[INET6_ADDRSTRLEN];

	memset (&buf, '\0', sizeof (buf));

	if (inet_ntop (AF_INET6, ip, buf, INET6_ADDRSTRLEN)) {
		return g_strdup (buf);
	} else {
		int j;
		GString *ip6_str = g_string_new (NULL);
		g_string_append_printf (ip6_str, "%02X", ip->s6_addr[0]);
		for (j = 1; j < 16; j++)
			g_string_append_printf (ip6_str, " %02X", ip->s6_addr[j]);
		g_warning ("%s: error converting IP6 address %s",
		           __func__, ip6_str->str);
		g_string_free (ip6_str, TRUE);
		return NULL;
	}
}

static const char *
get_dev_state_string (NMDeviceState state)
{
	if (state == NM_DEVICE_STATE_UNMANAGED)
		return "unmanaged";
	else if (state == NM_DEVICE_STATE_UNAVAILABLE)
		return "unavailable";
	else if (state == NM_DEVICE_STATE_DISCONNECTED)
		return "disconnected";
	else if (state == NM_DEVICE_STATE_PREPARE)
		return "connecting (prepare)";
	else if (state == NM_DEVICE_STATE_CONFIG)
		return "connecting (configuring)";
	else if (state == NM_DEVICE_STATE_NEED_AUTH)
		return "connecting (need authentication)";
	else if (state == NM_DEVICE_STATE_IP_CONFIG)
		return "connecting (getting IP configuration)";
	else if (state == NM_DEVICE_STATE_IP_CHECK)
		return "connecting (checking IP connectivity)";
	else if (state == NM_DEVICE_STATE_SECONDARIES)
		return "connecting (starting dependent connections)";
	else if (state == NM_DEVICE_STATE_ACTIVATED)
		return "connected";
	else if (state == NM_DEVICE_STATE_DEACTIVATING)
		return "disconnecting";
	else if (state == NM_DEVICE_STATE_FAILED)
		return "connection failed";
	return "unknown";
}

static NMConnection *
get_connection_for_active (NMActiveConnection *active)
{
	const char *path;

	g_return_val_if_fail (active != NULL, NULL);

	path = nm_active_connection_get_connection (active);
	g_return_val_if_fail (path != NULL, NULL);

	return (NMConnection *) g_hash_table_lookup (connections, path);
}

static void
detail_device (gpointer data, gpointer user_data)
{
	NMDevice *device = NM_DEVICE (data);
	char *tmp;
	NMDeviceState state;
	guint32 caps;
	guint32 speed;
	const GArray *array;
	gboolean is_default = FALSE;
	const char *id = NULL;
	NMActiveConnection *active;

	active = nm_device_get_active_connection (device);
	if (active) {
		NMConnection *connection;
		NMSettingConnection *s_con;

		is_default = nm_active_connection_get_default (active);

		connection = get_connection_for_active (active);
		if (connection) {
			s_con = nm_connection_get_setting_connection (connection);
			if (s_con)
				id = nm_setting_connection_get_id (s_con);
		}
	}

	print_header ("Device", nm_device_get_iface (device), id);

	/* General information */
	if (NM_IS_DEVICE_ETHERNET (device))
		print_string ("Type", "Wired");
	else if (NM_IS_DEVICE_WIFI (device))
		print_string ("Type", "802.11 WiFi");
	else if (NM_IS_DEVICE_MODEM (device)) {
		NMDeviceModemCapabilities modem_caps;

		modem_caps = nm_device_modem_get_current_capabilities (NM_DEVICE_MODEM (device));
		if (modem_caps & NM_DEVICE_MODEM_CAPABILITY_GSM_UMTS)
			print_string ("Type", "Mobile Broadband (GSM)");
		else if (modem_caps & NM_DEVICE_MODEM_CAPABILITY_CDMA_EVDO)
			print_string ("Type", "Mobile Broadband (CDMA)");
		else
			print_string ("Type", "Mobile Broadband (unknown)");
	} else if (NM_IS_DEVICE_BT (device))
		print_string ("Type", "Bluetooth");
	else if (NM_IS_DEVICE_WIMAX (device))
		print_string ("Type", "WiMAX");

	print_string ("Driver", nm_device_get_driver (device) ? nm_device_get_driver (device) : "(unknown)");

	state = nm_device_get_state (device);
	print_string ("State", get_dev_state_string (state));

	if (is_default)
		print_string ("Default", "yes");
	else
		print_string ("Default", "no");

	tmp = NULL;
	if (NM_IS_DEVICE_ETHERNET (device))
		tmp = g_strdup (nm_device_ethernet_get_hw_address (NM_DEVICE_ETHERNET (device)));
	else if (NM_IS_DEVICE_WIFI (device))
		tmp = g_strdup (nm_device_wifi_get_hw_address (NM_DEVICE_WIFI (device)));
	else if (NM_IS_DEVICE_WIMAX (device))
		tmp = g_strdup (nm_device_wimax_get_hw_address (NM_DEVICE_WIMAX (device)));

	if (tmp) {
		print_string ("HW Address", tmp);
		g_free (tmp);
	}

	/* Capabilities */
	caps = nm_device_get_capabilities (device);
	printf ("\n  Capabilities:\n");
	if (caps & NM_DEVICE_CAP_CARRIER_DETECT)
		print_string ("  Carrier Detect", "yes");

	speed = 0;
	if (NM_IS_DEVICE_ETHERNET (device)) {
		/* Speed in Mb/s */
		speed = nm_device_ethernet_get_speed (NM_DEVICE_ETHERNET (device));
	} else if (NM_IS_DEVICE_WIFI (device)) {
		/* Speed in b/s */
		speed = nm_device_wifi_get_bitrate (NM_DEVICE_WIFI (device));
		speed /= 1000;
	}

	if (speed) {
		char *speed_string;

		speed_string = g_strdup_printf ("%u Mb/s", speed);
		print_string ("  Speed", speed_string);
		g_free (speed_string);
	}

	/* Wireless specific information */
	if ((NM_IS_DEVICE_WIFI (device))) {
		guint32 wcaps;
		NMAccessPoint *active_ap = NULL;
		const char *active_bssid = NULL;
		const GPtrArray *aps;

		printf ("\n  Wireless Properties\n");

		wcaps = nm_device_wifi_get_capabilities (NM_DEVICE_WIFI (device));

		if (wcaps & (NM_WIFI_DEVICE_CAP_CIPHER_WEP40 | NM_WIFI_DEVICE_CAP_CIPHER_WEP104))
			print_string ("  WEP Encryption", "yes");
		if (wcaps & NM_WIFI_DEVICE_CAP_WPA)
			print_string ("  WPA Encryption", "yes");
		if (wcaps & NM_WIFI_DEVICE_CAP_RSN)
			print_string ("  WPA2 Encryption", "yes");

		if (nm_device_get_state (device) == NM_DEVICE_STATE_ACTIVATED) {
			active_ap = nm_device_wifi_get_active_access_point (NM_DEVICE_WIFI (device));
			active_bssid = active_ap ? nm_access_point_get_hw_address (active_ap) : NULL;
		}

		printf ("\n  Wireless Access Points %s\n", active_ap ? "(* = current AP)" : "");

		aps = nm_device_wifi_get_access_points (NM_DEVICE_WIFI (device));
		if (aps && aps->len)
			g_ptr_array_foreach ((GPtrArray *) aps, detail_access_point, (gpointer) active_bssid);
	} else if (NM_IS_DEVICE_ETHERNET (device)) {
		printf ("\n  Wired Properties\n");

		if (nm_device_ethernet_get_carrier (NM_DEVICE_ETHERNET (device)))
			print_string ("  Carrier", "on");
		else
			print_string ("  Carrier", "off");
	} else if (NM_IS_DEVICE_WIMAX (device)) {
		NMDeviceWimax *wimax = NM_DEVICE_WIMAX (device);
		NMWimaxNsp *active_nsp = NULL;
		const char *active_name = NULL;
		const GPtrArray *nsps;

		if (nm_device_get_state (device) == NM_DEVICE_STATE_ACTIVATED) {
			guint tmp_uint;
			gint tmp_int;
			const char *tmp_str;

			active_nsp = nm_device_wimax_get_active_nsp (wimax);
			active_name = active_nsp ? nm_wimax_nsp_get_name (active_nsp) : NULL;

			printf ("\n  Link Status\n");

			tmp_uint = nm_device_wimax_get_center_frequency (wimax);
			if (tmp_uint)
				tmp = g_strdup_printf ("%'.1f MHz", (double) tmp_uint / 1000.0);
			else
				tmp = g_strdup ("(unknown)");
			print_string ("  Center Freq.", tmp);
			g_free (tmp);

			tmp_int = nm_device_wimax_get_rssi (wimax);
			if (tmp_int)
				tmp = g_strdup_printf ("%d dBm", tmp_int);
			else
				tmp = g_strdup ("(unknown)");
			print_string ("  RSSI", tmp);
			g_free (tmp);

			tmp_int = nm_device_wimax_get_cinr (wimax);
			if (tmp_int)
				tmp = g_strdup_printf ("%d dB", tmp_int);
			else
				tmp = g_strdup ("(unknown)");
			print_string ("  CINR", tmp);
			g_free (tmp);

			tmp_int = nm_device_wimax_get_tx_power (wimax);
			if (tmp_int)
				tmp = g_strdup_printf ("%'.2f dBm", (float) tmp_int / 2.0);
			else
				tmp = g_strdup ("(unknown)");
			print_string ("  TX Power", tmp);
			g_free (tmp);

			tmp_str = nm_device_wimax_get_bsid (wimax);
			if (tmp_str)
				print_string ("  BSID", tmp_str);
			else
				print_string ("  BSID", "(unknown)");
		}

		printf ("\n  WiMAX NSPs %s\n", active_nsp ? "(* current NSP)" : "");

		nsps = nm_device_wimax_get_nsps (NM_DEVICE_WIMAX (device));
		if (nsps && nsps->len)
			g_ptr_array_foreach ((GPtrArray *) nsps, detail_nsp, (gpointer) active_name);
	}

	/* IP Setup info */
	if (state == NM_DEVICE_STATE_ACTIVATED) {
		NMIP4Config *cfg4 = nm_device_get_ip4_config (device);
		NMIP6Config *cfg6 = nm_device_get_ip6_config (device);
		GSList *iter;

		if (cfg4) {
			printf ("\n  IPv4 Settings:\n");

			for (iter = (GSList *) nm_ip4_config_get_addresses (cfg4); iter; iter = g_slist_next (iter)) {
				NMIP4Address *addr = (NMIP4Address *) iter->data;
				guint32 prefix = nm_ip4_address_get_prefix (addr);
				char *tmp2;

				tmp = ip4_address_as_string (nm_ip4_address_get_address (addr));
				print_string ("  Address", tmp);
				g_free (tmp);

				tmp2 = ip4_address_as_string (nm_utils_ip4_prefix_to_netmask (prefix));
				tmp = g_strdup_printf ("%d (%s)", prefix, tmp2);
				g_free (tmp2);
				print_string ("  Prefix", tmp);
				g_free (tmp);

				tmp = ip4_address_as_string (nm_ip4_address_get_gateway (addr));
				print_string ("  Gateway", tmp);
				g_free (tmp);
				printf ("\n");
			}

			array = nm_ip4_config_get_nameservers (cfg4);
			if (array) {
				int i;

				for (i = 0; i < array->len; i++) {
					tmp = ip4_address_as_string (g_array_index (array, guint32, i));
					print_string ("  DNS", tmp);
					g_free (tmp);
				}
			}
		}

		if (cfg6) {
			printf ("\n  IPv6 Settings:\n");

			for (iter = (GSList *) nm_ip6_config_get_addresses (cfg6); iter; iter = g_slist_next (iter)) {
				NMIP6Address *addr = (NMIP6Address *) iter->data;
				guint32 prefix = nm_ip6_address_get_prefix (addr);

				tmp = ip6_address_as_string (nm_ip6_address_get_address (addr));
				print_string ("  Address", tmp);
				g_free (tmp);

				tmp = g_strdup_printf ("%d", prefix);
				print_string ("  Prefix", tmp);
				g_free (tmp);

				tmp = ip6_address_as_string (nm_ip6_address_get_gateway (addr));
				print_string ("  Gateway", tmp);
				g_free (tmp);
				printf ("\n");
			}

			for (iter = (GSList *) nm_ip6_config_get_nameservers (cfg6); iter; iter = g_slist_next (iter)) {
				tmp = ip6_address_as_string (iter->data);
				print_string ("  DNS", tmp);
				g_free (tmp);
			}
		}
	}

	printf ("\n\n");
}

static const char *
get_vpn_state_string (NMVPNConnectionState state)
{
	switch (state) {
	case NM_VPN_CONNECTION_STATE_PREPARE:
		return "connecting (prepare)";
	case NM_VPN_CONNECTION_STATE_NEED_AUTH:
		return "connecting (need authentication)";
	case NM_VPN_CONNECTION_STATE_CONNECT:
		return "connecting";
	case NM_VPN_CONNECTION_STATE_IP_CONFIG_GET:
		return "connecting (getting IP configuration)";
	case NM_VPN_CONNECTION_STATE_ACTIVATED:
		return "connected";
	case NM_VPN_CONNECTION_STATE_FAILED:
		return "connection failed";
	case NM_VPN_CONNECTION_STATE_DISCONNECTED:
		return "disconnected";
	default:
		break;
	}

	return "unknown";
}

static void
detail_vpn (gpointer data, gpointer user_data)
{
	NMActiveConnection *active = NM_ACTIVE_CONNECTION (data);
	NMConnection *connection;
	NMSettingConnection *s_con;
	NMVPNConnectionState state;
	const char *banner;

	if (!NM_IS_VPN_CONNECTION (active))
		return;

	connection = get_connection_for_active (active);
	g_return_if_fail (connection != NULL);

	s_con = nm_connection_get_setting_connection (connection);
	g_return_if_fail (s_con != NULL);

	print_header ("VPN", NULL, nm_setting_connection_get_id (s_con));

	state = nm_vpn_connection_get_vpn_state (NM_VPN_CONNECTION (active));
	print_string ("State", get_vpn_state_string (state));

	if (nm_active_connection_get_default (active))
		print_string ("Default", "yes");
	else
		print_string ("Default", "no");

	banner = nm_vpn_connection_get_banner (NM_VPN_CONNECTION (active));
	if (banner) {
		char **lines, **iter;

		printf ("\n  Message:\n");
		lines = g_strsplit_set (banner, "\n\r", -1);
		for (iter = lines; *iter; iter++) {
			if (*iter && strlen (*iter))
				printf ("    %s\n", *iter);
		}
		g_strfreev (lines);
	}

	printf ("\n\n");
}

static void
get_one_connection (DBusGConnection *bus,
                    const char *path,
                    GHashTable *table)
{
	DBusGProxy *proxy;
	NMConnection *connection = NULL;
	GError *error = NULL;
	GHashTable *settings = NULL;

	g_return_if_fail (bus != NULL);
	g_return_if_fail (path != NULL);
	g_return_if_fail (table != NULL);

	proxy = dbus_g_proxy_new_for_name (bus, NM_DBUS_SERVICE,
	                                   path, NM_DBUS_IFACE_SETTINGS_CONNECTION);
	if (!proxy)
		return;

	if (!dbus_g_proxy_call (proxy, "GetSettings", &error,
	                        G_TYPE_INVALID,
	                        DBUS_TYPE_G_MAP_OF_MAP_OF_VARIANT, &settings,
	                        G_TYPE_INVALID)) {
		g_warning ("error: cannot retrieve connection: %s", error ? error->message : "(unknown)");
		goto out;
	}

	connection = nm_connection_new_from_hash (settings, &error);
	g_hash_table_destroy (settings);

	if (!connection) {
		g_warning ("error: invalid connection: '%s' / '%s' invalid: %d",
		           error ? g_type_name (nm_connection_lookup_setting_type_by_quark (error->domain)) : "(unknown)",
		           error ? error->message : "(unknown)",
		           error ? error->code : -1);
		goto out;
	}

	nm_connection_set_path (connection, path);
	g_hash_table_insert (table, g_strdup (path), g_object_ref (connection));

out:
	g_clear_error (&error);
	if (connection)
		g_object_unref (connection);
	g_object_unref (proxy);
}

static gboolean
get_all_connections (void)
{
	GError *error = NULL;
	DBusGConnection *bus;
	DBusGProxy *proxy = NULL;
	GPtrArray *paths = NULL;
	int i;
	gboolean sucess = FALSE;

	bus = dbus_g_bus_get (DBUS_BUS_SYSTEM, &error);
	if (error || !bus) {
		g_warning ("error: could not connect to dbus");
		goto out;
	}

	proxy = dbus_g_proxy_new_for_name (bus,
	                                   NM_DBUS_SERVICE,
	                                   NM_DBUS_PATH_SETTINGS,
	                                   NM_DBUS_IFACE_SETTINGS);
	if (!proxy) {
		g_warning ("error: failed to create DBus proxy for settings service");
		goto out;
	}

	if (!dbus_g_proxy_call (proxy, "ListConnections", &error,
                                G_TYPE_INVALID,
                                DBUS_TYPE_G_ARRAY_OF_OBJECT_PATH, &paths,
                                G_TYPE_INVALID)) {
		/* No connections or settings service may not be running */
		g_clear_error (&error);
		goto out;
	}

	connections = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

	for (i = 0; paths && (i < paths->len); i++)
		get_one_connection (bus, g_ptr_array_index (paths, i), connections);

	sucess = TRUE;

out:
	if (bus)
		dbus_g_connection_unref (bus);
	if (proxy)
		g_object_unref (proxy);

	return sucess;
}

int
main (int argc, char *argv[])
{
	NMClient *client;
	const GPtrArray *devices;
	const GPtrArray *active;

	g_type_init ();

	client = nm_client_new ();
	if (!client) {
		exit (1);
	}

	printf ("\nNetworkManager Tool\n\n");

	if (!get_nm_state (client)) {
		g_warning ("error: could not connect to NetworkManager");
		exit (1);
	}

	if (!get_all_connections ())
		exit (1);

	devices = nm_client_get_devices (client);
	if (devices)
		g_ptr_array_foreach ((GPtrArray *) devices, detail_device, NULL);

	active = nm_client_get_active_connections (client);
	if (active)
		g_ptr_array_foreach ((GPtrArray *) active, detail_vpn, NULL);

	g_object_unref (client);
	g_hash_table_unref (connections);

	return 0;
}
