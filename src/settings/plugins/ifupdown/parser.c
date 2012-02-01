/* -*- Mode: C; tab-width: 5; indent-tabs-mode: t; c-basic-offset: 5 -*- */

/* NetworkManager system settings service (ifupdown)
 *
 * Alexander Sack <asac@ubuntu.com>
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
 * (C) Copyright 2008 Canonical Ltd.
 */

#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#include <nm-connection.h>
#include <NetworkManager.h>
#include <nm-setting-connection.h>
#include <nm-setting-ip4-config.h>
#include <nm-setting-ppp.h>
#include <nm-setting-wired.h>
#include <nm-setting-wireless.h>
#include <nm-setting-8021x.h>
#include <nm-system-config-interface.h>
#include <nm-utils.h>

#include "parser.h"
#include "plugin.h"


#define WPA_PMK_LEN 32

#include "parser.h"

static const gchar*
_ifupdownplugin_guess_connection_type (if_block *block)
{
	if_data *curr = block->info;
	const gchar* ret_type = NULL;
	const gchar* value = ifparser_getkey(block, "inet");
	if(value && !strcmp("ppp", value)) {
		ret_type = NM_SETTING_PPP_SETTING_NAME;
	}

	while(!ret_type && curr) {
		if(!strncmp("wireless-", curr->key, strlen("wireless-")) ||
		   !strncmp("wpa-", curr->key, strlen("wpa-"))) {
			ret_type = NM_SETTING_WIRELESS_SETTING_NAME;
		}
		curr = curr->next;
	}

	if(!ret_type)
		ret_type = NM_SETTING_WIRED_SETTING_NAME;

	PLUGIN_PRINT("SCPluginIfupdown",
			   "guessed connection type (%s) = %s",
			   block->name, ret_type);
	return ret_type;
}


struct _Mapping {
	const gchar *domain;
	const gpointer target;
};

static gpointer
map_by_mapping(struct _Mapping *mapping, const gchar *key)
{
	struct _Mapping *curr = mapping;
	while(curr->domain) {
		if(!strcmp(curr->domain, key))
			return curr->target;
		curr++;
	}
	return NULL;
}

static void
update_wireless_setting_from_if_block(NMConnection *connection,
							   if_block *block)
{
	gint wpa_l= strlen("wpa-");
	gint wireless_l= strlen("wireless-");

	if_data *curr = block->info;
	const gchar* value = ifparser_getkey (block, "inet");
	struct _Mapping mapping[] = {
		{"ssid", "ssid"},
		{ NULL, NULL}
	};

	NMSettingWireless *wireless_setting = NULL;

	if(value && !strcmp("ppp", value)) {
		return;
	}

	PLUGIN_PRINT ("SCPlugin-Ifupdown", "update wireless settings (%s).", block->name);
	wireless_setting = NM_SETTING_WIRELESS(nm_setting_wireless_new());

	while(curr) {
		if(strlen(curr->key) > wireless_l &&
		   !strncmp("wireless-", curr->key, wireless_l)) {
			const gchar* newkey = map_by_mapping(mapping, curr->key+wireless_l);
			PLUGIN_PRINT ("SCPlugin-Ifupdown", "wireless setting key: %s='%s'",
					    newkey, curr->data);
			if(newkey && !strcmp("ssid", newkey)) {
				GByteArray *ssid;
				gint len = strlen(curr->data);

				ssid = g_byte_array_sized_new (len);
				g_byte_array_append (ssid, (const guint8 *) curr->data, len);
				g_object_set (wireless_setting, NM_SETTING_WIRELESS_SSID, ssid, NULL);
				g_byte_array_free (ssid, TRUE);
				PLUGIN_PRINT("SCPlugin-Ifupdown", "setting wireless ssid = %d", len);
			} else {
				g_object_set(wireless_setting,
					   newkey, curr->data,
					   NULL);
			}
		} else if(strlen(curr->key) > wpa_l &&
				!strncmp("wpa-", curr->key, wpa_l)) {
			const gchar* newkey = map_by_mapping(mapping, curr->key+wpa_l);

			if(newkey && !strcmp("ssid", newkey)) {
				GByteArray *ssid;
				gint len = strlen(curr->data);

				ssid = g_byte_array_sized_new (len);
				g_byte_array_append (ssid, (const guint8 *) curr->data, len);
				g_object_set (wireless_setting, NM_SETTING_WIRELESS_SSID, ssid, NULL);
				g_byte_array_free (ssid, TRUE);
				PLUGIN_PRINT("SCPlugin-Ifupdown", "setting wpa ssid = %d", len);
			} else if(newkey) {

				g_object_set(wireless_setting,
						   newkey, curr->data,
						   NULL);
				PLUGIN_PRINT ("SCPlugin-Ifupdown", "setting wpa newkey(%s)=data(%s)", newkey, curr->data);
			}
		}
		curr = curr->next;
	}
	nm_connection_add_setting(connection, (NMSetting*) wireless_setting);
}

