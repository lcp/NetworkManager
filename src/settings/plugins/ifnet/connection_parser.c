/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/*
 * Mu Qiao <qiaomuf@gmail.com>
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
 * Copyright (C) 1999-2010 Gentoo Foundation, Inc.
 */

#include "config.h"

#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <netinet/ether.h>
#include <errno.h>
#include <ctype.h>
#include <glib/gi18n.h>

#include <nm-setting-connection.h>
#include <nm-setting-ip4-config.h>
#include <nm-setting-ip6-config.h>
#include <nm-setting-ppp.h>
#include <nm-setting-pppoe.h>
#include <nm-setting-wired.h>
#include <nm-setting-wireless.h>
#include <nm-setting-8021x.h>
#include <nm-system-config-interface.h>
#include <nm-utils.h>

#include "net_utils.h"
#include "wpa_parser.h"
#include "connection_parser.h"
#include "nm-ifnet-connection.h"

static const char *
get_prefix (void)
{
	return _("System");
}

static void
update_connection_id (NMConnection *connection, const char *conn_name)
{
	gchar *idstr = NULL;
	gchar *uuid_base = NULL;
	gchar *uuid = NULL;
	int name_len;
	NMSettingConnection *setting;

	name_len = strlen (conn_name);
	if ((name_len > 2) && (g_str_has_prefix (conn_name, "0x"))) {
		gchar * conn_name_printable = utils_hexstr2bin (conn_name + 2, name_len - 2);
		idstr = g_strdup_printf ("%s (%s)", get_prefix (), conn_name_printable);
		g_free (conn_name_printable);
	} else
		idstr = g_strdup_printf ("%s (%s)", get_prefix (), conn_name);
	uuid_base = idstr;
	uuid = nm_utils_uuid_generate_from_string (uuid_base);
	setting =
	    (NMSettingConnection *) nm_connection_get_setting (connection,
							       NM_TYPE_SETTING_CONNECTION);
	g_object_set (setting, NM_SETTING_CONNECTION_ID, idstr,
		      NM_SETTING_CONNECTION_UUID, uuid, NULL);
	PLUGIN_PRINT (IFNET_PLUGIN_NAME,
		      "update_connection_setting_from_config_block: name:%s, id:%s, uuid: %s",
		      conn_name, idstr, uuid);

	g_free (uuid);
	g_free (idstr);
}

static gboolean eap_simple_reader (const char *eap_method,
                                   const char *ssid,
                                   NMSetting8021x *s_8021x,
                                   gboolean phase2,
                                   GError **error);

static gboolean eap_tls_reader (const char *eap_method,
                                const char *ssid,
                                NMSetting8021x *s_8021x,
                                gboolean phase2,
                                GError **error);

static gboolean eap_peap_reader (const char *eap_method,
                                 const char *ssid,
                                 NMSetting8021x *s_8021x,
                                 gboolean phase2,
                                 GError **error);

static gboolean eap_ttls_reader (const char *eap_method,
                                 const char *ssid,
                                 NMSetting8021x *s_8021x,
                                 gboolean phase2,
                                 GError **error);

typedef struct {
	const char *method;
	 gboolean (*reader) (const char *eap_method,
	                     const char *ssid,
	                     NMSetting8021x *s_8021x,
	                     gboolean phase2,
	                     GError **error);
	gboolean wifi_phase2_only;
} EAPReader;

static EAPReader eap_readers[] = {
	{"md5", eap_simple_reader, TRUE},
	{"pap", eap_simple_reader, TRUE},
	{"chap", eap_simple_reader, TRUE},
	{"mschap", eap_simple_reader, TRUE},
	{"mschapv2", eap_simple_reader, TRUE},
	{"leap", eap_simple_reader, TRUE},
	{"tls", eap_tls_reader, FALSE},
	{"peap", eap_peap_reader, FALSE},
	{"ttls", eap_ttls_reader, FALSE},
	{NULL, NULL}
};

/* reading identity and password */
static gboolean
eap_simple_reader (const char *eap_method,
                   const char *ssid,
                   NMSetting8021x *s_8021x,
                   gboolean phase2,
                   GError **error)
{
	const char *value;

	/* identity */
	value = wpa_get_value (ssid, "identity");
	if (!value) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing IEEE_8021X_IDENTITY for EAP method '%s'.",
			     eap_method);
		return FALSE;
	}
	g_object_set (s_8021x, NM_SETTING_802_1X_IDENTITY, value, NULL);

	/* password */
	value = wpa_get_value (ssid, "password");
	if (!value) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing IEEE_8021X_PASSWORD for EAP method '%s'.",
			     eap_method);
		return FALSE;
	}

	g_object_set (s_8021x, NM_SETTING_802_1X_PASSWORD, value, NULL);

	return TRUE;
}

static gboolean
eap_tls_reader (const char *eap_method,
                const char *ssid,
                NMSetting8021x *s_8021x,
                gboolean phase2,
                GError **error)
{
	const char *value;
	const char *ca_cert = NULL;
	const char *client_cert = NULL;
	const char *privkey = NULL;
	const char *privkey_password = NULL;
	gboolean success = FALSE;
	NMSetting8021xCKFormat privkey_format = NM_SETTING_802_1X_CK_FORMAT_UNKNOWN;

	/* identity */
	value = wpa_get_value (ssid, "identity");
	if (!value) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing IEEE_8021X_IDENTITY for EAP method '%s'.",
			     eap_method);
		return FALSE;
	}
	g_object_set (s_8021x, NM_SETTING_802_1X_IDENTITY, value, NULL);

	/* ca cert */
	ca_cert = wpa_get_value (ssid, phase2 ? "ca_cert2" : "ca_cert");
	if (ca_cert) {
		if (phase2) {
			if (!nm_setting_802_1x_set_phase2_ca_cert (s_8021x,
								   ca_cert,
								   NM_SETTING_802_1X_CK_SCHEME_PATH,
								   NULL, error))
				goto done;
		} else {
			if (!nm_setting_802_1x_set_ca_cert (s_8021x,
							    ca_cert,
							    NM_SETTING_802_1X_CK_SCHEME_PATH,
							    NULL, error))
				goto done;
		}
	} else {
		PLUGIN_WARN (IFNET_PLUGIN_NAME,
			     "    warning: missing %s for EAP"
			     " method '%s'; this is insecure!",
			     phase2 ? "IEEE_8021X_INNER_CA_CERT" :
			     "IEEE_8021X_CA_CERT", eap_method);
	}

	/* Private key password */
	privkey_password = wpa_get_value (ssid,
					  phase2 ? "private_key_passwd2" :
					  "private_key_passwd");

	if (!privkey_password) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing %s for EAP method '%s'.",
			     phase2 ? "IEEE_8021X_INNER_PRIVATE_KEY_PASSWORD" :
			     "IEEE_8021X_PRIVATE_KEY_PASSWORD", eap_method);
		goto done;
	}

	/* The private key itself */
	privkey = wpa_get_value (ssid, phase2 ? "private_key2" : "private_key");
	if (!privkey) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing %s for EAP method '%s'.",
			     phase2 ? "IEEE_8021X_INNER_PRIVATE_KEY" :
			     "IEEE_8021X_PRIVATE_KEY", eap_method);
		goto done;
	}

	if (phase2) {
		if (!nm_setting_802_1x_set_phase2_private_key (s_8021x,
							       privkey,
							       privkey_password,
							       NM_SETTING_802_1X_CK_SCHEME_PATH,
							       &privkey_format,
							       error))
			goto done;
	} else {
		if (!nm_setting_802_1x_set_private_key (s_8021x,
							privkey,
							privkey_password,
							NM_SETTING_802_1X_CK_SCHEME_PATH,
							&privkey_format, error))
			goto done;
	}

	/* Only set the client certificate if the private key is not PKCS#12 format,
	 * as NM (due to supplicant restrictions) requires.  If the key was PKCS#12,
	 * then nm_setting_802_1x_set_private_key() already set the client certificate
	 * to the same value as the private key.
	 */
	if (privkey_format == NM_SETTING_802_1X_CK_FORMAT_RAW_KEY
	    || privkey_format == NM_SETTING_802_1X_CK_FORMAT_X509) {
		client_cert = wpa_get_value (ssid,
					     phase2 ? "client_cert2" :
					     "client_cert");
		if (!client_cert) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Missing %s for EAP method '%s'.",
				     phase2 ? "IEEE_8021X_INNER_CLIENT_CERT" :
				     "IEEE_8021X_CLIENT_CERT", eap_method);
			goto done;
		}

		if (phase2) {
			if (!nm_setting_802_1x_set_phase2_client_cert (s_8021x,
								       client_cert,
								       NM_SETTING_802_1X_CK_SCHEME_PATH,
								       NULL,
								       error))
				goto done;
		} else {
			if (!nm_setting_802_1x_set_client_cert (s_8021x,
								client_cert,
								NM_SETTING_802_1X_CK_SCHEME_PATH,
								NULL, error))
				goto done;
		}
	}

	success = TRUE;

done:
	return success;
}

#define SCHEME_HASH "hash://server/sha256/"

static gboolean
eap_peap_reader (const char *eap_method,
                 const char *ssid,
                 NMSetting8021x *s_8021x,
                 gboolean phase2,
                 GError **error)
{
	const char *ca_cert = NULL;
	const char *inner_auth = NULL;
	const char *peapver = NULL;
	char **list = NULL, **iter, *lower;
	gboolean success = FALSE;

	ca_cert = wpa_get_value (ssid, "ca_cert");
	if (ca_cert) {
		if (g_str_has_prefix (ca_cert, SCHEME_HASH))
			if (!nm_setting_802_1x_set_ca_cert (s_8021x,
							    ca_cert,
							    NM_SETTING_802_1X_CK_SCHEME_HASH,
							    NULL, error))
				goto done;
		else
			if (!nm_setting_802_1x_set_ca_cert (s_8021x,
							    ca_cert,
							    NM_SETTING_802_1X_CK_SCHEME_PATH,
							    NULL, error))
				goto done;
	} else {
		PLUGIN_WARN (IFNET_PLUGIN_NAME, "    warning: missing "
			     "IEEE_8021X_CA_CERT for EAP method '%s'; this is"
			     " insecure!", eap_method);
	}

	peapver = wpa_get_value (ssid, "phase1");
	/* peap version, default is automatic */
	if (peapver && strstr (peapver, "peapver")) {
		if (strstr (peapver, "peapver=0"))
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE1_PEAPVER,
				      "0", NULL);
		else if (strstr (peapver, "peapver=1"))
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE1_PEAPVER,
				      "1", NULL);
		else {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Unknown IEEE_8021X_PEAP_VERSION value '%s'",
				     peapver);
			goto done;
		}
	}

	/* peaplabel */
	if (peapver && strstr (peapver, "peaplabel=1"))
		g_object_set (s_8021x, NM_SETTING_802_1X_PHASE1_PEAPLABEL, "1",
			      NULL);

	inner_auth = wpa_get_value (ssid, "phase2");
	if (!inner_auth) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing IEEE_8021X_INNER_AUTH_METHODS.");
		goto done;
	}
	/* Handle options for the inner auth method */
	list = g_strsplit (inner_auth, " ", 0);
	for (iter = list; iter && *iter; iter++) {
		gchar *pos = NULL;

		if (!strlen (*iter))
			continue;

		if (!(pos = strstr (*iter, "MSCHAPV2"))
		    || !(pos = strstr (*iter, "MD5"))
		    || !(pos = strstr (*iter, "GTC"))) {
			if (!eap_simple_reader
			    (pos, ssid, s_8021x, TRUE, error))
				goto done;
		} else if (!(pos = strstr (*iter, "TLS"))) {
			if (!eap_tls_reader (pos, ssid, s_8021x, TRUE, error))
				goto done;
		} else {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Unknown IEEE_8021X_INNER_AUTH_METHOD '%s'.",
				     *iter);
			goto done;
		}

		pos = strchr (*iter, '=');
		pos++;
		lower = g_ascii_strdown (pos, -1);
		g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTH, lower,
			      NULL);
		g_free (lower);
		break;
	}

	if (!nm_setting_802_1x_get_phase2_auth (s_8021x)) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "No valid IEEE_8021X_INNER_AUTH_METHODS found.");
		goto done;
	}

	success = TRUE;

done:
	if (list)
		g_strfreev (list);
	return success;
}

