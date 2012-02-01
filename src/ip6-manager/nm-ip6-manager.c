/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-ip6-manager.c - Handle IPv6 address configuration for NetworkManager
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
 * Copyright (C) 2009 - 2010 Red Hat, Inc.
 */

#include <errno.h>
#include <netinet/icmp6.h>

#include <netlink/route/addr.h>
#include <netlink/route/rtnl.h>
#include <netlink/route/route.h>

#include "nm-ip6-manager.h"
#include "nm-netlink-monitor.h"
#include "nm-netlink-utils.h"
#include "nm-netlink-compat.h"
#include "NetworkManagerUtils.h"
#include "nm-marshal.h"
#include "nm-logging.h"

/* Pre-DHCP addrconf timeout, in seconds */
#define NM_IP6_TIMEOUT 20

/* FIXME? Stolen from the kernel sources */
#define IF_RA_OTHERCONF 0x80
#define IF_RA_MANAGED   0x40
#define IF_RA_RCVD      0x20
#define IF_RS_SENT      0x10

typedef struct {
	NMNetlinkMonitor *monitor;
	GHashTable *devices;

	struct nl_sock *nlh;
	struct nl_cache *addr_cache, *route_cache;

	guint netlink_id;
} NMIP6ManagerPrivate;

#define NM_IP6_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_IP6_MANAGER, NMIP6ManagerPrivate))

G_DEFINE_TYPE (NMIP6Manager, nm_ip6_manager, G_TYPE_OBJECT)