typedef gchar* (*IfupdownStrDupeFunc) (gpointer value, gpointer data);
typedef gpointer (*IfupdownStrToTypeFunc) (const gchar* value);

static char*
normalize_dupe_wireless_key (gpointer value, gpointer data) {
	char* valuec = value;
	char* endc = valuec + strlen (valuec);
	char* delim = valuec;
	char* next = delim;
	char* result = malloc (strlen (valuec) + 1);
	char* result_cur = result;

	while (*delim && (next = strchr (delim, '-')) != NULL) {
		if (next == delim) {
			delim++;
			continue;
		}
		strncpy (result_cur, delim, next - delim);
		result_cur += next - delim;
		delim = next + 1;
	}
	if (*delim && strlen (valuec) > GPOINTER_TO_UINT(delim - valuec)) {
		strncpy (result_cur, delim, endc - delim);
		result_cur += endc - delim;
	}
	*result_cur = '\0';
	return result;
}

static char*
normalize_dupe (gpointer value, gpointer data) {
	return g_strdup(value);
}

static char*
normalize_tolower (gpointer value, gpointer data) {
	return g_ascii_strdown(value, -1);
}

static char *normalize_psk (gpointer value, gpointer data)
{
	if (strlen (value) >= 8 && strlen (value) <= 64)
		return g_strdup (value);
	return NULL;
}

static gpointer
string_to_gpointerint(const gchar* data)
{
	gint result = (gint) strtol (data, NULL, 10);
	return GINT_TO_POINTER(result);
}
	
static gpointer
string_to_glist_of_strings(const gchar* data)
{
	GSList *ret = NULL;
	gchar *string = (gchar*) data;
	while(string) {
		gchar* next = NULL;
		if( (next = strchr(string, ' '))  ||
		    (next = strchr(string, '\t')) ||
		    (next = strchr(string, '\0')) ) {

			gchar *part = g_strndup(string, (next - string));
			ret = g_slist_append(ret, part);
			if (*next)
				string = next+1;
			else
				string = NULL;
		} else {
			string = NULL;
		}
	}
	return ret;
}

static void
slist_free_all(gpointer slist)
{
	GSList *list = (GSList *) slist;
	g_slist_foreach (list, (GFunc) g_free, NULL);
	g_slist_free (list);
}