static gboolean
eap_ttls_reader (const char *eap_method,
                 const char *ssid,
                 NMSetting8021x *s_8021x,
                 gboolean phase2,
                 GError **error)
{
	gboolean success = FALSE;
	const char *anon_ident = NULL;
	const char *ca_cert = NULL;
	const char *tmp;
	char **list = NULL, **iter, *inner_auth = NULL;

	/* ca cert */
	ca_cert = wpa_get_value (ssid, "ca_cert");
	if (ca_cert) {
		if (g_str_has_prefix (ca_cert, SCHEME_HASH))
			if (!nm_setting_802_1x_set_ca_cert (s_8021x,
							    ca_cert,
							    NM_SETTING_802_1X_CK_SCHEME_HASH,
							    NULL, error))
				goto done;
		else
			if (!nm_setting_802_1x_set_ca_cert (s_8021x,
							    ca_cert,
							    NM_SETTING_802_1X_CK_SCHEME_PATH,
							    NULL, error))
				goto done;
	} else {
		PLUGIN_WARN (IFNET_PLUGIN_NAME, "    warning: missing "
			     "IEEE_8021X_CA_CERT for EAP method '%s'; this is"
			     " insecure!", eap_method);
	}

	/* anonymous indentity for tls */
	anon_ident = wpa_get_value (ssid, "anonymous_identity");
	if (anon_ident && strlen (anon_ident))
		g_object_set (s_8021x, NM_SETTING_802_1X_ANONYMOUS_IDENTITY,
			      anon_ident, NULL);

	tmp = wpa_get_value (ssid, "phase2");
	if (!tmp) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing IEEE_8021X_INNER_AUTH_METHODS.");
		goto done;
	}

	/* Handle options for the inner auth method */
	inner_auth = g_ascii_strdown (tmp, -1);
	list = g_strsplit (inner_auth, " ", 0);
	for (iter = list; iter && *iter; iter++) {
		gchar *pos = NULL;

		if (!strlen (*iter))
			continue;
		if ((pos = strstr (*iter, "mschapv2")) != NULL
		    || (pos = strstr (*iter, "mschap")) != NULL
		    || (pos = strstr (*iter, "pap")) != NULL
		    || (pos = strstr (*iter, "chap")) != NULL) {
			if (!eap_simple_reader
			    (pos, ssid, s_8021x, TRUE, error))
				goto done;
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTH,
				      pos, NULL);
		} else if ((pos = strstr (*iter, "tls")) != NULL) {
			if (!eap_tls_reader (pos, ssid, s_8021x, TRUE, error))
				goto done;
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTHEAP,
				      "tls", NULL);
		} else if ((pos = strstr (*iter, "mschapv2")) != NULL
			   || (pos = strstr (*iter, "md5")) != NULL) {
			if (!eap_simple_reader
			    (pos, ssid, s_8021x, TRUE, error)) {
				PLUGIN_WARN (IFNET_PLUGIN_NAME, "SIMPLE ERROR");
				goto done;
			}
			g_object_set (s_8021x, NM_SETTING_802_1X_PHASE2_AUTHEAP,
				      pos, NULL);
		} else {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Unknown IEEE_8021X_INNER_AUTH_METHOD '%s'.",
				     *iter);
			goto done;
		}
		break;
	}

	success = TRUE;
done:
	if (list)
		g_strfreev (list);
	g_free (inner_auth);
	return success;
}

/* type is already decided by net_parser, this function is just used to
 * doing tansformation*/
static const gchar *
guess_connection_type (const char *conn_name)
{
	const gchar *type = ifnet_get_data (conn_name, "type");
	const gchar *ret_type = NULL;

	if (!g_strcmp0 (type, "ppp"))
		ret_type = NM_SETTING_PPPOE_SETTING_NAME;

	if (!g_strcmp0 (type, "wireless"))
		ret_type = NM_SETTING_WIRELESS_SETTING_NAME;

	if (!ret_type)
		ret_type = NM_SETTING_WIRED_SETTING_NAME;

	PLUGIN_PRINT (IFNET_PLUGIN_NAME,
		      "guessed connection type (%s) = %s", conn_name, ret_type);
	return ret_type;
}

/* Reading mac address for setting connection option.
 * Unmanaged device mac address is required by NetworkManager*/
static gboolean
read_mac_address (const char *conn_name, GByteArray **array, GError **error)
{
	const char *value = ifnet_get_data (conn_name, "mac");
	struct ether_addr *mac;

	if (!value || !strlen (value))
		return TRUE;

	mac = ether_aton (value);
	if (!mac) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "The MAC address '%s' was invalid.", value);
		return FALSE;
	}

	*array = g_byte_array_sized_new (ETH_ALEN);
	g_byte_array_append (*array, (guint8 *) mac->ether_addr_octet, ETH_ALEN);
	return TRUE;
}

static void
make_wired_connection_setting (NMConnection *connection,
                               const char *conn_name,
                               GError **error)
{
	GByteArray *mac = NULL;
	NMSettingWired *s_wired = NULL;
	const char *value = NULL;

	s_wired = NM_SETTING_WIRED (nm_setting_wired_new ());

	/* mtu_xxx */
	value = ifnet_get_data (conn_name, "mtu");
	if (value) {
		long int mtu;

		errno = 0;
		mtu = strtol (value, NULL, 10);
		if (errno || mtu < 0 || mtu > 65535) {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "    warning: invalid MTU '%s' for %s",
				     value, conn_name);
		} else
			g_object_set (s_wired, NM_SETTING_WIRED_MTU,
				      (guint32) mtu, NULL);
	}

	if (read_mac_address (conn_name, &mac, error)) {
		if (mac) {
			g_object_set (s_wired, NM_SETTING_WIRED_MAC_ADDRESS,
				      mac, NULL);
			g_byte_array_free (mac, TRUE);
		}
	} else {
		g_object_unref (s_wired);
		s_wired = NULL;
	}
	if (s_wired)
		nm_connection_add_setting (connection, NM_SETTING (s_wired));
}

/* add NM_SETTING_IP4_CONFIG_DHCP_HOSTNAME, 
 * NM_SETTING_IP4_CONFIG_DHCP_CLIENT_ID in future*/
static void
make_ip4_setting (NMConnection *connection,
                  const char *conn_name,
                  GError **error)
{

	NMSettingIP4Config *ip4_setting =
	    NM_SETTING_IP4_CONFIG (nm_setting_ip4_config_new ());
	const char *value, *method;
	gboolean is_static_block = is_static_ip4 (conn_name);
	ip_block *iblock = NULL;

	/* set dhcp options (dhcp_xxx) */
	value = ifnet_get_data (conn_name, "dhcp");
	g_object_set (ip4_setting, NM_SETTING_IP4_CONFIG_IGNORE_AUTO_DNS, value
		      && strstr (value, "nodns") ? TRUE : FALSE,
		      NM_SETTING_IP4_CONFIG_IGNORE_AUTO_ROUTES, value
		      && strstr (value, "nogateway") ? TRUE : FALSE, NULL);

	if (!is_static_block) {
		method = ifnet_get_data (conn_name, "config");
		if (!method){
			g_set_error (error, ifnet_plugin_error_quark (), 0,
						 "Unknown config for %s", conn_name);
			g_object_unref (ip4_setting);
			return;
		}
		if (!strcmp (method, "dhcp"))
			g_object_set (ip4_setting,
						  NM_SETTING_IP4_CONFIG_METHOD,
						  NM_SETTING_IP4_CONFIG_METHOD_AUTO,
						  NM_SETTING_IP4_CONFIG_NEVER_DEFAULT, FALSE, NULL);
		else if (!strcmp (method, "autoip")){
			g_object_set (ip4_setting,
						  NM_SETTING_IP4_CONFIG_METHOD,
						  NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL,
						  NM_SETTING_IP4_CONFIG_NEVER_DEFAULT, FALSE, NULL);
			nm_connection_add_setting (connection, NM_SETTING (ip4_setting));
			return;
		} else if (!strcmp (method, "shared")){
			g_object_set (ip4_setting,
						  NM_SETTING_IP4_CONFIG_METHOD,
						  NM_SETTING_IP4_CONFIG_METHOD_SHARED,
						  NM_SETTING_IP4_CONFIG_NEVER_DEFAULT, FALSE, NULL);
			nm_connection_add_setting (connection, NM_SETTING (ip4_setting));
			return;
		} else {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
						 "Unknown config for %s", conn_name);
			g_object_unref (ip4_setting);
			return;
		}
		PLUGIN_PRINT (IFNET_PLUGIN_NAME, "Using %s method for %s",
					  method, conn_name);
	}else {
		iblock = convert_ip4_config_block (conn_name);
		if (!iblock) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Ifnet plugin: can't aquire ip configuration for %s",
				     conn_name);
			g_object_unref (ip4_setting);
			return;
		}
		/************** add all ip settings to the connection**********/
		while (iblock) {
			ip_block *current_iblock;
			NMIP4Address *ip4_addr = nm_ip4_address_new ();

			nm_ip4_address_set_address (ip4_addr, iblock->ip);
			nm_ip4_address_set_prefix (ip4_addr,
						   nm_utils_ip4_netmask_to_prefix
						   (iblock->netmask));
			/* currently all the IPs has the same gateway */
			nm_ip4_address_set_gateway (ip4_addr, iblock->gateway);
			if (iblock->gateway)
				g_object_set (ip4_setting,
					      NM_SETTING_IP4_CONFIG_IGNORE_AUTO_ROUTES,
					      TRUE, NULL);
			if (!nm_setting_ip4_config_add_address (ip4_setting, ip4_addr))
				PLUGIN_WARN (IFNET_PLUGIN_NAME,
					     "ignoring duplicate IP4 address");
			nm_ip4_address_unref (ip4_addr);
			current_iblock = iblock;
			iblock = iblock->next;
			destroy_ip_block (current_iblock);

		}
		g_object_set (ip4_setting,
		              NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_MANUAL,
		              NM_SETTING_IP4_CONFIG_NEVER_DEFAULT, !has_default_ip4_route (conn_name),
		              NULL);
	}

	/* add dhcp hostname and client id */
	if (!is_static_block && !strcmp (method, "dhcp")) {
		gchar *dhcp_hostname, *client_id;

		get_dhcp_hostname_and_client_id (&dhcp_hostname, &client_id);
		if (dhcp_hostname) {
			g_object_set (ip4_setting,
				      NM_SETTING_IP4_CONFIG_DHCP_HOSTNAME,
				      dhcp_hostname, NULL);
			PLUGIN_PRINT (IFNET_PLUGIN_NAME, "DHCP hostname: %s",
				      dhcp_hostname);
			g_free (dhcp_hostname);
		}
		if (client_id) {
			g_object_set (ip4_setting,
				      NM_SETTING_IP4_CONFIG_DHCP_CLIENT_ID,
				      client_id, NULL);
			PLUGIN_PRINT (IFNET_PLUGIN_NAME, "DHCP client id: %s",
				      client_id);
			g_free (client_id);
		}
	}

	/* add all IPv4 dns servers, IPv6 servers will be ignored */
	set_ip4_dns_servers (ip4_setting, conn_name);

	/* DNS searches */
	value = ifnet_get_data (conn_name, "dns_search");
	if (value) {
		char *stripped = g_strdup (value);
		char **searches = NULL;

		strip_string (stripped, '"');

		searches = g_strsplit (stripped, " ", 0);
		if (searches) {
			char **item;

			for (item = searches; *item; item++) {
				if (strlen (*item)) {
					if (!nm_setting_ip4_config_add_dns_search (ip4_setting, *item))
						PLUGIN_WARN
						    (IFNET_PLUGIN_NAME,
						     "    warning: duplicate DNS domain '%s'",
						     *item);
				}
			}
			g_strfreev (searches);
		}
	}

	/* static routes */
	iblock = convert_ip4_routes_block (conn_name);
	while (iblock) {
		ip_block *current_iblock = iblock;
		const char *metric_str;
		char *stripped;
		long int metric;
		NMIP4Route *route = nm_ip4_route_new ();

		nm_ip4_route_set_dest (route, iblock->ip);
		nm_ip4_route_set_next_hop (route, iblock->gateway);
		nm_ip4_route_set_prefix (route,
					 nm_utils_ip4_netmask_to_prefix
					 (iblock->netmask));
		if ((metric_str = ifnet_get_data (conn_name, "metric")) != NULL) {
			metric = strtol (metric_str, NULL, 10);
			nm_ip4_route_set_metric (route, (guint32) metric);
		} else {
			metric_str = ifnet_get_global_data ("metric");
			if (metric_str) {
				stripped = g_strdup (metric_str);
				strip_string (stripped, '"');
				metric = strtol (metric_str, NULL, 10);
				nm_ip4_route_set_metric (route,
							 (guint32) metric);
				g_free (stripped);
			}
		}

		if (!nm_setting_ip4_config_add_route (ip4_setting, route))
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "warning: duplicate IP4 route");
		PLUGIN_PRINT (IFNET_PLUGIN_NAME,
			      "new IP4 route:%d\n", iblock->ip);

		nm_ip4_route_unref (route);

		current_iblock = iblock;
		iblock = iblock->next;
		destroy_ip_block (current_iblock);
	}

	/* Finally add setting to connection */
	nm_connection_add_setting (connection, NM_SETTING (ip4_setting));
}