enum {
	ADDRCONF_COMPLETE,
	CONFIG_CHANGED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

typedef enum {
	NM_IP6_DEVICE_UNCONFIGURED,
	NM_IP6_DEVICE_GOT_LINK_LOCAL,
	NM_IP6_DEVICE_GOT_ROUTER_ADVERTISEMENT,
	NM_IP6_DEVICE_GOT_ADDRESS,
	NM_IP6_DEVICE_TIMED_OUT
} NMIP6DeviceState;

typedef struct {
	struct in6_addr addr;
	time_t expires;
} NMIP6RDNSS;

typedef struct {
	char domain[256];
	time_t expires;
} NMIP6DNSSL;

/******************************************************************/

typedef struct {
	NMIP6Manager *manager;
	char *iface;
	int ifindex;

	char *disable_ip6_path;
	gboolean disable_ip6_save_valid;
	guint32 disable_ip6_save;

	guint finish_addrconf_id;
	guint config_changed_id;

	NMIP6DeviceState state;
	NMIP6DeviceState target_state;
	gboolean addrconf_complete;

	GArray *rdnss_servers;
	guint rdnss_timeout_id;

	GArray *dnssl_domains;
	guint dnssl_timeout_id;

	guint ip6flags_poll_id;

	guint32 ra_flags;
} NMIP6Device;

static void
clear_config_changed (NMIP6Device *device)
{
	if (device->config_changed_id)
		g_source_remove (device->config_changed_id);
	device->config_changed_id = 0;
}

static void
nm_ip6_device_destroy (NMIP6Device *device)
{
	g_return_if_fail (device != NULL);

	/* reset the saved IPv6 value */
	if (device->disable_ip6_save_valid) {
		nm_utils_do_sysctl (device->disable_ip6_path,
		                    device->disable_ip6_save ? "1\n" : "0\n");
	}

	if (device->finish_addrconf_id)
		g_source_remove (device->finish_addrconf_id);

	clear_config_changed (device);

	g_free (device->iface);
	if (device->rdnss_servers)
		g_array_free (device->rdnss_servers, TRUE);
	if (device->rdnss_timeout_id)
		g_source_remove (device->rdnss_timeout_id);
	if (device->dnssl_domains)
		g_array_free (device->dnssl_domains, TRUE);
	if (device->dnssl_timeout_id)
		g_source_remove (device->dnssl_timeout_id);
	if (device->ip6flags_poll_id)
		g_source_remove (device->ip6flags_poll_id);

	g_slice_free (NMIP6Device, device);
}

static NMIP6Device *
nm_ip6_device_new (NMIP6Manager *manager, int ifindex)
{
	NMIP6ManagerPrivate *priv = NM_IP6_MANAGER_GET_PRIVATE (manager);
	NMIP6Device *device;

	g_return_val_if_fail (ifindex > 0, NULL);

	device = g_slice_new0 (NMIP6Device);
	if (!device) {
		nm_log_err (LOGD_IP6, "(%d): out of memory creating IP6 addrconf object.",
		            ifindex);
		return NULL;
	}

	device->ifindex = ifindex;
	device->iface = nm_netlink_index_to_iface (ifindex);
	if (!device->iface) {
		nm_log_err (LOGD_IP6, "(%d): could not find interface name from index.",
		            ifindex);
		goto error;
	}

	device->manager = manager;

	device->rdnss_servers = g_array_new (FALSE, FALSE, sizeof (NMIP6RDNSS));

	device->dnssl_domains = g_array_new (FALSE, FALSE, sizeof (NMIP6DNSSL));

	g_hash_table_replace (priv->devices, GINT_TO_POINTER (device->ifindex), device);

	/* and the original value of IPv6 enable/disable */
	device->disable_ip6_path = g_strdup_printf ("/proc/sys/net/ipv6/conf/%s/disable_ipv6",
	                                            device->iface);
	g_assert (device->disable_ip6_path);
	device->disable_ip6_save_valid = nm_utils_get_proc_sys_net_value (device->disable_ip6_path,
	                                                                  device->iface,
	                                                                  &device->disable_ip6_save);

	return device;

error:
	nm_ip6_device_destroy (device);
	return NULL;
}

static NMIP6Device *
nm_ip6_manager_get_device (NMIP6Manager *manager, int ifindex)
{
	NMIP6ManagerPrivate *priv;

	g_return_val_if_fail (manager != NULL, NULL);
	g_return_val_if_fail (NM_IS_IP6_MANAGER (manager), NULL);

	priv = NM_IP6_MANAGER_GET_PRIVATE (manager);
	return g_hash_table_lookup (priv->devices, GINT_TO_POINTER (ifindex));
}

/******************************************************************/

typedef struct {
	NMIP6Device *device;
	guint dhcp_opts;
	gboolean success;
} CallbackInfo;

static gboolean
finish_addrconf (gpointer user_data)
{
	CallbackInfo *info = user_data;
	NMIP6Device *device = info->device;
	NMIP6Manager *manager = device->manager;
	int ifindex;

	device->finish_addrconf_id = 0;
	device->addrconf_complete = TRUE;
	ifindex = device->ifindex;

	/* We're done, stop polling IPv6 flags */
	if (device->ip6flags_poll_id) {
		g_source_remove (device->ip6flags_poll_id);
		device->ip6flags_poll_id = 0;
	}

	/* And tell listeners that addrconf is complete */
	if (info->success) {
		g_signal_emit (manager, signals[ADDRCONF_COMPLETE], 0,
		               ifindex, info->dhcp_opts, TRUE);
	} else {
		nm_log_info (LOGD_IP6, "(%s): IP6 addrconf timed out or failed.",
		             device->iface);

		nm_ip6_manager_cancel_addrconf (manager, ifindex);
		g_signal_emit (manager, signals[ADDRCONF_COMPLETE], 0,
		               ifindex, info->dhcp_opts, FALSE);
	}

	return FALSE;
}

static gboolean
emit_config_changed (gpointer user_data)
{
	CallbackInfo *info = user_data;
	NMIP6Device *device = info->device;
	NMIP6Manager *manager = device->manager;

	device->config_changed_id = 0;
	g_signal_emit (manager, signals[CONFIG_CHANGED], 0,
	               device->ifindex,
	               info->dhcp_opts,
	               info->success);
	return FALSE;
}

static void set_rdnss_timeout (NMIP6Device *device);

static gboolean
rdnss_expired (gpointer user_data)
{
	NMIP6Device *device = user_data;
	CallbackInfo info = { device, IP6_DHCP_OPT_NONE, FALSE };

	nm_log_dbg (LOGD_IP6, "(%s): IPv6 RDNSS information expired", device->iface);

	set_rdnss_timeout (device);
	clear_config_changed (device);
	emit_config_changed (&info);
	return FALSE;
}

static void
set_rdnss_timeout (NMIP6Device *device)
{
	time_t expires = 0, now = time (NULL);
	NMIP6RDNSS *rdnss;
	int i;

	if (device->rdnss_timeout_id) {
		g_source_remove (device->rdnss_timeout_id);
		device->rdnss_timeout_id = 0;
	}

	/* Find the soonest expiration time. */
	for (i = 0; i < device->rdnss_servers->len; i++) {
		rdnss = &g_array_index (device->rdnss_servers, NMIP6RDNSS, i);
		if (rdnss->expires == 0)
			continue;

		/* If the entry has already expired, remove it; the "+ 1" is
		 * because g_timeout_add_seconds() might fudge the timing a
		 * bit.
		 */
		if (rdnss->expires <= now + 1) {
			char buf[INET6_ADDRSTRLEN + 1];

			if (inet_ntop (AF_INET6, &(rdnss->addr), buf, sizeof (buf)) > 0) {
				nm_log_dbg (LOGD_IP6, "(%s): removing expired RA-provided nameserver %s",
				            device->iface, buf);
			}
			g_array_remove_index (device->rdnss_servers, i--);
			continue;
		}

		if (!expires || rdnss->expires < expires)
			expires = rdnss->expires;
	}

	if (expires) {
		device->rdnss_timeout_id = g_timeout_add_seconds (MIN (expires - now, G_MAXUINT32 - 1),
		                                                  rdnss_expired,
		                                                  device);
	}
}

static void set_dnssl_timeout (NMIP6Device *device);

static gboolean
dnssl_expired (gpointer user_data)
{
	NMIP6Device *device = user_data;
	CallbackInfo info = { device, IP6_DHCP_OPT_NONE, FALSE };

	nm_log_dbg (LOGD_IP6, "(%s): IPv6 DNSSL information expired", device->iface);

	set_dnssl_timeout (device);
	clear_config_changed (device);
	emit_config_changed (&info);
	return FALSE;
}

static void
set_dnssl_timeout (NMIP6Device *device)
{
	time_t expires = 0, now = time (NULL);
	NMIP6DNSSL *dnssl;
	int i;

	if (device->dnssl_timeout_id) {
		g_source_remove (device->dnssl_timeout_id);
		device->dnssl_timeout_id = 0;
	}

	/* Find the soonest expiration time. */
	for (i = 0; i < device->dnssl_domains->len; i++) {
		dnssl = &g_array_index (device->dnssl_domains, NMIP6DNSSL, i);
		if (dnssl->expires == 0)
			continue;

		/* If the entry has already expired, remove it; the "+ 1" is
		 * because g_timeout_add_seconds() might fudge the timing a
		 * bit.
		 */
		if (dnssl->expires <= now + 1) {
			nm_log_dbg (LOGD_IP6, "(%s): removing expired RA-provided domain %s",
			            device->iface, dnssl->domain);
			g_array_remove_index (device->dnssl_domains, i--);
			continue;
		}

		if (!expires || dnssl->expires < expires)
			expires = dnssl->expires;
	}

	if (expires) {
		device->dnssl_timeout_id = g_timeout_add_seconds (MIN (expires - now, G_MAXUINT32 - 1),
		                                                  dnssl_expired,
		                                                  device);
	}
}

static CallbackInfo *
callback_info_new (NMIP6Device *device, guint dhcp_opts, gboolean success)
{
	CallbackInfo *info;

	info = g_malloc0 (sizeof (CallbackInfo));
	info->device = device;
	info->dhcp_opts = dhcp_opts;
	info->success = success;
	return info;
}

static const char *
state_to_string (NMIP6DeviceState state)
{
	switch (state) {
	case NM_IP6_DEVICE_UNCONFIGURED:
		return "unconfigured";
	case NM_IP6_DEVICE_GOT_LINK_LOCAL:
		return "got-link-local";
	case NM_IP6_DEVICE_GOT_ROUTER_ADVERTISEMENT:
		return "got-ra";
	case NM_IP6_DEVICE_GOT_ADDRESS:
		return "got-address";
	case NM_IP6_DEVICE_TIMED_OUT:
		return "timed-out";
	default:
		return "unknown";
	}
}

static void
nm_ip6_device_sync_from_netlink (NMIP6Device *device, gboolean config_changed)
{
	NMIP6Manager *manager = device->manager;
	NMIP6ManagerPrivate *priv = NM_IP6_MANAGER_GET_PRIVATE (manager);
	struct rtnl_addr *rtnladdr;
	struct nl_addr *nladdr;
	struct in6_addr *addr;
	CallbackInfo *info;
	guint dhcp_opts = IP6_DHCP_OPT_NONE;
	gboolean found_linklocal = FALSE, found_other = FALSE;

	nm_log_dbg (LOGD_IP6, "(%s): syncing with netlink (ra_flags 0x%X) (state/target '%s'/'%s')",
	            device->iface, device->ra_flags,
	            state_to_string (device->state),
	            state_to_string (device->target_state));

	/* Look for any IPv6 addresses the kernel may have set for the device */
	for (rtnladdr = (struct rtnl_addr *) nl_cache_get_first (priv->addr_cache);
		 rtnladdr;
		 rtnladdr = (struct rtnl_addr *) nl_cache_get_next ((struct nl_object *) rtnladdr)) {
		char buf[INET6_ADDRSTRLEN];

		if (rtnl_addr_get_ifindex (rtnladdr) != device->ifindex)
			continue;

		nladdr = rtnl_addr_get_local (rtnladdr);
		if (!nladdr || nl_addr_get_family (nladdr) != AF_INET6)
			continue;

		addr = nl_addr_get_binary_addr (nladdr);

		if (inet_ntop (AF_INET6, addr, buf, INET6_ADDRSTRLEN) > 0) {
			nm_log_dbg (LOGD_IP6, "(%s): netlink address: %s",
			            device->iface, buf);
		}

		if (IN6_IS_ADDR_LINKLOCAL (addr)) {
			if (device->state == NM_IP6_DEVICE_UNCONFIGURED)
				device->state = NM_IP6_DEVICE_GOT_LINK_LOCAL;
			found_linklocal = TRUE;
		} else {
			if (device->state < NM_IP6_DEVICE_GOT_ADDRESS)
				device->state = NM_IP6_DEVICE_GOT_ADDRESS;
			found_other = TRUE;
		}
	}

	/* There might be a LL address hanging around on the interface from
	 * before in the initial run, but if it goes away later, make sure we
	 * regress from GOT_LINK_LOCAL back to UNCONFIGURED.
	 */
	if ((device->state == NM_IP6_DEVICE_GOT_LINK_LOCAL) && !found_linklocal)
		device->state = NM_IP6_DEVICE_UNCONFIGURED;

	nm_log_dbg (LOGD_IP6, "(%s): addresses synced (state %s)",
	            device->iface, state_to_string (device->state));

	/* We only care about router advertisements if we want a real IPv6 address */
	if (   (device->target_state == NM_IP6_DEVICE_GOT_ADDRESS)
	    && (device->ra_flags & IF_RA_RCVD)) {

		if (device->state < NM_IP6_DEVICE_GOT_ROUTER_ADVERTISEMENT)
			device->state = NM_IP6_DEVICE_GOT_ROUTER_ADVERTISEMENT;

		if (device->ra_flags & IF_RA_MANAGED) {
			dhcp_opts = IP6_DHCP_OPT_MANAGED;
			nm_log_dbg (LOGD_IP6, "router advertisement deferred to DHCPv6");
		} else if (device->ra_flags & IF_RA_OTHERCONF) {
			dhcp_opts = IP6_DHCP_OPT_OTHERCONF;
			nm_log_dbg (LOGD_IP6, "router advertisement requests parallel DHCPv6");
		}
	}

	if (!device->addrconf_complete) {
		/* Managed mode (ie DHCP only) short-circuits automatic addrconf, so
		 * we don't bother waiting for the device's target state to be reached
		 * when the RA requests managed mode.
		 */
		if (   (device->state >= device->target_state)
		    || (dhcp_opts == IP6_DHCP_OPT_MANAGED)) {
			/* device->finish_addrconf_id may currently be a timeout
			 * rather than an idle, so we remove the existing source.
			 */
			if (device->finish_addrconf_id)
				g_source_remove (device->finish_addrconf_id);

			nm_log_dbg (LOGD_IP6, "(%s): reached target state or Managed-mode requested (state '%s') (dhcp opts 0x%X)",
			            device->iface, state_to_string (device->state),
			            dhcp_opts);

			info = callback_info_new (device, dhcp_opts, TRUE);
			device->finish_addrconf_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
			                                              finish_addrconf,
			                                              info,
			                                              (GDestroyNotify) g_free);
		}
	} else if (config_changed) {
		if (!device->config_changed_id) {
			gboolean success = TRUE;

			/* If for some reason an RA-provided address disappeared, we need
			 * to make sure we fail the connection as it's no longer valid.
			 */
			if (   (device->state == NM_IP6_DEVICE_GOT_ADDRESS)
			    && (device->target_state == NM_IP6_DEVICE_GOT_ADDRESS)
			    && !found_other) {
				nm_log_dbg (LOGD_IP6, "(%s): RA-provided address no longer valid",
				            device->iface);
				success = FALSE;
			}

			info = callback_info_new (device, dhcp_opts, success);
			device->config_changed_id = g_idle_add_full (G_PRIORITY_DEFAULT_IDLE,
			                                             emit_config_changed,
			                                             info,
			                                             (GDestroyNotify) g_free);
		}
	}
}