static void
update_wireless_security_setting_from_if_block(NMConnection *connection,
									  if_block *block)
{
	gint wpa_l= strlen("wpa-");
	gint wireless_l= strlen("wireless-");
	if_data *curr = block->info;
	const gchar* value = ifparser_getkey (block, "inet");
	struct _Mapping mapping[] = {
		{"psk", "psk"},
		{"identity", "leap-username"},
		{"password", "leap-password"},
		{"key", "wep-key0"},
		{"key-mgmt", "key-mgmt"},
		{"group", "group"},
		{"pairwise", "pairwise"},
		{"proto", "proto"},
		{"pin", "pin"},
		{"wep-key0", "wep-key0"},
		{"wep-key1", "wep-key1"},
		{"wep-key2", "wep-key2"},
		{"wep-key3", "wep-key3"},
		{"wep-tx-keyidx", "wep-tx-keyidx"},
		{ NULL, NULL}
	};

	struct _Mapping dupe_mapping[] = {
		{"psk", normalize_psk},
		{"identity", normalize_dupe},
		{"password", normalize_dupe},
		{"key", normalize_dupe_wireless_key},
		{"key-mgmt", normalize_tolower},
		{"group", normalize_tolower},
		{"pairwise", normalize_tolower},
		{"proto", normalize_tolower},
		{"pin", normalize_dupe},
		{"wep-key0", normalize_dupe_wireless_key},
		{"wep-key1", normalize_dupe_wireless_key},
		{"wep-key2", normalize_dupe_wireless_key},
		{"wep-key3", normalize_dupe_wireless_key},
		{"wep-tx-keyidx", normalize_dupe},
		{ NULL, NULL}
	};

	struct _Mapping type_mapping[] = {
		{"group", string_to_glist_of_strings},
		{"pairwise", string_to_glist_of_strings},
		{"proto", string_to_glist_of_strings},
		{"wep-tx-keyidx", string_to_gpointerint},
		{ NULL, NULL}
	};

	struct _Mapping free_type_mapping[] = {
		{"group", slist_free_all},
		{"pairwise", slist_free_all},
		{"proto", slist_free_all},
		{ NULL, NULL}
	};

	NMSettingWirelessSecurity *wireless_security_setting;
	NMSettingWireless *s_wireless;
	gboolean security = FALSE;

	if(value && !strcmp("ppp", value)) {
		return;
	}

	s_wireless = nm_connection_get_setting_wireless(connection);
	g_return_if_fail(s_wireless);

	PLUGIN_PRINT ("SCPlugin-Ifupdown","update wireless security settings (%s).", block->name);
	wireless_security_setting =
		NM_SETTING_WIRELESS_SECURITY(nm_setting_wireless_security_new());

	while(curr) {
		if(strlen(curr->key) > wireless_l &&
		   !strncmp("wireless-", curr->key, wireless_l)) {

			gchar *property_value = NULL;
			gpointer typed_property_value = NULL;
			const gchar* newkey = map_by_mapping(mapping, curr->key+wireless_l);
			IfupdownStrDupeFunc dupe_func = map_by_mapping (dupe_mapping, curr->key+wireless_l);
			IfupdownStrToTypeFunc type_map_func = map_by_mapping (type_mapping, curr->key+wireless_l);
			GFreeFunc free_func = map_by_mapping (free_type_mapping, curr->key+wireless_l);
			if(!newkey || !dupe_func) {
				g_warning("no (wireless) mapping found for key: %s", curr->key);
				goto next;
			}
			property_value = (*dupe_func) (curr->data, connection);
			PLUGIN_PRINT ("SCPlugin-Ifupdown", "setting wireless security key: %s=%s",
					    newkey, property_value);

			if (type_map_func) {
				errno = 0;
				typed_property_value = (*type_map_func) (property_value);
				if(errno)
					goto wireless_next;
			}
		    
			g_object_set(wireless_security_setting,
					   newkey, typed_property_value ? typed_property_value : property_value,
					   NULL);
			security = TRUE;

		wireless_next:
			g_free(property_value);
			if (typed_property_value && free_func)
				(*free_func) (typed_property_value);

		} else if(strlen(curr->key) > wpa_l &&
				!strncmp("wpa-", curr->key, wpa_l)) {

			gchar *property_value = NULL;
			gpointer typed_property_value = NULL;
			const gchar* newkey = map_by_mapping(mapping, curr->key+wpa_l);
			IfupdownStrDupeFunc dupe_func = map_by_mapping (dupe_mapping, curr->key+wpa_l);
			IfupdownStrToTypeFunc type_map_func = map_by_mapping (type_mapping, curr->key+wpa_l);
			GFreeFunc free_func = map_by_mapping (free_type_mapping, curr->key+wpa_l);
			if(!newkey || !dupe_func) {
				goto next;
			}
			property_value = (*dupe_func) (curr->data, connection);
			PLUGIN_PRINT ("SCPlugin-Ifupdown", "setting wpa security key: %s=%s",
					    newkey,
#ifdef DEBUG_SECRETS
					    property_value
#else // DEBUG_SECRETS
					    !strcmp("key", newkey) ||
					    !strcmp("leap-password", newkey) ||
					    !strcmp("pin", newkey) ||
					    !strcmp("psk", newkey) ||
					    !strcmp("wep-key0", newkey) ||
					    !strcmp("wep-key1", newkey) ||
					    !strcmp("wep-key2", newkey) ||
					    !strcmp("wep-key3", newkey) ||
					    NULL ?
					    "<omitted>" : property_value
#endif // DEBUG_SECRETS
					    );

			if (type_map_func) {
				errno = 0;
				typed_property_value = (*type_map_func) (property_value);
				if(errno)
					goto wpa_next;
			}
		    
			g_object_set(wireless_security_setting,
					   newkey, typed_property_value ? typed_property_value : property_value,
					   NULL);
			security = TRUE;

		wpa_next:
			g_free(property_value);
			if (free_func && typed_property_value)
				(*free_func) (typed_property_value);
		}
	next:
		curr = curr->next;
	}


	if(security) {
		nm_connection_add_setting(connection, NM_SETTING(wireless_security_setting));
		g_object_set(s_wireless, NM_SETTING_WIRELESS_SEC, NM_SETTING_WIRELESS_SECURITY_SETTING_NAME, NULL);
	}

}