static void
make_ip6_setting (NMConnection *connection,
                  const char *conn_name,
                  GError **error)
{
	NMSettingIP6Config *s_ip6 = NULL;
	gboolean is_static_block = is_static_ip6 (conn_name);

	// used to disable IPv6
	gboolean ipv6_enabled = FALSE;
	gchar *method = NM_SETTING_IP6_CONFIG_METHOD_MANUAL;
	const char *value;
	ip6_block *iblock;
	gboolean never_default = !has_default_ip6_route (conn_name);

	s_ip6 = (NMSettingIP6Config *) nm_setting_ip6_config_new ();
	if (!s_ip6) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Could not allocate IP6 setting");
		return;
	}

	value = ifnet_get_data (conn_name, "enable_ipv6");
	if (value && is_true (value))
		ipv6_enabled = TRUE;

	//FIXME Handle other methods that NM supports in future
	// Currently only Manual and DHCP are supported
	if (!ipv6_enabled) {
		g_object_set (s_ip6,
			      NM_SETTING_IP6_CONFIG_METHOD,
			      NM_SETTING_IP6_CONFIG_METHOD_IGNORE, NULL);
		goto done;
	} else if (!is_static_block) {
		// config_eth* contains "dhcp6"
		method = NM_SETTING_IP6_CONFIG_METHOD_AUTO;
		never_default = FALSE;
	}
	// else if (!has_ip6_address(conn_name))
	// doesn't have "dhcp6" && doesn't have any ipv6 address
	// method = NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL;
	else
		// doesn't have "dhcp6" && has at least one ipv6 address
		method = NM_SETTING_IP6_CONFIG_METHOD_MANUAL;
	PLUGIN_PRINT (IFNET_PLUGIN_NAME, "IPv6 for %s enabled, using %s",
		      conn_name, method);

	g_object_set (s_ip6,
		      NM_SETTING_IP6_CONFIG_METHOD, method,
		      NM_SETTING_IP6_CONFIG_IGNORE_AUTO_DNS, FALSE,
		      NM_SETTING_IP6_CONFIG_IGNORE_AUTO_ROUTES, FALSE,
		      NM_SETTING_IP6_CONFIG_NEVER_DEFAULT, never_default, NULL);

	/* Make manual settings */
	if (!strcmp (method, NM_SETTING_IP6_CONFIG_METHOD_MANUAL)) {
		ip6_block *current_iblock;

		iblock = convert_ip6_config_block (conn_name);
		if (!iblock) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Ifnet plugin: can't aquire ip6 configuration for %s",
				     conn_name);
			goto error;
		}
		/* add all IPv6 addresses */
		while (iblock) {
			NMIP6Address *ip6_addr = nm_ip6_address_new ();

			nm_ip6_address_set_address (ip6_addr, iblock->ip);
			nm_ip6_address_set_prefix (ip6_addr, iblock->prefix);
			if (nm_setting_ip6_config_add_address (s_ip6, ip6_addr)) {
				PLUGIN_PRINT (IFNET_PLUGIN_NAME,
					      "ipv6 addresses count: %d",
					      nm_setting_ip6_config_get_num_addresses
					      (s_ip6));
			} else {
				PLUGIN_WARN (IFNET_PLUGIN_NAME,
					     "ignoring duplicate IP4 address");
			}
			nm_ip6_address_unref (ip6_addr);
			current_iblock = iblock;
			iblock = iblock->next;
			destroy_ip6_block (current_iblock);
		}

	} else if (!strcmp (method, NM_SETTING_IP6_CONFIG_METHOD_AUTO)) {
		/* - autoconf or DHCPv6 stuff goes here */
	}
	// DNS Servers, set NM_SETTING_IP6_CONFIG_IGNORE_AUTO_DNS TRUE here
	set_ip6_dns_servers (s_ip6, conn_name);

	/* DNS searches ('DOMAIN' key) are read by make_ip4_setting() and included in NMSettingIP4Config */

	// Add routes
	iblock = convert_ip6_routes_block (conn_name);
	if (iblock)
		g_object_set (s_ip6, NM_SETTING_IP6_CONFIG_IGNORE_AUTO_ROUTES,
			      TRUE, NULL);
	/* Add all IPv6 routes */
	while (iblock) {
		ip6_block *current_iblock = iblock;
		const char *metric_str;
		char *stripped;
		long int metric = 1;
		NMIP6Route *route = nm_ip6_route_new ();

		nm_ip6_route_set_dest (route, iblock->ip);
		nm_ip6_route_set_next_hop (route, iblock->next_hop);
		nm_ip6_route_set_prefix (route, iblock->prefix);
		/* metric is not per routes configuration right now 
		 * global metric is also supported (metric="x") */
		if ((metric_str = ifnet_get_data (conn_name, "metric")) != NULL) {
			metric = strtol (metric_str, NULL, 10);
			nm_ip6_route_set_metric (route, (guint32) metric);
		} else {
			metric_str = ifnet_get_global_data ("metric");
			if (metric_str) {
				stripped = g_strdup (metric_str);
				strip_string (stripped, '"');
				metric = strtol (metric_str, NULL, 10);
				nm_ip6_route_set_metric (route,
							 (guint32) metric);
				g_free (stripped);
			} else
				nm_ip6_route_set_metric (route, (guint32) 1);
		}

		if (!nm_setting_ip6_config_add_route (s_ip6, route))
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "    warning: duplicate IP6 route");
		PLUGIN_PRINT (IFNET_PLUGIN_NAME, "    info: new IP6 route");
		nm_ip6_route_unref (route);

		current_iblock = iblock;
		iblock = iblock->next;
		destroy_ip6_block (current_iblock);
	}

done:
	nm_connection_add_setting (connection, NM_SETTING (s_ip6));
	return;

error:
	g_object_unref (s_ip6);
	PLUGIN_WARN (IFNET_PLUGIN_NAME, "    warning: Ignore IPv6 for %s",
		     conn_name);
	return;
}

static NMSetting *
make_wireless_connection_setting (const char *conn_name,
                                  NMSetting8021x **s_8021x,
                                  GError **error)
{
	GByteArray *array, *mac = NULL;
	NMSettingWireless *wireless_setting = NULL;
	gboolean adhoc = FALSE;
	const char *value;
	const char *type;

	/* PPP over WIFI is not supported yet */
	g_return_val_if_fail (conn_name != NULL
			      && strcmp (ifnet_get_data (conn_name, "type"),
					 "ppp") != 0, NULL);
	type = ifnet_get_data (conn_name, "type");
	if (strcmp (type, "ppp") == 0) {
		PLUGIN_WARN (IFNET_PLUGIN_NAME,
			     "PPP over WIFI is not supported yet");
		return NULL;
	}

	wireless_setting = NM_SETTING_WIRELESS (nm_setting_wireless_new ());
	if (read_mac_address (conn_name, &mac, error)) {
		if (mac) {
			g_object_set (wireless_setting,
				      NM_SETTING_WIRELESS_MAC_ADDRESS, mac,
				      NULL);
			g_byte_array_free (mac, TRUE);

		}
	} else {
		g_object_unref (wireless_setting);
		return NULL;
	}

	/* handle ssid (hex and ascii) */
	if (conn_name) {
		gsize ssid_len = 0, value_len = strlen (conn_name);
		const char *p;
		char *tmp, *converted = NULL;

		ssid_len = value_len;
		if ((value_len > 2) && (g_str_has_prefix (conn_name, "0x"))) {
			/* Hex representation */
			if (value_len % 2) {
				g_set_error (error, ifnet_plugin_error_quark (),
					     0,
					     "Invalid SSID '%s' size (looks like hex but length not multiple of 2)",
					     conn_name);
				goto error;
			}
			// ignore "0x"
			p = conn_name + 2;
			if (!is_hex (p)) {
				g_set_error (error,
					     ifnet_plugin_error_quark (),
					     0,
					     "Invalid SSID '%s' character (looks like hex SSID but '%c' isn't a hex digit)",
					     conn_name, *p);
				goto error;

			}
			tmp = utils_hexstr2bin (p, value_len - 2);
			ssid_len = (value_len - 2) / 2;
			converted = g_malloc0 (ssid_len + 1);
			memcpy (converted, tmp, ssid_len);
			g_free (tmp);
		}

		if (ssid_len > 32 || ssid_len == 0) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Invalid SSID '%s' (size %zu not between 1 and 32 inclusive)",
				     conn_name, ssid_len);
			goto error;
		}
		array = g_byte_array_sized_new (ssid_len);
		g_byte_array_append (array, (const guint8 *) (converted ? converted : conn_name), ssid_len);
		g_object_set (wireless_setting, NM_SETTING_WIRELESS_SSID, array, NULL);
		g_byte_array_free (array, TRUE);
		g_free (converted);
	} else {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing SSID");
		goto error;
	}

	/* mode=0: infrastructure 
	 * mode=1: adhoc */
	value = wpa_get_value (conn_name, "mode");
	if (value)
		adhoc = strcmp (value, "1") == 0 ? TRUE : FALSE;

	if (exist_ssid (conn_name)) {
		const char *mode = adhoc ? "adhoc" : "infrastructure";

		g_object_set (wireless_setting, NM_SETTING_WIRELESS_MODE, mode,
			      NULL);
		PLUGIN_PRINT (IFNET_PLUGIN_NAME, "Using mode: %s", mode);
	}

	/* BSSID setting */
	value = wpa_get_value (conn_name, "bssid");
	if (value) {
		struct ether_addr *eth;
		GByteArray *bssid;

		eth = ether_aton (value);
		if (!eth) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Invalid BSSID '%s'", value);
			goto error;
		}

		bssid = g_byte_array_sized_new (ETH_ALEN);
		g_byte_array_append (bssid, eth->ether_addr_octet, ETH_ALEN);
		g_object_set (wireless_setting, NM_SETTING_WIRELESS_BSSID,
			      bssid, NULL);
		g_byte_array_free (bssid, TRUE);

	}

	/* mtu_ssid="xx" */
	value = ifnet_get_data (conn_name, "mtu");
	if (value) {
		long int mtu;

		errno = 0;
		mtu = strtol (value, NULL, 10);
		if (errno || mtu < 0 || mtu > 50000) {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "    warning: invalid MTU '%s' for %s",
				     value, conn_name);
		} else
			g_object_set (wireless_setting, NM_SETTING_WIRELESS_MTU,
				      (guint32) mtu, NULL);

	}

	PLUGIN_PRINT (IFNET_PLUGIN_NAME, "wireless_setting added for %s",
		      conn_name);
	return NM_SETTING (wireless_setting);
error:
	if (wireless_setting)
		g_object_unref (wireless_setting);
	return NULL;

}

static NMSettingWirelessSecurity *
make_leap_setting (const char *ssid, GError **error)
{
	NMSettingWirelessSecurity *wsec;
	const char *value;

	wsec =
	    NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());

	value = wpa_get_value (ssid, "key_mgmt");
	if (!value || strcmp (value, "IEEE8021X"))
		goto error;	/* Not LEAP */

	value = wpa_get_value (ssid, "eap");
	if (!value || strcasecmp (value, "LEAP"))
		goto error;	/* Not LEAP */

	value = wpa_get_value (ssid, "password");
	if (value && strlen (value))
		g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_LEAP_PASSWORD,
			      value, NULL);

	value = wpa_get_value (ssid, "identity");
	if (!value || !strlen (value)) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing LEAP identity");
		goto error;
	}
	g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_LEAP_USERNAME, value,
		      NULL);

	g_object_set (wsec,
		      NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "ieee8021x",
		      NM_SETTING_WIRELESS_SECURITY_AUTH_ALG, "leap", NULL);

	return wsec;
error:
	if (wsec)
		g_object_unref (wsec);
	return NULL;
}