static void
ref_object (struct nl_object *obj, void *data)
{
	struct nl_object **out = data;

	nl_object_get (obj);
	*out = obj;
}

static NMIP6Device *
process_addr (NMIP6Manager *manager, struct nl_msg *msg)
{
	NMIP6ManagerPrivate *priv = NM_IP6_MANAGER_GET_PRIVATE (manager);
	NMIP6Device *device;
	struct rtnl_addr *rtnladdr;
	int old_size;

	nm_log_dbg (LOGD_IP6, "processing netlink new/del address message");

	rtnladdr = NULL;
	nl_msg_parse (msg, ref_object, &rtnladdr);
	if (!rtnladdr) {
		nm_log_dbg (LOGD_IP6, "error processing netlink new/del address message");
		return NULL;
	}

	device = nm_ip6_manager_get_device (manager, rtnl_addr_get_ifindex (rtnladdr));
	if (!device) {
		nm_log_dbg (LOGD_IP6, "ignoring message for unknown device");
		rtnl_addr_put (rtnladdr);
		return NULL;
	}

	old_size = nl_cache_nitems (priv->addr_cache);
	nl_cache_include (priv->addr_cache, (struct nl_object *)rtnladdr, NULL, NULL);
	rtnl_addr_put (rtnladdr);

	/* The kernel will re-notify us of automatically-added addresses
	 * every time it gets another router advertisement. We only want
	 * to notify higher levels if we actually changed something.
	 */
	if (nl_cache_nitems (priv->addr_cache) == old_size) {
		nm_log_dbg (LOGD_IP6, "(%s): address cache unchanged, ignoring message",
		            device->iface);
		return NULL;
	}

	return device;
}