static void
update_wired_setting_from_if_block(NMConnection *connection,
							if_block *block)
{
	NMSettingWired *s_wired = NULL;
	s_wired = NM_SETTING_WIRED(nm_setting_wired_new());
	nm_connection_add_setting(connection, NM_SETTING(s_wired));
}

static GQuark
eni_plugin_error_quark() {
	static GQuark error_quark = 0;

	if(!error_quark) {
		error_quark = g_quark_from_static_string ("eni-plugin-error-quark");
	}

	return error_quark;
}

static void
ifupdown_ip4_add_dns (NMSettingIP4Config *s_ip4, const char *dns)
{
	struct in_addr addr;
	char **list, **iter;

	if (dns == NULL)
		return;

	list = g_strsplit_set (dns, " \t", -1);
	for (iter = list; iter && *iter; iter++) {
		g_strstrip (*iter);
		if (isblank (*iter[0]))
			continue;
		if (!inet_pton (AF_INET, *iter, &addr)) {
			PLUGIN_WARN ("SCPlugin-Ifupdown",
					   "    warning: ignoring invalid nameserver '%s'", *iter);
			continue;
		}

		if (!nm_setting_ip4_config_add_dns (s_ip4, addr.s_addr)) {
			PLUGIN_WARN ("SCPlugin-Ifupdown",
					   "    warning: duplicate DNS domain '%s'", *iter);
		}
	}
	g_strfreev (list);
}

static gboolean
update_ip4_setting_from_if_block(NMConnection *connection,
						   if_block *block,
						   GError **error)
{

	NMSettingIP4Config *s_ip4 = NM_SETTING_IP4_CONFIG (nm_setting_ip4_config_new());
	const char *type = ifparser_getkey(block, "inet");
	gboolean is_static = type && !strcmp("static", type);

	if (!is_static) {
		g_object_set (s_ip4, NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_AUTO, NULL);
	} else {
		struct in_addr tmp_addr, tmp_mask, tmp_gw;
		NMIP4Address *addr;
		const char *address_v;
		const char *netmask_v;
		const char *gateway_v;
		const char *nameserver_v;
		const char *nameservers_v;
		const char *search_v;
		char **list, **iter;
		guint32 netmask_int = 32;

		/* Address */
		address_v = ifparser_getkey (block, "address");
		if (!address_v || !inet_pton (AF_INET, address_v, &tmp_addr)) {
			g_set_error (error, eni_plugin_error_quark (), 0,
			             "Missing IPv4 address '%s'",
			             address_v ? address_v : "(none)");
			goto error;
		}

		/* mask/prefix */
		netmask_v = ifparser_getkey (block, "netmask");
		if (netmask_v) {
			if (!inet_pton (AF_INET, netmask_v, &tmp_mask)) {
				g_set_error (error, eni_plugin_error_quark (), 0,
						   "Invalid IPv4 netmask '%s'", netmask_v);
				goto error;
			}
			netmask_int = nm_utils_ip4_netmask_to_prefix (tmp_mask.s_addr);
		}

		/* gateway */
		gateway_v = ifparser_getkey (block, "gateway");
		if (!gateway_v)
			gateway_v = address_v;  /* dcbw: whaaa?? */
		if (!inet_pton (AF_INET, gateway_v, &tmp_gw)) {
			g_set_error (error, eni_plugin_error_quark (), 0,
					   "Invalid IPv4 gateway '%s'", gateway_v);
			goto error;
		}

		/* Add the new address to the setting */
		addr = nm_ip4_address_new ();
		nm_ip4_address_set_address (addr, tmp_addr.s_addr);
		nm_ip4_address_set_prefix (addr, netmask_int);
		nm_ip4_address_set_gateway (addr, tmp_gw.s_addr);

		if (nm_setting_ip4_config_add_address (s_ip4, addr)) {
			PLUGIN_PRINT("SCPlugin-Ifupdown", "addresses count: %d",
			             nm_setting_ip4_config_get_num_addresses (s_ip4));
		} else {
			PLUGIN_PRINT("SCPlugin-Ifupdown", "ignoring duplicate IP4 address");
		}

		nameserver_v = ifparser_getkey (block, "dns-nameserver");
		ifupdown_ip4_add_dns (s_ip4, nameserver_v);

		nameservers_v = ifparser_getkey (block, "dns-nameservers");
		ifupdown_ip4_add_dns (s_ip4, nameservers_v);

		if (!nm_setting_ip4_config_get_num_dns (s_ip4))
			PLUGIN_PRINT("SCPlugin-Ifupdown", "No dns-nameserver configured in /etc/network/interfaces");

		/* DNS searches */
		search_v = ifparser_getkey (block, "dns-search");
		if (search_v) {
			list = g_strsplit_set (search_v, " \t", -1);
			for (iter = list; iter && *iter; iter++) {
				g_strstrip (*iter);
				if (isblank (*iter[0]))
					continue;
				if (!nm_setting_ip4_config_add_dns_search (s_ip4, *iter)) {
					PLUGIN_WARN ("SCPlugin-Ifupdown",
							   "    warning: duplicate DNS domain '%s'", *iter);
				}
			}
			g_strfreev (list);
		}

		g_object_set (s_ip4, NM_SETTING_IP4_CONFIG_METHOD, NM_SETTING_IP4_CONFIG_METHOD_MANUAL, NULL);
	}

	nm_connection_add_setting (connection, NM_SETTING (s_ip4));
	return TRUE;

error:
	g_object_unref (s_ip4);
	return FALSE;
}

