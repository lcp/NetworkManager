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
 * Copyright (C) 2007 - 2008 Novell, Inc.
 */

#include <config.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <ctype.h>

#include "nm-policy.h"
#include "NetworkManagerUtils.h"
#include "nm-wifi-ap.h"
#include "nm-activation-request.h"
#include "nm-logging.h"
#include "nm-device.h"
#include "nm-dbus-manager.h"
#include "nm-setting-ip4-config.h"
#include "nm-setting-connection.h"
#include "nm-system.h"
#include "nm-dns-manager.h"
#include "nm-vpn-manager.h"
#include "nm-policy-hostname.h"
#include "nm-manager-auth.h"
#include "nm-firewall-manager.h"

struct NMPolicy {
	NMManager *manager;
	guint update_state_id;
	GSList *pending_activation_checks;
	GSList *manager_ids;
	GSList *settings_ids;
	GSList *dev_ids;

	NMVPNManager *vpn_manager;
	gulong vpn_activated_id;
	gulong vpn_deactivated_id;

	NMFirewallManager *fw_manager;

	NMSettings *settings;

	NMDevice *default_device4;
	NMDevice *default_device6;

	HostnameThread *lookup;

	gint reset_retries_id;  /* idle handler for resetting the retries count */

	char *orig_hostname; /* hostname at NM start time */
	char *cur_hostname;  /* hostname we want to assign */
	gboolean hostname_changed;  /* TRUE if NM ever set the hostname */
};

#define RETRIES_TAG "autoconnect-retries"
#define RETRIES_DEFAULT	4
#define RESET_RETRIES_TIMESTAMP_TAG "reset-retries-timestamp-tag"
#define RESET_RETRIES_TIMER 300
#define FAILURE_REASON_TAG "failure-reason"

static void schedule_activate_all (NMPolicy *policy);


static NMDevice *
get_best_ip4_device (NMManager *manager, NMActRequest **out_req)
{
	GSList *devices, *iter;
	NMDevice *best = NULL;
	int best_prio = G_MAXINT;

	g_return_val_if_fail (manager != NULL, NULL);
	g_return_val_if_fail (NM_IS_MANAGER (manager), NULL);
	g_return_val_if_fail (out_req != NULL, NULL);
	g_return_val_if_fail (*out_req == NULL, NULL);

	devices = nm_manager_get_devices (manager);
	for (iter = devices; iter; iter = g_slist_next (iter)) {
		NMDevice *dev = NM_DEVICE (iter->data);
		NMDeviceType devtype = nm_device_get_device_type (dev);
		NMActRequest *req;
		NMConnection *connection;
		NMIP4Config *ip4_config;
		NMSettingIP4Config *s_ip4;
		int prio;
		guint i;
		gboolean can_default = FALSE;
		const char *method = NULL;

		if (nm_device_get_state (dev) != NM_DEVICE_STATE_ACTIVATED)
			continue;

		ip4_config = nm_device_get_ip4_config (dev);
		if (!ip4_config)
			continue;

		req = nm_device_get_act_request (dev);
		g_assert (req);
		connection = nm_act_request_get_connection (req);
		g_assert (connection);

		/* Never set the default route through an IPv4LL-addressed device */
		s_ip4 = nm_connection_get_setting_ip4_config (connection);
		if (s_ip4)
			method = nm_setting_ip4_config_get_method (s_ip4);

		if (s_ip4 && !strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL))
			continue;

		/* Make sure at least one of this device's IP addresses has a gateway */
		for (i = 0; i < nm_ip4_config_get_num_addresses (ip4_config); i++) {
			NMIP4Address *addr;

			addr = nm_ip4_config_get_address (ip4_config, i);
			if (nm_ip4_address_get_gateway (addr)) {
				can_default = TRUE;
				break;
			}
		}

		if (!can_default && (devtype != NM_DEVICE_TYPE_MODEM))
			continue;

		/* 'never-default' devices can't ever be the default */
		if (   (s_ip4 && nm_setting_ip4_config_get_never_default (s_ip4))
		    || nm_ip4_config_get_never_default (ip4_config))
			continue;

		prio = nm_device_get_priority (dev);
		if (prio > 0 && prio < best_prio) {
			best = dev;
			best_prio = prio;
			*out_req = req;
		}
	}

	return best;
}

static NMDevice *
get_best_ip6_device (NMManager *manager, NMActRequest **out_req)
{
	GSList *devices, *iter;
	NMDevice *best = NULL;
	int best_prio = G_MAXINT;

	g_return_val_if_fail (manager != NULL, NULL);
	g_return_val_if_fail (NM_IS_MANAGER (manager), NULL);
	g_return_val_if_fail (out_req != NULL, NULL);
	g_return_val_if_fail (*out_req == NULL, NULL);

	devices = nm_manager_get_devices (manager);
	for (iter = devices; iter; iter = g_slist_next (iter)) {
		NMDevice *dev = NM_DEVICE (iter->data);
		NMDeviceType devtype = nm_device_get_device_type (dev);
		NMActRequest *req;
		NMConnection *connection;
		NMIP6Config *ip6_config;
		NMSettingIP6Config *s_ip6;
		int prio;
		guint i;
		gboolean can_default = FALSE;
		const char *method = NULL;

		if (nm_device_get_state (dev) != NM_DEVICE_STATE_ACTIVATED)
			continue;

		ip6_config = nm_device_get_ip6_config (dev);
		if (!ip6_config)
			continue;

		req = nm_device_get_act_request (dev);
		g_assert (req);
		connection = nm_act_request_get_connection (req);
		g_assert (connection);

		/* Never set the default route through an IPv4LL-addressed device */
		s_ip6 = nm_connection_get_setting_ip6_config (connection);
		if (s_ip6)
			method = nm_setting_ip6_config_get_method (s_ip6);

		if (method && !strcmp (method, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL))
			continue;

		/* Make sure at least one of this device's IP addresses has a gateway */
		for (i = 0; i < nm_ip6_config_get_num_addresses (ip6_config); i++) {
			NMIP6Address *addr;

			addr = nm_ip6_config_get_address (ip6_config, i);
			if (nm_ip6_address_get_gateway (addr)) {
				can_default = TRUE;
				break;
			}
		}

		if (!can_default && (devtype != NM_DEVICE_TYPE_MODEM))
			continue;

		/* 'never-default' devices can't ever be the default */
		if (s_ip6 && nm_setting_ip6_config_get_never_default (s_ip6))
			continue;

		prio = nm_device_get_priority (dev);
		if (prio > 0 && prio < best_prio) {
			best = dev;
			best_prio = prio;
			*out_req = req;
		}
	}

	return best;
}