static NMIP6Device *
process_route (NMIP6Manager *manager, struct nl_msg *msg)
{
	NMIP6ManagerPrivate *priv = NM_IP6_MANAGER_GET_PRIVATE (manager);
	NMIP6Device *device;
	struct rtnl_route *rtnlroute;
	int old_size;

	nm_log_dbg (LOGD_IP6, "processing netlink new/del route message");

	rtnlroute = NULL;
	nl_msg_parse (msg, ref_object, &rtnlroute);
	if (!rtnlroute) {
		nm_log_dbg (LOGD_IP6, "error processing netlink new/del route message");
		return NULL;
	}

	device = nm_ip6_manager_get_device (manager, rtnl_route_get_oif (rtnlroute));
	if (!device) {
		nm_log_dbg (LOGD_IP6, "ignoring message for unknown device");
		rtnl_route_put (rtnlroute);
		return NULL;
	}

	old_size = nl_cache_nitems (priv->route_cache);
	nl_cache_include (priv->route_cache, (struct nl_object *)rtnlroute, NULL, NULL);
	rtnl_route_put (rtnlroute);

	/* As above in process_addr */
	if (nl_cache_nitems (priv->route_cache) == old_size) {
		nm_log_dbg (LOGD_IP6, "(%s): route cache unchanged, ignoring message",
		            device->iface);
		return NULL;
	}

	return device;
}

static NMIP6Device *
process_prefix (NMIP6Manager *manager, struct nl_msg *msg)
{
	struct prefixmsg *pmsg;
	NMIP6Device *device;

	/* We don't care about the prefix itself, but if we receive a
	 * router advertisement telling us to use DHCP, we might not
	 * get any RTM_NEWADDRs or RTM_NEWROUTEs, so this is our only
	 * way to notice immediately that an RA was received.
	 */

	nm_log_dbg (LOGD_IP6, "processing netlink new prefix message");

	if (!nlmsg_valid_hdr (nlmsg_hdr (msg), sizeof(*pmsg))) {
		nm_log_dbg (LOGD_IP6, "ignoring invalid prefix message");
		return NULL;
	}

	pmsg = (struct prefixmsg *) NLMSG_DATA (nlmsg_hdr (msg));
	device = nm_ip6_manager_get_device (manager, pmsg->prefix_ifindex);

	if (!device || device->addrconf_complete) {
		nm_log_dbg (LOGD_IP6, "(%s): ignoring unknown or completed device",
		            device ? device->iface : "(none)");
		return NULL;
	}

	return device;
}

/* RDNSS parsing code based on rdnssd, Copyright 2007 Pierre Ynard,
 * Rémi Denis-Courmont. GPLv2/3
 */

#define ND_OPT_RDNSS 25
#define ND_OPT_DNSSL 31

struct nd_opt_rdnss {
	uint8_t nd_opt_rdnss_type;
	uint8_t nd_opt_rdnss_len;
	uint16_t nd_opt_rdnss_reserved1;
	uint32_t nd_opt_rdnss_lifetime;
	/* followed by one or more IPv6 addresses */
} __attribute__ ((packed));

struct nd_opt_dnssl {
	uint8_t nd_opt_dnssl_type;
	uint8_t nd_opt_dnssl_len;
	uint16_t nd_opt_dnssl_reserved1;
	uint32_t nd_opt_dnssl_lifetime;
	/* followed by one or more suffixes */
} __attribute__ ((packed));

static gboolean
process_nduseropt_rdnss (NMIP6Device *device, struct nd_opt_hdr *opt)
{
	size_t opt_len;
	struct nd_opt_rdnss *rdnss_opt;
	time_t now = time (NULL);
	struct in6_addr *addr;
	GArray *new_servers;
	NMIP6RDNSS server, *cur_server;
	gboolean changed = FALSE;
	guint i;

	opt_len = opt->nd_opt_len;

	if (opt_len < 3 || (opt_len & 1) == 0)
		return FALSE;

	rdnss_opt = (struct nd_opt_rdnss *) opt;

	new_servers = g_array_new (FALSE, FALSE, sizeof (NMIP6RDNSS));

	/* Pad the DNS server expiry somewhat to give a bit of slack in cases
	 * where one RA gets lost or something (which can happen on unreliable
	 * links like WiFi where certain types of frames are not retransmitted).
	 * Note that 0 has special meaning and is therefore not adjusted.
	 */
	server.expires = ntohl (rdnss_opt->nd_opt_rdnss_lifetime);
	if (server.expires > 0)
		server.expires += now + 10;

	for (addr = (struct in6_addr *) (rdnss_opt + 1); opt_len >= 2; addr++, opt_len -= 2) {
		char buf[INET6_ADDRSTRLEN + 1];

		if (!inet_ntop (AF_INET6, addr, buf, sizeof (buf))) {
			nm_log_warn (LOGD_IP6, "(%s): received invalid RA-provided nameserver", device->iface);
			continue;
		}

		/* Update the cached timeout if we already saw this server */
		for (i = 0; i < device->rdnss_servers->len; i++) {
			cur_server = &(g_array_index (device->rdnss_servers, NMIP6RDNSS, i));

			if (!IN6_ARE_ADDR_EQUAL (addr, &cur_server->addr))
				continue;

			cur_server->expires = server.expires;

			if (server.expires > 0) {
				nm_log_dbg (LOGD_IP6, "(%s): refreshing RA-provided nameserver %s (expires in %ld seconds)",
				            device->iface, buf,
				            server.expires - now);
				break;
			}

			nm_log_dbg (LOGD_IP6, "(%s): removing RA-provided nameserver %s on router request",
			            device->iface, buf);

			g_array_remove_index (device->rdnss_servers, i);
			changed = TRUE;
			break;
		}

		if (server.expires == 0)
			continue;
		if (i < device->rdnss_servers->len)
			continue;

		nm_log_dbg (LOGD_IP6, "(%s): found RA-provided nameserver %s (expires in %ld seconds)",
		            device->iface, buf, server.expires - now);

		server.addr = *addr;
		g_array_append_val (new_servers, server);
	}

	/* New servers must be added in the order they are listed in the
	 * RA option and before any existing servers.
	 *
	 * Note: This is the place to remove servers if we want to cap the
	 *       number of resolvers. The RFC states that the one to expire
	 *       first of the existing servers should be removed.
	 */
	if (new_servers->len) {
		g_array_prepend_vals (device->rdnss_servers,
		                      new_servers->data, new_servers->len);
		changed = TRUE;
	}

	g_array_free (new_servers, TRUE);

	/* Timeouts may have changed even if IPs didn't */
	set_rdnss_timeout (device);

	return changed;
}