static void
ifupdown_ip6_add_dns (NMSettingIP6Config *s_ip6, const char *dns)
{
	struct in6_addr addr;
	char **list, **iter;

	if (dns == NULL)
		return;

	list = g_strsplit_set (dns, " \t", -1);
	for (iter = list; iter && *iter; iter++) {
		g_strstrip (*iter);
		if (isblank (*iter[0]))
			continue;
		if (!inet_pton (AF_INET6, *iter, &addr)) {
			PLUGIN_WARN ("SCPlugin-Ifupdown",
					   "    warning: ignoring invalid nameserver '%s'", *iter);
			continue;
		}

		if (!nm_setting_ip6_config_add_dns (s_ip6, &addr)) {
			PLUGIN_WARN ("SCPlugin-Ifupdown",
					   "    warning: duplicate DNS domain '%s'", *iter);
		}
	}
	g_strfreev (list);
}

static gboolean
update_ip6_setting_from_if_block(NMConnection *connection,
						   if_block *block,
						   GError **error)
{
	NMSettingIP6Config *s_ip6 = NM_SETTING_IP6_CONFIG (nm_setting_ip6_config_new());
	const char *type = ifparser_getkey(block, "inet6");
	gboolean is_static = type && (!strcmp("static", type) ||
							!strcmp("v4tunnel", type));

	if (!is_static) {
		g_object_set(s_ip6, NM_SETTING_IP6_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_AUTO, NULL);
	} else {
		struct in6_addr tmp_addr, tmp_gw;
		NMIP6Address *addr;
		const char *address_v;
		const char *prefix_v;
		const char *gateway_v;
		const char *nameserver_v;
		const char *nameservers_v;
		const char *search_v;
		int prefix_int = 128;
		char **list, **iter;

		/* Address */
		address_v = ifparser_getkey(block, "address");
		if (!address_v || !inet_pton (AF_INET6, address_v, &tmp_addr)) {
			g_set_error (error, eni_plugin_error_quark (), 0,
			             "Missing IPv6 address '%s'",
			             address_v ? address_v : "(none)");
			goto error;
		}

		/* Prefix */
		prefix_v = ifparser_getkey(block, "netmask");
		if (prefix_v)
			prefix_int = g_ascii_strtoll (prefix_v, NULL, 10);

		/* Gateway */
		gateway_v = ifparser_getkey (block, "gateway");
		if (!gateway_v)
			gateway_v = address_v;  /* dcbw: whaaa?? */
		if (!inet_pton (AF_INET6, gateway_v, &tmp_gw)) {
			g_set_error (error, eni_plugin_error_quark (), 0,
					   "Invalid IPv6 gateway '%s'", gateway_v);
			goto error;
		}

		/* Add the new address to the setting */
		addr = nm_ip6_address_new ();
		nm_ip6_address_set_address (addr, &tmp_addr);
		nm_ip6_address_set_prefix (addr, prefix_int);
		nm_ip6_address_set_gateway (addr, &tmp_gw);

		if (nm_setting_ip6_config_add_address (s_ip6, addr)) {
			PLUGIN_PRINT("SCPlugin-Ifupdown", "addresses count: %d",
					   nm_setting_ip6_config_get_num_addresses (s_ip6));
		} else {
			PLUGIN_PRINT("SCPlugin-Ifupdown", "ignoring duplicate IP6 address");
		}

		nameserver_v = ifparser_getkey(block, "dns-nameserver");
		ifupdown_ip6_add_dns (s_ip6, nameserver_v);

		nameservers_v = ifparser_getkey(block, "dns-nameservers");
		ifupdown_ip6_add_dns (s_ip6, nameservers_v);

		if (!nm_setting_ip6_config_get_num_dns (s_ip6))
			PLUGIN_PRINT("SCPlugin-Ifupdown", "No dns-nameserver configured in /etc/network/interfaces");

		/* DNS searches */
		search_v = ifparser_getkey (block, "dns-search");
		if (search_v) {
			list = g_strsplit_set (search_v, " \t", -1);
			for (iter = list; iter && *iter; iter++) {
				g_strstrip (*iter);
				if (isblank (*iter[0]))
					continue;
				if (!nm_setting_ip6_config_add_dns_search (s_ip6, *iter)) {
					PLUGIN_WARN ("SCPlugin-Ifupdown",
							   "    warning: duplicate DNS domain '%s'", *iter);
				}
			}
			g_strfreev (list);
		}

		g_object_set (s_ip6,
		              NM_SETTING_IP6_CONFIG_METHOD, NM_SETTING_IP6_CONFIG_METHOD_MANUAL,
		              NULL);
	}

	nm_connection_add_setting (connection, NM_SETTING (s_ip6));
	return TRUE;

error:
	g_object_unref (s_ip6);
	return FALSE;
}