static gboolean
add_one_wep_key (const char *ssid,
                 const char *key,
                 int key_idx,
                 NMSettingWirelessSecurity *s_wsec,
                 GError **error)
{
	const char *value;
	char *converted = NULL;
	gboolean success = FALSE;

	g_return_val_if_fail (ssid != NULL, FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (key_idx >= 0 && key_idx <= 3, FALSE);
	g_return_val_if_fail (s_wsec != NULL, FALSE);

	value = wpa_get_value (ssid, key);
	if (!value)
		return TRUE;

	/* Validate keys */
	if (strlen (value) == 10 || strlen (value) == 26) {
		/* Hexadecimal WEP key */
		if (!is_hex (value)) {
			g_set_error (error, ifnet_plugin_error_quark (),
				     0, "Invalid hexadecimal WEP key.");
			goto out;
		}
		converted = g_strdup (value);
	} else if (value[0] == '"'
		   && (strlen (value) == 7 || strlen (value) == 15)) {
		/* ASCII passphrase */
		char *tmp = g_strdup (value);
		char *p = strip_string (tmp, '"');

		if (!is_ascii (p)) {
			g_set_error (error, ifnet_plugin_error_quark (),
				     0, "Invalid ASCII WEP passphrase.");
			g_free (tmp);
			goto out;

		}

		converted = utils_bin2hexstr (tmp, strlen (tmp), strlen (tmp) * 2);
		g_free (tmp);
	} else {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Invalid WEP key length. Key: %s", value);
		goto out;
	}

	if (converted) {
		nm_setting_wireless_security_set_wep_key (s_wsec, key_idx, converted);
		g_free (converted);
		success = TRUE;
	}

out:
	return success;
}

static gboolean
add_wep_keys (const char *ssid,
              NMSettingWirelessSecurity *s_wsec,
              GError **error)
{
	if (!add_one_wep_key (ssid, "wep_key0", 0, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ssid, "wep_key1", 1, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ssid, "wep_key2", 2, s_wsec, error))
		return FALSE;
	if (!add_one_wep_key (ssid, "wep_key3", 3, s_wsec, error))
		return FALSE;
	return TRUE;

}

static NMSettingWirelessSecurity *
make_wep_setting (const char *ssid, GError **error)
{
	const char *auth_alg, *value;
	int default_key_idx = 0;
	NMSettingWirelessSecurity *s_wireless_sec;

	s_wireless_sec =
	    NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());
	g_object_set (s_wireless_sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
		      "none", NULL);

	/* default key index */
	value = wpa_get_value (ssid, "wep_tx_keyidx");
	if (value) {
		default_key_idx = atoi (value);
		if (default_key_idx >= 0 && default_key_idx <= 3) {
			g_object_set (s_wireless_sec,
				      NM_SETTING_WIRELESS_SECURITY_WEP_TX_KEYIDX,
				      default_key_idx, NULL);
			PLUGIN_PRINT (IFNET_PLUGIN_NAME,
				      "Default key index: %d", default_key_idx);
		} else {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Invalid default WEP key '%s'", value);
			goto error;
		}
	}

	if (!add_wep_keys (ssid, s_wireless_sec, error))
		goto error;

	/* If there's a default key, ensure that key exists */
	if ((default_key_idx == 1)
	    && !nm_setting_wireless_security_get_wep_key (s_wireless_sec, 1)) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Default WEP key index was 2, but no valid KEY2 exists.");
		goto error;
	} else if ((default_key_idx == 2)
		   && !nm_setting_wireless_security_get_wep_key (s_wireless_sec,
								 2)) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Default WEP key index was 3, but no valid KEY3 exists.");
		goto error;
	} else if ((default_key_idx == 3)
		   && !nm_setting_wireless_security_get_wep_key (s_wireless_sec,
								 3)) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Default WEP key index was 4, but no valid KEY4 exists.");
		goto error;
	}

	/* authentication algorithms */
	auth_alg = wpa_get_value (ssid, "auth_alg");
	if (auth_alg) {
		if (strcmp (auth_alg, "OPEN") == 0) {
			g_object_set (s_wireless_sec,
				      NM_SETTING_WIRELESS_SECURITY_AUTH_ALG,
				      "open", NULL);
			PLUGIN_PRINT (IFNET_PLUGIN_NAME,
				      "WEP: Use open system authentication");
		} else if (strcmp (auth_alg, "SHARED") == 0) {
			g_object_set (s_wireless_sec,
				      NM_SETTING_WIRELESS_SECURITY_AUTH_ALG,
				      "shared", NULL);
			PLUGIN_PRINT (IFNET_PLUGIN_NAME,
				      "WEP: Use shared system authentication");
		} else {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Invalid WEP authentication algorithm '%s'",
				     auth_alg);
			goto error;
		}

	}

	if (!nm_setting_wireless_security_get_wep_key (s_wireless_sec, 0)
	    && !nm_setting_wireless_security_get_wep_key (s_wireless_sec, 1)
	    && !nm_setting_wireless_security_get_wep_key (s_wireless_sec, 2)
	    && !nm_setting_wireless_security_get_wep_key (s_wireless_sec, 3)
	    && !nm_setting_wireless_security_get_wep_tx_keyidx (s_wireless_sec)) {
		if (auth_alg && !strcmp (auth_alg, "shared")) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "WEP Shared Key authentication is invalid for "
				     "unencrypted connections.");
			goto error;
		}
		/* Unencrypted */
		g_object_unref (s_wireless_sec);
		s_wireless_sec = NULL;
	}
	return s_wireless_sec;

error:
	if (s_wireless_sec)
		g_object_unref (s_wireless_sec);
	return NULL;
}

static char *
parse_wpa_psk (const char *psk, GError **error)
{
	char *hashed = NULL;
	gboolean quoted = FALSE;

	if (!psk) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing WPA_PSK for WPA-PSK key management");
		return NULL;
	}

	/* Passphrase must be between 10 and 66 characters in length becuase WPA
	 * hex keys are exactly 64 characters (no quoting), and WPA passphrases
	 * are between 8 and 63 characters (inclusive), plus optional quoting if
	 * the passphrase contains spaces.
	 */

	if (psk[0] == '"' && psk[strlen (psk) - 1] == '"')
		quoted = TRUE;
	if (!quoted && (strlen (psk) == 64)) {
		/* Verify the hex PSK; 64 digits */
		if (!is_hex (psk)) {
			g_set_error (error, ifnet_plugin_error_quark (),
				     0,
				     "Invalid WPA_PSK (contains non-hexadecimal characters)");
			goto out;
		}
		hashed = g_strdup (psk);
	} else {
		char *stripped = g_strdup (psk);

		strip_string (stripped, '"');

		/* Length check */
		if (strlen (stripped) < 8 || strlen (stripped) > 63) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Invalid WPA_PSK (passphrases must be between "
				     "8 and 63 characters long (inclusive))");
			g_free (stripped);
			goto out;
		}

		hashed = g_strdup (stripped);
		g_free (stripped);
	}

	if (!hashed) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Invalid WPA_PSK (doesn't look like a passphrase or hex key)");
		goto out;
	}

out:
	return hashed;
}

static gboolean
fill_wpa_ciphers (const char *ssid,
                  NMSettingWirelessSecurity *wsec,
                  gboolean group,
                  gboolean adhoc)
{
	const char *value;
	char **list = NULL, **iter;
	int i = 0;

	value = wpa_get_value (ssid, group ? "group" : "pairwise");
	if (!value)
		return TRUE;

	list = g_strsplit_set (value, " ", 0);
	for (iter = list; iter && *iter; iter++, i++) {
		/* Ad-Hoc configurations cannot have pairwise ciphers, and can only
		 * have one group cipher.  Ignore any additional group ciphers and
		 * any pairwise ciphers specified.
		 */
		if (adhoc) {
			if (group && (i > 0)) {
				PLUGIN_WARN (IFNET_PLUGIN_NAME,
					     "    warning: ignoring group cipher '%s' (only one group cipher allowed in Ad-Hoc mode)",
					     *iter);
				continue;
			} else if (!group) {
				PLUGIN_WARN (IFNET_PLUGIN_NAME,
					     "    warning: ignoring pairwise cipher '%s' (pairwise not used in Ad-Hoc mode)",
					     *iter);
				continue;
			}
		}

		if (!strcmp (*iter, "CCMP")) {
			if (group)
				nm_setting_wireless_security_add_group (wsec,
									"ccmp");
			else
				nm_setting_wireless_security_add_pairwise (wsec,
									   "ccmp");
		} else if (!strcmp (*iter, "TKIP")) {
			if (group)
				nm_setting_wireless_security_add_group (wsec,
									"tkip");
			else
				nm_setting_wireless_security_add_pairwise (wsec,
									   "tkip");
		} else if (group && !strcmp (*iter, "WEP104"))
			nm_setting_wireless_security_add_group (wsec, "wep104");
		else if (group && !strcmp (*iter, "WEP40"))
			nm_setting_wireless_security_add_group (wsec, "wep40");
		else {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "    warning: ignoring invalid %s cipher '%s'",
				     group ? "CIPHER_GROUP" : "CIPHER_PAIRWISE",
				     *iter);
		}
	}

	if (list)
		g_strfreev (list);
	return TRUE;
}

static NMSetting8021x *
fill_8021x (const char *ssid,
            const char *key_mgmt,
            gboolean wifi,
            GError **error)
{
	NMSetting8021x *s_8021x;
	const char *value;
	char **list, **iter;

	value = wpa_get_value (ssid, "eap");
	if (!value) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing IEEE_8021X_EAP_METHODS for key management '%s'",
			     key_mgmt);
		return NULL;
	}

	list = g_strsplit (value, " ", 0);

	s_8021x = (NMSetting8021x *) nm_setting_802_1x_new ();
	/* Validate and handle each EAP method */
	for (iter = list; iter && *iter; iter++) {
		EAPReader *eap = &eap_readers[0];
		gboolean found = FALSE;
		char *lower = NULL;

		lower = g_ascii_strdown (*iter, -1);
		while (eap->method && !found) {
			if (strcmp (eap->method, lower))
				goto next;

			/* Some EAP methods don't provide keying material, thus they
			 * cannot be used with WiFi unless they are an inner method
			 * used with TTLS or PEAP or whatever.
			 */
			if (wifi && eap->wifi_phase2_only) {
				PLUGIN_WARN (IFNET_PLUGIN_NAME,
					     "    warning: ignored invalid "
					     "IEEE_8021X_EAP_METHOD '%s'; not allowed for wifi.",
					     lower);
				goto next;
			}

			/* Parse EAP method specific options */
			if (!(*eap->reader)
			    (lower, ssid, s_8021x, FALSE, error)) {
				g_free (lower);
				goto error;
			}
			nm_setting_802_1x_add_eap_method (s_8021x, lower);
			found = TRUE;

		next:
			eap++;
		}

		if (!found) {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "    warning: ignored unknown"
				     "IEEE_8021X_EAP_METHOD '%s'.", lower);
		}
		g_free (lower);
	}
	g_strfreev (list);

	if (nm_setting_802_1x_get_num_eap_methods (s_8021x) == 0) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "No valid EAP methods found in IEEE_8021X_EAP_METHODS.");
		goto error;
	}

	return s_8021x;

error:
	g_object_unref (s_8021x);
	return NULL;
}

static NMSettingWirelessSecurity *
make_wpa_setting (const char *ssid,
                  NMSetting8021x **s_8021x,
                  GError **error)
{
	NMSettingWirelessSecurity *wsec;
	const char *value;
	char *lower;
	gboolean adhoc = FALSE;

	if (!exist_ssid (ssid)) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "No security info found for ssid: %s", ssid);
		return NULL;
	}

	wsec =
	    NM_SETTING_WIRELESS_SECURITY (nm_setting_wireless_security_new ());

	/* mode=1: adhoc
	 * mode=0: infrastructure */
	value = wpa_get_value (ssid, "mode");
	if (value)
		adhoc = strcmp (value, "1") == 0 ? TRUE : FALSE;

	value = wpa_get_value (ssid, "key_mgmt");
	/* Not WPA or Dynamic WEP */
	if (!value)
		goto error;
	if (strcmp (value, "WPA-PSK") && strcmp (value, "WPA-EAP"))
		goto error;
	/* Pairwise and Group ciphers */
	fill_wpa_ciphers (ssid, wsec, FALSE, adhoc);
	fill_wpa_ciphers (ssid, wsec, TRUE, adhoc);

	/* WPA and/or RSN */
	if (adhoc) {
		/* Ad-Hoc mode only supports WPA proto for now */
		nm_setting_wireless_security_add_proto (wsec, "wpa");
	} else {
		nm_setting_wireless_security_add_proto (wsec, "wpa");
		nm_setting_wireless_security_add_proto (wsec, "rsn");

	}

	if (!strcmp (value, "WPA-PSK")) {
		char *psk = parse_wpa_psk (wpa_get_value (ssid, "psk"), error);

		if (!psk)
			goto error;
		g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_PSK, psk,
			      NULL);
		g_free (psk);

		if (adhoc)
			g_object_set (wsec,
				      NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
				      "wpa-none", NULL);
		else
			g_object_set (wsec,
				      NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
				      "wpa-psk", NULL);
	} else if (!strcmp (value, "WPA-EAP") || !strcmp (value, "IEEE8021X")) {
		if (adhoc) {
			g_set_error (error, ifnet_plugin_error_quark (), 0,
				     "Ad-Hoc mode cannot be used with KEY_MGMT type '%s'",
				     value);
			goto error;
		}
		*s_8021x = fill_8021x (ssid, value, TRUE, error);
		if (!*s_8021x)
			goto error;

		lower = g_ascii_strdown (value, -1);
		g_object_set (wsec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
			      lower, NULL);
		g_free (lower);
	} else {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Unknown wireless KEY_MGMT type '%s'", value);
		goto error;
	}
	return wsec;