static const char *
parse_dnssl_domain (const unsigned char *buffer, size_t maxlen)
{
	static char domain[256];
	size_t label_len;

	domain[0] = '\0';

	while (maxlen > 0) {
		label_len = *buffer;
		buffer++;
		maxlen--;

		if (label_len == 0)
			return domain;

		if (label_len > maxlen)
			return NULL;
		if ((sizeof (domain) - strlen (domain)) < (label_len + 2))
			return NULL;

		if (domain[0] != '\0')
			strcat (domain, ".");
		strncat (domain, (const char *)buffer, label_len);
		buffer += label_len;
		maxlen -= label_len;
	}

	return NULL;
}

static gboolean
process_nduseropt_dnssl (NMIP6Device *device, struct nd_opt_hdr *opt)
{
	size_t opt_len;
	struct nd_opt_dnssl *dnssl_opt;
	unsigned char *opt_ptr;
	time_t now = time (NULL);
	GArray *new_domains;
	NMIP6DNSSL domain, *cur_domain;
	gboolean changed;
	guint i;

	opt_len = opt->nd_opt_len;

	if (opt_len < 2)
		return FALSE;

	dnssl_opt = (struct nd_opt_dnssl *) opt;

	opt_ptr = (unsigned char *)(dnssl_opt + 1);
	opt_len = (opt_len - 1) * 8; /* prefer bytes for later handling */

	new_domains = g_array_new (FALSE, FALSE, sizeof (NMIP6DNSSL));

	changed = FALSE;

	/* Pad the DNS server expiry somewhat to give a bit of slack in cases
	 * where one RA gets lost or something (which can happen on unreliable
	 * links like wifi where certain types of frames are not retransmitted).
	 * Note that 0 has special meaning and is therefore not adjusted.
	 */
	domain.expires = ntohl (dnssl_opt->nd_opt_dnssl_lifetime);
	if (domain.expires > 0)
		domain.expires += now + 10;

	while (opt_len) {
		const char *domain_str;

		domain_str = parse_dnssl_domain (opt_ptr, opt_len);
		if (domain_str == NULL) {
			nm_log_dbg (LOGD_IP6, "(%s): invalid DNSSL option, parsing aborted",
			            device->iface);
			break;
		}

		/* The DNSSL encoding of domains happen to occupy the same size
		 * as the length of the resulting string, including terminating
		 * null. */
		opt_ptr += strlen (domain_str) + 1;
		opt_len -= strlen (domain_str) + 1;

		/* Ignore empty domains. They're probably just padding... */
		if (domain_str[0] == '\0')
			continue;

		/* Update cached domain information if we've seen this domain before */
		for (i = 0; i < device->dnssl_domains->len; i++) {
			cur_domain = &(g_array_index (device->dnssl_domains, NMIP6DNSSL, i));

			if (strcmp (domain_str, cur_domain->domain) != 0)
				continue;

			cur_domain->expires = domain.expires;

			if (domain.expires > 0) {
				nm_log_dbg (LOGD_IP6, "(%s): refreshing RA-provided domain %s (expires in %ld seconds)",
				            device->iface, domain_str,
				            domain.expires - now);
				break;
			}

			nm_log_dbg (LOGD_IP6, "(%s): removing RA-provided domain %s on router request",
			            device->iface, domain_str);

			g_array_remove_index (device->dnssl_domains, i);
			changed = TRUE;
			break;
		}

		if (domain.expires == 0)
			continue;
		if (i < device->dnssl_domains->len)
			continue;

		nm_log_dbg (LOGD_IP6, "(%s): found RA-provided domain %s (expires in %ld seconds)",
		            device->iface, domain_str, domain.expires - now);

		g_assert (strlen (domain_str) < sizeof (domain.domain));
		strcpy (domain.domain, domain_str);
		g_array_append_val (new_domains, domain);
	}

	/* New domains must be added in the order they are listed in the
	 * RA option and before any existing domains.
	 *
	 * Note: This is the place to remove domains if we want to cap the
	 *       number of domains. The RFC states that the one to expire
	 *       first of the existing domains should be removed.
	 */
	if (new_domains->len) {
		g_array_prepend_vals (device->dnssl_domains,
		                      new_domains->data, new_domains->len);
		changed = TRUE;
	}

	g_array_free (new_domains, TRUE);

	/* Timeouts may have changed even if domains didn't */
	set_dnssl_timeout (device);

	return changed;
}

static NMIP6Device *
process_nduseropt (NMIP6Manager *manager, struct nl_msg *msg)
{
	NMIP6Device *device;
	struct nduseroptmsg *ndmsg;
	struct nd_opt_hdr *opt;
	guint opts_len;
	gboolean changed = FALSE;

	nm_log_dbg (LOGD_IP6, "processing netlink nduseropt message");

	ndmsg = (struct nduseroptmsg *) NLMSG_DATA (nlmsg_hdr (msg));

	if (!nlmsg_valid_hdr (nlmsg_hdr (msg), sizeof (*ndmsg)) ||
	    nlmsg_datalen (nlmsg_hdr (msg)) <
		(ndmsg->nduseropt_opts_len + sizeof (*ndmsg))) {
		nm_log_dbg (LOGD_IP6, "ignoring invalid nduseropt message");
		return NULL;
	}

	if (ndmsg->nduseropt_family != AF_INET6 ||
		ndmsg->nduseropt_icmp_type != ND_ROUTER_ADVERT ||
		ndmsg->nduseropt_icmp_code != 0) {
		nm_log_dbg (LOGD_IP6, "ignoring non-Router Advertisement message");
		return NULL;
	}

	device = nm_ip6_manager_get_device (manager, ndmsg->nduseropt_ifindex);
	if (!device) {
		nm_log_dbg (LOGD_IP6, "ignoring message for unknown device");
		return NULL;
	}

	opt = (struct nd_opt_hdr *) (ndmsg + 1);
	opts_len = ndmsg->nduseropt_opts_len;

	while (opts_len >= sizeof (struct nd_opt_hdr)) {
		size_t nd_opt_len = opt->nd_opt_len;

		if (nd_opt_len == 0 || opts_len < (nd_opt_len << 3))
			break;

		switch (opt->nd_opt_type) {
		case ND_OPT_RDNSS:
			changed = process_nduseropt_rdnss (device, opt);
			break;
		case ND_OPT_DNSSL:
			changed = process_nduseropt_dnssl (device, opt);
			break;
		}

		opts_len -= opt->nd_opt_len << 3;
		opt = (struct nd_opt_hdr *) ((uint8_t *) opt + (opt->nd_opt_len << 3));
	}

	if (changed)
		return device;
	else
		return NULL;
}