static void
_set_hostname (NMPolicy *policy,
               const char *new_hostname,
               const char *msg)
{
	NMDnsManager *dns_mgr;

	/* The incoming hostname *can* be NULL, which will get translated to
	 * 'localhost.localdomain' or such in the hostname policy code, but we
	 * keep cur_hostname = NULL in the case because we need to know that
	 * there was no valid hostname to start with.
	 */

	/* Don't change the hostname or update DNS this is the first time we're
	 * trying to change the hostname, and it's not actually changing.
	 */
	if (   policy->orig_hostname
	    && (policy->hostname_changed == FALSE)
	    && g_strcmp0 (policy->orig_hostname, new_hostname) == 0)
		return;

	/* Don't change the hostname or update DNS if the hostname isn't actually
	 * going to change.
	 */
	if (g_strcmp0 (policy->cur_hostname, new_hostname) == 0)
		return;

	g_free (policy->cur_hostname);
	policy->cur_hostname = g_strdup (new_hostname);
	policy->hostname_changed = TRUE;

	dns_mgr = nm_dns_manager_get (NULL);
	nm_dns_manager_set_hostname (dns_mgr, policy->cur_hostname);
	g_object_unref (dns_mgr);

	if (nm_policy_set_system_hostname (policy->cur_hostname, msg))
		nm_utils_call_dispatcher ("hostname", NULL, NULL, NULL, NULL, NULL);
}

static void
lookup_callback (HostnameThread *thread,
                 int result,
                 const char *hostname,
                 gpointer user_data)
{
	NMPolicy *policy = (NMPolicy *) user_data;
	char *msg;

	/* Update the hostname if the calling lookup thread is the in-progress one */
	if (!hostname_thread_is_dead (thread) && (thread == policy->lookup)) {
		policy->lookup = NULL;
		if (!hostname) {
			/* Fall back to localhost.localdomain */
			msg = g_strdup_printf ("address lookup failed: %d", result);
			_set_hostname (policy, NULL, msg);
			g_free (msg);
		} else
			_set_hostname (policy, hostname, "from address lookup");
	}
	hostname_thread_free (thread);
}

static void
update_system_hostname (NMPolicy *policy, NMDevice *best4, NMDevice *best6)
{
	char *configured_hostname = NULL;
	NMActRequest *best_req4 = NULL;
	NMActRequest *best_req6 = NULL;
	const char *dhcp_hostname, *p;

	g_return_if_fail (policy != NULL);

	if (policy->lookup) {
		hostname_thread_kill (policy->lookup);
		policy->lookup = NULL;
	}

	/* Hostname precedence order:
	 *
	 * 1) a configured hostname (from settings)
	 * 2) automatic hostname from the default device's config (DHCP, VPN, etc)
	 * 3) the original hostname when NM started
	 * 4) reverse-DNS of the best device's IPv4 address
	 *
	 */

	/* Try a persistent hostname first */
	g_object_get (G_OBJECT (policy->manager), NM_MANAGER_HOSTNAME, &configured_hostname, NULL);
	if (configured_hostname) {
		_set_hostname (policy, configured_hostname, "from system configuration");
		g_free (configured_hostname);
		return;
	}

	/* Try automatically determined hostname from the best device's IP config */
	if (!best4)
		best4 = get_best_ip4_device (policy->manager, &best_req4);
	if (!best6)
		best6 = get_best_ip6_device (policy->manager, &best_req6);

	if (!best4 && !best6) {
		/* No best device; fall back to original hostname or if there wasn't
		 * one, 'localhost.localdomain'
		 */
		_set_hostname (policy, policy->orig_hostname, "no default device");
		return;
	}

	if (best4) {
		NMDHCP4Config *dhcp4_config;

		/* Grab a hostname out of the device's DHCP4 config */
		dhcp4_config = nm_device_get_dhcp4_config (best4);
		if (dhcp4_config) {
			p = dhcp_hostname = nm_dhcp4_config_get_option (dhcp4_config, "host_name");
			if (dhcp_hostname && strlen (dhcp_hostname)) {
				/* Sanity check; strip leading spaces */
				while (*p) {
					if (!isblank (*p++)) {
						_set_hostname (policy, p-1, "from DHCPv4");
						return;
					}
				}
				nm_log_warn (LOGD_DNS, "DHCPv4-provided hostname '%s' looks invalid; ignoring it",
				             dhcp_hostname);
			}
		}
	} else if (best6) {
		NMDHCP6Config *dhcp6_config;

		/* Grab a hostname out of the device's DHCP6 config */
		dhcp6_config = nm_device_get_dhcp6_config (best6);
		if (dhcp6_config) {
			p = dhcp_hostname = nm_dhcp6_config_get_option (dhcp6_config, "host_name");
			if (dhcp_hostname && strlen (dhcp_hostname)) {
				/* Sanity check; strip leading spaces */
				while (*p) {
					if (!isblank (*p++)) {
						_set_hostname (policy, p-1, "from DHCPv6");
						return;
					}
				}
				nm_log_warn (LOGD_DNS, "DHCPv6-provided hostname '%s' looks invalid; ignoring it",
				             dhcp_hostname);
			}
		}
	}

	/* If no automatically-configured hostname, try using the hostname from
	 * when NM started up.
	 */
	if (policy->orig_hostname) {
		_set_hostname (policy, policy->orig_hostname, "from system startup");
		return;
	}

	/* No configured hostname, no automatically determined hostname, and no
	 * bootup hostname. Start reverse DNS of the current IPv4 or IPv6 address.
	 */
	if (best4) {
		NMIP4Config *ip4_config;
		NMIP4Address *addr4;

		ip4_config = nm_device_get_ip4_config (best4);
		if (   !ip4_config
		    || (nm_ip4_config_get_num_nameservers (ip4_config) == 0)
		    || (nm_ip4_config_get_num_addresses (ip4_config) == 0)) {
			/* No valid IP4 config (!!); fall back to localhost.localdomain */
			_set_hostname (policy, NULL, "no IPv4 config");
			return;
		}

		addr4 = nm_ip4_config_get_address (ip4_config, 0);
		g_assert (addr4); /* checked for > 1 address above */

		/* Start the hostname lookup thread */
		policy->lookup = hostname4_thread_new (nm_ip4_address_get_address (addr4), lookup_callback, policy);
	} else if (best6) {
		NMIP6Config *ip6_config;
		NMIP6Address *addr6;

		ip6_config = nm_device_get_ip6_config (best6);
		if (   !ip6_config
		    || (nm_ip6_config_get_num_nameservers (ip6_config) == 0)
		    || (nm_ip6_config_get_num_addresses (ip6_config) == 0)) {
			/* No valid IP6 config (!!); fall back to localhost.localdomain */
			_set_hostname (policy, NULL, "no IPv6 config");
			return;
		}

		addr6 = nm_ip6_config_get_address (ip6_config, 0);
		g_assert (addr6); /* checked for > 1 address above */

		/* Start the hostname lookup thread */
		policy->lookup = hostname6_thread_new (nm_ip6_address_get_address (addr6), lookup_callback, policy);
	}

	if (!policy->lookup) {
		/* Fall back to 'localhost.localdomain' */
		_set_hostname (policy, NULL, "error starting hostname thread");
	}
}