error:
	if (wsec)
		g_object_unref (wsec);
	return NULL;
}

static NMSettingWirelessSecurity *
make_wireless_security_setting (const char *conn_name,
                                NMSetting8021x **s_8021x,
                                GError ** error)
{
	NMSettingWirelessSecurity *wsec = NULL;
	const char *ssid;
	gboolean adhoc = FALSE;
	const char *value;

	g_return_val_if_fail (conn_name != NULL
			      && strcmp (ifnet_get_data (conn_name, "type"),
					 "ppp") != 0, NULL);
	if (!wpa_get_value (conn_name, "ssid"))
		return NULL;
	PLUGIN_PRINT (IFNET_PLUGIN_NAME,
		      "updating wireless security settings (%s).", conn_name);

	ssid = conn_name;
	value = wpa_get_value (ssid, "mode");
	if (value)
		adhoc = strcmp (value, "1") == 0 ? TRUE : FALSE;

	if (!adhoc) {
		wsec = make_leap_setting (ssid, error);
		if (error && *error)
			goto error;
	}
	if (!wsec) {
		wsec = make_wpa_setting (ssid, s_8021x, error);
		if (error && *error)
			goto error;
	}
	if (!wsec) {
		wsec = make_wep_setting (ssid, error);
		if (error && *error)
			goto error;
	}

	if (!wsec) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Can't handle security information for ssid: %s",
			     conn_name);
	}

	return wsec;
error:
	return NULL;
}

/* Currently only support username and password */
static void
make_pppoe_connection_setting (NMConnection *connection,
                               const char *conn_name,
                               GError **error)
{
	NMSettingPPPOE *s_pppoe;
	NMSettingPPP *s_ppp;
	const char *value;

	s_pppoe = NM_SETTING_PPPOE (nm_setting_pppoe_new ());

	/* username */
	value = ifnet_get_data (conn_name, "username");
	if (!value) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "ppp requires at lease a username");
		return;
	}
	g_object_set (s_pppoe, NM_SETTING_PPPOE_USERNAME, value, NULL);

	/* password */
	value = ifnet_get_data (conn_name, "password");
	if (!value) {
		value = "";
	}

	g_object_set (s_pppoe, NM_SETTING_PPPOE_PASSWORD, value, NULL);
	nm_connection_add_setting (connection, NM_SETTING (s_pppoe));

	/* PPP setting */
	s_ppp = (NMSettingPPP *) nm_setting_ppp_new ();
	nm_connection_add_setting (connection, NM_SETTING (s_ppp));
}

NMConnection *
ifnet_update_connection_from_config_block (const char *conn_name, GError **error)
{
	const gchar *type = NULL;
	NMConnection *connection = NULL;
	NMSettingConnection *setting = NULL;
	NMSetting8021x *s_8021x = NULL;
	NMSettingWirelessSecurity *wsec = NULL;
	gboolean auto_conn = TRUE;
	const char *value = NULL;
	gboolean success = FALSE;

	connection = nm_connection_new ();
	if (!connection)
		return NULL;
	setting =
	    (NMSettingConnection *) nm_connection_get_setting (connection,
							       NM_TYPE_SETTING_CONNECTION);
	if (!setting) {
		setting = NM_SETTING_CONNECTION (nm_setting_connection_new ());
		g_assert (setting);
		nm_connection_add_setting (connection, NM_SETTING (setting));
	}

	type = guess_connection_type (conn_name);
	value = ifnet_get_data (conn_name, "auto");
	if (value && !strcmp (value, "false"))
		auto_conn = FALSE;
	update_connection_id (connection, conn_name);
	g_object_set (setting, NM_SETTING_CONNECTION_TYPE, type,
		      NM_SETTING_CONNECTION_READ_ONLY, FALSE,
		      NM_SETTING_CONNECTION_AUTOCONNECT, auto_conn, NULL);

	if (!strcmp (NM_SETTING_WIRED_SETTING_NAME, type)
	    || !strcmp (NM_SETTING_PPPOE_SETTING_NAME, type)) {
		/* wired setting */
		make_wired_connection_setting (connection, conn_name, error);
		if (error && *error) {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "Found error: %s", (*error)->message);
			goto error;
		}
		/* pppoe setting */
		if (!strcmp (NM_SETTING_PPPOE_SETTING_NAME, type))
			make_pppoe_connection_setting (connection, conn_name,
						       error);
		if (error && *error) {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "Found error: %s", (*error)->message);
			goto error;
		}
	} else if (!strcmp (NM_SETTING_WIRELESS_SETTING_NAME, type)) {
		/* wireless setting */
		NMSetting *wireless_setting;

		wireless_setting = make_wireless_connection_setting (conn_name, &s_8021x, error);
		if (!wireless_setting)
			goto error;
		nm_connection_add_setting (connection, wireless_setting);

		if (error && *error) {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "Found error: %s", (*error)->message);
			goto error;
		}

		/* wireless security setting */
		wsec = make_wireless_security_setting (conn_name, &s_8021x, error);
		if (wsec) {
			nm_connection_add_setting (connection,
						   NM_SETTING (wsec));
			if (s_8021x)
				nm_connection_add_setting (connection,
							   NM_SETTING
							   (s_8021x));
			g_object_set (wireless_setting, NM_SETTING_WIRELESS_SEC,
				      NM_SETTING_WIRELESS_SECURITY_SETTING_NAME,
				      NULL);
		}

		if (error && *error) {
			PLUGIN_WARN (IFNET_PLUGIN_NAME,
				     "Found error: %s", (*error)->message);
			goto error;
		}

	} else
		goto error;

	/* IPv4 setting */
	make_ip4_setting (connection, conn_name, error);
	if (error && *error) {
		PLUGIN_WARN (IFNET_PLUGIN_NAME, "Found error: %s", (*error)->message);
		goto error;
	}

	/* IPv6 setting */
	make_ip6_setting (connection, conn_name, error);
	if (error && *error) {
		PLUGIN_WARN (IFNET_PLUGIN_NAME, "Found error: %s", (*error)->message);
		goto error;
	}

	success = nm_connection_verify (connection, error);
	if (error && *error)
		PLUGIN_WARN (IFNET_PLUGIN_NAME, "Found error: %s", (*error)->message);
	PLUGIN_PRINT (IFNET_PLUGIN_NAME, "Connection verified %s:%d", conn_name, success);
	if (!success)
		goto error;
	return connection;

error:
	g_object_unref (connection);
	return NULL;
}

typedef NMSetting8021xCKScheme (*SchemeFunc) (NMSetting8021x * setting);
typedef const char *(*PathFunc) (NMSetting8021x * setting);
typedef const char *(*HashFunc) (NMSetting8021x * setting);
typedef const GByteArray *(*BlobFunc) (NMSetting8021x * setting);

typedef struct ObjectType {
	const char *setting_key;
	SchemeFunc scheme_func;
	PathFunc path_func;
	HashFunc hash_func;
	BlobFunc blob_func;
	const char *conn_name_key;
	const char *suffix;
} ObjectType;

static const ObjectType ca_type = {
	NM_SETTING_802_1X_CA_CERT,
	nm_setting_802_1x_get_ca_cert_scheme,
	nm_setting_802_1x_get_ca_cert_path,
	nm_setting_802_1x_get_ca_cert_hash,
	nm_setting_802_1x_get_ca_cert_blob,
	"ca_cert",
	"ca-cert.der"
};

static const ObjectType phase2_ca_type = {
	NM_SETTING_802_1X_PHASE2_CA_CERT,
	nm_setting_802_1x_get_phase2_ca_cert_scheme,
	nm_setting_802_1x_get_phase2_ca_cert_path,
	NULL,
	nm_setting_802_1x_get_phase2_ca_cert_blob,
	"ca_cert2",
	"inner-ca-cert.der"
};

static const ObjectType client_type = {
	NM_SETTING_802_1X_CLIENT_CERT,
	nm_setting_802_1x_get_client_cert_scheme,
	nm_setting_802_1x_get_client_cert_path,
	NULL,
	nm_setting_802_1x_get_client_cert_blob,
	"client_cert",
	"client-cert.der"
};

static const ObjectType phase2_client_type = {
	NM_SETTING_802_1X_PHASE2_CLIENT_CERT,
	nm_setting_802_1x_get_phase2_client_cert_scheme,
	nm_setting_802_1x_get_phase2_client_cert_path,
	NULL,
	nm_setting_802_1x_get_phase2_client_cert_blob,
	"client_cert2",
	"inner-client-cert.der"
};

static const ObjectType pk_type = {
	NM_SETTING_802_1X_PRIVATE_KEY,
	nm_setting_802_1x_get_private_key_scheme,
	nm_setting_802_1x_get_private_key_path,
	NULL,
	nm_setting_802_1x_get_private_key_blob,
	"private_key",
	"private-key.pem"
};

static const ObjectType phase2_pk_type = {
	NM_SETTING_802_1X_PHASE2_PRIVATE_KEY,
	nm_setting_802_1x_get_phase2_private_key_scheme,
	nm_setting_802_1x_get_phase2_private_key_path,
	NULL,
	nm_setting_802_1x_get_phase2_private_key_blob,
	"private_key2",
	"inner-private-key.pem"
};

static const ObjectType p12_type = {
	NM_SETTING_802_1X_PRIVATE_KEY,
	nm_setting_802_1x_get_private_key_scheme,
	nm_setting_802_1x_get_private_key_path,
	NULL,
	nm_setting_802_1x_get_private_key_blob,
	"private_key",
	"private-key.p12"
};

static const ObjectType phase2_p12_type = {
	NM_SETTING_802_1X_PHASE2_PRIVATE_KEY,
	nm_setting_802_1x_get_phase2_private_key_scheme,
	nm_setting_802_1x_get_phase2_private_key_path,
	NULL,
	nm_setting_802_1x_get_phase2_private_key_blob,
	"private_key2",
	"inner-private-key.p12"
};

static gboolean
write_object (NMSetting8021x *s_8021x,
              const char *conn_name,
              const GByteArray *override_data,
              const ObjectType *objtype,
              GError **error)
{
	NMSetting8021xCKScheme scheme;
	const char *path = NULL;
	const char *hash = NULL;
	const GByteArray *blob = NULL;

	g_return_val_if_fail (conn_name != NULL, FALSE);
	g_return_val_if_fail (objtype != NULL, FALSE);
	if (override_data)
		/* if given explicit data to save, always use that instead of asking
		 * the setting what to do.
		 */
		blob = override_data;
	else {
		scheme = (*(objtype->scheme_func)) (s_8021x);
		switch (scheme) {
		case NM_SETTING_802_1X_CK_SCHEME_BLOB:
			blob = (*(objtype->blob_func)) (s_8021x);
			break;
		case NM_SETTING_802_1X_CK_SCHEME_PATH:
			path = (*(objtype->path_func)) (s_8021x);
			break;
		case NM_SETTING_802_1X_CK_SCHEME_HASH:
			hash = (*(objtype->hash_func)) (s_8021x);
			break;
		default:
			break;
		}
	}

	/* If the object path was specified, prefer that over any raw cert data that
	 * may have been sent.
	 */
	if (path) {
		wpa_set_data (conn_name, (gchar *) objtype->conn_name_key,
			      (gchar *) path);
		return TRUE;
	}

	/* If the object hash was specified, prefer that over any raw cert data that
	 * may have been sent.
	 */
	if (hash) {
		wpa_set_data (conn_name, (gchar *) objtype->conn_name_key,
			      (gchar *) hash);
		return TRUE;
	}

	/* does not support writing encryption data now */
	if (blob) {
		PLUGIN_WARN (IFNET_PLUGIN_NAME,
			     "    warning: Currently we do not support certs writing.");
	}

	return TRUE;
}