static struct nla_policy link_policy[IFLA_MAX + 1] = {
	[IFLA_PROTINFO] = { .type = NLA_NESTED },
};

static struct nla_policy link_prot_policy[IFLA_INET6_MAX + 1] = {
	[IFLA_INET6_FLAGS]	= { .type = NLA_U32 },
};

static char *
ra_flags_to_string (guint32 ra_flags)
{
	GString *s = g_string_sized_new (20);

	g_string_append (s, " (");
	if (ra_flags & IF_RS_SENT)
		g_string_append_c (s, 'S');

	if (ra_flags & IF_RA_RCVD)
		g_string_append_c (s, 'R');

	if (ra_flags & IF_RA_OTHERCONF)
		g_string_append_c (s, 'O');

	if (ra_flags & IF_RA_MANAGED)
		g_string_append_c (s, 'M');

	g_string_append_c (s, ')');
	return g_string_free (s, FALSE);
}

static NMIP6Device *
process_newlink (NMIP6Manager *manager, struct nl_msg *msg)
{
	struct nlmsghdr *hdr = nlmsg_hdr (msg);
	struct ifinfomsg *ifi;
	NMIP6Device *device;
	struct nlattr *tb[IFLA_MAX + 1];
	struct nlattr *pi[IFLA_INET6_MAX + 1];
	int err;
	char *flags_str = NULL;

	/* FIXME: we have to do this manually for now since libnl doesn't yet
	 * support the IFLA_PROTINFO attribute of NEWLINK messages.  When it does,
	 * we can get rid of this function and just grab IFLA_PROTINFO from
	 * nm_ip6_device_sync_from_netlink(), then get the IFLA_INET6_FLAGS out of
	 * the PROTINFO.
	 */
	err = nlmsg_parse (hdr, sizeof (*ifi), tb, IFLA_MAX, link_policy);
	if (err < 0) {
		nm_log_dbg (LOGD_IP6, "ignoring invalid newlink netlink message "
				      "while parsing PROTINFO attribute");
		return NULL;
	}

	ifi = nlmsg_data (hdr);
	if (ifi->ifi_family != AF_INET6) {
		nm_log_dbg (LOGD_IP6, "ignoring netlink message family %d", ifi->ifi_family);
		return NULL;
	}

	device = nm_ip6_manager_get_device (manager, ifi->ifi_index);
	if (!device || device->addrconf_complete) {
		nm_log_dbg (LOGD_IP6, "(%s): ignoring unknown or completed device",
		            device ? device->iface : "(none)");
		return NULL;
	}

	if (!tb[IFLA_PROTINFO]) {
		nm_log_dbg (LOGD_IP6, "(%s): message had no PROTINFO attribute", device->iface);
		return NULL;
	}

	err = nla_parse_nested (pi, IFLA_INET6_MAX, tb[IFLA_PROTINFO], link_prot_policy);
	if (err < 0) {
		nm_log_dbg (LOGD_IP6, "(%s): error parsing PROTINFO flags", device->iface);
		return NULL;
	}
	if (!pi[IFLA_INET6_FLAGS]) {
		nm_log_dbg (LOGD_IP6, "(%s): message had no PROTINFO flags", device->iface);
		return NULL;
	}

	device->ra_flags = nla_get_u32 (pi[IFLA_INET6_FLAGS]);

	if (nm_logging_level_enabled (LOGL_DEBUG))
		flags_str = ra_flags_to_string (device->ra_flags);
	nm_log_dbg (LOGD_IP6, "(%s): got IPv6 flags 0x%X%s",
	            device->iface, device->ra_flags, flags_str ? flags_str : "");
	g_free (flags_str);

	return device;
}

static void
netlink_notification (NMNetlinkMonitor *monitor, struct nl_msg *msg, gpointer user_data)
{
	NMIP6Manager *manager = (NMIP6Manager *) user_data;
	NMIP6Device *device;
	struct nlmsghdr *hdr;
	gboolean config_changed = FALSE;

	hdr = nlmsg_hdr (msg);
	nm_log_dbg (LOGD_HW, "netlink event type %d", hdr->nlmsg_type);
	switch (hdr->nlmsg_type) {
	case RTM_NEWADDR:
	case RTM_DELADDR:
		device = process_addr (manager, msg);
		config_changed = TRUE;
		break;
	case RTM_NEWROUTE:
	case RTM_DELROUTE:
		device = process_route (manager, msg);
		config_changed = TRUE;
		break;
	case RTM_NEWPREFIX:
		device = process_prefix (manager, msg);
		break;
	case RTM_NEWNDUSEROPT:
		device = process_nduseropt (manager, msg);
		config_changed = TRUE;
		break;
	case RTM_NEWLINK:
		device = process_newlink (manager, msg);
		config_changed = TRUE;
		break;
	default:
		return;
	}

	if (device) {
		nm_log_dbg (LOGD_IP6, "(%s): syncing device with netlink changes", device->iface);
		nm_ip6_device_sync_from_netlink (device, config_changed);
	}
}

gboolean
nm_ip6_manager_prepare_interface (NMIP6Manager *manager,
                                  int ifindex,
                                  NMSettingIP6Config *s_ip6,
                                  const char *accept_ra_path)
{
	NMIP6ManagerPrivate *priv;
	NMIP6Device *device;
	const char *method = NULL;

	g_return_val_if_fail (NM_IS_IP6_MANAGER (manager), FALSE);
	g_return_val_if_fail (ifindex > 0, FALSE);

	priv = NM_IP6_MANAGER_GET_PRIVATE (manager);

	device = nm_ip6_device_new (manager, ifindex);
	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (   strchr (device->iface, '/') == NULL
	                      && strcmp (device->iface, "all") != 0
	                      && strcmp (device->iface, "default") != 0,
	                      FALSE);

	if (s_ip6)
		method = nm_setting_ip6_config_get_method (s_ip6);
	if (!method)
		method = NM_SETTING_IP6_CONFIG_METHOD_AUTO;

	/* Establish target state and turn router advertisement acceptance on or off */
	if (!strcmp (method, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL)) {
		device->target_state = NM_IP6_DEVICE_GOT_LINK_LOCAL;
		nm_utils_do_sysctl (accept_ra_path, "0\n");
	} else {
		device->target_state = NM_IP6_DEVICE_GOT_ADDRESS;
		nm_utils_do_sysctl (accept_ra_path, "2\n");
	}

	return TRUE;
}