static void
update_ip4_routing_and_dns (NMPolicy *policy, gboolean force_update)
{
	NMDnsIPConfigType dns_type = NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE;
	NMDevice *best = NULL;
	NMActRequest *best_req = NULL;
	NMDnsManager *dns_mgr;
	GSList *devices = NULL, *iter, *vpns;
	NMIP4Config *ip4_config = NULL;
	NMIP4Address *addr;
	const char *ip_iface = NULL;
	NMConnection *connection = NULL;
	NMSettingConnection *s_con = NULL;
	const char *connection_id;
	int ip_ifindex = 0;

	best = get_best_ip4_device (policy->manager, &best_req);
	if (!best)
		goto out;
	if (!force_update && (best == policy->default_device4))
		goto out;

	/* If a VPN connection is active, it is preferred */
	vpns = nm_vpn_manager_get_active_connections (policy->vpn_manager);
	for (iter = vpns; iter; iter = g_slist_next (iter)) {
		NMVPNConnection *candidate = NM_VPN_CONNECTION (iter->data);
		NMConnection *vpn_connection;
		NMSettingIP4Config *s_ip4;
		gboolean can_default = TRUE;
		NMVPNConnectionState vpn_state;

		/* If it's marked 'never-default', don't make it default */
		vpn_connection = nm_vpn_connection_get_connection (candidate);
		g_assert (vpn_connection);

		/* Check the active IP4 config from the VPN service daemon */
		ip4_config = nm_vpn_connection_get_ip4_config (candidate);
		if (ip4_config && nm_ip4_config_get_never_default (ip4_config))
			can_default = FALSE;

		/* Check the user's preference from the NMConnection */
		s_ip4 = nm_connection_get_setting_ip4_config (vpn_connection);
		if (s_ip4 && nm_setting_ip4_config_get_never_default (s_ip4))
			can_default = FALSE;

		vpn_state = nm_vpn_connection_get_vpn_state (candidate);
		if (can_default && (vpn_state == NM_VPN_CONNECTION_STATE_ACTIVATED)) {
			NMIP4Config *parent_ip4;
			NMDevice *parent;

			ip_iface = nm_vpn_connection_get_ip_iface (candidate);
			ip_ifindex = nm_vpn_connection_get_ip_ifindex (candidate);
			connection = nm_vpn_connection_get_connection (candidate);
			addr = nm_ip4_config_get_address (ip4_config, 0);

			parent = nm_vpn_connection_get_parent_device (candidate);
			parent_ip4 = nm_device_get_ip4_config (parent);

			nm_system_replace_default_ip4_route_vpn (ip_ifindex,
			                                         nm_ip4_address_get_gateway (addr),
			                                         nm_vpn_connection_get_ip4_internal_gateway (candidate),
			                                         nm_ip4_config_get_mss (ip4_config),
			                                         nm_device_get_ip_ifindex (parent),
			                                         nm_ip4_config_get_mss (parent_ip4));

			dns_type = NM_DNS_IP_CONFIG_TYPE_VPN;
		}
	}
	g_slist_free (vpns);

	/* The best device gets the default route if a VPN connection didn't */
	if (!ip_iface || !ip4_config) {
		connection = nm_act_request_get_connection (best_req);
		ip_iface = nm_device_get_ip_iface (best);
		ip_ifindex = nm_device_get_ip_ifindex (best);
		ip4_config = nm_device_get_ip4_config (best);
		g_assert (ip4_config);
		addr = nm_ip4_config_get_address (ip4_config, 0);

		nm_system_replace_default_ip4_route (ip_ifindex,
		                                     nm_ip4_address_get_gateway (addr),
		                                     nm_ip4_config_get_mss (ip4_config));
		dns_type = NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE;
	}

	if (!ip_iface || !ip4_config) {
		nm_log_warn (LOGD_CORE, "couldn't determine IP interface (%p) or IPv4 config (%p)!",
		             ip_iface, ip4_config);
		goto out;
	}

	/* Update the default active connection.  Only mark the new default
	 * active connection after setting default = FALSE on all other connections
	 * first.  The order is important, we don't want two connections marked
	 * default at the same time ever.
	 */
	devices = nm_manager_get_devices (policy->manager);
	for (iter = devices; iter; iter = g_slist_next (iter)) {
		NMDevice *dev = NM_DEVICE (iter->data);
		NMActRequest *req;

		req = nm_device_get_act_request (dev);
		if (req && (req != best_req))
			nm_act_request_set_default (req, FALSE);
	}

	dns_mgr = nm_dns_manager_get (NULL);
	nm_dns_manager_add_ip4_config (dns_mgr, ip_iface, ip4_config, dns_type);
	g_object_unref (dns_mgr);

	/* Now set new default active connection _after_ updating DNS info, so that
	 * if the connection is shared dnsmasq picks up the right stuff.
	 */
	if (best_req)
		nm_act_request_set_default (best_req, TRUE);

	if (connection)
		s_con = nm_connection_get_setting_connection (connection);

	connection_id = s_con ? nm_setting_connection_get_id (s_con) : NULL;
	if (connection_id) {
		nm_log_info (LOGD_CORE, "Policy set '%s' (%s) as default for IPv4 routing and DNS.", connection_id, ip_iface);
	} else {
		nm_log_info (LOGD_CORE, "Policy set (%s) as default for IPv4 routing and DNS.", ip_iface);
	}

out:
	policy->default_device4 = best;
}