static gboolean
write_8021x_certs (NMSetting8021x *s_8021x,
                   gboolean phase2,
                   const char *conn_name,
                   GError **error)
{
	char *password = NULL;
	const ObjectType *otype = NULL;
	gboolean is_pkcs12 = FALSE, success = FALSE;
	const GByteArray *blob = NULL;
	GByteArray *enc_key = NULL;
	gchar *generated_pw = NULL;

	/* CA certificate */
	if (phase2)
		otype = &phase2_ca_type;
	else
		otype = &ca_type;

	if (!write_object (s_8021x, conn_name, NULL, otype, error))
		return FALSE;

	/* Private key */
	if (phase2) {
		if (nm_setting_802_1x_get_phase2_private_key_scheme (s_8021x) !=
		    NM_SETTING_802_1X_CK_SCHEME_UNKNOWN) {
			if (nm_setting_802_1x_get_phase2_private_key_format
			    (s_8021x) == NM_SETTING_802_1X_CK_FORMAT_PKCS12)
				is_pkcs12 = TRUE;
		}
		password = (char *)
		    nm_setting_802_1x_get_phase2_private_key_password (s_8021x);
	} else {
		if (nm_setting_802_1x_get_private_key_scheme (s_8021x) !=
		    NM_SETTING_802_1X_CK_SCHEME_UNKNOWN) {
			if (nm_setting_802_1x_get_private_key_format (s_8021x)
			    == NM_SETTING_802_1X_CK_FORMAT_PKCS12)
				is_pkcs12 = TRUE;
		}
		password = (char *)
		    nm_setting_802_1x_get_private_key_password (s_8021x);
	}

	if (is_pkcs12)
		otype = phase2 ? &phase2_p12_type : &p12_type;
	else
		otype = phase2 ? &phase2_pk_type : &pk_type;

	if ((*(otype->scheme_func)) (s_8021x) ==
	    NM_SETTING_802_1X_CK_SCHEME_BLOB)
		blob = (*(otype->blob_func)) (s_8021x);

	/* Only do the private key re-encrypt dance if we got the raw key data, which
	 * by definition will be unencrypted.  If we're given a direct path to the
	 * private key file, it'll be encrypted, so we don't need to re-encrypt.
	 */
	if (blob && !is_pkcs12) {
		/* Encrypt the unencrypted private key with the fake password */
		enc_key =
		    nm_utils_rsa_key_encrypt (blob, password, &generated_pw,
					      error);
		if (!enc_key)
			goto out;

		if (generated_pw)
			password = generated_pw;
	}

	/* Save the private key */
	if (!write_object
	    (s_8021x, conn_name, enc_key ? enc_key : blob, otype, error))
		goto out;

	if (phase2)
		wpa_set_data (conn_name, "private_key_passwd2", password);
	else
		wpa_set_data (conn_name, "private_key_passwd", password);

	/* Client certificate */
	if (is_pkcs12) {
		wpa_set_data (conn_name,
			      phase2 ? "client_cert2" : "client_cert", NULL);
	} else {
		if (phase2)
			otype = &phase2_client_type;
		else
			otype = &client_type;

		/* Save the client certificate */
		if (!write_object (s_8021x, conn_name, NULL, otype, error))
			goto out;
	}

	success = TRUE;
out:
	if (generated_pw) {
		memset (generated_pw, 0, strlen (generated_pw));
		g_free (generated_pw);
	}
	if (enc_key) {
		memset (enc_key->data, 0, enc_key->len);
		g_byte_array_free (enc_key, TRUE);
	}
	return success;
}

static gboolean
write_8021x_setting (NMConnection *connection,
                     const char *conn_name,
                     gboolean wired,
                     GError **error)
{
	NMSetting8021x *s_8021x;
	const char *value;
	char *tmp = NULL;
	gboolean success = FALSE;
	GString *phase2_auth;
	GString *phase1;

	s_8021x =
	    (NMSetting8021x *) nm_connection_get_setting (connection,
							  NM_TYPE_SETTING_802_1X);

	if (!s_8021x) {
		return TRUE;
	}

	PLUGIN_PRINT (IFNET_PLUGIN_NAME, "Adding 8021x setting for %s",
		      conn_name);

	/* If wired, write KEY_MGMT */
	if (wired)
		wpa_set_data (conn_name, "key_mgmt", "IEEE8021X");

	/* EAP method */
	if (nm_setting_802_1x_get_num_eap_methods (s_8021x)) {
		value = nm_setting_802_1x_get_eap_method (s_8021x, 0);
		if (value)
			tmp = g_ascii_strup (value, -1);
	}
	wpa_set_data (conn_name, "eap", tmp ? tmp : NULL);
	g_free (tmp);

	wpa_set_data (conn_name, "identity",
		      (gchar *) nm_setting_802_1x_get_identity (s_8021x));

	wpa_set_data (conn_name, "anonymous_identity", (gchar *)
		      nm_setting_802_1x_get_anonymous_identity (s_8021x));

	wpa_set_data (conn_name, "password",
		      (gchar *) nm_setting_802_1x_get_password (s_8021x));

	phase1 = g_string_new (NULL);

	/* PEAP version */
	wpa_set_data (conn_name, "phase1", NULL);
	value = nm_setting_802_1x_get_phase1_peapver (s_8021x);
	if (value && (!strcmp (value, "0") || !strcmp (value, "1")))
		g_string_append_printf (phase1, "peapver=%s ", value);

	/* PEAP label */
	value = nm_setting_802_1x_get_phase1_peaplabel (s_8021x);
	if (value && !strcmp (value, "1"))
		g_string_append_printf (phase1, "peaplabel=%s ", value);
	if (phase1->len) {
		tmp = g_strstrip (g_strdup (phase1->str));
		wpa_set_data (conn_name, "phase1", tmp);
		g_free (tmp);
	}

	/* Phase2 auth methods */
	wpa_set_data (conn_name, "phase2", NULL);
	phase2_auth = g_string_new (NULL);

	value = nm_setting_802_1x_get_phase2_auth (s_8021x);
	if (value) {
		tmp = g_ascii_strup (value, -1);
		g_string_append_printf (phase2_auth, "auth=%s ", tmp);
		g_free (tmp);
	}

	/* Phase2 auth heap */
	value = nm_setting_802_1x_get_phase2_autheap (s_8021x);
	if (value) {
		tmp = g_ascii_strup (value, -1);
		g_string_append_printf (phase2_auth, "autheap=%s ", tmp);
		g_free (tmp);
	}
	tmp = g_strstrip (g_strdup (phase2_auth->str));
	wpa_set_data (conn_name, "phase2", phase2_auth->len ? tmp : NULL);
	g_free (tmp);

	g_string_free (phase2_auth, TRUE);
	g_string_free (phase1, TRUE);

	success = write_8021x_certs (s_8021x, FALSE, conn_name, error);
	if (success) {
		/* phase2/inner certs */
		success = write_8021x_certs (s_8021x, TRUE, conn_name, error);
	}

	return success;
}

static gboolean
write_wireless_security_setting (NMConnection * connection,
				 gchar * conn_name,
				 gboolean adhoc,
				 gboolean * no_8021x, GError ** error)
{
	NMSettingWirelessSecurity *s_wsec;
	const char *key_mgmt, *auth_alg, *key, *cipher, *psk;
	gboolean wep = FALSE, wpa = FALSE;
	char *tmp;
	guint32 i, num;
	GString *str;

	s_wsec =
	    (NMSettingWirelessSecurity *) nm_connection_get_setting (connection,
								     NM_TYPE_SETTING_WIRELESS_SECURITY);
	if (!s_wsec) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing '%s' setting",
			     NM_SETTING_WIRELESS_SECURITY_SETTING_NAME);
		return FALSE;
	}

	key_mgmt = nm_setting_wireless_security_get_key_mgmt (s_wsec);
	g_assert (key_mgmt);

	auth_alg = nm_setting_wireless_security_get_auth_alg (s_wsec);

	if (!strcmp (key_mgmt, "none")) {
		wpa_set_data (conn_name, "key_mgmt", "NONE");
		wep = TRUE;
		*no_8021x = TRUE;
	} else if (!strcmp (key_mgmt, "wpa-none")
		   || !strcmp (key_mgmt, "wpa-psk")) {
		wpa_set_data (conn_name, "key_mgmt", "WPA-PSK");
		wpa = TRUE;
		*no_8021x = TRUE;
	} else if (!strcmp (key_mgmt, "ieee8021x")) {
		wpa_set_data (conn_name, "key_mgmt", "IEEE8021X");
	} else if (!strcmp (key_mgmt, "wpa-eap")) {
		wpa_set_data (conn_name, "key_mgmt", "WPA-EAP");
		wpa = TRUE;
	} else
		PLUGIN_WARN (IFNET_PLUGIN_NAME, "Unknown key_mgmt: %s", key_mgmt);

	if (auth_alg) {
		if (!strcmp (auth_alg, "shared"))
			wpa_set_data (conn_name, "auth_alg", "SHARED");
		else if (!strcmp (auth_alg, "open"))
			wpa_set_data (conn_name, "auth_alg", "OPEN");
		else if (!strcmp (auth_alg, "leap")) {
			wpa_set_data (conn_name, "auth_alg", "LEAP");
			wpa_set_data (conn_name, "eap", "LEAP");
			wpa_set_data (conn_name, "identity", (gchar *)
				      nm_setting_wireless_security_get_leap_username
				      (s_wsec));
			wpa_set_data (conn_name, "password", (gchar *)
				      nm_setting_wireless_security_get_leap_password
				      (s_wsec));
			*no_8021x = TRUE;
		}
	} else
		wpa_set_data (conn_name, "auth_alg", NULL);

	/* Default WEP TX key index */
	wpa_set_data (conn_name, "wep_tx_keyidx", NULL);
	if (wep) {
		tmp =
		    g_strdup_printf ("%d",
				     nm_setting_wireless_security_get_wep_tx_keyidx
				     (s_wsec));
		wpa_set_data (conn_name, "wep_tx_keyidx", tmp);
		g_free (tmp);
	}

	/* WEP keys */
	for (i = 0; i < 4; i++) {
		int length;

		key = nm_setting_wireless_security_get_wep_key (s_wsec, i);
		if (!key)
			continue;
		tmp = g_strdup_printf ("wep_key%d", i);
		length = strlen (key);
		if (length == 10 || length == 26 || length == 58)
			wpa_set_data (conn_name, tmp, (gchar *) key);
		else {
			gchar *tmp_key = g_strdup_printf ("\"%s\"", key);

			wpa_set_data (conn_name, tmp, tmp_key);
			g_free (tmp_key);
		}
		g_free (tmp);
	}

	/* WPA Pairwise ciphers */
	wpa_set_data (conn_name, "pairwise", NULL);
	str = g_string_new (NULL);
	num = nm_setting_wireless_security_get_num_pairwise (s_wsec);
	for (i = 0; i < num; i++) {
		if (i > 0)
			g_string_append_c (str, ' ');
		cipher = nm_setting_wireless_security_get_pairwise (s_wsec, i);
		tmp = g_ascii_strup (cipher, -1);
		g_string_append (str, tmp);
		g_free (tmp);
	}
	if (strlen (str->str))
		wpa_set_data (conn_name, "pairwise", str->str);
	g_string_free (str, TRUE);

	/* WPA Group ciphers */
	wpa_set_data (conn_name, "group", NULL);
	str = g_string_new (NULL);
	num = nm_setting_wireless_security_get_num_groups (s_wsec);
	for (i = 0; i < num; i++) {
		if (i > 0)
			g_string_append_c (str, ' ');
		cipher = nm_setting_wireless_security_get_group (s_wsec, i);
		tmp = g_ascii_strup (cipher, -1);
		g_string_append (str, tmp);
		g_free (tmp);
	}
	if (strlen (str->str))
		wpa_set_data (conn_name, "group", str->str);
	g_string_free (str, TRUE);

	/* WPA Passphrase */
	if (wpa) {
		GString *quoted = NULL;

		psk = nm_setting_wireless_security_get_psk (s_wsec);
		if (psk && (strlen (psk) != 64)) {
			quoted = g_string_sized_new (strlen (psk) + 2);
			g_string_append_c (quoted, '"');
			g_string_append (quoted, psk);
			g_string_append_c (quoted, '"');
		}
		/* psk will be lost here if we don't check it for NULL */
		if (psk)
			wpa_set_data (conn_name, "psk",
					  quoted ? quoted->str : (gchar *) psk);
		if (quoted)
			g_string_free (quoted, TRUE);
	} else
		wpa_set_data (conn_name, "psk", NULL);

	return TRUE;
}

/* remove old ssid and add new one*/
static void
update_wireless_ssid (NMConnection *connection,
                      const char *conn_name,
                      const char *ssid,
                      gboolean hex)
{
	if(strcmp (conn_name, ssid)){
		ifnet_delete_network (conn_name);
		wpa_delete_security (conn_name);
	}

	ifnet_add_network (ssid, "wireless");
	wpa_add_security (ssid);
}