static gboolean
poll_ip6_flags (gpointer user_data)
{
	nm_netlink_monitor_request_ip6_info (NM_NETLINK_MONITOR (user_data), NULL);
	return TRUE;
}

void
nm_ip6_manager_begin_addrconf (NMIP6Manager *manager, int ifindex)
{
	NMIP6ManagerPrivate *priv;
	NMIP6Device *device;
	CallbackInfo *info;

	g_return_if_fail (NM_IS_IP6_MANAGER (manager));
	g_return_if_fail (ifindex > 0);

	priv = NM_IP6_MANAGER_GET_PRIVATE (manager);

	device = (NMIP6Device *) g_hash_table_lookup (priv->devices, GINT_TO_POINTER (ifindex));
	g_return_if_fail (device != NULL);

	nm_log_info (LOGD_IP6, "Activation (%s) Beginning IP6 addrconf.", device->iface);

	device->addrconf_complete = FALSE;
	device->ra_flags = 0;

	/* Set up a timeout on the transaction to kill it after the timeout */
	info = callback_info_new (device, 0, FALSE);
	device->finish_addrconf_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
	                                                         NM_IP6_TIMEOUT,
	                                                         finish_addrconf,
	                                                         info,
	                                                         (GDestroyNotify) g_free);

	/* Bounce IPv6 on the interface to ensure the kernel will start looking for
	 * new RAs; there doesn't seem to be a better way to do this right now.
	 */
	if (device->target_state >= NM_IP6_DEVICE_GOT_ADDRESS) {
		nm_utils_do_sysctl (device->disable_ip6_path, "1\n");
		g_usleep (200);
		nm_utils_do_sysctl (device->disable_ip6_path, "0\n");
	}

	device->ip6flags_poll_id = g_timeout_add_seconds (1, poll_ip6_flags, priv->monitor);

	/* Kick off the initial IPv6 flags request */
	nm_netlink_monitor_request_ip6_info (priv->monitor, NULL);

	/* Sync flags, etc, from netlink; this will also notice if the
	 * device is already fully configured and schedule the
	 * ADDRCONF_COMPLETE signal in that case.
	 */
	nm_ip6_device_sync_from_netlink (device, FALSE);
}

void
nm_ip6_manager_cancel_addrconf (NMIP6Manager *manager, int ifindex)
{
	g_return_if_fail (NM_IS_IP6_MANAGER (manager));
	g_return_if_fail (ifindex > 0);

	g_hash_table_remove (NM_IP6_MANAGER_GET_PRIVATE (manager)->devices,
	                     GINT_TO_POINTER (ifindex));
}

#define FIRST_ROUTE(m) ((struct rtnl_route *) nl_cache_get_first (m))
#define NEXT_ROUTE(m) ((struct rtnl_route *) nl_cache_get_next ((struct nl_object *) m))

#define FIRST_ADDR(m) ((struct rtnl_addr *) nl_cache_get_first (m))
#define NEXT_ADDR(m) ((struct rtnl_addr *) nl_cache_get_next ((struct nl_object *) m))

NMIP6Config *
nm_ip6_manager_get_ip6_config (NMIP6Manager *manager, int ifindex)
{
	NMIP6ManagerPrivate *priv;
	NMIP6Device *device;
	NMIP6Config *config;
	struct rtnl_addr *rtnladdr;
	struct nl_addr *nladdr;
	struct in6_addr *addr;
	NMIP6Address *ip6addr;
	struct rtnl_route *rtnlroute;
	struct nl_addr *nldest, *nlgateway;
	struct in6_addr *dest, *gateway;
	gboolean defgw_set = FALSE;
	struct in6_addr defgw;
	uint32_t metric;
	NMIP6Route *ip6route;
	int i;

	g_return_val_if_fail (NM_IS_IP6_MANAGER (manager), NULL);
	g_return_val_if_fail (ifindex > 0, NULL);

	priv = NM_IP6_MANAGER_GET_PRIVATE (manager);

	device = (NMIP6Device *) g_hash_table_lookup (priv->devices,
	                                              GINT_TO_POINTER (ifindex));
	if (!device) {
		nm_log_warn (LOGD_IP6, "(%d): addrconf not started.", ifindex);
		return NULL;
	}

	config = nm_ip6_config_new ();
	if (!config) {
		nm_log_err (LOGD_IP6, "(%s): out of memory creating IP6 config object.",
		            device->iface);
		return NULL;
	}

	/* Make sure we refill the route and address caches, otherwise we won't get
	 * up-to-date information here since the netlink route/addr change messages
	 * may be lagging a bit.
	 */
	nl_cache_refill (priv->nlh, priv->route_cache);
	nl_cache_refill (priv->nlh, priv->addr_cache);

	/* Add routes */
	for (rtnlroute = FIRST_ROUTE (priv->route_cache); rtnlroute; rtnlroute = NEXT_ROUTE (rtnlroute)) {
		/* Make sure it's an IPv6 route for this device */
		if (rtnl_route_get_oif (rtnlroute) != device->ifindex)
			continue;
		if (rtnl_route_get_family (rtnlroute) != AF_INET6)
			continue;

		nldest = rtnl_route_get_dst (rtnlroute);
		if (!nldest || nl_addr_get_family (nldest) != AF_INET6)
			continue;
		dest = nl_addr_get_binary_addr (nldest);

		nlgateway = rtnl_route_get_gateway (rtnlroute);
		if (!nlgateway || nl_addr_get_family (nlgateway) != AF_INET6)
			continue;
		gateway = nl_addr_get_binary_addr (nlgateway);

		if (rtnl_route_get_dst_len (rtnlroute) == 0) {
			/* Default gateway route; don't add to normal routes but to each address */
			if (!defgw_set) {
				memcpy (&defgw, gateway, sizeof (defgw));
				defgw_set = TRUE;
			}
			continue;
		}

		/* Also ignore link-local routes where the destination and gateway are
		 * the same, which apparently get added by the kernel but return -EINVAL
		 * when we try to add them via netlink.
		 */
		if (gateway && IN6_ARE_ADDR_EQUAL (dest, gateway))
			continue;

		ip6route = nm_ip6_route_new ();
		nm_ip6_route_set_dest (ip6route, dest);
		nm_ip6_route_set_prefix (ip6route, rtnl_route_get_dst_len (rtnlroute));
		nm_ip6_route_set_next_hop (ip6route, gateway);
		rtnl_route_get_metric(rtnlroute, 1, &metric);
		if (metric != UINT_MAX)
			nm_ip6_route_set_metric (ip6route, metric);
		nm_ip6_config_take_route (config, ip6route);
	}

	/* Add addresses */
	for (rtnladdr = FIRST_ADDR (priv->addr_cache); rtnladdr; rtnladdr = NEXT_ADDR (rtnladdr)) {
		if (rtnl_addr_get_ifindex (rtnladdr) != device->ifindex)
			continue;

		nladdr = rtnl_addr_get_local (rtnladdr);
		if (!nladdr || nl_addr_get_family (nladdr) != AF_INET6)
			continue;

		addr = nl_addr_get_binary_addr (nladdr);
		ip6addr = nm_ip6_address_new ();
		nm_ip6_address_set_prefix (ip6addr, rtnl_addr_get_prefixlen (rtnladdr));
		nm_ip6_address_set_address (ip6addr, addr);
		nm_ip6_config_take_address (config, ip6addr);
		if (defgw_set)
			nm_ip6_address_set_gateway (ip6addr, &defgw);
	}

	/* Add DNS servers */
	if (device->rdnss_servers) {
		NMIP6RDNSS *rdnss = (NMIP6RDNSS *)(device->rdnss_servers->data);

		for (i = 0; i < device->rdnss_servers->len; i++)
			nm_ip6_config_add_nameserver (config, &rdnss[i].addr);
	}

	/* Add DNS domains */
	if (device->dnssl_domains) {
		NMIP6DNSSL *dnssl = (NMIP6DNSSL *)(device->dnssl_domains->data);

		for (i = 0; i < device->dnssl_domains->len; i++)
			nm_ip6_config_add_domain (config, dnssl[i].domain);
	}

	return config;
}