static void
update_ip6_routing_and_dns (NMPolicy *policy, gboolean force_update)
{
	NMDnsIPConfigType dns_type = NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE;
	NMDevice *best = NULL;
	NMActRequest *best_req = NULL;
	NMDnsManager *dns_mgr;
	GSList *devices = NULL, *iter;
#if 0
	GSList *vpns;
#endif
	NMIP6Config *ip6_config = NULL;
	NMIP6Address *addr;
	const char *ip_iface = NULL;
	int ip_ifindex = -1;
	NMConnection *connection = NULL;
	NMSettingConnection *s_con = NULL;
	const char *connection_id;

	best = get_best_ip6_device (policy->manager, &best_req);
	if (!best)
		goto out;
	if (!force_update && (best == policy->default_device6))
		goto out;

#if 0
	/* If a VPN connection is active, it is preferred */
	vpns = nm_vpn_manager_get_active_connections (policy->vpn_manager);
	for (iter = vpns; iter; iter = g_slist_next (iter)) {
		NMVPNConnection *candidate = NM_VPN_CONNECTION (iter->data);
		NMConnection *vpn_connection;
		NMSettingIP6Config *s_ip6;
		gboolean can_default = TRUE;
		NMVPNConnectionState vpn_state;

		/* If it's marked 'never-default', don't make it default */
		vpn_connection = nm_vpn_connection_get_connection (candidate);
		g_assert (vpn_connection);
		s_ip6 = nm_connection_get_setting_ip6_config (vpn_connection);
		if (s_ip6 && nm_setting_ip6_config_get_never_default (s_ip6))
			can_default = FALSE;

		vpn_state = nm_vpn_connection_get_vpn_state (candidate);
		if (can_default && (vpn_state == NM_VPN_CONNECTION_STATE_ACTIVATED)) {
			NMIP6Config *parent_ip6;
			NMDevice *parent;

			ip_iface = nm_vpn_connection_get_ip_iface (candidate);
			connection = nm_vpn_connection_get_connection (candidate);
			ip6_config = nm_vpn_connection_get_ip6_config (candidate);
			addr = nm_ip6_config_get_address (ip6_config, 0);

			parent = nm_vpn_connection_get_parent_device (candidate);
			parent_ip6 = nm_device_get_ip6_config (parent);

			nm_system_replace_default_ip6_route_vpn (ip_iface,
			                                         nm_ip6_address_get_gateway (addr),
			                                         nm_vpn_connection_get_ip4_internal_gateway (candidate),
			                                         nm_ip6_config_get_mss (ip4_config),
			                                         nm_device_get_ip_iface (parent),
			                                         nm_ip6_config_get_mss (parent_ip4));

			dns_type = NM_DNS_IP_CONFIG_TYPE_VPN;
		}
	}
	g_slist_free (vpns);
#endif

	/* The best device gets the default route if a VPN connection didn't */
	if (!ip_iface || !ip6_config) {
		connection = nm_act_request_get_connection (best_req);
		ip_iface = nm_device_get_ip_iface (best);
		ip_ifindex = nm_device_get_ip_ifindex (best);
		ip6_config = nm_device_get_ip6_config (best);
		g_assert (ip6_config);
		addr = nm_ip6_config_get_address (ip6_config, 0);

		nm_system_replace_default_ip6_route (ip_ifindex, nm_ip6_address_get_gateway (addr));

		dns_type = NM_DNS_IP_CONFIG_TYPE_BEST_DEVICE;
	}

	if (!ip_iface || !ip6_config) {
		nm_log_warn (LOGD_CORE, "couldn't determine IP interface (%p) or IPv6 config (%p)!",
		             ip_iface, ip6_config);
		goto out;
	}

	/* Update the default active connection.  Only mark the new default
	 * active connection after setting default = FALSE on all other connections
	 * first.  The order is important, we don't want two connections marked
	 * default at the same time ever.
	 */
	devices = nm_manager_get_devices (policy->manager);
	for (iter = devices; iter; iter = g_slist_next (iter)) {
		NMDevice *dev = NM_DEVICE (iter->data);
		NMActRequest *req;

		req = nm_device_get_act_request (dev);
		if (req && (req != best_req))
			nm_act_request_set_default6 (req, FALSE);
	}

	dns_mgr = nm_dns_manager_get (NULL);
	nm_dns_manager_add_ip6_config (dns_mgr, ip_iface, ip6_config, dns_type);
	g_object_unref (dns_mgr);

	/* Now set new default active connection _after_ updating DNS info, so that
	 * if the connection is shared dnsmasq picks up the right stuff.
	 */
	if (best_req)
		nm_act_request_set_default6 (best_req, TRUE);

	if (connection)
		s_con = nm_connection_get_setting_connection (connection);

	connection_id = s_con ? nm_setting_connection_get_id (s_con) : NULL;
	if (connection_id) {
		nm_log_info (LOGD_CORE, "Policy set '%s' (%s) as default for IPv6 routing and DNS.", connection_id, ip_iface);
	} else {
		nm_log_info (LOGD_CORE, "Policy set (%s) as default for IPv6 routing and DNS.", ip_iface);
	}

out:
	policy->default_device6 = best;
}

static void
update_routing_and_dns (NMPolicy *policy, gboolean force_update)
{
	update_ip4_routing_and_dns (policy, force_update);
	update_ip6_routing_and_dns (policy, force_update);

	/* Update the system hostname */
	update_system_hostname (policy, policy->default_device4, policy->default_device6);
}

static void
set_connection_auto_retries (NMConnection *connection, guint retries)
{
	/* add +1 so that the tag still exists if the # retries is 0 */
	g_object_set_data (G_OBJECT (connection), RETRIES_TAG, GUINT_TO_POINTER (retries + 1));
}

static guint32
get_connection_auto_retries (NMConnection *connection)
{
	/* subtract 1 to handle the +1 from set_connection_auto_retries() */
	return GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (connection), RETRIES_TAG)) - 1;
}

typedef struct {
	NMPolicy *policy;
	NMDevice *device;
	guint id;
} ActivateData;

static void
activate_data_free (ActivateData *data)
{
	if (data->id)
		g_source_remove (data->id);
	g_object_unref (data->device);
	memset (data, 0, sizeof (*data));
	g_free (data);
}

static gboolean
check_master_dependency (NMManager *manager, NMDevice *device, NMConnection *connection)
{
	NMSettingConnection *s_con;
	NMDevice *master_device;
	const char *master;
	NMActRequest *req;

	s_con = nm_connection_get_setting_connection (connection);
	g_assert (s_con);

	master = nm_setting_connection_get_master (s_con);

	/* no master defined, proceed with activation */
	if (!master)
		return TRUE;

	master_device = nm_manager_get_device_by_master (manager, master, NULL);

	/* If master device is not yet present, postpone activation until later */
	if (!master_device)
		return FALSE;

	/* Make all slaves wait for the master connection to activate. */
	req = nm_device_get_act_request (master_device);
	if (!req || !nm_act_request_get_connection (req))
		return FALSE;

	nm_device_set_master (device, master_device);

	return TRUE;
}