static gboolean
write_wireless_setting (NMConnection *connection,
                        const char *conn_name,
                        gboolean *no_8021x,
                        const char **out_new_name,
                        GError **error)
{
	NMSettingWireless *s_wireless;
	const GByteArray *ssid, *mac, *bssid;
	const char *mode;
	char buf[33];
	guint32 mtu, i;
	gboolean adhoc = FALSE, hex_ssid = FALSE;
	gchar *ssid_str, *tmp;

	s_wireless = (NMSettingWireless *) nm_connection_get_setting (connection, NM_TYPE_SETTING_WIRELESS);
	if (!s_wireless) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing '%s' setting",
			     NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}

	ssid = nm_setting_wireless_get_ssid (s_wireless);
	if (!ssid) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing SSID in '%s' setting",
			     NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}
	if (!ssid->len || ssid->len > 32) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Invalid SSID in '%s' setting",
			     NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}

	/* If the SSID contains any non-alnum characters, we need to use
	 * the hex notation of the SSID instead. (Because openrc doesn't
	 * support these characters, see bug #356337)
	 */
	for (i = 0; i < ssid->len; i++) {
		if (!isalnum (ssid->data[i])) {
			hex_ssid = TRUE;
			break;
		}
	}

	if (hex_ssid) {
		GString *str;

		/* Hex SSIDs don't get quoted */
		str = g_string_sized_new (ssid->len * 2 + 3);
		g_string_append (str, "0x");
		for (i = 0; i < ssid->len; i++)
			g_string_append_printf (str, "%02X", ssid->data[i]);
		update_wireless_ssid (connection, conn_name, str->str, hex_ssid);
		ssid_str = g_string_free (str, FALSE);
	} else {
		/* Printable SSIDs get quoted */
		memset (buf, 0, sizeof (buf));
		memcpy (buf, ssid->data, ssid->len);
		g_strstrip (buf);
		update_wireless_ssid (connection, conn_name, buf, hex_ssid);
		ssid_str = g_strdup (buf);
	}

	ifnet_set_data (ssid_str, "mac", NULL);
	mac = nm_setting_wireless_get_mac_address (s_wireless);
	if (mac) {
		tmp = g_strdup_printf ("%02X:%02X:%02X:%02X:%02X:%02X",
				       mac->data[0], mac->data[1], mac->data[2],
				       mac->data[3], mac->data[4],
				       mac->data[5]);
		ifnet_set_data (ssid_str, "mac", tmp);
		g_free (tmp);
	}

	ifnet_set_data (ssid_str, "mtu", NULL);
	mtu = nm_setting_wireless_get_mtu (s_wireless);
	if (mtu) {
		tmp = g_strdup_printf ("%u", mtu);
		ifnet_set_data (ssid_str, "mtu", tmp);
		g_free (tmp);
	}

	ifnet_set_data (ssid_str, "mode", NULL);
	mode = nm_setting_wireless_get_mode (s_wireless);
	if (!mode || !strcmp (mode, "infrastructure")) {
		wpa_set_data (ssid_str, "mode", "0");
	} else if (!strcmp (mode, "adhoc")) {
		wpa_set_data (ssid_str, "mode", "1");
		adhoc = TRUE;
	} else {
		PLUGIN_WARN (IFNET_PLUGIN_NAME,
			     "Invalid mode '%s' in '%s' setting",
			     mode, NM_SETTING_WIRELESS_SETTING_NAME);
		return FALSE;
	}

	wpa_set_data (ssid_str, "bssid", NULL);
	bssid = nm_setting_wireless_get_bssid (s_wireless);
	if (bssid) {
		tmp = g_strdup_printf ("%02X:%02X:%02X:%02X:%02X:%02X",
				       bssid->data[0], bssid->data[1],
				       bssid->data[2], bssid->data[3],
				       bssid->data[4], bssid->data[5]);
		wpa_set_data (ssid_str, "bssid", tmp);
		g_free (tmp);
	}

	if (nm_setting_wireless_get_security (s_wireless)) {
		if (!write_wireless_security_setting
		    (connection, ssid_str, adhoc, no_8021x, error))
			return FALSE;
	} else
		wpa_delete_security (ssid_str);

	if (out_new_name)
		*out_new_name = ifnet_get_data (ssid_str, "name");
	g_free (ssid_str);
	return TRUE;
}

static gboolean
write_wired_setting (NMConnection *connection,
                     const char *conn_name,
                     GError **error)
{
	NMSettingWired *s_wired;
	const GByteArray *mac;
	char *tmp;
	guint32 mtu;

	s_wired =
	    (NMSettingWired *) nm_connection_get_setting (connection,
							  NM_TYPE_SETTING_WIRED);
	if (!s_wired) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing '%s' setting",
			     NM_SETTING_WIRED_SETTING_NAME);
		return FALSE;
	}

	ifnet_set_data (conn_name, "mac", NULL);
	mac = nm_setting_wired_get_mac_address (s_wired);
	if (mac) {
		tmp = g_strdup_printf ("%02X:%02X:%02X:%02X:%02X:%02X",
				       mac->data[0], mac->data[1], mac->data[2],
				       mac->data[3], mac->data[4],
				       mac->data[5]);
		ifnet_set_data (conn_name, "mac", tmp);
		g_free (tmp);
	}

	ifnet_set_data (conn_name, "mtu", NULL);
	mtu = nm_setting_wired_get_mtu (s_wired);
	if (mtu) {
		tmp = g_strdup_printf ("%u", mtu);
		ifnet_set_data (conn_name, "mtu", tmp);
		g_free (tmp);
	}
	//FIXME may add connection type in future
	//ifnet_set_data (conn_name, "TYPE", TYPE_ETHERNET);

	return TRUE;
}

static void
write_connection_setting (NMSettingConnection *s_con, const char *conn_name)
{
	ifnet_set_data (conn_name, "auto",
			nm_setting_connection_get_autoconnect (s_con) ? "true" :
			"false");
}

static gboolean
write_ip4_setting (NMConnection *connection, const char *conn_name, GError **error)
{
	NMSettingIP4Config *s_ip4;
	const char *value;
	char *tmp;
	guint32 i, num;
	GString *searches;
	GString *ips;
	GString *routes;
	GString *dns;
	gboolean has_def_route = FALSE;
	gboolean success = FALSE;

	s_ip4 =
	    (NMSettingIP4Config *) nm_connection_get_setting (connection,
							      NM_TYPE_SETTING_IP4_CONFIG);
	if (!s_ip4) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing '%s' setting",
			     NM_SETTING_IP4_CONFIG_SETTING_NAME);
		return FALSE;
	}
	routes = g_string_new (NULL);

	value = nm_setting_ip4_config_get_method (s_ip4);
	g_assert (value);
	if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_MANUAL)) {

		num = nm_setting_ip4_config_get_num_addresses (s_ip4);
		ips = g_string_new (NULL);
		/* IPv4 addresses */
		for (i = 0; i < num; i++) {
			char buf[INET_ADDRSTRLEN + 1];
			NMIP4Address *addr;
			guint32 ip;

			addr = nm_setting_ip4_config_get_address (s_ip4, i);

			memset (buf, 0, sizeof (buf));
			ip = nm_ip4_address_get_address (addr);
			inet_ntop (AF_INET, (const void *) &ip, &buf[0],
				   sizeof (buf));
			g_string_append_printf (ips, "\"%s", &buf[0]);

			tmp =
			    g_strdup_printf ("%u",
					     nm_ip4_address_get_prefix (addr));
			g_string_append_printf (ips, "/%s\" ", tmp);
			g_free (tmp);

			/* only the first gateway will be written */
			if (!has_def_route && nm_ip4_address_get_gateway (addr)) {
				memset (buf, 0, sizeof (buf));
				ip = nm_ip4_address_get_gateway (addr);
				inet_ntop (AF_INET, (const void *) &ip, &buf[0],
					   sizeof (buf));
				g_string_append_printf (routes,
							"\"default via %s\" ",
							&buf[0]);
				has_def_route = TRUE;
			}
		}
		ifnet_set_data (conn_name, "config", ips->str);
		g_string_free (ips, TRUE);
	} else if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_SHARED))
		ifnet_set_data (conn_name, "config", "shared");
	else if (!strcmp (value, NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL))
		ifnet_set_data (conn_name, "config", "autoip");
	else
		ifnet_set_data (conn_name, "config", "dhcp");

	/* DNS Servers */
	num = nm_setting_ip4_config_get_num_dns (s_ip4);
	if (num > 0) {
		dns = g_string_new (NULL);
		for (i = 0; i < num; i++) {
			char buf[INET_ADDRSTRLEN + 1];
			guint32 ip;

			ip = nm_setting_ip4_config_get_dns (s_ip4, i);

			memset (buf, 0, sizeof (buf));
			inet_ntop (AF_INET, (const void *) &ip, &buf[0],
				   sizeof (buf));
			g_string_append_printf (dns, " %s", buf);
		}
		ifnet_set_data (conn_name, "dns_servers", dns->str);
		g_string_free (dns, TRUE);
	} else
		ifnet_set_data (conn_name, "dns_servers", NULL);

	/* DNS Searches */
	num = nm_setting_ip4_config_get_num_dns_searches (s_ip4);
	if (num > 0) {
		searches = g_string_new (NULL);
		for (i = 0; i < num; i++) {
			if (i > 0)
				g_string_append_c (searches, ' ');
			g_string_append (searches,
					 nm_setting_ip4_config_get_dns_search
					 (s_ip4, i));
		}
		ifnet_set_data (conn_name, "dns_search", searches->str);
		g_string_free (searches, TRUE);
	} else
		ifnet_set_data (conn_name, "dns_search", NULL);
	/* FIXME Will be implemented when configuration supports it
	   if (!strcmp(value, NM_SETTING_IP4_CONFIG_METHOD_AUTO)) {
	   value = nm_setting_ip4_config_get_dhcp_hostname(s_ip4);
	   if (value)
	   ifnet_set_data(conn_name, "DHCP_HOSTNAME", value,
	   FALSE);

	   value = nm_setting_ip4_config_get_dhcp_client_id(s_ip4);
	   if (value)
	   ifnet_set_data(conn_name, "DHCP_CLIENT_ID", value,
	   FALSE);
	   }
	 */

	/* Static routes */
	num = nm_setting_ip4_config_get_num_routes (s_ip4);
	if (num > 0) {
		for (i = 0; i < num; i++) {
			char buf[INET_ADDRSTRLEN + 1];
			NMIP4Route *route;
			guint32 ip;

			route = nm_setting_ip4_config_get_route (s_ip4, i);

			memset (buf, 0, sizeof (buf));
			ip = nm_ip4_route_get_dest (route);
			inet_ntop (AF_INET, (const void *) &ip, &buf[0],
				   sizeof (buf));
			g_string_append_printf (routes, "\"%s", buf);

			tmp =
			    g_strdup_printf ("%u",
					     nm_ip4_route_get_prefix (route));
			g_string_append_printf (routes, "/%s via ", tmp);
			g_free (tmp);

			memset (buf, 0, sizeof (buf));
			ip = nm_ip4_route_get_next_hop (route);
			inet_ntop (AF_INET, (const void *) &ip, &buf[0],
				   sizeof (buf));
			g_string_append_printf (routes, "%s\" ", buf);
		}
	}
	if (routes->len > 0)
		ifnet_set_data (conn_name, "routes", routes->str);
	else
		ifnet_set_data (conn_name, "routes", NULL);
	g_string_free (routes, TRUE);

	success = TRUE;

	return success;
}

static gboolean
write_route6_file (NMSettingIP6Config *s_ip6, const char *conn_name, GError **error)
{
	char dest[INET6_ADDRSTRLEN + 1];
	char next_hop[INET6_ADDRSTRLEN + 1];
	NMIP6Route *route;
	const struct in6_addr *ip;
	guint32 prefix;
	guint32 i, num;
	GString *routes_string;
	const char *old_routes;

	g_return_val_if_fail (s_ip6 != NULL, FALSE);
	num = nm_setting_ip6_config_get_num_routes (s_ip6);
	if (num == 0) {
		return TRUE;
	}

	old_routes = ifnet_get_data (conn_name, "routes");
	routes_string = g_string_new (old_routes);
	if (old_routes)
		g_string_append (routes_string, "\" ");
	for (i = 0; i < num; i++) {
		route = nm_setting_ip6_config_get_route (s_ip6, i);

		memset (dest, 0, sizeof (dest));
		ip = nm_ip6_route_get_dest (route);
		inet_ntop (AF_INET6, (const void *) ip, &dest[0],
			   sizeof (dest));

		prefix = nm_ip6_route_get_prefix (route);

		memset (next_hop, 0, sizeof (next_hop));
		ip = nm_ip6_route_get_next_hop (route);
		inet_ntop (AF_INET6, (const void *) ip, &next_hop[0],
			   sizeof (next_hop));

		g_string_append_printf (routes_string, "\"%s/%u via %s\" ",
					dest, prefix, next_hop);
	}
	if (num > 0)
		ifnet_set_data (conn_name, "routes", routes_string->str);
	g_string_free (routes_string, TRUE);

	return TRUE;
}