/******************************************************************/

static NMIP6Manager *
nm_ip6_manager_new (void)
{
	NMIP6Manager *manager;
	NMIP6ManagerPrivate *priv;

	manager = g_object_new (NM_TYPE_IP6_MANAGER, NULL);
	priv = NM_IP6_MANAGER_GET_PRIVATE (manager);

	if (!priv->devices) {
		nm_log_err (LOGD_IP6, "not enough memory to initialize IP6 manager tables");
		g_object_unref (manager);
		manager = NULL;
	}

	return manager;
}

static NMIP6Manager *singleton = NULL;

NMIP6Manager *
nm_ip6_manager_get (void)
{
	if (!singleton) {
		singleton = nm_ip6_manager_new ();
		g_assert (singleton);
	} else
		g_object_ref (singleton);

	return singleton;
}

static void
nm_ip6_manager_init (NMIP6Manager *manager)
{
	NMIP6ManagerPrivate *priv = NM_IP6_MANAGER_GET_PRIVATE (manager);

	priv->devices = g_hash_table_new_full (g_direct_hash, g_direct_equal,
	                                       NULL,
	                                       (GDestroyNotify) nm_ip6_device_destroy);

	priv->monitor = nm_netlink_monitor_get ();
	nm_netlink_monitor_subscribe (priv->monitor, RTNLGRP_IPV6_IFADDR, NULL);
	nm_netlink_monitor_subscribe (priv->monitor, RTNLGRP_IPV6_PREFIX, NULL);
	nm_netlink_monitor_subscribe (priv->monitor, RTNLGRP_IPV6_ROUTE, NULL);
	nm_netlink_monitor_subscribe (priv->monitor, RTNLGRP_ND_USEROPT, NULL);
	nm_netlink_monitor_subscribe (priv->monitor, RTNLGRP_LINK, NULL);

	priv->netlink_id = g_signal_connect (priv->monitor, "notification",
	                                     G_CALLBACK (netlink_notification), manager);

	priv->nlh = nm_netlink_get_default_handle ();
	rtnl_addr_alloc_cache (priv->nlh, &priv->addr_cache);
	g_warn_if_fail (priv->addr_cache != NULL);
	rtnl_route_alloc_cache (priv->nlh, NETLINK_ROUTE, NL_AUTO_PROVIDE, &priv->route_cache);
	g_warn_if_fail (priv->route_cache != NULL);
}

static void
finalize (GObject *object)
{
	NMIP6ManagerPrivate *priv = NM_IP6_MANAGER_GET_PRIVATE (object);

	g_signal_handler_disconnect (priv->monitor, priv->netlink_id);

	g_hash_table_destroy (priv->devices);
	g_object_unref (priv->monitor);
	nl_cache_free (priv->addr_cache);
	nl_cache_free (priv->route_cache);

	singleton = NULL;

	G_OBJECT_CLASS (nm_ip6_manager_parent_class)->finalize (object);
}

static void
nm_ip6_manager_class_init (NMIP6ManagerClass *manager_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (manager_class);

	g_type_class_add_private (manager_class, sizeof (NMIP6ManagerPrivate));

	/* virtual methods */
	object_class->finalize = finalize;

	/* signals */
	signals[ADDRCONF_COMPLETE] =
		g_signal_new ("addrconf-complete",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMIP6ManagerClass, addrconf_complete),
					  NULL, NULL,
					  _nm_marshal_VOID__INT_UINT_BOOLEAN,
					  G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_UINT, G_TYPE_BOOLEAN);

	signals[CONFIG_CHANGED] =
		g_signal_new ("config-changed",
					  G_OBJECT_CLASS_TYPE (object_class),
					  G_SIGNAL_RUN_FIRST,
					  G_STRUCT_OFFSET (NMIP6ManagerClass, config_changed),
					  NULL, NULL,
					  _nm_marshal_VOID__INT_UINT_BOOLEAN,
					  G_TYPE_NONE, 3, G_TYPE_INT, G_TYPE_UINT, G_TYPE_BOOLEAN);
}