static gboolean
auto_activate_device (gpointer user_data)
{
	ActivateData *data = (ActivateData *) user_data;
	NMPolicy *policy;
	NMConnection *best_connection;
	char *specific_object = NULL;
	GSList *connections, *iter;

	g_assert (data);
	policy = data->policy;

	data->id = 0;
	policy->pending_activation_checks = g_slist_remove (policy->pending_activation_checks, data);

	// FIXME: if a device is already activating (or activated) with a connection
	// but another connection now overrides the current one for that device,
	// deactivate the device and activate the new connection instead of just
	// bailing if the device is already active
	if (nm_device_get_act_request (data->device))
		goto out;

	iter = connections = nm_settings_get_connections (policy->settings);

	/* Remove connections that shouldn't be auto-activated */
	while (iter) {
		NMSettingsConnection *candidate = NM_SETTINGS_CONNECTION (iter->data);
		gboolean remove_it = FALSE;
		const char *permission;

		/* Grab next item before we possibly delete the current item */
		iter = g_slist_next (iter);

		/* Ignore connections that were tried too many times or are not visible
		 * to any logged-in users.  Also ignore shared wifi connections for
		 * which no user has the shared wifi permission.
		 */
		if (   get_connection_auto_retries (NM_CONNECTION (candidate)) == 0
		    || nm_settings_connection_is_visible (candidate) == FALSE)
			remove_it = TRUE;
		else {
			permission = nm_utils_get_shared_wifi_permission (NM_CONNECTION (candidate));
			if (permission) {
				if (nm_settings_connection_check_permission (candidate, permission) == FALSE)
					remove_it = TRUE;
			}
		}

		if (remove_it)
			connections = g_slist_remove (connections, candidate);
	}

	best_connection = nm_device_get_best_auto_connection (data->device, connections, &specific_object);
	if (best_connection) {
		GError *error = NULL;

		if (!check_master_dependency (data->policy->manager, data->device, best_connection)) {
			nm_log_info (LOGD_DEVICE, "Connection '%s' auto-activation postponed: master not available",
			             nm_connection_get_id (best_connection));
			goto postpone;
		}

		nm_log_info (LOGD_DEVICE, "Auto-activating connection '%s'.",
		             nm_connection_get_id (best_connection));
		if (!nm_manager_activate_connection (policy->manager,
		                                     best_connection,
		                                     specific_object,
		                                     nm_device_get_path (data->device),
		                                     NULL,
		                                     &error)) {
			nm_log_info (LOGD_DEVICE, "Connection '%s' auto-activation failed: (%d) %s",
			             nm_connection_get_id (best_connection), error->code, error->message);
			g_error_free (error);
		}
	}

 postpone:
	g_slist_free (connections);

 out:
	activate_data_free (data);
	return FALSE;
}

static ActivateData *
activate_data_new (NMPolicy *policy, NMDevice *device, guint delay_seconds)
{
	ActivateData *data;

	data = g_malloc0 (sizeof (ActivateData));
	data->policy = policy;
	data->device = g_object_ref (device);
	if (delay_seconds > 0)
		data->id = g_timeout_add_seconds (delay_seconds, auto_activate_device, data);
	else
		data->id = g_idle_add (auto_activate_device, data);
	return data;
}

static ActivateData *
find_pending_activation (GSList *list, NMDevice *device)
{
	GSList *iter;

	for (iter = list; iter; iter = g_slist_next (iter)) {
		if (((ActivateData *) iter->data)->device == device)
			return iter->data;
	}
	return NULL;
}

/*****************************************************************************/

static void
vpn_connection_activated (NMVPNManager *manager,
                          NMVPNConnection *vpn,
                          gpointer user_data)
{
	update_routing_and_dns ((NMPolicy *) user_data, TRUE);
}

static void
vpn_connection_deactivated (NMVPNManager *manager,
                            NMVPNConnection *vpn,
                            NMVPNConnectionState state,
                            NMVPNConnectionStateReason reason,
                            gpointer user_data)
{
	update_routing_and_dns ((NMPolicy *) user_data, TRUE);
}

static void
global_state_changed (NMManager *manager, NMState state, gpointer user_data)
{
}

static void
hostname_changed (NMManager *manager, GParamSpec *pspec, gpointer user_data)
{
	update_system_hostname ((NMPolicy *) user_data, NULL, NULL);
}

static void
reset_retries_all (NMSettings *settings, NMDevice *device)
{
	GSList *connections, *iter;
	GError *error = NULL;

	connections = nm_settings_get_connections (settings);
	for (iter = connections; iter; iter = g_slist_next (iter)) {
		if (!device || nm_device_check_connection_compatible (device, iter->data, &error))
			set_connection_auto_retries (NM_CONNECTION (iter->data), RETRIES_DEFAULT);
		g_clear_error (&error);
	}
	g_slist_free (connections);
}

static void
reset_retries_for_failed_secrets (NMSettings *settings)
{
	GSList *connections, *iter;

	connections = nm_settings_get_connections (settings);
	for (iter = connections; iter; iter = g_slist_next (iter)) {
		NMDeviceStateReason reason = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (iter->data), FAILURE_REASON_TAG));

		if (reason == NM_DEVICE_STATE_REASON_NO_SECRETS) {
			set_connection_auto_retries (NM_CONNECTION (iter->data), RETRIES_DEFAULT);
			g_object_set_data (G_OBJECT (iter->data), FAILURE_REASON_TAG, GUINT_TO_POINTER (0));
		}
	}
	g_slist_free (connections);
}

static void
sleeping_changed (NMManager *manager, GParamSpec *pspec, gpointer user_data)
{
	NMPolicy *policy = user_data;
	gboolean sleeping = FALSE, enabled = FALSE;

	g_object_get (G_OBJECT (manager), NM_MANAGER_SLEEPING, &sleeping, NULL);
	g_object_get (G_OBJECT (manager), NM_MANAGER_NETWORKING_ENABLED, &enabled, NULL);

	/* Reset retries on all connections so they'll checked on wakeup */
	if (sleeping || !enabled)
		reset_retries_all (policy->settings, NULL);
}

static void
schedule_activate_check (NMPolicy *policy, NMDevice *device, guint delay_seconds)
{
	ActivateData *data;
	NMDeviceState state;

	if (nm_manager_get_state (policy->manager) == NM_STATE_ASLEEP)
		return;

	state = nm_device_get_state (device);
	if (state < NM_DEVICE_STATE_DISCONNECTED)
		return;

	if (!nm_device_get_enabled (device))
		return;

	if (!nm_device_autoconnect_allowed (device))
		return;

	/* Schedule an auto-activation if there isn't one already for this device */
	if (find_pending_activation (policy->pending_activation_checks, device) == NULL) {
		data = activate_data_new (policy, device, delay_seconds);
		policy->pending_activation_checks = g_slist_append (policy->pending_activation_checks, data);
	}
}