static gboolean
write_ip6_setting (NMConnection *connection, const char *conn_name, GError **error)
{
	NMSettingIP6Config *s_ip6;
	const char *value;
	char *prefix;
	guint32 i, num;
	GString *searches;
	char buf[INET6_ADDRSTRLEN + 1];
	NMIP6Address *addr;
	const struct in6_addr *ip;

	s_ip6 =
	    (NMSettingIP6Config *) nm_connection_get_setting (connection,
							      NM_TYPE_SETTING_IP6_CONFIG);
	if (!s_ip6) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing '%s' setting",
			     NM_SETTING_IP6_CONFIG_SETTING_NAME);
		return FALSE;
	}

	value = nm_setting_ip6_config_get_method (s_ip6);
	g_assert (value);
	if (!strcmp (value, NM_SETTING_IP6_CONFIG_METHOD_IGNORE)) {
		ifnet_set_data (conn_name, "enable_ipv6", "false");
		return TRUE;
	} else if (!strcmp (value, NM_SETTING_IP6_CONFIG_METHOD_MANUAL)) {
		/* nothing to do now */
	} else {
		// if (!strcmp(value, NM_SETTING_IP6_CONFIG_METHOD_AUTO)) {
		const char *config = ifnet_get_data (conn_name, "config");
		gchar *tmp;

		if (!config)
			tmp = g_strdup_printf ("dhcp6");
		else
			tmp = g_strdup_printf ("%s\" \"dhcp6\"", config);
		ifnet_set_data (conn_name, "config", tmp);
		g_free (tmp);
	}
	/* else if (!strcmp(value, NM_SETTING_IP6_CONFIG_METHOD_MANUAL)) {
	   } else if (!strcmp(value, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL)) {
	   } else if (!strcmp(value, NM_SETTING_IP6_CONFIG_METHOD_SHARED)) {
	   } */

	/* Remember to set IPv6 enabled */
	ifnet_set_data (conn_name, "enable_ipv6", "true");

	if (!strcmp (value, NM_SETTING_IP6_CONFIG_METHOD_MANUAL)) {
		const char *config = ifnet_get_data (conn_name, "config");
		gchar *tmp;
		GString *ip_str;

		if (!config)
			config = "";
		num = nm_setting_ip6_config_get_num_addresses (s_ip6);

		/* IPv6 addresses */
		ip_str = g_string_new (NULL);
		for (i = 0; i < num; i++) {
			addr = nm_setting_ip6_config_get_address (s_ip6, i);
			ip = nm_ip6_address_get_address (addr);
			prefix =
			    g_strdup_printf ("%u",
					     nm_ip6_address_get_prefix (addr));
			memset (buf, 0, sizeof (buf));
			inet_ntop (AF_INET6, (const void *) ip, buf,
				   sizeof (buf));
			g_string_append_printf (ip_str, "\"%s/", buf);
			g_string_append_printf (ip_str, "%s\" ", prefix);
			g_free (prefix);
		}
		tmp = g_strdup_printf ("%s\" %s", config, ip_str->str);
		ifnet_set_data (conn_name, "config", tmp);
		g_free (tmp);
		g_string_free (ip_str, TRUE);
	}

	/* DNS Servers */
	num = nm_setting_ip6_config_get_num_dns (s_ip6);
	if (num > 0) {
		const char *dns_servers = ifnet_get_data (conn_name, "dns_servers");
		gchar *tmp;
		GString *dns_string = g_string_new (NULL);

		if (!dns_servers)
			dns_servers = "";
		for (i = 0; i < num; i++) {
			ip = nm_setting_ip6_config_get_dns (s_ip6, i);

			memset (buf, 0, sizeof (buf));
			inet_ntop (AF_INET6, (const void *) ip, buf,
				   sizeof (buf));
			if (!strstr (dns_servers, buf))
				g_string_append_printf (dns_string, "%s ", buf);
		}
		tmp = g_strdup_printf ("%s %s", dns_servers, dns_string->str);
		ifnet_set_data (conn_name, "dns_servers", tmp);
		g_free (tmp);
		g_string_free (dns_string, TRUE);

	} else
		/* DNS Searches */
		num = nm_setting_ip6_config_get_num_dns_searches (s_ip6);
	if (num > 0) {
		const char *ip4_domains;

		ip4_domains = ifnet_get_data (conn_name, "dns_search");
		if (!ip4_domains)
			ip4_domains = "";
		searches = g_string_new (ip4_domains);
		for (i = 0; i < num; i++) {
			const gchar *search = NULL;

			search =
			    nm_setting_ip6_config_get_dns_search (s_ip6, i);
			if (search && !strstr (searches->str, search)) {
				if (searches->len > 0)
					g_string_append_c (searches, ' ');
				g_string_append (searches, search);
			}
		}
		ifnet_set_data (conn_name, "dns_search", searches->str);
		g_string_free (searches, TRUE);
	}

	write_route6_file (s_ip6, conn_name, error);
	if (error && *error)
		return FALSE;
	return TRUE;
}

static gboolean
write_pppoe_setting (const char *conn_name, NMSettingPPPOE * s_pppoe)
{
	const gchar *value;

	value = nm_setting_pppoe_get_username (s_pppoe);
	if (!value) {
		return FALSE;
	}
	ifnet_set_data (conn_name, "username", (gchar *) value);

	value = nm_setting_pppoe_get_password (s_pppoe);
	/* password could be NULL here */
	if (value) {
		ifnet_set_data (conn_name, "password", (gchar *) value);
	}
	return TRUE;
}

gboolean
ifnet_update_parsers_by_connection (NMConnection *connection,
                                    const char *conn_name,
                                    const char *config_file,
                                    const char *wpa_file,
                                    gchar **out_new_name,
                                    GError **error)
{
	NMSettingConnection *s_con;
	NMSettingIP6Config *s_ip6;
	gboolean success = FALSE;
	const char *type;
	gboolean no_8021x = FALSE;
	gboolean wired = FALSE, pppoe = TRUE;
	const char *new_name = NULL;

	s_con =
	    NM_SETTING_CONNECTION (nm_connection_get_setting
				   (connection, NM_TYPE_SETTING_CONNECTION));
	if (!s_con) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing '%s' setting",
			     NM_SETTING_CONNECTION_SETTING_NAME);
		return FALSE;
	}


	type = nm_setting_connection_get_connection_type (s_con);
	if (!type) {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Missing connection type!");
		goto out;
	}

	if (!strcmp (type, NM_SETTING_WIRED_SETTING_NAME)) {
		/* Writing wired setting */
		if (!write_wired_setting (connection, conn_name, error))
			goto out;
		wired = TRUE;
		no_8021x = TRUE;
	} else if (!strcmp (type, NM_SETTING_WIRELESS_SETTING_NAME)) {
		/* Writing wireless setting */
		if (!write_wireless_setting (connection, conn_name, &no_8021x, &new_name, error))
			goto out;
	} else if (!strcmp (type, NM_SETTING_PPPOE_SETTING_NAME)) {
		NMSettingPPPOE *s_pppoe;

		/* Writing pppoe setting */
		s_pppoe = NM_SETTING_PPPOE (nm_connection_get_setting (connection, NM_TYPE_SETTING_PPPOE));
		if (!write_pppoe_setting (conn_name, s_pppoe))
			goto out;
		pppoe = TRUE;
		wired = TRUE;
		no_8021x = TRUE;
	} else {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Can't write connection type '%s'", type);
		goto out;
	}

	/* connection name may have been updated; use it when writing out
	 * the rest of the settings.
	 */
	if (new_name)
		conn_name = new_name;

	//FIXME wired connection doesn't support 8021x now
	if (!no_8021x) {
		if (!write_8021x_setting (connection, conn_name, wired, error))
			goto out;
	}

	/* IPv4 Setting */
	if (!write_ip4_setting (connection, conn_name, error))
		goto out;

	s_ip6 = (NMSettingIP6Config *) nm_connection_get_setting (connection, NM_TYPE_SETTING_IP6_CONFIG);
	if (s_ip6) {
		/* IPv6 Setting */
		if (!write_ip6_setting (connection, conn_name, error))
			goto out;
	}

	/* Connection Setting */
	write_connection_setting (s_con, conn_name);

	/* connection id will be displayed in nm-applet */
	update_connection_id (connection, conn_name);

	success = ifnet_flush_to_file (config_file);
	if (success)
		wpa_flush_to_file (wpa_file);

	if (out_new_name)
		*out_new_name = g_strdup (conn_name);

out:
	return success;
}

gboolean
ifnet_delete_connection_in_parsers (const char *conn_name,
                                    const char *config_file,
                                    const char *wpa_file)
{
	gboolean result = FALSE;

	ifnet_delete_network (conn_name);
	result = ifnet_flush_to_file (config_file);
	if (result) {
		/* connection may not have security information
		 * so simply ignore the return value*/
		wpa_delete_security (conn_name);
		wpa_flush_to_file (wpa_file);
	}

	return result;
}

/* get the available wired name(eth*). */
static gchar *
get_wired_name ()
{
	int i = 0;

	for (; i < 256; i++) {
		gchar *conn_name = g_strdup_printf ("eth%d", i);

		if (!ifnet_has_network (conn_name)) {
			return conn_name;
		} else
			g_free (conn_name);
	}
	return NULL;
}

/* get the available pppoe name(ppp*). */
static gchar *
get_ppp_name ()
{
	int i = 0;

	for (; i < 256; i++) {
		gchar *conn_name = g_strdup_printf ("ppp%d", i);

		if (!ifnet_has_network (conn_name)) {
			return conn_name;
		} else
			g_free (conn_name);
	}
	return NULL;
}

/* get wireless ssid */
static gchar *
get_wireless_name (NMConnection * connection)
{
	NMSettingWireless *s_wireless;
	const GByteArray *ssid;
	gboolean hex_ssid = FALSE;
	gchar *result = NULL;
	char buf[33];
	int i = 0;

	s_wireless =
	    (NMSettingWireless *) nm_connection_get_setting (connection,
							     NM_TYPE_SETTING_WIRELESS);
	if (!s_wireless)
		return NULL;

	ssid = nm_setting_wireless_get_ssid (s_wireless);
	if (!ssid->len || ssid->len > 32) {
		return NULL;
	}

	for (i = 0; i < ssid->len; i++) {
		if (!isprint (ssid->data[i])) {
			hex_ssid = TRUE;
			break;
		}
	}

	if (hex_ssid) {
		GString *str;

		str = g_string_sized_new (ssid->len * 2 + 3);
		g_string_append (str, "0x");
		for (i = 0; i < ssid->len; i++)
			g_string_append_printf (str, "%02X", ssid->data[i]);
		result = g_strdup (str->str);
		g_string_free (str, TRUE);
	} else {
		memset (buf, 0, sizeof (buf));
		memcpy (buf, ssid->data, ssid->len);
		result = g_strdup_printf ("%s", buf);
		g_strstrip (result);
	}

	return result;
}

char *
ifnet_add_new_connection (NMConnection *connection,
                          const char *config_file,
                          const char *wpa_file,
                          GError **error)
{
	NMSettingConnection *s_con;
	gboolean success = FALSE;
	const char *type;
	gchar *new_type, *new_name = NULL;

	s_con = NM_SETTING_CONNECTION (nm_connection_get_setting (connection, NM_TYPE_SETTING_CONNECTION));
	g_assert (s_con);
	type = nm_setting_connection_get_connection_type (s_con);
	g_assert (type);

	PLUGIN_PRINT (IFNET_PLUGIN_NAME, "Adding %s connection", type);

	/* get name and type
	 * Wireless type: wireless
	 * Wired type: wired
	 * PPPoE type: ppp*/
	if (!strcmp (type, NM_SETTING_WIRED_SETTING_NAME)) {
		new_name = get_wired_name ();
		if (!new_name)
			goto out;
		new_type = "wired";
	} else if (!strcmp (type, NM_SETTING_WIRELESS_SETTING_NAME)) {
		new_name = get_wireless_name (connection);
		new_type = "wireless";
	} else if (!strcmp (type, NM_SETTING_PPPOE_SETTING_NAME)) {
		new_name = get_ppp_name ();
		if (!new_name)
			goto out;
		new_type = "ppp";
	} else {
		g_set_error (error, ifnet_plugin_error_quark (), 0,
			     "Can't write connection type '%s'", type);
		goto out;
	}

	if (ifnet_add_network (new_name, new_type)) {
		success = ifnet_update_parsers_by_connection (connection,
		                                              new_name,
		                                              config_file,
		                                              wpa_file,
		                                              NULL,
		                                              error);
	}

	PLUGIN_PRINT (IFNET_PLUGIN_NAME, "Added new connection: %s, result: %s",
	              new_name, success ? "success" : "fail");

out:
	if (!success)
		g_free (new_name);
	return success ? new_name : NULL;
}