gboolean
ifupdown_update_connection_from_if_block (NMConnection *connection,
                                          if_block *block,
                                          GError **error)
{
	const char *type = NULL;
	char *idstr = NULL;
	char *uuid_base = NULL;
	char *uuid = NULL;
	NMSettingConnection *s_con;
	gboolean success = FALSE;

	s_con = nm_connection_get_setting_connection (connection);
	if(!s_con) {
		s_con = NM_SETTING_CONNECTION (nm_setting_connection_new());
		g_assert (s_con);
		nm_connection_add_setting (connection, NM_SETTING (s_con));
	}

	type = _ifupdownplugin_guess_connection_type (block);
	idstr = g_strconcat ("Ifupdown (", block->name, ")", NULL);
	uuid_base = idstr;

	uuid = nm_utils_uuid_generate_from_string (uuid_base);
	g_object_set (s_con,
	              NM_SETTING_CONNECTION_TYPE, type,
	              NM_SETTING_CONNECTION_ID, idstr,
	              NM_SETTING_CONNECTION_UUID, uuid,
	              NM_SETTING_CONNECTION_READ_ONLY, TRUE,
	              NM_SETTING_CONNECTION_AUTOCONNECT, FALSE,
	              NULL);
	g_free (uuid);

	PLUGIN_PRINT("SCPlugin-Ifupdown", "update_connection_setting_from_if_block: name:%s, type:%s, id:%s, uuid: %s",
			   block->name, type, idstr, nm_setting_connection_get_uuid (s_con));

	if (!strcmp (NM_SETTING_WIRED_SETTING_NAME, type))
		update_wired_setting_from_if_block (connection, block);
	else if (!strcmp (NM_SETTING_WIRELESS_SETTING_NAME, type)) {
		update_wireless_setting_from_if_block (connection, block);
		update_wireless_security_setting_from_if_block (connection, block);
	}

	if (ifparser_haskey(block, "inet6"))
		success = update_ip6_setting_from_if_block (connection, block, error);
	else
		success = update_ip4_setting_from_if_block (connection, block, error);

	if (success == TRUE)
		success = nm_connection_verify (connection, error);

	g_free (idstr);
	return success;
}