static gboolean
reset_connections_retries (gpointer user_data)
{
	NMPolicy *policy = (NMPolicy *) user_data;
	GSList *connections, *iter;
	time_t con_stamp, min_stamp, now;
	gboolean changed = FALSE;

	policy->reset_retries_id = 0;

	min_stamp = now = time (NULL);
	connections = nm_settings_get_connections (policy->settings);
	for (iter = connections; iter; iter = g_slist_next (iter)) {
		con_stamp = GPOINTER_TO_SIZE (g_object_get_data (G_OBJECT (iter->data), RESET_RETRIES_TIMESTAMP_TAG));
		if (con_stamp == 0)
			continue;
		if (con_stamp + RESET_RETRIES_TIMER <= now) {
			set_connection_auto_retries (NM_CONNECTION (iter->data), RETRIES_DEFAULT);
			g_object_set_data (G_OBJECT (iter->data), RESET_RETRIES_TIMESTAMP_TAG, GSIZE_TO_POINTER (0));
			changed = TRUE;
			continue;
		}
		if (con_stamp < min_stamp)
			min_stamp = con_stamp;
	}
	g_slist_free (connections);

	/* Schedule the handler again if there are some stamps left */
	if (min_stamp != now)
		policy->reset_retries_id = g_timeout_add_seconds (RESET_RETRIES_TIMER - (now - min_stamp), reset_connections_retries, policy);

	/* If anything changed, try to activate the newly re-enabled connections */
	if (changed)
		schedule_activate_all (policy);

	return FALSE;
}

static NMConnection *
get_device_connection (NMDevice *device)
{
	NMActRequest *req = NULL;

	req = nm_device_get_act_request (device);
	return req ? nm_act_request_get_connection (req) : NULL;
}

static void schedule_activate_all (NMPolicy *policy);

static void
activate_slave_connections (NMPolicy *policy, NMConnection *connection,
                            NMDevice *device)
{
	const char *master_device;
	GSList *connections, *iter;

	master_device = nm_device_get_iface (device);
	g_assert (master_device);

	connections = nm_settings_get_connections (policy->settings);
	for (iter = connections; iter; iter = g_slist_next (iter)) {
		NMConnection *slave;
		NMSettingConnection *s_slave_con;

		slave = NM_CONNECTION (iter->data);
		g_assert (slave);

		s_slave_con = nm_connection_get_setting_connection (slave);
		g_assert (s_slave_con);

		if (!g_strcmp0 (nm_setting_connection_get_master (s_slave_con), master_device))
			set_connection_auto_retries (slave, RETRIES_DEFAULT);
	}

	g_slist_free (connections);

	schedule_activate_all (policy);
}

static void
device_state_changed (NMDevice *device,
                      NMDeviceState new_state,
                      NMDeviceState old_state,
                      NMDeviceStateReason reason,
                      gpointer user_data)
{
	NMPolicy *policy = (NMPolicy *) user_data;
	NMConnection *connection = get_device_connection (device);

	if (connection)
		g_object_set_data (G_OBJECT (connection), FAILURE_REASON_TAG, GUINT_TO_POINTER (0));

	switch (new_state) {
	case NM_DEVICE_STATE_FAILED:
		/* Mark the connection invalid if it failed during activation so that
		 * it doesn't get automatically chosen over and over and over again.
		 */
		if (   connection
		    && old_state >= NM_DEVICE_STATE_PREPARE
		    && old_state <= NM_DEVICE_STATE_ACTIVATED) {
			guint32 tries = get_connection_auto_retries (connection);

			if (reason == NM_DEVICE_STATE_REASON_NO_SECRETS) {
				/* If the connection couldn't get the secrets it needed (ex because
				 * the user canceled, or no secrets exist), there's no point in
				 * automatically retrying because it's just going to fail anyway.
				 */
				set_connection_auto_retries (connection, 0);

				/* Mark the connection as failed due to missing secrets so that we can reset
				 * RETRIES_TAG and automatically re-try when an secret agent registers.
				 */
				g_object_set_data (G_OBJECT (connection), FAILURE_REASON_TAG, GUINT_TO_POINTER (NM_DEVICE_STATE_REASON_NO_SECRETS));
			} else if (tries > 0) {
				/* Otherwise if it's a random failure, just decrease the number
				 * of automatic retries so that the connection gets tried again
				 * if it still has a retry count.
				 */
				set_connection_auto_retries (connection, tries - 1);
			}

			if (get_connection_auto_retries (connection) == 0) {
				nm_log_info (LOGD_DEVICE, "Marking connection '%s' invalid.", nm_connection_get_id (connection));
				/* Schedule a handler to reset retries count */
				g_object_set_data (G_OBJECT (connection), RESET_RETRIES_TIMESTAMP_TAG, GSIZE_TO_POINTER ((gsize) time (NULL)));
				if (!policy->reset_retries_id)
					policy->reset_retries_id = g_timeout_add_seconds (RESET_RETRIES_TIMER, reset_connections_retries, policy);
			}
			nm_connection_clear_secrets (connection);
		}
		schedule_activate_check (policy, device, 3);
		break;
	case NM_DEVICE_STATE_ACTIVATED:
		if (connection) {
			/* Reset auto retries back to default since connection was successful */
			set_connection_auto_retries (connection, RETRIES_DEFAULT);

			/* And clear secrets so they will always be requested from the
			 * settings service when the next connection is made.
			 */
			nm_connection_clear_secrets (connection);
		}

		update_routing_and_dns (policy, FALSE);
		break;
	case NM_DEVICE_STATE_UNMANAGED:
		if (   old_state == NM_DEVICE_STATE_UNAVAILABLE
		    || old_state == NM_DEVICE_STATE_DISCONNECTED) {
			/* If the device was never activated, there's no point in
			 * updating routing or DNS.  This allows us to keep the previous
			 * resolv.conf or routes from before NM started if no device was
			 * ever managed by NM.
			 */
			break;
		}
	case NM_DEVICE_STATE_UNAVAILABLE:
		update_routing_and_dns (policy, FALSE);
		break;
	case NM_DEVICE_STATE_DISCONNECTED:
		/* Reset RETRIES_TAG when carrier on. If cable was unplugged
		 * and plugged again, we should try to reconnect */
		if (reason == NM_DEVICE_STATE_REASON_CARRIER && old_state == NM_DEVICE_STATE_UNAVAILABLE)
			reset_retries_all (policy->settings, device);

		/* Device is now available for auto-activation */
		update_routing_and_dns (policy, FALSE);
		schedule_activate_check (policy, device, 0);
		break;

	case NM_DEVICE_STATE_PREPARE:
		/* Reset auto-connect retries of all slaves and schedule them for
		 * activation. */
		activate_slave_connections (policy, connection, device);
		break;

	default:
		break;
	}
}

static void
device_ip_config_changed (NMDevice *device,
                          GParamSpec *pspec,
                          gpointer user_data)
{
	update_routing_and_dns ((NMPolicy *) user_data, TRUE);
}

static void
wireless_networks_changed (NMDevice *device, GObject *ap, gpointer user_data)
{
	schedule_activate_check ((NMPolicy *) user_data, device, 0);
}

static void
nsps_changed (NMDevice *device, GObject *nsp, gpointer user_data)
{
	schedule_activate_check ((NMPolicy *) user_data, device, 0);
}

static void
modem_enabled_changed (NMDevice *device, gpointer user_data)
{
	schedule_activate_check ((NMPolicy *) (user_data), device, 0);
}

typedef struct {
	gulong id;
	NMDevice *device;
} DeviceSignalId;

static void
_connect_device_signal (NMPolicy *policy, NMDevice *device, const char *name, gpointer callback)
{
	DeviceSignalId *data;

	data = g_slice_new0 (DeviceSignalId);
	g_assert (data);
	data->id = g_signal_connect (device, name, callback, policy);
	data->device = device;
	policy->dev_ids = g_slist_prepend (policy->dev_ids, data);
}

static void
device_added (NMManager *manager, NMDevice *device, gpointer user_data)
{
	NMPolicy *policy = (NMPolicy *) user_data;

	_connect_device_signal (policy, device, "state-changed", device_state_changed);
	_connect_device_signal (policy, device, "notify::" NM_DEVICE_IP4_CONFIG, device_ip_config_changed);
	_connect_device_signal (policy, device, "notify::" NM_DEVICE_IP6_CONFIG, device_ip_config_changed);

	switch (nm_device_get_device_type (device)) {
	case NM_DEVICE_TYPE_WIFI:
		_connect_device_signal (policy, device, "access-point-added", wireless_networks_changed);
		_connect_device_signal (policy, device, "access-point-removed", wireless_networks_changed);
		break;
	case NM_DEVICE_TYPE_WIMAX:
		_connect_device_signal (policy, device, "nsp-added", nsps_changed);
		_connect_device_signal (policy, device, "nsp-removed", nsps_changed);
		break;
	case NM_DEVICE_TYPE_MODEM:
		_connect_device_signal (policy, device, "enable-changed", modem_enabled_changed);
		break;
	default:
		break;
	}
}

static void
device_removed (NMManager *manager, NMDevice *device, gpointer user_data)
{
	NMPolicy *policy = (NMPolicy *) user_data;
	ActivateData *tmp;
	GSList *iter;

	/* Clear any idle callbacks for this device */
	tmp = find_pending_activation (policy->pending_activation_checks, device);
	if (tmp) {
		policy->pending_activation_checks = g_slist_remove (policy->pending_activation_checks, tmp);
		activate_data_free (tmp);
	}

	/* Clear any signal handlers for this device */
	iter = policy->dev_ids;
	while (iter) {
		DeviceSignalId *data = iter->data;
		GSList *next = g_slist_next (iter);

		if (data->device == device) {
			g_signal_handler_disconnect (data->device, data->id);
			g_slice_free (DeviceSignalId, data);
			policy->dev_ids = g_slist_delete_link (policy->dev_ids, iter);
		}
		iter = next;
	}

	update_routing_and_dns (policy, FALSE);
}

static void
schedule_activate_all (NMPolicy *policy)
{
	GSList *iter, *devices;

	devices = nm_manager_get_devices (policy->manager);
	for (iter = devices; iter; iter = g_slist_next (iter))
		schedule_activate_check (policy, NM_DEVICE (iter->data), 0);
}

static void
connection_added (NMSettings *settings,
                  NMConnection *connection,
                  gpointer user_data)
{
	set_connection_auto_retries (connection, RETRIES_DEFAULT);
	schedule_activate_all ((NMPolicy *) user_data);
}

static void
connections_loaded (NMSettings *settings, gpointer user_data)
{
	// FIXME: "connections-loaded" signal is emmitted *before* we connect to it
	// in nm_policy_new(). So this function is never called. Currently we work around
	// that by calling reset_retries_all() in nm_policy_new()
	
	/* Initialize connections' auto-retries */
	reset_retries_all (settings, NULL);

	schedule_activate_all ((NMPolicy *) user_data);
}

static void
add_to_zone_cb (GError *error,
                gpointer user_data1,
                gpointer user_data2)
{
	NMDevice *device = NM_DEVICE (user_data1);

	if (error) {
		/* FIXME: what do we do here? */
	}

	g_object_unref (device);
}

static void
inform_firewall_about_zone (NMPolicy *policy, NMConnection *connection)
{
	NMSettingConnection *s_con = nm_connection_get_setting_connection (connection);
	GSList *iter, *devices;

	devices = nm_manager_get_devices (policy->manager);
	for (iter = devices; iter; iter = g_slist_next (iter)) {
		NMDevice *dev = NM_DEVICE (iter->data);

		if (   (get_device_connection (dev) == connection)
		    && (nm_device_get_state (dev) == NM_DEVICE_STATE_ACTIVATED)) {
			nm_firewall_manager_add_to_zone (policy->fw_manager,
			                                 nm_device_get_ip_iface (dev),
			                                 nm_setting_connection_get_zone (s_con),
			                                 add_to_zone_cb,
			                                 g_object_ref (dev),
			                                 NULL);
		}
	}
}

static void
connection_updated (NMSettings *settings,
                    NMConnection *connection,
                    gpointer user_data)
{
	NMPolicy *policy = (NMPolicy *) user_data;

	inform_firewall_about_zone (policy, connection);

	/* Reset auto retries back to default since connection was updated */
	set_connection_auto_retries (connection, RETRIES_DEFAULT);

	schedule_activate_all (policy);
}

static void
_deactivate_if_active (NMManager *manager, NMConnection *connection)
{
	GPtrArray *list;
	int i;

	list = nm_manager_get_active_connections_by_connection (manager, connection);
	if (!list)
		return;

	for (i = 0; i < list->len; i++) {
		char *path = g_ptr_array_index (list, i);
		GError *error = NULL;

		if (!nm_manager_deactivate_connection (manager, path, NM_DEVICE_STATE_REASON_CONNECTION_REMOVED, &error)) {
			nm_log_warn (LOGD_DEVICE, "Connection '%s' disappeared, but error deactivating it: (%d) %s",
			             nm_connection_get_id (connection), error->code, error->message);
			g_error_free (error);
		}
		g_free (path);
	}
	g_ptr_array_free (list, TRUE);
}

static void
connection_removed (NMSettings *settings,
                    NMConnection *connection,
                    gpointer user_data)
{
	NMPolicy *policy = user_data;

	_deactivate_if_active (policy->manager, connection);
}

static void
connection_visibility_changed (NMSettings *settings,
                               NMSettingsConnection *connection,
                               gpointer user_data)
{
	NMPolicy *policy = user_data;

	if (nm_settings_connection_is_visible (connection))
		schedule_activate_all (policy);
	else
		_deactivate_if_active (policy->manager, NM_CONNECTION (connection));
}

static void
secret_agent_registered (NMSettings *settings,
                         NMSecretAgent *agent,
                         gpointer user_data)
{
	/* The registered secret agent may provide some missing secrets. Thus we
	 * reset retries count here and schedule activation, so that the
	 * connections failed due to missing secrets may re-try auto-connection.
	 */
	reset_retries_for_failed_secrets (settings);
	schedule_activate_all ((NMPolicy *) user_data);
}

static void
_connect_manager_signal (NMPolicy *policy, const char *name, gpointer callback)
{
	guint id;

	id = g_signal_connect (policy->manager, name, callback, policy);
	policy->manager_ids = g_slist_prepend (policy->manager_ids, GUINT_TO_POINTER (id));
}

static void
_connect_settings_signal (NMPolicy *policy, const char *name, gpointer callback)
{
	guint id;

	id = g_signal_connect (policy->settings, name, callback, policy);
	policy->settings_ids = g_slist_prepend (policy->settings_ids, GUINT_TO_POINTER (id));
}

NMPolicy *
nm_policy_new (NMManager *manager,
               NMVPNManager *vpn_manager,
               NMSettings *settings)
{
	NMPolicy *policy;
	static gboolean initialized = FALSE;
	gulong id;
	char hostname[HOST_NAME_MAX + 2];

	g_return_val_if_fail (NM_IS_MANAGER (manager), NULL);
	g_return_val_if_fail (initialized == FALSE, NULL);

	policy = g_malloc0 (sizeof (NMPolicy));
	policy->manager = g_object_ref (manager);
	policy->settings = g_object_ref (settings);
	policy->update_state_id = 0;

	/* Grab hostname on startup and use that if nothing provides one */
	memset (hostname, 0, sizeof (hostname));
	if (gethostname (&hostname[0], HOST_NAME_MAX) == 0) {
		/* only cache it if it's a valid hostname */
		if (   strlen (hostname)
		    && strcmp (hostname, "localhost")
		    && strcmp (hostname, "localhost.localdomain")
		    && strcmp (hostname, "(none)"))
			policy->orig_hostname = g_strdup (hostname);
	}

	policy->vpn_manager = g_object_ref (vpn_manager);
	id = g_signal_connect (policy->vpn_manager, "connection-activated",
	                       G_CALLBACK (vpn_connection_activated), policy);
	policy->vpn_activated_id = id;
	id = g_signal_connect (policy->vpn_manager, "connection-deactivated",
	                       G_CALLBACK (vpn_connection_deactivated), policy);
	policy->vpn_deactivated_id = id;

	policy->fw_manager = nm_firewall_manager_get();

	_connect_manager_signal (policy, "state-changed", global_state_changed);
	_connect_manager_signal (policy, "notify::" NM_MANAGER_HOSTNAME, hostname_changed);
	_connect_manager_signal (policy, "notify::" NM_MANAGER_SLEEPING, sleeping_changed);
	_connect_manager_signal (policy, "notify::" NM_MANAGER_NETWORKING_ENABLED, sleeping_changed);
	_connect_manager_signal (policy, "device-added", device_added);
	_connect_manager_signal (policy, "device-removed", device_removed);

	_connect_settings_signal (policy, NM_SETTINGS_SIGNAL_CONNECTIONS_LOADED, connections_loaded);
	_connect_settings_signal (policy, NM_SETTINGS_SIGNAL_CONNECTION_ADDED, connection_added);
	_connect_settings_signal (policy, NM_SETTINGS_SIGNAL_CONNECTION_UPDATED, connection_updated);
	_connect_settings_signal (policy, NM_SETTINGS_SIGNAL_CONNECTION_REMOVED, connection_removed);
	_connect_settings_signal (policy, NM_SETTINGS_SIGNAL_CONNECTION_VISIBILITY_CHANGED,
	                          connection_visibility_changed);
	_connect_settings_signal (policy, NM_SETTINGS_SIGNAL_AGENT_REGISTERED, secret_agent_registered);

	/* Initialize connections' auto-retries */
	reset_retries_all (policy->settings, NULL);

	initialized = TRUE;
	return policy;
}

void
nm_policy_destroy (NMPolicy *policy)
{
	GSList *iter;

	g_return_if_fail (policy != NULL);

	/* Tell any existing hostname lookup thread to die, it'll get cleaned up
	 * by the lookup thread callback.
	  */
	if (policy->lookup) {
		hostname_thread_kill (policy->lookup);
		policy->lookup = NULL;
	}

	g_slist_foreach (policy->pending_activation_checks, (GFunc) activate_data_free, NULL);
	g_slist_free (policy->pending_activation_checks);

	g_signal_handler_disconnect (policy->vpn_manager, policy->vpn_activated_id);
	g_signal_handler_disconnect (policy->vpn_manager, policy->vpn_deactivated_id);
	g_object_unref (policy->vpn_manager);

	g_object_unref (policy->fw_manager);

	for (iter = policy->manager_ids; iter; iter = g_slist_next (iter))
		g_signal_handler_disconnect (policy->manager, GPOINTER_TO_UINT (iter->data));
	g_slist_free (policy->manager_ids);

	for (iter = policy->settings_ids; iter; iter = g_slist_next (iter))
		g_signal_handler_disconnect (policy->settings, GPOINTER_TO_UINT (iter->data));
	g_slist_free (policy->settings_ids);

	for (iter = policy->dev_ids; iter; iter = g_slist_next (iter)) {
		DeviceSignalId *data = iter->data;

		g_signal_handler_disconnect (data->device, data->id);
		g_slice_free (DeviceSignalId, data);
	}
	g_slist_free (policy->dev_ids);

	if (policy->reset_retries_id)
		g_source_remove (policy->reset_retries_id);

	g_free (policy->orig_hostname);
	g_free (policy->cur_hostname);

	g_object_unref (policy->settings);
	g_object_unref (policy->manager);
	g_free (policy);
}

