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
 * Copyright (C) 2005 - 2011 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 */

#include <config.h>
#include <glib.h>
#include <glib/gi18n.h>
#include <dbus/dbus.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>

#include "nm-glib-compat.h"
#include "nm-device.h"
#include "nm-device-private.h"
#include "NetworkManagerUtils.h"
#include "nm-system.h"
#include "nm-dhcp-manager.h"
#include "nm-dbus-manager.h"
#include "nm-dns-manager.h"
#include "nm-utils.h"
#include "nm-logging.h"
#include "nm-netlink-monitor.h"
#include "nm-netlink-utils.h"
#include "nm-netlink-compat.h"
#include "nm-setting-ip4-config.h"
#include "nm-setting-ip6-config.h"
#include "nm-setting-connection.h"
#include "nm-dnsmasq-manager.h"
#include "nm-dhcp4-config.h"
#include "nm-ip6-manager.h"
#include "nm-marshal.h"
#include "nm-rfkill.h"
#include "nm-firewall-manager.h"
#include "nm-properties-changed-signal.h"

static void impl_device_disconnect (NMDevice *device, DBusGMethodInvocation *context);

#include "nm-device-interface-glue.h"

#define PENDING_IP4_CONFIG "pending-ip4-config"
#define PENDING_IP6_CONFIG "pending-ip6-config"

/***********************************************************/
typedef enum {
	NM_DEVICE_ERROR_CONNECTION_ACTIVATING = 0,
	NM_DEVICE_ERROR_CONNECTION_INVALID,
	NM_DEVICE_ERROR_NOT_ACTIVE,
} NMDeviceError;

#define NM_DEVICE_ERROR (nm_device_error_quark ())
#define NM_TYPE_DEVICE_ERROR (nm_device_error_get_type ())

static GQuark
nm_device_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("nm-device-error");
	return quark;
}

/* This should really be standard. */
#define ENUM_ENTRY(NAME, DESC) { NAME, "" #NAME "", DESC }

static GType
nm_device_error_get_type (void)
{
	static GType etype = 0;

	if (etype == 0) {
		static const GEnumValue values[] = {
			/* Connection is already activating. */
			ENUM_ENTRY (NM_DEVICE_ERROR_CONNECTION_ACTIVATING, "ConnectionActivating"),
			/* Connection is invalid for this device. */
			ENUM_ENTRY (NM_DEVICE_ERROR_CONNECTION_INVALID, "ConnectionInvalid"),
			/* Operation could not be performed because the device is not active. */
			ENUM_ENTRY (NM_DEVICE_ERROR_NOT_ACTIVE, "NotActive"),
			{ 0, 0, 0 }
		};
		etype = g_enum_register_static ("NMDeviceError", values);
	}
	return etype;
}

/***********************************************************/

enum {
	STATE_CHANGED,
	DISCONNECT_REQUEST,
	AUTOCONNECT_ALLOWED,
	LAST_SIGNAL,
};
static guint signals[LAST_SIGNAL] = { 0 };

enum {
	PROP_0,
	PROP_UDI,
	PROP_IFACE,
	PROP_IP_IFACE,
	PROP_DRIVER,
	PROP_CAPABILITIES,
	PROP_IP4_ADDRESS,
	PROP_IP4_CONFIG,
	PROP_DHCP4_CONFIG,
	PROP_IP6_CONFIG,
	PROP_DHCP6_CONFIG,
	PROP_STATE,
	PROP_ACTIVE_CONNECTION,
	PROP_DEVICE_TYPE,
	PROP_MANAGED,
	PROP_FIRMWARE_MISSING,
	PROP_TYPE_DESC,
	PROP_RFKILL_TYPE,
	PROP_IFINDEX,
	LAST_PROP
};

/***********************************************************/

G_DEFINE_ABSTRACT_TYPE (NMDevice, nm_device, G_TYPE_OBJECT)

#define NM_DEVICE_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), NM_TYPE_DEVICE, NMDevicePrivate))

typedef enum {
	IP_NONE = 0,
	IP_CONF,
	IP_DONE
} IpState;

typedef struct {
	NMDeviceState state;
	NMDeviceStateReason reason;
	guint id;
} QueuedState;

typedef struct {
	gboolean disposed;
	gboolean initialized;

	NMDeviceState state;
	QueuedState   queued_state;

	char *        udi;
	char *        path;
	char *        iface;   /* may change, could be renamed by user */
	int           ifindex;
	char *        ip_iface;
	int           ip_ifindex;
	NMDeviceType  type;
	char *        type_desc;
	guint32       capabilities;
	char *        driver;
	gboolean      managed; /* whether managed by NM or not */
	RfKillType    rfkill_type;
	gboolean      firmware_missing;

	guint32         ip4_address;

	NMActRequest *  act_request;
	guint           act_source_id;
	gpointer        act_source_func;
	guint           act_source6_id;
	gpointer        act_source6_func;
	gulong          secrets_updated_id;
	gulong          secrets_failed_id;

	/* Generic DHCP stuff */
	NMDHCPManager * dhcp_manager;
	guint32         dhcp_timeout;
	GByteArray *    dhcp_anycast_address;

	/* IP4 configuration info */
	NMIP4Config *   ip4_config;			/* Config from DHCP, PPP, or system config files */
	IpState         ip4_state;
	NMDHCPClient *  dhcp4_client;
	gulong          dhcp4_state_sigid;
	gulong          dhcp4_timeout_sigid;
	NMDHCP4Config * dhcp4_config;

	/* dnsmasq stuff for shared connections */
	NMDnsMasqManager *dnsmasq_manager;
	gulong            dnsmasq_state_id;

	/* Firewall Manager */
	NMFirewallManager *fw_manager;
	DBusGProxyCall    *fw_call;

	/* avahi-autoipd stuff */
	GPid    aipd_pid;
	guint   aipd_watch;
	guint   aipd_timeout;

	/* IP6 configuration info */
	NMIP6Config *  ip6_config;
	IpState        ip6_state;

	NMIP6Manager * ip6_manager;
	gulong         ip6_addrconf_sigid;
	gulong         ip6_config_changed_sigid;
	gboolean       ip6_waiting_for_config;
	/* IP6 config from autoconf */
	NMIP6Config *  ac_ip6_config;

	char *         ip6_accept_ra_path;
	guint32        ip6_accept_ra_save;

	NMDHCPClient *  dhcp6_client;
	guint32         dhcp6_mode;
	gulong          dhcp6_state_sigid;
	gulong          dhcp6_timeout_sigid;
	NMDHCP6Config * dhcp6_config;
	/* IP6 config from DHCP */
	NMIP6Config *   dhcp6_ip6_config;

	/* inhibit autoconnect feature */
	gboolean	autoconnect_inhibit;

	/* master interface for bridge, bond, vlan, etc */
	NMDevice *	master;
} NMDevicePrivate;

static void nm_device_take_down (NMDevice *dev, gboolean wait, NMDeviceStateReason reason);

static gboolean nm_device_bring_up (NMDevice *self, gboolean block, gboolean *no_firmware);
static gboolean nm_device_is_up (NMDevice *self);

static gboolean nm_device_set_ip4_config (NMDevice *dev,
                                          NMIP4Config *config,
                                          gboolean assumed,
                                          NMDeviceStateReason *reason);
static gboolean nm_device_set_ip6_config (NMDevice *dev,
                                          NMIP6Config *config,
                                          gboolean assumed,
                                          NMDeviceStateReason *reason);

static gboolean nm_device_activate_ip6_config_commit (gpointer user_data);

static void dhcp4_cleanup (NMDevice *self, gboolean stop, gboolean release);

static const char *reason_to_string (NMDeviceStateReason reason);

static void
nm_device_init (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	priv->type = NM_DEVICE_TYPE_UNKNOWN;
	priv->capabilities = NM_DEVICE_CAP_NONE;
	priv->state = NM_DEVICE_STATE_UNMANAGED;
	priv->dhcp_timeout = 0;
	priv->rfkill_type = RFKILL_TYPE_UNKNOWN;
}

static void
update_accept_ra_save (NMDevice *self)
{
	NMDevicePrivate *priv;
	const char *ip_iface;
	char *new_path;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	ip_iface = nm_device_get_ip_iface (self);

	new_path = g_strdup_printf ("/proc/sys/net/ipv6/conf/%s/accept_ra", ip_iface);
	g_assert (new_path);

	if (priv->ip6_accept_ra_path) {
		/* If the IP iface is different from before, use the new value */
		if (!strcmp (new_path, priv->ip6_accept_ra_path)) {
			g_free (new_path);
			return;
		}
		g_free (priv->ip6_accept_ra_path);
	}

	/* Grab the original value of "accept_ra" so we can restore it when NM exits */
	priv->ip6_accept_ra_path = new_path;
	if (!nm_utils_get_proc_sys_net_value (priv->ip6_accept_ra_path,
	                                      ip_iface,
	                                      &priv->ip6_accept_ra_save)) {
		g_free (priv->ip6_accept_ra_path);
		priv->ip6_accept_ra_path = NULL;
	}
}

static GObject*
constructor (GType type,
			 guint n_construct_params,
			 GObjectConstructParam *construct_params)
{
	GObject *object;
	NMDevice *dev;
	NMDevicePrivate *priv;

	object = G_OBJECT_CLASS (nm_device_parent_class)->constructor (type,
													   n_construct_params,
													   construct_params);
	if (!object)
		return NULL;

	dev = NM_DEVICE (object);
	priv = NM_DEVICE_GET_PRIVATE (dev);

	if (!priv->udi) {
		nm_log_err (LOGD_DEVICE, "No device udi provided, ignoring");
		goto error;
	}

	if (!priv->iface) {
		nm_log_err (LOGD_DEVICE, "No device interface provided, ignoring");
		goto error;
	}

	priv->capabilities |= NM_DEVICE_GET_CLASS (dev)->get_generic_capabilities (dev);
	if (!(priv->capabilities & NM_DEVICE_CAP_NM_SUPPORTED)) {
		nm_log_warn (LOGD_DEVICE, "(%s): Device unsupported, ignoring.", priv->iface);
		goto error;
	}

	priv->dhcp_manager = nm_dhcp_manager_get ();

	priv->fw_manager = nm_firewall_manager_get ();

	update_accept_ra_save (dev);

	priv->initialized = TRUE;
	return object;

error:
	g_object_unref (dev);
	return NULL;
}

static void
constructed (GObject *object)
{
	NMDevice *dev = NM_DEVICE (object);

	if (NM_DEVICE_GET_CLASS (dev)->update_hw_address)
		NM_DEVICE_GET_CLASS (dev)->update_hw_address (dev);

	if (NM_DEVICE_GET_CLASS (dev)->update_permanent_hw_address)
		NM_DEVICE_GET_CLASS (dev)->update_permanent_hw_address (dev);

	if (NM_DEVICE_GET_CLASS (dev)->update_initial_hw_address)
		NM_DEVICE_GET_CLASS (dev)->update_initial_hw_address (dev);

	if (G_OBJECT_CLASS (nm_device_parent_class)->constructed)
		G_OBJECT_CLASS (nm_device_parent_class)->constructed (object);
}

static gboolean
nm_device_hw_is_up (NMDevice *self)
{
	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);

	if (NM_DEVICE_GET_CLASS (self)->hw_is_up)
		return NM_DEVICE_GET_CLASS (self)->hw_is_up (self);

	return TRUE;
}

static guint32
real_get_generic_capabilities (NMDevice *dev)
{
	return 0;
}

void
nm_device_set_path (NMDevice *self, const char *path)
{
	NMDevicePrivate *priv;

	g_return_if_fail (self != NULL);

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->path == NULL);

	priv->path = g_strdup (path);
}

const char *
nm_device_get_path (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_DEVICE_GET_PRIVATE (self)->path;
}

const char *
nm_device_get_udi (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_DEVICE_GET_PRIVATE (self)->udi;
}

/*
 * Get/set functions for iface
 */
const char *
nm_device_get_iface (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_DEVICE_GET_PRIVATE (self)->iface;
}

int
nm_device_get_ifindex (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, 0);

	return NM_DEVICE_GET_PRIVATE (self)->ifindex;
}

const char *
nm_device_get_ip_iface (NMDevice *self)
{
	NMDevicePrivate *priv;

	g_return_val_if_fail (self != NULL, NULL);

	priv = NM_DEVICE_GET_PRIVATE (self);
	/* If it's not set, default to iface */
	return priv->ip_iface ? priv->ip_iface : priv->iface;
}

int
nm_device_get_ip_ifindex (NMDevice *self)
{
	NMDevicePrivate *priv;

	g_return_val_if_fail (self != NULL, 0);

	priv = NM_DEVICE_GET_PRIVATE (self);
	/* If it's not set, default to iface */
	return priv->ip_iface ? priv->ip_ifindex : priv->ifindex;
}

void
nm_device_set_ip_iface (NMDevice *self, const char *iface)
{
	NMDevicePrivate *priv;
	char *old_ip_iface;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	old_ip_iface = priv->ip_iface;
	priv->ip_ifindex = 0;

	priv->ip_iface = g_strdup (iface);
	if (priv->ip_iface) {
		priv->ip_ifindex = nm_netlink_iface_to_index (priv->ip_iface);
		if (priv->ip_ifindex < 0) {
			nm_log_warn (LOGD_HW, "(%s): failed to look up interface index", iface);
		}
	}

	/* Emit change notification */
	if (g_strcmp0 (old_ip_iface, priv->ip_iface))
		g_object_notify (G_OBJECT (self), NM_DEVICE_IP_IFACE);
	g_free (old_ip_iface);
}


/*
 * Get/set functions for driver
 */
const char *
nm_device_get_driver (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_DEVICE_GET_PRIVATE (self)->driver;
}


/*
 * Get/set functions for type
 */
NMDeviceType
nm_device_get_device_type (NMDevice *self)
{
	g_return_val_if_fail (NM_IS_DEVICE (self), NM_DEVICE_TYPE_UNKNOWN);

	return NM_DEVICE_GET_PRIVATE (self)->type;
}


int
nm_device_get_priority (NMDevice *dev)
{
	g_return_val_if_fail (NM_IS_DEVICE (dev), -1);

	return (int) nm_device_get_device_type (dev);
}


/*
 * Accessor for capabilities
 */
guint32
nm_device_get_capabilities (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NM_DEVICE_CAP_NONE);

	return NM_DEVICE_GET_PRIVATE (self)->capabilities;
}

/*
 * Accessor for type-specific capabilities
 */
guint32
nm_device_get_type_capabilities (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NM_DEVICE_CAP_NONE);

	return NM_DEVICE_GET_CLASS (self)->get_type_capabilities (self);
}

static guint32
real_get_type_capabilities (NMDevice *self)
{
	return NM_DEVICE_CAP_NONE;
}


const char *
nm_device_get_type_desc (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_DEVICE_GET_PRIVATE (self)->type_desc;
}

NMDevice *
nm_device_get_master (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_DEVICE_GET_PRIVATE (self)->master;
}

const char *
nm_device_get_master_path (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	if (NM_DEVICE_GET_PRIVATE (self)->master)
		return nm_device_get_path (NM_DEVICE_GET_PRIVATE (self)->master);

	return NULL;
}

void
nm_device_set_master (NMDevice *self, NMDevice *master)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE(self);

	if (priv->master)
		g_object_unref (priv->master);
	priv->master = master ? g_object_ref (master) : NULL;

	if (priv->act_request)
		g_object_notify (G_OBJECT (priv->act_request), NM_ACTIVE_CONNECTION_MASTER);
}

/*
 * nm_device_get_act_request
 *
 * Return the devices activation request, if any.
 *
 */
NMActRequest *
nm_device_get_act_request (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);

	return NM_DEVICE_GET_PRIVATE (self)->act_request;
}

NMConnection *
nm_device_get_connection (NMDevice *self)
{
	NMActRequest *req;

	req = nm_device_get_act_request (self);
	g_assert (req);

	return nm_act_request_get_connection (req);
}

gboolean
nm_device_is_available (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->firmware_missing)
		return FALSE;

	if (NM_DEVICE_GET_CLASS (self)->is_available)
		return NM_DEVICE_GET_CLASS (self)->is_available (self);
	return TRUE;
}

gboolean
nm_device_get_enabled (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);

	if (NM_DEVICE_GET_CLASS (self)->get_enabled)
		return NM_DEVICE_GET_CLASS (self)->get_enabled (self);
	return TRUE;
}

void
nm_device_set_enabled (NMDevice *self, gboolean enabled)
{
	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_DEVICE (self));

	if (NM_DEVICE_GET_CLASS (self)->set_enabled)
		NM_DEVICE_GET_CLASS (self)->set_enabled (self, enabled);
}

RfKillType
nm_device_get_rfkill_type (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);

	return NM_DEVICE_GET_PRIVATE (self)->rfkill_type;
}

static gboolean
autoconnect_allowed_accumulator (GSignalInvocationHint *ihint,
                                 GValue *return_accu,
                                 const GValue *handler_return, gpointer data)
{
	if (!g_value_get_boolean (handler_return))
		g_value_set_boolean (return_accu, FALSE);
	return TRUE;
}

gboolean
nm_device_autoconnect_allowed (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	GValue instance = { 0, };
	GValue retval = { 0, };

	g_value_init (&instance, G_TYPE_OBJECT);
	g_value_take_object (&instance, self);

	g_value_init (&retval, G_TYPE_BOOLEAN);
	if (priv->autoconnect_inhibit)
		g_value_set_boolean (&retval, FALSE);
	else
		g_value_set_boolean (&retval, TRUE);

	/* Use g_signal_emitv() rather than g_signal_emit() to avoid the return
	 * value being changed if no handlers are connected */
	g_signal_emitv (&instance, signals[AUTOCONNECT_ALLOWED], 0, &retval);
	return g_value_get_boolean (&retval);
}

NMConnection *
nm_device_get_best_auto_connection (NMDevice *dev,
                                    GSList *connections,
                                    char **specific_object)
{
	guint32 caps;

	g_return_val_if_fail (NM_IS_DEVICE (dev), NULL);
	g_return_val_if_fail (specific_object != NULL, NULL);
	g_return_val_if_fail (*specific_object == NULL, NULL);

	caps = nm_device_get_capabilities (dev);
	/* Don't use devices that SUCK */
	if (!(caps & NM_DEVICE_CAP_NM_SUPPORTED))
		return NULL;

	if (!NM_DEVICE_GET_CLASS (dev)->get_best_auto_connection)
		return NULL;

	return NM_DEVICE_GET_CLASS (dev)->get_best_auto_connection (dev, connections, specific_object);
}

gboolean
nm_device_complete_connection (NMDevice *self,
                               NMConnection *connection,
                               const char *specific_object,
                               const GSList *existing_connections,
                               GError **error)
{
	gboolean success = FALSE;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (connection != NULL, FALSE);

	if (!NM_DEVICE_GET_CLASS (self)->complete_connection) {
		g_set_error (error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_CONNECTION_INVALID,
		             "Device class %s had no complete_connection method",
		             G_OBJECT_TYPE_NAME (self));
		return FALSE;
	}

	success = NM_DEVICE_GET_CLASS (self)->complete_connection (self,
	                                                           connection,
	                                                           specific_object,
	                                                           existing_connections,
	                                                           error);
	if (success)
		success = nm_connection_verify (connection, error);

	return success;
}

gboolean
nm_device_check_connection_compatible (NMDevice *device,
                                       NMConnection *connection,
                                       GError **error)
{
	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);
	g_return_val_if_fail (connection != NULL, FALSE);
	g_return_val_if_fail (NM_IS_CONNECTION (connection), FALSE);
	g_return_val_if_fail (error != NULL, FALSE);
	g_return_val_if_fail (*error == NULL, FALSE);

	if (NM_DEVICE_GET_CLASS (device)->check_connection_compatible)
		return NM_DEVICE_GET_CLASS (device)->check_connection_compatible (device, connection, error);
	return TRUE;
}

gboolean
nm_device_can_assume_connections (NMDevice *device)
{
	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	return !!NM_DEVICE_GET_CLASS (device)->connection_match_config;
}

static void
dnsmasq_state_changed_cb (NMDnsMasqManager *manager, guint32 status, gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);

	switch (status) {
	case NM_DNSMASQ_STATUS_DEAD:
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_SHARED_START_FAILED);
		break;
	default:
		break;
	}
}

static void
activation_source_clear (NMDevice *self, gboolean remove_source, int family)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	guint *act_source_id;
	gpointer *act_source_func;

	if (family == AF_INET6) {
		act_source_id = &priv->act_source6_id;
		act_source_func = &priv->act_source6_func;
	} else {
		act_source_id = &priv->act_source_id;
		act_source_func = &priv->act_source_func;
	}

	if (*act_source_id) {
		if (remove_source)
			g_source_remove (*act_source_id);
		*act_source_id = 0;
		*act_source_func = NULL;
	}
}

static void
activation_source_schedule (NMDevice *self, GSourceFunc func, int family)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	guint *act_source_id;
	gpointer *act_source_func;

	if (family == AF_INET6) {
		act_source_id = &priv->act_source6_id;
		act_source_func = &priv->act_source6_func;
	} else {
		act_source_id = &priv->act_source_id;
		act_source_func = &priv->act_source_func;
	}

	if (*act_source_id) {
		nm_log_err (LOGD_DEVICE, "activation stage already scheduled");
	}

	/* Don't bother rescheduling the same function that's about to
	 * run anyway.  Fixes issues with crappy wireless drivers sending
	 * streams of associate events before NM has had a chance to process
	 * the first one.
	 */
	if (!*act_source_id || (*act_source_func != func)) {
		activation_source_clear (self, TRUE, family);
		*act_source_id = g_idle_add (func, self);
		*act_source_func = func;
	}
}

gboolean
nm_device_ip_config_should_fail (NMDevice *self, gboolean ip6)
{
	NMConnection *connection;
	NMSettingIP4Config *s_ip4;
	NMSettingIP6Config *s_ip6;

	g_return_val_if_fail (self != NULL, TRUE);

	connection = nm_device_get_connection (self);
	g_assert (connection);

	/* Fail the connection if the failed IP method is required to complete */
	if (ip6) {
		s_ip6 = nm_connection_get_setting_ip6_config (connection);
		if (s_ip6 && !nm_setting_ip6_config_get_may_fail (s_ip6))
			return TRUE;
	} else {
		s_ip4 = nm_connection_get_setting_ip4_config (connection);
		if (s_ip4 && !nm_setting_ip4_config_get_may_fail (s_ip4))
			return TRUE;
	}

	return FALSE;
}

static gboolean
ip6_method_matches (NMConnection *connection, const char *match)
{
	NMSettingIP6Config *s_ip6;
	const char *method = NULL;

	s_ip6 = nm_connection_get_setting_ip6_config (connection);
	if (s_ip6) {
		method = nm_setting_ip6_config_get_method (s_ip6);
		g_assert (method);
	}

	/* Treat missing IP6 setting as IGNORE */
	if (!s_ip6 && !strcmp (match, NM_SETTING_IP6_CONFIG_METHOD_IGNORE))
		return TRUE;

	return method && !strcmp (method, match);
}

static NMActStageReturn
real_act_stage1_prepare (NMDevice *self, NMDeviceStateReason *reason)
{
	return NM_ACT_STAGE_RETURN_SUCCESS;
}

static gboolean
handle_slave_activation (NMDevice *slave, NMDevice *master)
{
	NMConnection *connection;
	NMSettingConnection *s_con;

	connection = nm_device_get_connection (slave);
	g_assert (connection);

	s_con = nm_connection_get_setting_connection (connection);
	g_assert (s_con);

	if (nm_setting_connection_is_slave_type (s_con, NM_SETTING_BOND_SETTING_NAME)) {
		/*
		 * Bonding
		 *
		 * Kernel expects slaves to be down while the enslaving is
		 * taking place.
		 */
		nm_device_hw_take_down (slave, TRUE);

		if (!nm_system_iface_enslave (slave, master))
			return FALSE;

		nm_device_hw_bring_up (slave, TRUE, NULL);
	} else {
		nm_log_warn (LOGD_DEVICE, "(%s): Unable to enslave. Unknown slave type '%s'",
		             nm_device_get_iface (slave), nm_setting_connection_get_slave_type (s_con));

		/* Abort activation */
		return FALSE;
	}

	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 1 of 5 (Device Prepare) enslaved to %s",
	             nm_device_get_iface (slave),
				 nm_device_get_iface (master));

	return TRUE;
}

static void
handle_slave_deactivation (NMDevice *slave, NMDevice *master)
{
	NMConnection *connection;
	NMSettingConnection *s_con;

	connection = nm_device_get_connection (slave);
	g_assert (connection);

	s_con = nm_connection_get_setting_connection (connection);
	g_assert (s_con);

	if (nm_setting_connection_is_slave_type (s_con, NM_SETTING_BOND_SETTING_NAME)) {
		nm_system_iface_release (slave, master);
		nm_log_info (LOGD_DEVICE, "Device %s released from master %s",
			         nm_device_get_iface (slave), nm_device_get_iface (master));
	}
}

/*
 * nm_device_activate_stage1_device_prepare
 *
 * Prepare for device activation
 *
 */
static gboolean
nm_device_activate_stage1_device_prepare (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	const char *iface;
	NMActStageReturn ret;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;
	NMDevice *master;

	/* Clear the activation source ID now that this stage has run */
	activation_source_clear (self, FALSE, 0);

	priv->ip4_state = priv->ip6_state = IP_NONE;

	iface = nm_device_get_iface (self);
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 1 of 5 (Device Prepare) started...", iface);
	nm_device_state_changed (self, NM_DEVICE_STATE_PREPARE, NM_DEVICE_STATE_REASON_NONE);

	if ((master = nm_device_get_master (self))) {
		if (!handle_slave_activation (self, master)) {
			nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
			goto out;
		}
	}

	ret = NM_DEVICE_GET_CLASS (self)->act_stage1_prepare (self, &reason);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE) {
		goto out;
	} else if (ret == NM_ACT_STAGE_RETURN_FAILURE) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);

	nm_device_activate_schedule_stage2_device_config (self);

out:
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 1 of 5 (Device Prepare) complete.", iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage1_device_prepare
 *
 * Prepare a device for activation
 *
 */
void
nm_device_activate_schedule_stage1_device_prepare (NMDevice *self)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->act_request);

	activation_source_schedule (self, nm_device_activate_stage1_device_prepare, 0);

	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 1 of 5 (Device Prepare) scheduled...",
	             nm_device_get_iface (self));
}

static NMActStageReturn
real_act_stage2_config (NMDevice *dev, NMDeviceStateReason *reason)
{
	/* Nothing to do */
	return NM_ACT_STAGE_RETURN_SUCCESS;
}

/*
 * nm_device_activate_stage2_device_config
 *
 * Determine device parameters and set those on the device, ie
 * for wireless devices, set SSID, keys, etc.
 *
 */
static gboolean
nm_device_activate_stage2_device_config (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	const char *     iface;
	NMActStageReturn ret;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;
	gboolean no_firmware = FALSE;

	/* Clear the activation source ID now that this stage has run */
	activation_source_clear (self, FALSE, 0);

	iface = nm_device_get_iface (self);
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 2 of 5 (Device Configure) starting...", iface);
	nm_device_state_changed (self, NM_DEVICE_STATE_CONFIG, NM_DEVICE_STATE_REASON_NONE);

	if (!nm_device_bring_up (self, FALSE, &no_firmware)) {
		if (no_firmware)
			nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_FIRMWARE_MISSING);
		else
			nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_CONFIG_FAILED);
		goto out;
	}

	ret = NM_DEVICE_GET_CLASS (self)->act_stage2_config (self, &reason);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE)
		goto out;
	else if (ret == NM_ACT_STAGE_RETURN_FAILURE)
	{
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);	

	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 2 of 5 (Device Configure) successful.", iface);

	nm_device_activate_schedule_stage3_ip_config_start (self);

out:
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 2 of 5 (Device Configure) complete.", iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_stage2_device_config
 *
 * Schedule setup of the hardware device
 *
 */
void
nm_device_activate_schedule_stage2_device_config (NMDevice *self)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->act_request);

	activation_source_schedule (self, nm_device_activate_stage2_device_config, 0);

	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 2 of 5 (Device Configure) scheduled...",
	         nm_device_get_iface (self));
}

/*********************************************/
/* avahi-autoipd stuff */

static void
aipd_timeout_remove (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->aipd_timeout) {
		g_source_remove (priv->aipd_timeout);
		priv->aipd_timeout = 0;
	}
}

static void
aipd_cleanup (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->aipd_watch) {
		g_source_remove (priv->aipd_watch);
		priv->aipd_watch = 0;
	}

	if (priv->aipd_pid > 0) {
		kill (priv->aipd_pid, SIGKILL);

		/* ensure the child is reaped */
		nm_log_dbg (LOGD_AUTOIP4, "waiting for avahi-autoipd pid %d to exit", priv->aipd_pid);
		waitpid (priv->aipd_pid, NULL, 0);
		nm_log_dbg (LOGD_AUTOIP4, "avahi-autoip pid %d cleaned up", priv->aipd_pid);

		priv->aipd_pid = -1;
	}

	aipd_timeout_remove (self);
}

static NMIP4Config *
aipd_get_ip4_config (NMDevice *self, struct in_addr lla)
{
	NMIP4Config *config = NULL;
	NMIP4Address *addr;
	NMIP4Route *route;

	config = nm_ip4_config_new ();
	g_assert (config);

	addr = nm_ip4_address_new ();
	nm_ip4_address_set_address (addr, (guint32) lla.s_addr);
	nm_ip4_address_set_prefix (addr, 16);
	nm_ip4_config_take_address (config, addr);

	/* Add a multicast route for link-local connections: destination= 224.0.0.0, netmask=240.0.0.0 */
	route = nm_ip4_route_new ();
	nm_ip4_route_set_dest (route, (guint32) htonl (0xE0000000L));
	nm_ip4_route_set_prefix (route, 4);
	nm_ip4_route_set_next_hop (route, (guint32) 0);
	nm_ip4_route_set_metric (route, 0);
	nm_ip4_config_take_route (config, route);

	return config;	
}

static void
autoip_changed (NMDevice *self,
                NMIP4Config *config,
                NMSettingIP4Config *s_ip4)
{
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;

	nm_utils_merge_ip4_config (config, s_ip4);
	if (!nm_device_set_ip4_config (self, config, FALSE, &reason)) {
		nm_log_err (LOGD_AUTOIP4, "(%s): failed to update IP4 config in response to autoip event.",
			        nm_device_get_iface (self));
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
	}
}

#define IPV4LL_NETWORK (htonl (0xA9FE0000L))
#define IPV4LL_NETMASK (htonl (0xFFFF0000L))

void
nm_device_handle_autoip4_event (NMDevice *self,
                                const char *event,
                                const char *address)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMConnection *connection = NULL;
	NMSettingIP4Config *s_ip4 = NULL;
	const char *iface, *method = NULL;

	g_return_if_fail (event != NULL);

	if (priv->act_request == NULL)
		return;

	connection = nm_act_request_get_connection (priv->act_request);
	g_assert (connection);

	/* Ignore if the connection isn't an AutoIP connection */
	s_ip4 = nm_connection_get_setting_ip4_config (connection);
	if (s_ip4)
		method = nm_setting_ip4_config_get_method (s_ip4);

	if (g_strcmp0 (method, NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL) != 0)
		return;

	iface = nm_device_get_iface (self);

	if (strcmp (event, "BIND") == 0) {
		struct in_addr lla;
		NMIP4Config *config;

		if (inet_pton (AF_INET, address, &lla) <= 0) {
			nm_log_err (LOGD_AUTOIP4, "(%s): invalid address %s received from avahi-autoipd.",
			            iface, address);
			nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_AUTOIP_ERROR);
			return;
		}

		if ((lla.s_addr & IPV4LL_NETMASK) != IPV4LL_NETWORK) {
			nm_log_err (LOGD_AUTOIP4, "(%s): invalid address %s received from avahi-autoipd (not link-local).",
			            iface, address);
			nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_AUTOIP_ERROR);
			return;
		}

		config = aipd_get_ip4_config (self, lla);
		if (config == NULL) {
			nm_log_err (LOGD_AUTOIP4, "failed to get autoip config");
			nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE);
			return;
		}

		if (priv->ip4_state == IP_CONF) {
			aipd_timeout_remove (self);
			nm_device_activate_schedule_ip4_config_result (self, config);
		} else if (priv->ip4_state == IP_DONE) {
			autoip_changed (self, config, s_ip4);
		} else
			g_assert_not_reached ();

		g_object_unref (config);
	} else {
		nm_log_warn (LOGD_AUTOIP4, "(%s): autoip address %s no longer valid because '%s'.",
		            iface, address, event);

		/* The address is gone; terminate the connection or fail activation */
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED);
	}
}

static void
aipd_watch_cb (GPid pid, gint status, gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMDeviceState state;
	const char *iface;

	if (!priv->aipd_watch)
		return;
	priv->aipd_watch = 0;

	iface = nm_device_get_iface (self);

	if (WIFEXITED (status)) {
		nm_log_dbg (LOGD_AUTOIP4, "(%s): avahi-autoipd exited with error code %d",
		            iface, WEXITSTATUS (status));
	} else if (WIFSTOPPED (status)) {
		nm_log_warn (LOGD_AUTOIP4, "(%s): avahi-autoipd stopped unexpectedly with signal %d",
		            iface, WSTOPSIG (status));
	} else if (WIFSIGNALED (status)) {
		nm_log_warn (LOGD_AUTOIP4, "(%s): avahi-autoipd died with signal %d",
		             iface, WTERMSIG (status));
	} else {
		nm_log_warn (LOGD_AUTOIP4, "(%s): avahi-autoipd died from an unknown cause", iface);
	}

	aipd_cleanup (self);

	state = nm_device_get_state (self);
	if (nm_device_is_activating (self) || (state == NM_DEVICE_STATE_ACTIVATED))
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_AUTOIP_FAILED);
}

static gboolean
aipd_timeout_cb (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->aipd_timeout) {
		nm_log_info (LOGD_AUTOIP4, "(%s): avahi-autoipd timed out.", nm_device_get_iface (self));
		priv->aipd_timeout = 0;
		aipd_cleanup (self);

		if (priv->ip4_state == IP_CONF)
			nm_device_activate_schedule_ip4_config_timeout (self);
	}

	return FALSE;
}

static void
aipd_child_setup (gpointer user_data G_GNUC_UNUSED)
{
	/* We are in the child process at this point.
	 * Give child it's own program group for signal
	 * separation.
	 */
	pid_t pid = getpid ();
	setpgid (pid, pid);
}

static NMActStageReturn
aipd_start (NMDevice *self, NMDeviceStateReason *reason)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	const char *iface = nm_device_get_iface (self);
	char *argv[6], *cmdline;
	const char **aipd_binary = NULL;
	static const char *aipd_paths[] = {
		"/usr/sbin/avahi-autoipd",
		"/usr/local/sbin/avahi-autoipd",
		NULL
	};
	int i = 0;
	GError *error = NULL;

	aipd_cleanup (self);

	/* Find avahi-autoipd */
	aipd_binary = aipd_paths;
	while (*aipd_binary != NULL) {
		if (g_file_test (*aipd_binary, G_FILE_TEST_EXISTS))
			break;
		aipd_binary++;
	}

	if (!*aipd_binary) {
		nm_log_warn (LOGD_DEVICE | LOGD_AUTOIP4,
		             "Activation (%s) Stage 3 of 5 (IP Configure Start) failed"
		             " to start avahi-autoipd: not found", iface);
		*reason = NM_DEVICE_STATE_REASON_AUTOIP_START_FAILED;
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	argv[i++] = (char *) (*aipd_binary);
	argv[i++] = "--script";
	argv[i++] = LIBEXECDIR "/nm-avahi-autoipd.action";
	if (nm_logging_level_enabled (LOGL_DEBUG))
		argv[i++] = "--debug";
	argv[i++] = (char *) nm_device_get_ip_iface (self);
	argv[i++] = NULL;

	cmdline = g_strjoinv (" ", argv);
	nm_log_dbg (LOGD_AUTOIP4, "running: %s", cmdline);
	g_free (cmdline);

	if (!g_spawn_async ("/", argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
	                    &aipd_child_setup, NULL, &(priv->aipd_pid), &error)) {
		nm_log_warn (LOGD_DEVICE | LOGD_AUTOIP4,
		             "Activation (%s) Stage 3 of 5 (IP Configure Start) failed"
		             " to start avahi-autoipd: %s",
		             iface,
		             error && error->message ? error->message : "(unknown)");
		g_clear_error (&error);
		aipd_cleanup (self);
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	nm_log_info (LOGD_DEVICE | LOGD_AUTOIP4,
	             "Activation (%s) Stage 3 of 5 (IP Configure Start) started"
	             " avahi-autoipd...", iface);

	/* Monitor the child process so we know when it dies */
	priv->aipd_watch = g_child_watch_add (priv->aipd_pid, aipd_watch_cb, self);

	/* Start a timeout to bound the address attempt */
	priv->aipd_timeout = g_timeout_add_seconds (20, aipd_timeout_cb, self);

	return NM_ACT_STAGE_RETURN_POSTPONE;
}

/*********************************************/
/* DHCPv4 stuff */

static void
dhcp4_add_option_cb (gpointer key, gpointer value, gpointer user_data)
{
	nm_dhcp4_config_add_option (NM_DHCP4_CONFIG (user_data),
	                            (const char *) key,
	                            (const char *) value);
}

static void
dhcp4_lease_change (NMDevice *device, NMIP4Config *config)
{
	NMConnection *connection;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;

	if (config == NULL) {
		nm_log_warn (LOGD_DHCP4, "(%s): failed to get DHCPv4 config for rebind",
		             nm_device_get_ip_iface (device));
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED);
		return;
	}

	connection = nm_device_get_connection (device);
	g_assert (connection);

	/* Merge with user overrides */
	nm_utils_merge_ip4_config (config, nm_connection_get_setting_ip4_config (connection));

	if (!nm_device_set_ip4_config (device, config, FALSE, &reason)) {
		nm_log_warn (LOGD_DHCP6, "(%s): failed to update IPv4 config in response to DHCP event.",
		             nm_device_get_ip_iface (device));
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, reason);
		return;
	}

	/* Notify dispatcher scripts of new DHCP4 config */
	nm_utils_call_dispatcher ("dhcp4-change", connection, device, NULL, NULL, NULL);
}

static void
dhcp4_fail (NMDevice *device, gboolean timeout)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);

	nm_dhcp4_config_reset (priv->dhcp4_config);

	if (timeout || (priv->ip4_state == IP_CONF))
		nm_device_activate_schedule_ip4_config_timeout (device);
	else if (priv->ip4_state == IP_DONE)
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED);
}

static void
dhcp4_state_changed (NMDHCPClient *client,
                     NMDHCPState state,
                     gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);
	NMDeviceState dev_state;
	NMIP4Config *config;

	g_return_if_fail (nm_dhcp_client_get_ipv6 (client) == FALSE);

	nm_log_dbg (LOGD_DHCP4, "(%s): new DHCPv4 client state %d",
	            nm_device_get_iface (device), state);

	dev_state = nm_device_get_state (device);

	switch (state) {
	case DHC_BOUND4:     /* lease obtained */
	case DHC_RENEW4:     /* lease renewed */
	case DHC_REBOOT:     /* have valid lease, but now obtained a different one */
	case DHC_REBIND4:    /* new, different lease */
		config = nm_dhcp_client_get_ip4_config (priv->dhcp4_client, FALSE);
		if (priv->ip4_state == IP_CONF)
			nm_device_activate_schedule_ip4_config_result (device, config);
		else if (priv->ip4_state == IP_DONE)
			dhcp4_lease_change (device, config);

		if (config) {
			/* Update the DHCP4 config object with new DHCP options */
			nm_dhcp4_config_reset (priv->dhcp4_config);
			nm_dhcp_client_foreach_option (priv->dhcp4_client,
			                               dhcp4_add_option_cb,
			                               priv->dhcp4_config);
			g_object_notify (G_OBJECT (device), NM_DEVICE_DHCP4_CONFIG);

			g_object_unref (config);
		}
		break;
	case DHC_TIMEOUT: /* timed out contacting DHCP server */
		dhcp4_fail (device, TRUE);
		break;
	case DHC_END: /* dhclient exited normally */
	case DHC_FAIL: /* all attempts to contact server timed out, sleeping */
	case DHC_ABEND: /* dhclient exited abnormally */
		/* dhclient quit and can't get/renew a lease; so kill the connection */
		dhcp4_fail (device, FALSE);
		break;
	default:
		break;
	}
}

static void
dhcp4_timeout (NMDHCPClient *client, gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);

	g_return_if_fail (nm_device_get_act_request (device) != NULL);
	g_return_if_fail (nm_dhcp_client_get_ipv6 (client) == FALSE);

	nm_dhcp_client_stop (client, FALSE);
	dhcp4_fail (device, TRUE);
}

static NMActStageReturn
dhcp4_start (NMDevice *self,
             NMConnection *connection,
             NMDeviceStateReason *reason)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMSettingIP4Config *s_ip4;
	guint8 *anycast = NULL;

	s_ip4 = nm_connection_get_setting_ip4_config (connection);

	if (priv->dhcp_anycast_address)
		anycast = priv->dhcp_anycast_address->data;

	/* Clear old exported DHCP options */
	if (priv->dhcp4_config)
		g_object_unref (priv->dhcp4_config);
	priv->dhcp4_config = nm_dhcp4_config_new ();

	/* Begin DHCP on the interface */
	g_warn_if_fail (priv->dhcp4_client == NULL);
	priv->dhcp4_client = nm_dhcp_manager_start_ip4 (priv->dhcp_manager,
	                                                nm_device_get_ip_iface (self),
	                                                nm_connection_get_uuid (connection),
	                                                s_ip4,
	                                                priv->dhcp_timeout,
	                                                anycast);
	if (!priv->dhcp4_client) {
		*reason = NM_DEVICE_STATE_REASON_DHCP_START_FAILED;
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	priv->dhcp4_state_sigid = g_signal_connect (priv->dhcp4_client,
	                                            "state-changed",
	                                            G_CALLBACK (dhcp4_state_changed),
	                                            self);
	priv->dhcp4_timeout_sigid = g_signal_connect (priv->dhcp4_client,
	                                              "timeout",
	                                              G_CALLBACK (dhcp4_timeout),
	                                              self);

	/* DHCP devices will be notified by the DHCP manager when stuff happens */
	return NM_ACT_STAGE_RETURN_POSTPONE;
}

gboolean
nm_device_dhcp4_renew (NMDevice *self, gboolean release)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMActStageReturn ret;
	NMDeviceStateReason reason;
	NMConnection *connection;

	g_return_val_if_fail (priv->dhcp4_client != NULL, FALSE);

	nm_log_info (LOGD_DHCP4, "(%s): DHCPv4 lease renewal requested",
	             nm_device_get_iface (self));

	/* Terminate old DHCP instance and release the old lease */
	dhcp4_cleanup (self, TRUE, release);

	connection = nm_device_get_connection (self);
	g_assert (connection);

	/* Start DHCP again on the interface */
	ret = dhcp4_start (self, connection, &reason);

	return (ret != NM_ACT_STAGE_RETURN_FAILURE);
}

/*********************************************/

static GHashTable *shared_ips = NULL;

static void
release_shared_ip (gpointer data)
{
	g_hash_table_remove (shared_ips, data);
}

static guint32
reserve_shared_ip (void)
{
	guint32 start = (guint32) ntohl (0x0a2a0001); /* 10.42.0.1 */
	guint32 count = 0;

	while (g_hash_table_lookup (shared_ips, GUINT_TO_POINTER (start + count))) {
		count += ntohl (0x100);
		if (count > ntohl (0xFE00)) {
			nm_log_err (LOGD_SHARING, "ran out of shared IP addresses!");
			return 0;
		}
	}

	g_hash_table_insert (shared_ips, GUINT_TO_POINTER (start + count), GUINT_TO_POINTER (TRUE));
	return start + count;
}

static NMIP4Config *
shared4_new_config (NMDevice *self, NMDeviceStateReason *reason)
{
	NMIP4Config *config = NULL;
	NMIP4Address *addr;
	guint32 tmp_addr;

	g_return_val_if_fail (self != NULL, NULL);

	if (G_UNLIKELY (shared_ips == NULL))
		shared_ips = g_hash_table_new (g_direct_hash, g_direct_equal);

	tmp_addr = reserve_shared_ip ();
	if (!tmp_addr) {
		*reason = NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE;
		return NULL;
	}

	config = nm_ip4_config_new ();
	addr = nm_ip4_address_new ();
	nm_ip4_address_set_address (addr, tmp_addr);
	nm_ip4_address_set_prefix (addr, 24);
	nm_ip4_config_take_address (config, addr);

	/* Remove the address lock when the object gets disposed */
	g_object_set_data_full (G_OBJECT (config), "shared-ip",
	                        GUINT_TO_POINTER (tmp_addr), release_shared_ip);

	return config;
}

/*********************************************/

static NMActStageReturn
real_act_stage3_ip4_config_start (NMDevice *self,
                                  NMIP4Config **out_config,
                                  NMDeviceStateReason *reason)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMConnection *connection;
	NMSettingIP4Config *s_ip4;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;
	const char *method = NM_SETTING_IP4_CONFIG_METHOD_AUTO;

	g_return_val_if_fail (reason != NULL, NM_ACT_STAGE_RETURN_FAILURE);

	connection = nm_device_get_connection (self);
	g_assert (connection);

	/* If we did not receive IP4 configuration information, default to DHCP */
	s_ip4 = nm_connection_get_setting_ip4_config (connection);
	if (s_ip4)
		method = nm_setting_ip4_config_get_method (s_ip4);
	else if (nm_connection_is_type (connection, NM_SETTING_BOND_SETTING_NAME))
		method = NM_SETTING_IP4_CONFIG_METHOD_DISABLED;

	/* Start IPv4 addressing based on the method requested */
	if (strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_AUTO) == 0)
		ret = dhcp4_start (self, connection, reason);
	else if (strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_LINK_LOCAL) == 0)
		ret = aipd_start (self, reason);
	else if (strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_MANUAL) == 0) {
		/* Use only IPv4 config from the connection data */
		*out_config = nm_ip4_config_new ();
		g_assert (*out_config);
		ret = NM_ACT_STAGE_RETURN_SUCCESS;
	} else if (strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_SHARED) == 0) {
		*out_config = shared4_new_config (self, reason);
		if (*out_config) {
			priv->dnsmasq_manager = nm_dnsmasq_manager_new (nm_device_get_ip_iface (self));
			ret = NM_ACT_STAGE_RETURN_SUCCESS;
		} else
			ret = NM_ACT_STAGE_RETURN_FAILURE;
	} else if (s_ip4 && !strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_DISABLED)) {
		/* Nothing to do... */
		ret = NM_ACT_STAGE_RETURN_STOP;
	} else {
		nm_log_warn (LOGD_IP4, "(%s): unhandled IPv4 config method; will fail",
		             nm_device_get_ip_iface (self));
	}

	return ret;
}

/*********************************************/
/* DHCPv6 stuff */

static void
dhcp6_add_option_cb (gpointer key, gpointer value, gpointer user_data)
{
	nm_dhcp6_config_add_option (NM_DHCP6_CONFIG (user_data),
	                            (const char *) key,
	                            (const char *) value);
}

static void
merge_ip6_configs (NMIP6Config *dst, NMIP6Config *src)
{
	guint32 i;

	g_return_if_fail (src != NULL);
	g_return_if_fail (dst != NULL);

	/* addresses */
	for (i = 0; i < nm_ip6_config_get_num_addresses (src); i++)
		nm_ip6_config_add_address (dst, nm_ip6_config_get_address (src, i));

	/* ptp address; only replace if src doesn't have one */
	if (!nm_ip6_config_get_ptp_address (dst))
		nm_ip6_config_set_ptp_address (dst, nm_ip6_config_get_ptp_address (src));

	/* nameservers */
	for (i = 0; i < nm_ip6_config_get_num_nameservers (src); i++)
		nm_ip6_config_add_nameserver (dst, nm_ip6_config_get_nameserver (src, i));

	/* routes */
	for (i = 0; i < nm_ip6_config_get_num_routes (src); i++)
		nm_ip6_config_add_route (dst, nm_ip6_config_get_route (src, i));

	/* domains */
	for (i = 0; i < nm_ip6_config_get_num_domains (src); i++)
		nm_ip6_config_add_domain (dst, nm_ip6_config_get_domain (src, i));

	/* dns searches */
	for (i = 0; i < nm_ip6_config_get_num_searches (src); i++)
		nm_ip6_config_add_search (dst, nm_ip6_config_get_search (src, i));

	if (!nm_ip6_config_get_mss (dst))
		nm_ip6_config_set_mss (dst, nm_ip6_config_get_mss (src));
}

static gboolean
ip6_config_merge_and_apply (NMDevice *self,
                            NMIP6Config *src_config,
                            NMDeviceStateReason *out_reason)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMConnection *connection;
	gboolean assumed, success;
	NMIP6Config *composite;

	connection = nm_device_get_connection (self);
	g_assert (connection);

	/* If no config was passed in, create a new one */
	composite = nm_ip6_config_new ();
	g_assert (composite);

	/* Merge in the given config first, if any */
	if (src_config)
		merge_ip6_configs (composite, src_config);

	/* Merge RA and DHCPv6 configs into the composite config */
	if (priv->ac_ip6_config && (src_config != priv->ac_ip6_config))
		merge_ip6_configs (composite, priv->ac_ip6_config);
	if (priv->dhcp6_ip6_config && (src_config != priv->dhcp6_ip6_config))
		merge_ip6_configs (composite, priv->dhcp6_ip6_config);

	/* Merge user overrides into the composite config */
	nm_utils_merge_ip6_config (composite, nm_connection_get_setting_ip6_config (connection));

	assumed = nm_act_request_get_assumed (priv->act_request);
	success = nm_device_set_ip6_config (self, composite, assumed, out_reason);
	g_object_unref (composite);
	return success;
}

static void
dhcp6_lease_change (NMDevice *device)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);
	NMConnection *connection;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;

	if (priv->dhcp6_ip6_config == NULL) {
		nm_log_warn (LOGD_DHCP6, "(%s): failed to get DHCPv6 config for rebind",
		             nm_device_get_ip_iface (device));
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED);
		return;
	}

	g_assert (priv->dhcp6_client);  /* sanity check */

	connection = nm_device_get_connection (device);
	g_assert (connection);

	/* Apply the updated config */
	if (ip6_config_merge_and_apply (device, NULL, &reason) == FALSE) {
		nm_log_warn (LOGD_DHCP6, "(%s): failed to update IPv6 config in response to DHCP event.",
		             nm_device_get_ip_iface (device));
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, reason);
	} else {
		/* Notify dispatcher scripts of new DHCPv6 config */
		nm_utils_call_dispatcher ("dhcp6-change", connection, device, NULL, NULL, NULL);
	}
}

static void
dhcp6_fail (NMDevice *device, gboolean timeout)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);

	nm_dhcp6_config_reset (priv->dhcp6_config);

	if (timeout || (priv->ip6_state == IP_CONF))
		nm_device_activate_schedule_ip6_config_timeout (device);
	else if (priv->ip6_state == IP_DONE)
		nm_device_state_changed (device, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED);
}

static void
dhcp6_state_changed (NMDHCPClient *client,
                     NMDHCPState state,
                     gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);
	NMDeviceState dev_state;

	g_return_if_fail (nm_dhcp_client_get_ipv6 (client) == TRUE);

	nm_log_dbg (LOGD_DHCP6, "(%s): new DHCPv6 client state %d",
	            nm_device_get_iface (device), state);

	dev_state = nm_device_get_state (device);

	switch (state) {
	case DHC_BOUND6:
	case DHC_RENEW6:     /* lease renewed */
	case DHC_REBOOT:     /* have valid lease, but now obtained a different one */
	case DHC_REBIND6:    /* new, different lease */
		if (priv->dhcp6_ip6_config)
			g_object_unref (priv->dhcp6_ip6_config);
		priv->dhcp6_ip6_config = nm_dhcp_client_get_ip6_config (priv->dhcp6_client, FALSE);
		if (priv->ip6_state == IP_CONF)
			nm_device_activate_schedule_ip6_config_result (device, priv->dhcp6_ip6_config);
		else if (priv->ip6_state == IP_DONE)
			dhcp6_lease_change (device);

		if (priv->dhcp6_ip6_config) {
			/* Update the DHCP6 config object with new DHCP options */
			nm_dhcp6_config_reset (priv->dhcp6_config);
			nm_dhcp_client_foreach_option (priv->dhcp6_client,
			                               dhcp6_add_option_cb,
			                               priv->dhcp6_config);
			g_object_notify (G_OBJECT (device), NM_DEVICE_DHCP6_CONFIG);
		}
		break;
	case DHC_TIMEOUT: /* timed out contacting DHCP server */
		dhcp6_fail (device, TRUE);
		break;
	case DHC_END: /* dhclient exited normally */
		/* In IPv6 info-only mode, the client doesn't handle leases so it
		 * may exit right after getting a response from the server.  That's
		 * normal.  In that case we just ignore the exit.
		 */
		if (priv->dhcp6_mode == IP6_DHCP_OPT_OTHERCONF)
			break;
		/* Otherwise, fall through */
	case DHC_FAIL: /* all attempts to contact server timed out, sleeping */
	case DHC_ABEND: /* dhclient exited abnormally */
		/* dhclient quit and can't get/renew a lease; so kill the connection */
		dhcp6_fail (device, FALSE);
		break;
	default:
		break;
	}
}

static void
dhcp6_timeout (NMDHCPClient *client, gpointer user_data)
{
	NMDevice *device = NM_DEVICE (user_data);

	g_return_if_fail (nm_device_get_act_request (device) != NULL);
	g_return_if_fail (nm_dhcp_client_get_ipv6 (client) == TRUE);

	nm_dhcp_client_stop (client, FALSE);
	dhcp6_fail (device, TRUE);
}

static NMActStageReturn
dhcp6_start (NMDevice *self,
             NMConnection *connection,
             guint32 dhcp_opt,
             NMDeviceStateReason *reason)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;
	guint8 *anycast = NULL;
	const char *ip_iface;
	const struct in6_addr dest = { { { 0xFF,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 } } };
	int err;

	if (!connection) {
		connection = nm_device_get_connection (self);
		g_assert (connection);
	}

	/* Begin a DHCP transaction on the interface */

	if (priv->dhcp_anycast_address)
		anycast = priv->dhcp_anycast_address->data;

	/* Clear old exported DHCP options */
	if (priv->dhcp6_config)
		g_object_unref (priv->dhcp6_config);
	priv->dhcp6_config = nm_dhcp6_config_new ();

	g_warn_if_fail (priv->dhcp6_ip6_config == NULL);
	if (priv->dhcp6_ip6_config) {
		g_object_unref (priv->dhcp6_ip6_config);
		priv->dhcp6_ip6_config = NULL;
	}

	/* DHCPv6 communicates with the DHCPv6 server via two multicast addresses,
	 * ff02::1:2 (link-scope) and ff05::1:3 (site-scope).  Make sure we have
	 * a multicast route (ff00::/8) for client <-> server communication.
	 */
	err = nm_system_set_ip6_route (priv->ip_iface ? priv->ip_ifindex : priv->ifindex,
	                               &dest, 8, NULL, 256, 0, RTPROT_BOOT, RT_TABLE_LOCAL, NULL);
	if (err && (err != -NLE_EXIST)) {
		nm_log_err (LOGD_DEVICE | LOGD_IP6,
		            "(%s): failed to add IPv6 multicast route: %s",
		            priv->ip_iface ? priv->ip_iface : priv->iface, nl_geterror (err));
	}

	ip_iface = nm_device_get_ip_iface (self);
	priv->dhcp6_client = nm_dhcp_manager_start_ip6 (priv->dhcp_manager,
	                                                ip_iface,
	                                                nm_connection_get_uuid (connection),
	                                                nm_connection_get_setting_ip6_config (connection),
	                                                priv->dhcp_timeout,
	                                                anycast,
	                                                (dhcp_opt == IP6_DHCP_OPT_OTHERCONF) ? TRUE : FALSE);
	if (priv->dhcp6_client) {
		priv->dhcp6_state_sigid = g_signal_connect (priv->dhcp6_client,
		                                            "state-changed",
		                                            G_CALLBACK (dhcp6_state_changed),
		                                            self);
		priv->dhcp6_timeout_sigid = g_signal_connect (priv->dhcp6_client,
		                                              "timeout",
		                                              G_CALLBACK (dhcp6_timeout),
		                                              self);

		/* DHCP devices will be notified by the DHCP manager when stuff happens */
		ret = NM_ACT_STAGE_RETURN_POSTPONE;
	} else {
		*reason = NM_DEVICE_STATE_REASON_DHCP_START_FAILED;
		ret = NM_ACT_STAGE_RETURN_FAILURE;
	}

	return ret;
}

/******************************************/

static void
ip6_addrconf_complete (NMIP6Manager *ip6_manager,
                       int ifindex,
                       guint dhcp_opts,
                       gboolean success,
                       gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMConnection *connection;
	NMActStageReturn ret;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;

	if (ifindex != nm_device_get_ip_ifindex (self))
		return;
	g_return_if_fail (priv->act_request != NULL);
	connection = nm_device_get_connection (self);
	g_assert (connection);

	if (!priv->ip6_waiting_for_config)
		return;

	priv->ip6_waiting_for_config = FALSE;

	if (!success) {
		nm_device_activate_schedule_ip6_config_timeout (self);
		return;
	}

	priv->dhcp6_mode = dhcp_opts;

	/* If addrconf is all that's required, we're done */
	if (priv->dhcp6_mode == IP6_DHCP_OPT_NONE) {
		priv->ac_ip6_config = nm_ip6_manager_get_ip6_config (ip6_manager, ifindex);
		nm_device_activate_schedule_ip6_config_result (self, priv->ac_ip6_config);
		return;
	}

	/* If the router said to use DHCP for managed or otherconf, do it */

	/* Don't re-start DHCPv6 if it's already in progress */
	if (priv->ip6_state != IP_CONF)
		return;

	nm_log_info (LOGD_DEVICE | LOGD_DHCP6,
	             "Activation (%s) Stage 3 of 5 (IP Configure Start) starting DHCPv6"
	             " as requested by IPv6 router...",
	             priv->iface);

	ret = dhcp6_start (self, connection, priv->dhcp6_mode, &reason);
	switch (ret) {
	case NM_ACT_STAGE_RETURN_SUCCESS:
		/* Shouldn't get this, but handle it anyway */
		g_warn_if_reached ();
		priv->ac_ip6_config = nm_ip6_manager_get_ip6_config (ip6_manager, ifindex);
		nm_device_activate_schedule_ip6_config_result (self, priv->ac_ip6_config);
		break;
	case NM_ACT_STAGE_RETURN_POSTPONE:
		/* Cache acquired autoconf config and wait for DHCPv6 to complete */
		priv->ac_ip6_config = nm_ip6_manager_get_ip6_config (ip6_manager, ifindex);
		break;
	default:
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		break;
	}
}

static void
ip6_config_changed (NMIP6Manager *ip6_manager,
                    int ifindex,
                    guint dhcp_opts,
                    gboolean success,
                    gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;

	if (ifindex != nm_device_get_ip_ifindex (self))
		return;
	g_return_if_fail (priv->act_request != NULL);

	/* If autoconf failed and IPv6 previously succeeded, fail */
	if (!success && (priv->ip6_state == IP_DONE)) {
		nm_device_state_changed (self,
		                         NM_DEVICE_STATE_FAILED,
		                         NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE);
		return;
	}

	/* FIXME: re-run DHCPv6 here to get any new nameservers or whatever */

	if (priv->ac_ip6_config)
		g_object_unref (priv->ac_ip6_config);
	priv->ac_ip6_config = nm_ip6_manager_get_ip6_config (ip6_manager, ifindex);

	if (ip6_config_merge_and_apply (self, NULL, &reason) == FALSE) {
		nm_log_warn (LOGD_DHCP6, "(%s): failed to update IPv6 config in response to Router Advertisement.",
		             nm_device_get_ip_iface (self));
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
	}
}

static gboolean
addrconf6_start (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMConnection *connection;
	gboolean success;

	connection = nm_device_get_connection (self);
	g_assert (connection);

	g_warn_if_fail (priv->ac_ip6_config == NULL);
	if (priv->ac_ip6_config) {
		g_object_unref (priv->ac_ip6_config);
		priv->ac_ip6_config = NULL;
	}

	if (!priv->ip6_manager) {
		priv->ip6_manager = nm_ip6_manager_get ();
		priv->ip6_addrconf_sigid = g_signal_connect (priv->ip6_manager,
		                                             "addrconf-complete",
		                                             G_CALLBACK (ip6_addrconf_complete),
		                                             self);
		priv->ip6_config_changed_sigid = g_signal_connect (priv->ip6_manager,
		                                                   "config-changed",
		                                                   G_CALLBACK (ip6_config_changed),
		                                                   self);
	}

	success = nm_ip6_manager_prepare_interface (priv->ip6_manager,
	                                            nm_device_get_ip_ifindex (self),
	                                            nm_connection_get_setting_ip6_config (connection),
	                                            priv->ip6_accept_ra_path);
	if (success) {
		priv->ip6_waiting_for_config = TRUE;
		nm_ip6_manager_begin_addrconf (priv->ip6_manager, nm_device_get_ip_ifindex (self));
	}

	return success;
}

static void
addrconf6_cleanup (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->ac_ip6_config) {
		g_object_unref (priv->ac_ip6_config);
		priv->ac_ip6_config = NULL;
	}

	if (!priv->ip6_manager)
		return;

	if (priv->ip6_addrconf_sigid) {
		g_signal_handler_disconnect (priv->ip6_manager,
		                             priv->ip6_addrconf_sigid);
		priv->ip6_addrconf_sigid = 0;
	}
	if (priv->ip6_config_changed_sigid) {
		g_signal_handler_disconnect (priv->ip6_manager,
		                             priv->ip6_config_changed_sigid);
		priv->ip6_config_changed_sigid = 0;
	}

	nm_ip6_manager_cancel_addrconf (priv->ip6_manager, nm_device_get_ip_ifindex (self));
	g_object_unref (priv->ip6_manager);
	priv->ip6_manager = NULL;
}

/******************************************/

static NMActStageReturn
real_act_stage3_ip6_config_start (NMDevice *self,
                                  NMIP6Config **out_config,
                                  NMDeviceStateReason *reason)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	const char *ip_iface;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;
	NMConnection *connection;

	g_return_val_if_fail (reason != NULL, NM_ACT_STAGE_RETURN_FAILURE);

	connection = nm_device_get_connection (self);
	g_assert (connection);

	ip_iface = nm_device_get_ip_iface (self);

	update_accept_ra_save (self);

	priv->dhcp6_mode = IP6_DHCP_OPT_NONE;

	if (   ip6_method_matches (connection, NM_SETTING_IP6_CONFIG_METHOD_AUTO)
	    || ip6_method_matches (connection, NM_SETTING_IP6_CONFIG_METHOD_LINK_LOCAL)) {
		if (!addrconf6_start (self)) {
			*reason = NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE;
			ret = NM_ACT_STAGE_RETURN_FAILURE;
		} else
			ret = NM_ACT_STAGE_RETURN_POSTPONE;
	} else if (ip6_method_matches (connection, NM_SETTING_IP6_CONFIG_METHOD_DHCP)) {
		/* Router advertisements shouldn't be used in pure DHCP mode */
		if (priv->ip6_accept_ra_path)
			nm_utils_do_sysctl (priv->ip6_accept_ra_path, "0\n");

		priv->dhcp6_mode = IP6_DHCP_OPT_MANAGED;
		ret = dhcp6_start (self, connection, priv->dhcp6_mode, reason);
	} else if (ip6_method_matches (connection, NM_SETTING_IP6_CONFIG_METHOD_IGNORE)) {
		/* reset the saved RA value when ipv6 is ignored */
		if (priv->ip6_accept_ra_path) {
			nm_utils_do_sysctl (priv->ip6_accept_ra_path,
			                    priv->ip6_accept_ra_save ? "1\n" : "0\n");
		}
		ret = NM_ACT_STAGE_RETURN_STOP;
	} else if (ip6_method_matches (connection, NM_SETTING_IP6_CONFIG_METHOD_MANUAL)) {
		/* New blank config */
		*out_config = nm_ip6_config_new ();
		g_assert (*out_config);

		/* Router advertisements shouldn't be used in manual mode */
		if (priv->ip6_accept_ra_path)
			nm_utils_do_sysctl (priv->ip6_accept_ra_path, "0\n");
		ret = NM_ACT_STAGE_RETURN_SUCCESS;
	} else {
		nm_log_warn (LOGD_IP6, "(%s): unhandled IPv6 config method; will fail",
		             nm_device_get_ip_iface (self));
	}

	/* Other methods (shared) aren't implemented yet */

	return ret;
}


/*
 * nm_device_activate_stage3_ip_config_start
 *
 * Begin automatic/manual IP configuration
 *
 */
static gboolean
nm_device_activate_stage3_ip_config_start (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	const char *iface;
	NMActStageReturn ret;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;
	NMIP4Config *ip4_config = NULL;
	NMIP6Config *ip6_config = NULL;
	int ifindex;

	/* Clear the activation source ID now that this stage has run */
	activation_source_clear (self, FALSE, 0);

	iface = nm_device_get_iface (self);
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 3 of 5 (IP Configure Start) started...", iface);
	nm_device_state_changed (self, NM_DEVICE_STATE_IP_CONFIG, NM_DEVICE_STATE_REASON_NONE);

	/* Make sure the interface is up before trying to do anything with it */
	ifindex = nm_device_get_ip_ifindex (self);
	if ((ifindex > 0) && (nm_system_iface_is_up (ifindex) == FALSE))
		nm_system_iface_set_up (ifindex, TRUE, NULL);

	priv->ip4_state = priv->ip6_state = IP_CONF;

	ret = NM_DEVICE_GET_CLASS (self)->act_stage3_ip4_config_start (self, &ip4_config, &reason);
	if (ret == NM_ACT_STAGE_RETURN_SUCCESS) {
		g_assert (ip4_config);
		nm_device_activate_schedule_ip4_config_result (self, ip4_config);
		g_object_unref (ip4_config);
	} else if (ret == NM_ACT_STAGE_RETURN_FAILURE) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		goto out;
	} else if (ret == NM_ACT_STAGE_RETURN_STOP) {
		/* Nothing to do */
		priv->ip4_state = IP_DONE;
	} else
		g_assert (ret == NM_ACT_STAGE_RETURN_POSTPONE);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage3_ip6_config_start (self, &ip6_config, &reason);
	if (ret == NM_ACT_STAGE_RETURN_SUCCESS) {
		g_assert (ip6_config);
		nm_device_activate_schedule_ip6_config_result (self, ip6_config);
		g_object_unref (ip6_config);
	} else if (ret == NM_ACT_STAGE_RETURN_FAILURE) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		goto out;
	} else if (ret == NM_ACT_STAGE_RETURN_STOP) {
		/* Nothing to do */
		priv->ip6_state = IP_DONE;
	} else
		g_assert (ret == NM_ACT_STAGE_RETURN_POSTPONE);

out:
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 3 of 5 (IP Configure Start) complete.", iface);

	/* Handle interfaces (bond slaves, etc) that won't have any IP config; they
	 * need to move to ACTIVATED.
	 */
	if (priv->ip4_state == IP_DONE && priv->ip6_state == IP_DONE) {
		/* FIXME: call layer2 stuff to set MTU? */
		nm_device_state_changed (self, NM_DEVICE_STATE_ACTIVATED, NM_DEVICE_STATE_REASON_NONE);
	}

	return FALSE;
}


/*
 * nm_device_activate_schedule_stage3_ip_config_start
 *
 * Schedule IP configuration start
 */
void
nm_device_activate_schedule_stage3_ip_config_start (NMDevice *self)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->act_request);

	activation_source_schedule (self, nm_device_activate_stage3_ip_config_start, 0);

	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 3 of 5 (IP Configure Start) scheduled.",
	             nm_device_get_iface (self));
}

static NMActStageReturn
real_act_stage4_ip4_config_timeout (NMDevice *self, NMDeviceStateReason *reason)
{
	if (nm_device_ip_config_should_fail (self, FALSE)) {
		*reason = NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE;
		return NM_ACT_STAGE_RETURN_FAILURE;
	}
	return NM_ACT_STAGE_RETURN_SUCCESS;
}


/*
 * nm_device_activate_stage4_ip4_config_timeout
 *
 * Time out on retrieving the IPv4 config.
 *
 */
static gboolean
nm_device_activate_ip4_config_timeout (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	const char *iface;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;

	/* Clear the activation source ID now that this stage has run */
	activation_source_clear (self, FALSE, AF_INET);

	iface = nm_device_get_iface (self);
	nm_log_info (LOGD_DEVICE | LOGD_IP4,
	             "Activation (%s) Stage 4 of 5 (IPv4 Configure Timeout) started...",
	             iface);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage4_ip4_config_timeout (self, &reason);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE)
		goto out;
	else if (ret == NM_ACT_STAGE_RETURN_FAILURE) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);	

	priv->ip4_state = IP_DONE;

	/* If IPv4 failed and IPv6 failed, the activation fails */
	if ((priv->ip6_state == IP_DONE) && (priv->ip6_config == NULL)) {
		nm_device_state_changed (self,
				                 NM_DEVICE_STATE_FAILED,
				                 NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE);
	}

out:
	nm_log_info (LOGD_DEVICE | LOGD_IP4,
	             "Activation (%s) Stage 4 of 5 (IPv4 Configure Timeout) complete.",
	             iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_ip4_config_timeout
 *
 * Deal with a timeout of the IPv4 configuration
 *
 */
void
nm_device_activate_schedule_ip4_config_timeout (NMDevice *self)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->act_request);

	activation_source_schedule (self, nm_device_activate_ip4_config_timeout, AF_INET);

	nm_log_info (LOGD_DEVICE | LOGD_IP4,
	             "Activation (%s) Stage 4 of 5 (IPv4 Configure Timeout) scheduled...",
	             nm_device_get_iface (self));
}


static NMActStageReturn
real_act_stage4_ip6_config_timeout (NMDevice *self, NMDeviceStateReason *reason)
{
	if (nm_device_ip_config_should_fail (self, TRUE)) {
		*reason = NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE;
		return NM_ACT_STAGE_RETURN_FAILURE;
	}

	return NM_ACT_STAGE_RETURN_SUCCESS;
}


/*
 * nm_device_activate_ip6_config_timeout
 *
 * Time out on retrieving the IPv6 config.
 *
 */
static gboolean
nm_device_activate_ip6_config_timeout (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	const char *iface;
	NMActStageReturn ret = NM_ACT_STAGE_RETURN_FAILURE;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;

	/* Clear the activation source ID now that this stage has run */
	activation_source_clear (self, FALSE, AF_INET6);

	iface = nm_device_get_iface (self);
	nm_log_info (LOGD_DEVICE | LOGD_IP6,
	             "Activation (%s) Stage 4 of 5 (IPv6 Configure Timeout) started...",
	             iface);

	ret = NM_DEVICE_GET_CLASS (self)->act_stage4_ip6_config_timeout (self, &reason);
	if (ret == NM_ACT_STAGE_RETURN_POSTPONE)
		goto out;
	else if (ret == NM_ACT_STAGE_RETURN_FAILURE) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		goto out;
	}
	g_assert (ret == NM_ACT_STAGE_RETURN_SUCCESS);

	priv->ip6_state = IP_DONE;

	/* If IPv6 failed and IPv4 failed, the activation fails */
	if ((priv->ip4_state == IP_DONE) && (priv->ip4_config == NULL)) {
		nm_device_state_changed (self,
				                 NM_DEVICE_STATE_FAILED,
				                 NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE);
	}

out:
	nm_log_info (LOGD_DEVICE | LOGD_IP6,
	             "Activation (%s) Stage 4 of 5 (IPv6 Configure Timeout) complete.",
	             iface);
	return FALSE;
}


/*
 * nm_device_activate_schedule_ip6_config_timeout
 *
 * Deal with a timeout of the IPv6 configuration
 *
 */
void
nm_device_activate_schedule_ip6_config_timeout (NMDevice *self)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->act_request);

	activation_source_schedule (self, nm_device_activate_ip6_config_timeout, AF_INET6);

	nm_log_info (LOGD_DEVICE | LOGD_IP6,
	             "Activation (%s) Stage 4 of 5 (IPv6 Configure Timeout) scheduled...",
	             nm_device_get_iface (self));
}

static void
share_child_setup (gpointer user_data G_GNUC_UNUSED)
{
	/* We are in the child process at this point */
	pid_t pid = getpid ();
	setpgid (pid, pid);
}

static gboolean
share_init (void)
{
	int status;
	char *modules[] = { "ip_tables", "iptable_nat", "nf_nat_ftp", "nf_nat_irc",
	                    "nf_nat_sip", "nf_nat_tftp", "nf_nat_pptp", "nf_nat_h323",
	                    NULL };
	char **iter;

	if (!nm_utils_do_sysctl ("/proc/sys/net/ipv4/ip_forward", "1\n")) {
		nm_log_err (LOGD_SHARING, "Error starting IP forwarding: (%d) %s",
					errno, strerror (errno));
		return FALSE;
	}

	if (!nm_utils_do_sysctl ("/proc/sys/net/ipv4/ip_dynaddr", "1\n")) {
		nm_log_err (LOGD_SHARING, "error starting IP forwarding: (%d) %s",
					errno, strerror (errno));
	}

	for (iter = modules; *iter; iter++) {
		char *argv[3] = { "/sbin/modprobe", *iter, NULL };
		char *envp[1] = { NULL };
		GError *error = NULL;

		if (!g_spawn_sync ("/", argv, envp, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
		                   share_child_setup, NULL, NULL, NULL, &status, &error)) {
			nm_log_err (LOGD_SHARING, "error loading NAT module %s: (%d) %s",
			            *iter, error ? error->code : 0,
			            (error && error->message) ? error->message : "unknown");
			if (error)
				g_error_free (error);
		}
	}

	return TRUE;
}

static void
add_share_rule (NMActRequest *req, const char *table, const char *fmt, ...)
{
	va_list args;
	char *cmd;

	va_start (args, fmt);
	cmd = g_strdup_vprintf (fmt, args);
	va_end (args);

	nm_act_request_add_share_rule (req, table, cmd);
	g_free (cmd);
}

static gboolean
start_sharing (NMDevice *self, NMIP4Config *config)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMActRequest *req;
	GError *error = NULL;
	char str_addr[INET_ADDRSTRLEN + 1];
	char str_mask[INET_ADDRSTRLEN + 1];
	guint32 netmask, network;
	NMIP4Address *ip4_addr;
	const char *ip_iface;

	g_return_val_if_fail (config != NULL, FALSE);

	ip_iface = nm_device_get_ip_iface (self);

	ip4_addr = nm_ip4_config_get_address (config, 0);
	if (!ip4_addr || !nm_ip4_address_get_address (ip4_addr))
		return FALSE;

	netmask = nm_utils_ip4_prefix_to_netmask (nm_ip4_address_get_prefix (ip4_addr));
	if (!inet_ntop (AF_INET, &netmask, str_mask, sizeof (str_mask)))
		return FALSE;

	network = nm_ip4_address_get_address (ip4_addr) & netmask;
	if (!inet_ntop (AF_INET, &network, str_addr, sizeof (str_addr)))
		return FALSE;

	if (!share_init ())
		return FALSE;

	req = nm_device_get_act_request (self);
	g_assert (req);

	add_share_rule (req, "filter", "INPUT --in-interface %s --protocol tcp --destination-port 53 --jump ACCEPT", ip_iface);
	add_share_rule (req, "filter", "INPUT --in-interface %s --protocol udp --destination-port 53 --jump ACCEPT", ip_iface);
	add_share_rule (req, "filter", "INPUT --in-interface %s --protocol tcp --destination-port 67 --jump ACCEPT", ip_iface);
	add_share_rule (req, "filter", "INPUT --in-interface %s --protocol udp --destination-port 67 --jump ACCEPT", ip_iface);
	add_share_rule (req, "filter", "FORWARD --in-interface %s --jump REJECT", ip_iface);
	add_share_rule (req, "filter", "FORWARD --out-interface %s --jump REJECT", ip_iface);
	add_share_rule (req, "filter", "FORWARD --in-interface %s --out-interface %s --jump ACCEPT", ip_iface, ip_iface);
	add_share_rule (req, "filter", "FORWARD --source %s/%s --in-interface %s --jump ACCEPT", str_addr, str_mask, ip_iface);
	add_share_rule (req, "filter", "FORWARD --destination %s/%s --out-interface %s --match state --state ESTABLISHED,RELATED --jump ACCEPT", str_addr, str_mask, ip_iface);
	add_share_rule (req, "nat", "POSTROUTING --source %s/%s ! --destination %s/%s --jump MASQUERADE", str_addr, str_mask, str_addr, str_mask);

	nm_act_request_set_shared (req, TRUE);

	if (!nm_dnsmasq_manager_start (priv->dnsmasq_manager, config, &error)) {
		nm_log_err (LOGD_SHARING, "(%s/%s): failed to start dnsmasq: %s",
		            nm_device_get_iface (self), ip_iface, error->message);
		g_error_free (error);
		nm_act_request_set_shared (req, FALSE);
		return FALSE;
	}

	priv->dnsmasq_state_id = g_signal_connect (priv->dnsmasq_manager, "state-changed",
	                                           G_CALLBACK (dnsmasq_state_changed_cb),
	                                           self);
	return TRUE;
}

static gboolean
nm_device_activate_ip4_config_commit (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMActRequest *req;
	NMIP4Config *config = NULL;
	const char *iface, *method = NULL;
	NMConnection *connection;
	NMSettingIP4Config *s_ip4;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;
	gboolean assumed;
	int ifindex;

	/* Clear the activation source ID now that this stage has run */
	activation_source_clear (self, FALSE, AF_INET);

	iface = nm_device_get_iface (self);
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 5 of 5 (IPv4 Commit) started...",
	             iface);

	req = nm_device_get_act_request (self);
	g_assert (req);
	connection = nm_act_request_get_connection (req);
	g_assert (connection);

	config = g_object_get_data (G_OBJECT (req), PENDING_IP4_CONFIG);
	g_assert (config);

	/* Make sure the interface is up again just because */
	ifindex = nm_device_get_ip_ifindex (self);
	if ((ifindex > 0) && (nm_system_iface_is_up (ifindex) == FALSE))
		nm_system_iface_set_up (ifindex, TRUE, NULL);

	/* Allow setting MTU etc */
	if (NM_DEVICE_GET_CLASS (self)->ip4_config_pre_commit)
		NM_DEVICE_GET_CLASS (self)->ip4_config_pre_commit (self, config);

	/* Merge with user overrides */
	nm_utils_merge_ip4_config (config, nm_connection_get_setting_ip4_config (connection));

	assumed = nm_act_request_get_assumed (priv->act_request);
	if (!nm_device_set_ip4_config (self, config, assumed, &reason)) {
		nm_log_info (LOGD_DEVICE | LOGD_IP4,
			         "Activation (%s) Stage 5 of 5 (IPv4 Commit) failed",
					 iface);
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
		goto out;
	}

	/* Start IPv4 sharing if we need it */
	s_ip4 = nm_connection_get_setting_ip4_config (connection);
	if (s_ip4)
		method = nm_setting_ip4_config_get_method (s_ip4);

	if (g_strcmp0 (method, NM_SETTING_IP4_CONFIG_METHOD_SHARED) == 0) {
		if (!start_sharing (self, config)) {
			nm_log_warn (LOGD_SHARING, "Activation (%s) Stage 5 of 5 (IPv4 Commit) start sharing failed.", iface);
			nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_SHARED_START_FAILED);
			goto out;
		}
	}

	/* Enter the ACTIVATED state if this is the first method to complete */
	priv->ip4_state = IP_DONE;
	if (nm_device_get_state (self) == NM_DEVICE_STATE_IP_CONFIG)
		nm_device_state_changed (self, NM_DEVICE_STATE_ACTIVATED, NM_DEVICE_STATE_REASON_NONE);

out:
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 5 of 5 (IPv4 Commit) complete.",
	             iface);

	/* Balance IP config creation; nm_device_set_ip4_config() takes a reference */
	g_object_set_data (G_OBJECT (req), PENDING_IP4_CONFIG, NULL);

	return FALSE;
}

static void
fw_add_to_zone_cb (GError *error,
                   gpointer user_data1,
                   gpointer user_data2)
{
	NMDevice *self = NM_DEVICE (user_data1);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	int family = GPOINTER_TO_INT (user_data2);
	char ipver = 'x';
	guint32 logd = LOGD_NONE;

	priv->fw_call = NULL;

	if (error) {
		/* FIXME: fail the device activation? */
	}

	if (family == AF_INET) {
		activation_source_schedule (self, nm_device_activate_ip4_config_commit, AF_INET);
		ipver = '4';
		logd = LOGD_IP4;
	} else if (family == AF_INET6) {
		activation_source_schedule (self, nm_device_activate_ip6_config_commit, AF_INET6);
		ipver = '6';
		logd = LOGD_IP6;
	} else
		g_assert_not_reached ();

	nm_log_info (LOGD_DEVICE | logd,
	             "Activation (%s) Stage 5 of 5 (IPv%c Configure Commit) scheduled...",
	             nm_device_get_iface (self), ipver);
}

static void
fw_add_to_zone (NMDevice *self, int family)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMConnection *connection;
	NMSettingConnection *s_con = NULL;

	/* Only set the interface's zone if the device isn't yet activated.  If
	 * already activated, the zone has already been set.
	 */
	if (nm_device_get_state (self) == NM_DEVICE_STATE_ACTIVATED) {
		fw_add_to_zone_cb (NULL, self, GINT_TO_POINTER (family));
		return;
	}

	/* Otherwise tell the firewall to add the interface to the specified zone */
	connection = nm_device_get_connection (self);
	g_assert (connection);
	s_con = nm_connection_get_setting_connection (connection);
	priv->fw_call = nm_firewall_manager_add_to_zone (priv->fw_manager,
	                                                 nm_device_get_ip_iface (self),
	                                                 nm_setting_connection_get_zone (s_con),
	                                                 fw_add_to_zone_cb,
	                                                 self,
	                                                 GINT_TO_POINTER (family));
}

void
nm_device_activate_schedule_ip4_config_result (NMDevice *self, NMIP4Config *config)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->act_request);

	if (config == NULL) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE);
		return;
	}

	g_object_set_data_full (G_OBJECT (priv->act_request),
	                        PENDING_IP4_CONFIG,
	                        g_object_ref (config),
	                        g_object_unref);

	fw_add_to_zone (self, AF_INET);
}

gboolean
nm_device_activate_ip4_state_in_conf (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, FALSE);
	return NM_DEVICE_GET_PRIVATE (self)->ip4_state == IP_CONF;
}

static gboolean
nm_device_activate_ip6_config_commit (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMActRequest *req;
	NMIP6Config *config = NULL;
	const char *iface;
	NMConnection *connection;
	NMDeviceStateReason reason = NM_DEVICE_STATE_REASON_NONE;
	int ifindex;

	/* Clear the activation source ID now that this stage has run */
	activation_source_clear (self, FALSE, AF_INET6);

	iface = nm_device_get_iface (self);
	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 5 of 5 (IPv6 Commit) started...",
	             iface);

	req = nm_device_get_act_request (self);
	g_assert (req);
	connection = nm_act_request_get_connection (req);
	g_assert (connection);

	config = g_object_get_data (G_OBJECT (req), PENDING_IP6_CONFIG);
	g_assert (config);

	/* Make sure the interface is up again just because */
	ifindex = nm_device_get_ip_ifindex (self);
	if ((ifindex > 0) && (nm_system_iface_is_up (ifindex) == FALSE))
		nm_system_iface_set_up (ifindex, TRUE, NULL);

	/* Allow setting MTU etc */
	if (NM_DEVICE_GET_CLASS (self)->ip6_config_pre_commit)
		NM_DEVICE_GET_CLASS (self)->ip6_config_pre_commit (self, config);

	if (ip6_config_merge_and_apply (self, config, &reason)) {
		/* Enter the ACTIVATED state if this is the first method to complete */
		priv->ip6_state = IP_DONE;
		if (nm_device_get_state (self) == NM_DEVICE_STATE_IP_CONFIG)
			nm_device_state_changed (self, NM_DEVICE_STATE_ACTIVATED, NM_DEVICE_STATE_REASON_NONE);
	} else {
		nm_log_info (LOGD_DEVICE | LOGD_IP6,
			         "Activation (%s) Stage 5 of 5 (IPv6 Commit) failed",
					 iface);
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, reason);
	}

	nm_log_info (LOGD_DEVICE, "Activation (%s) Stage 5 of 5 (IPv6 Commit) complete.",
	             iface);

	/* Balance IP config creation; nm_device_set_ip6_config() takes a reference */
	g_object_set_data (G_OBJECT (req), PENDING_IP6_CONFIG, NULL);

	return FALSE;
}

void
nm_device_activate_schedule_ip6_config_result (NMDevice *self, NMIP6Config *config)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	g_return_if_fail (priv->act_request);

	if (config == NULL) {
		nm_device_state_changed (self, NM_DEVICE_STATE_FAILED, NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE);
		return;
	}

	/* Save the pending config */
	g_object_set_data_full (G_OBJECT (priv->act_request),
	                        PENDING_IP6_CONFIG,
	                        g_object_ref (config),
	                        g_object_unref);

	fw_add_to_zone (self, AF_INET6);
}

gboolean
nm_device_activate_ip6_state_in_conf (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, FALSE);
	return NM_DEVICE_GET_PRIVATE (self)->ip6_state == IP_CONF;
}

static void
clear_act_request (NMDevice *self)
{
	NMDevicePrivate * priv;

	g_return_if_fail (self != NULL);

	priv = NM_DEVICE_GET_PRIVATE (self);

	if (!priv->act_request)
		return;

	if (priv->secrets_updated_id) {
		g_signal_handler_disconnect (priv->act_request,
		                             priv->secrets_updated_id);
		priv->secrets_updated_id = 0;
	}

	if (priv->secrets_failed_id) {
		g_signal_handler_disconnect (priv->act_request,
		                             priv->secrets_failed_id);
		priv->secrets_failed_id = 0;
	}

	nm_act_request_set_default (priv->act_request, FALSE);

	g_object_unref (priv->act_request);
	priv->act_request = NULL;
}

static void
queued_state_clear (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->queued_state.id) {
		nm_log_dbg (LOGD_DEVICE, "(%s): clearing queued state transition (id %d)",
		            nm_device_get_iface (self), priv->queued_state.id);
		g_source_remove (priv->queued_state.id);
	}
	memset (&priv->queued_state, 0, sizeof (priv->queued_state));
}

static void
dhcp4_cleanup (NMDevice *self, gboolean stop, gboolean release)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->dhcp4_config) {
		g_object_notify (G_OBJECT (self), NM_DEVICE_DHCP4_CONFIG);
		g_object_unref (priv->dhcp4_config);
		priv->dhcp4_config = NULL;
	}

	if (priv->dhcp4_client) {
		/* Stop any ongoing DHCP transaction on this device */
		if (priv->dhcp4_state_sigid) {
			g_signal_handler_disconnect (priv->dhcp4_client, priv->dhcp4_state_sigid);
			priv->dhcp4_state_sigid = 0;
		}

		if (priv->dhcp4_timeout_sigid) {
			g_signal_handler_disconnect (priv->dhcp4_client, priv->dhcp4_timeout_sigid);
			priv->dhcp4_timeout_sigid = 0;
		}

		if (stop)
			nm_dhcp_client_stop (priv->dhcp4_client, release);

		g_object_unref (priv->dhcp4_client);
		priv->dhcp4_client = NULL;
	}
}

static void
dhcp6_cleanup (NMDevice *self, gboolean stop, gboolean release)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	priv->dhcp6_mode = IP6_DHCP_OPT_NONE;

	if (priv->dhcp6_ip6_config) {
		g_object_unref (priv->dhcp6_ip6_config);
		priv->dhcp6_ip6_config = NULL;
	}

	if (priv->dhcp6_config) {
		g_object_notify (G_OBJECT (self), NM_DEVICE_DHCP6_CONFIG);
		g_object_unref (priv->dhcp6_config);
		priv->dhcp6_config = NULL;
	}

	if (priv->dhcp6_client) {
		if (priv->dhcp6_state_sigid) {
			g_signal_handler_disconnect (priv->dhcp6_client, priv->dhcp6_state_sigid);
			priv->dhcp6_state_sigid = 0;
		}

		if (priv->dhcp6_timeout_sigid) {
			g_signal_handler_disconnect (priv->dhcp6_client, priv->dhcp6_timeout_sigid);
			priv->dhcp6_timeout_sigid = 0;
		}

		if (stop)
			nm_dhcp_client_stop (priv->dhcp6_client, release);

		g_object_unref (priv->dhcp6_client);
		priv->dhcp6_client = NULL;
	}
}

static void
dnsmasq_cleanup (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (!priv->dnsmasq_manager)
		return;

	if (priv->dnsmasq_state_id) {
		g_signal_handler_disconnect (priv->dnsmasq_manager, priv->dnsmasq_state_id);
		priv->dnsmasq_state_id = 0;
	}

	nm_dnsmasq_manager_stop (priv->dnsmasq_manager);
	g_object_unref (priv->dnsmasq_manager);
	priv->dnsmasq_manager = NULL;
}

static void
_update_ip4_address (NMDevice *self)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	struct ifreq req;
	guint32 new_address;
	int fd;

	g_return_if_fail (self  != NULL);

	fd = socket (PF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		nm_log_err (LOGD_IP4, "couldn't open control socket.");
		return;
	}

	memset (&req, 0, sizeof (struct ifreq));
	strncpy (req.ifr_name, nm_device_get_ip_iface (self), IFNAMSIZ);
	if (ioctl (fd, SIOCGIFADDR, &req) == 0) {
		new_address = ((struct sockaddr_in *)(&req.ifr_addr))->sin_addr.s_addr;
		if (new_address != priv->ip4_address)
			priv->ip4_address = new_address;
	}
	close (fd);
}

/*
 * nm_device_deactivate
 *
 * Remove a device's routing table entries and IP address.
 *
 */
static void
nm_device_deactivate (NMDevice *self, NMDeviceStateReason reason)
{
	NMDevicePrivate *priv;
	NMDeviceStateReason ignored = NM_DEVICE_STATE_REASON_NONE;
	NMDevice *master;
	gboolean tried_ipv6 = FALSE;
	int ifindex, family;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_DEVICE (self));

	nm_log_info (LOGD_DEVICE, "(%s): deactivating device (reason '%s') [%d]",
	             nm_device_get_iface (self), reason_to_string (reason), reason);

	/* Save whether or not we tried IPv6 for later */
	priv = NM_DEVICE_GET_PRIVATE (self);
	if (priv->ip6_manager || priv->ip6_config)
		tried_ipv6 = TRUE;

	/* Clean up when device was deactivated during call to firewall */
	if (priv->fw_call) {
		nm_firewall_manager_cancel_add (priv->fw_manager, priv->fw_call);
		priv->fw_call = NULL;
	}

	/* Break the activation chain */
	activation_source_clear (self, TRUE, AF_INET);
	activation_source_clear (self, TRUE, AF_INET6);

	/* Clear any queued transitions */
	queued_state_clear (self);

	priv->ip4_state = priv->ip6_state = IP_NONE;

	dhcp4_cleanup (self, TRUE, FALSE);
	dhcp6_cleanup (self, TRUE, FALSE);
	addrconf6_cleanup (self);
	dnsmasq_cleanup (self);
	aipd_cleanup (self);

	nm_device_set_ip_iface (self, NULL);

	/* Turn off router advertisements until they are needed */
	if (priv->ip6_accept_ra_path)
		nm_utils_do_sysctl (priv->ip6_accept_ra_path, "0\n");

	/* Call device type-specific deactivation */
	if (NM_DEVICE_GET_CLASS (self)->deactivate)
		NM_DEVICE_GET_CLASS (self)->deactivate (self);

	if ((master = nm_device_get_master (self)))
		handle_slave_deactivation (self, master);

	/* Tear down an existing activation request */
	clear_act_request (self);

	/* Take out any entries in the routing table and any IP address the device had. */
	ifindex = nm_device_get_ip_ifindex (self);
	family = tried_ipv6 ? AF_UNSPEC : AF_INET;
	if (ifindex >= 0) {
		nm_system_iface_flush_routes (ifindex, family);
		nm_system_iface_flush_addresses (ifindex, family);
	}
	_update_ip4_address (self);

	/* Clean up nameservers and addresses */
	nm_device_set_ip4_config (self, NULL, FALSE, &ignored);
	nm_device_set_ip6_config (self, NULL, FALSE, &ignored);
}

gboolean
nm_device_disconnect (NMDevice *device, GError **error)
{
	NMDevicePrivate *priv;

	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	priv = NM_DEVICE_GET_PRIVATE (device);
	if (priv->state <= NM_DEVICE_STATE_DISCONNECTED) {
		g_set_error_literal (error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_NOT_ACTIVE,
		                     "Cannot disconnect an inactive device.");
		return FALSE;
	}

	priv->autoconnect_inhibit = TRUE;	
	nm_device_state_changed (device,
	                         NM_DEVICE_STATE_DISCONNECTED,
	                         NM_DEVICE_STATE_REASON_USER_REQUESTED);
	return TRUE;
}

static void
impl_device_disconnect (NMDevice *device, DBusGMethodInvocation *context)
{
	g_signal_emit (device, signals[DISCONNECT_REQUEST], 0, context);
}

gboolean
nm_device_activate (NMDevice *self, NMActRequest *req, GError **error)
{
	NMDevicePrivate *priv;
	NMConnection *connection;

	g_return_val_if_fail (self != NULL, FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (req != NULL, FALSE);
	g_return_val_if_fail (NM_IS_ACT_REQUEST (req), FALSE);

	priv = NM_DEVICE_GET_PRIVATE (self);

	connection = nm_act_request_get_connection (req);
	g_assert (connection);

	nm_log_info (LOGD_DEVICE, "Activation (%s) starting connection '%s'",
	             nm_device_get_iface (self),
	             nm_connection_get_id (connection));

	/* Make sure this connection isn't activated already, or in the process of
	 * being activated.
	 */
	if (   nm_device_is_activating (self)
	    || (nm_device_get_state (self) == NM_DEVICE_STATE_ACTIVATED)) {
		NMConnection *new = nm_act_request_get_connection (req);
		NMConnection *current = nm_act_request_get_connection (priv->act_request);

		if (new == current) {
			g_set_error_literal (error, NM_DEVICE_ERROR, NM_DEVICE_ERROR_CONNECTION_ACTIVATING,
			                     "Connection is already activating");
			return FALSE;
		}
	}

	priv->act_request = g_object_ref (req);

	if (!nm_act_request_get_assumed (req)) {
		/* HACK: update the state a bit early to avoid a race between the 
		 * scheduled stage1 handler and nm_policy_device_change_check() thinking
		 * that the activation request isn't deferred because the deferred bit
		 * gets cleared a bit too early, when the connection becomes valid.
		 */
		nm_device_state_changed (self, NM_DEVICE_STATE_PREPARE, NM_DEVICE_STATE_REASON_NONE);
		nm_device_activate_schedule_stage1_device_prepare (self);
	} else {
		/* If it's an assumed connection, let the device subclass short-circuit
		 * the normal connection process and just copy its IP configs from the
		 * interface.
		 */
		nm_device_state_changed (self, NM_DEVICE_STATE_IP_CONFIG, NM_DEVICE_STATE_REASON_NONE);
		nm_device_activate_schedule_stage3_ip_config_start (self);
	}

	return TRUE;
}

/*
 * nm_device_is_activating
 *
 * Return whether or not the device is currently activating itself.
 *
 */
gboolean
nm_device_is_activating (NMDevice *device)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);
	NMDeviceState state;

	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	state = nm_device_get_state (device);
	if (state >= NM_DEVICE_STATE_PREPARE && state <= NM_DEVICE_STATE_SECONDARIES)
		return TRUE;

	/* There's a small race between the time when stage 1 is scheduled
	 * and when the device actually sets STATE_PREPARE when the activation
	 * handler is actually run.  If there's an activation handler scheduled
	 * we're activating anyway.
	 */
	return priv->act_source_id ? TRUE : FALSE;
}


gboolean
nm_device_can_interrupt_activation (NMDevice *self)
{
	gboolean	interrupt = FALSE;

	g_return_val_if_fail (self != NULL, FALSE);

	if (NM_DEVICE_GET_CLASS (self)->can_interrupt_activation)
		interrupt = NM_DEVICE_GET_CLASS (self)->can_interrupt_activation (self);
	return interrupt;
}

/* IP Configuration stuff */

NMDHCP4Config *
nm_device_get_dhcp4_config (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (NM_IS_DEVICE (self), NULL);

	return NM_DEVICE_GET_PRIVATE (self)->dhcp4_config;
}

NMIP4Config *
nm_device_get_ip4_config (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (NM_IS_DEVICE (self), NULL);

	return NM_DEVICE_GET_PRIVATE (self)->ip4_config;
}


static gboolean
nm_device_set_ip4_config (NMDevice *self,
                          NMIP4Config *new_config,
                          gboolean assumed,
                          NMDeviceStateReason *reason)
{
	NMDevicePrivate *priv;
	const char *ip_iface;
	NMIP4Config *old_config = NULL;
	gboolean success = TRUE;
	NMIP4ConfigCompareFlags diff = NM_IP4_COMPARE_FLAG_ALL;
	NMDnsManager *dns_mgr;
	int ip_ifindex;

	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (reason != NULL, FALSE);

	priv = NM_DEVICE_GET_PRIVATE (self);
	ip_iface = nm_device_get_ip_iface (self);

	old_config = priv->ip4_config;

	if (new_config && old_config)
		diff = nm_ip4_config_diff (new_config, old_config);

	/* No actual change, do nothing */
	if (diff == NM_IP4_COMPARE_FLAG_NONE)
		return TRUE;

	dns_mgr = nm_dns_manager_get (NULL);
	if (old_config) {
		/* Remove any previous IP4 Config from the DNS manager */
		nm_dns_manager_remove_ip4_config (dns_mgr, ip_iface, old_config);
		g_object_unref (old_config);
		priv->ip4_config = NULL;
	}

	if (new_config) {
		priv->ip4_config = g_object_ref (new_config);

		/* Don't touch the device's actual IP config if the connection is
		 * assumed when NM starts.
		 */
		if (!assumed) {
			ip_ifindex = nm_device_get_ip_ifindex (self);
			success = nm_system_apply_ip4_config (ip_ifindex, new_config, nm_device_get_priority (self), diff);
		}

		if (success || assumed) {
			/* Export over D-Bus */
			if (!nm_ip4_config_get_dbus_path (new_config))
				nm_ip4_config_export (new_config);

			/* Add the DNS information to the DNS manager */
			nm_dns_manager_add_ip4_config (dns_mgr, ip_iface, new_config, NM_DNS_IP_CONFIG_TYPE_DEFAULT);

			_update_ip4_address (self);
		}
	}
	g_object_unref (dns_mgr);

	g_object_notify (G_OBJECT (self), NM_DEVICE_IP4_CONFIG);

	return success;
}

static gboolean
nm_device_set_ip6_config (NMDevice *self,
                          NMIP6Config *new_config,
                          gboolean assumed,
                          NMDeviceStateReason *reason)
{
	NMDevicePrivate *priv;
	const char *ip_iface;
	NMIP6Config *old_config = NULL;
	gboolean success = TRUE;
	NMIP6ConfigCompareFlags diff = NM_IP6_COMPARE_FLAG_ALL;
	NMDnsManager *dns_mgr;
	int ip_ifindex;

	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (reason != NULL, FALSE);

	priv = NM_DEVICE_GET_PRIVATE (self);
	ip_iface = nm_device_get_ip_iface (self);
	ip_ifindex = nm_device_get_ip_ifindex (self);

	old_config = priv->ip6_config;

	if (new_config && old_config)
		diff = nm_ip6_config_diff (new_config, old_config);

	/* No actual change, do nothing */
	if (diff == NM_IP6_COMPARE_FLAG_NONE)
		return TRUE;

	dns_mgr = nm_dns_manager_get (NULL);
	if (old_config) {
		/* Remove any previous IP6 Config from the DNS manager */
		nm_dns_manager_remove_ip6_config (dns_mgr, ip_iface, old_config);
		g_object_unref (old_config);
		priv->ip6_config = NULL;
	}

	if (new_config) {
		priv->ip6_config = g_object_ref (new_config);

		/* Don't touch the device's actual IP config if the connection is
		 * assumed when NM starts.
		 */
		if (!assumed)
			success = nm_system_apply_ip6_config (ip_ifindex, new_config, nm_device_get_priority (self), diff);

		if (success || assumed) {
			/* Export over D-Bus */
			if (!nm_ip6_config_get_dbus_path (new_config))
				nm_ip6_config_export (new_config);

			/* Add the DNS information to the DNS manager */
			nm_dns_manager_add_ip6_config (dns_mgr, ip_iface, new_config, NM_DNS_IP_CONFIG_TYPE_DEFAULT);
		}
	}
	g_object_unref (dns_mgr);

	g_object_notify (G_OBJECT (self), NM_DEVICE_IP6_CONFIG);

	return success;
}

NMDHCP6Config *
nm_device_get_dhcp6_config (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (NM_IS_DEVICE (self), NULL);

	return NM_DEVICE_GET_PRIVATE (self)->dhcp6_config;
}

NMIP6Config *
nm_device_get_ip6_config (NMDevice *self)
{
	g_return_val_if_fail (self != NULL, NULL);
	g_return_val_if_fail (NM_IS_DEVICE (self), NULL);

	return NM_DEVICE_GET_PRIVATE (self)->ip6_config;
}

static gboolean
nm_device_is_up (NMDevice *self)
{
	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);

	if (NM_DEVICE_GET_CLASS (self)->is_up)
		return NM_DEVICE_GET_CLASS (self)->is_up (self);

	return TRUE;
}

gboolean
nm_device_hw_bring_up (NMDevice *self, gboolean block, gboolean *no_firmware)
{
	gboolean success;
	guint32 tries = 0;

	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);

	if (nm_device_hw_is_up (self))
		goto out;

	nm_log_info (LOGD_HW, "(%s): bringing up device.", nm_device_get_iface (self));

	if (NM_DEVICE_GET_CLASS (self)->hw_bring_up) {
		success = NM_DEVICE_GET_CLASS (self)->hw_bring_up (self, no_firmware);
		if (!success)
			return FALSE;
	}

	/* Wait for the device to come up if requested */
	while (block && !nm_device_hw_is_up (self) && (tries++ < 50))
		g_usleep (200);

	if (!nm_device_hw_is_up (self)) {
		nm_log_warn (LOGD_HW, "(%s): device not up after timeout!", nm_device_get_iface (self));
		return FALSE;
	}

out:
	/* Can only get HW address of some devices when they are up */
	if (NM_DEVICE_GET_CLASS (self)->update_hw_address)
		NM_DEVICE_GET_CLASS (self)->update_hw_address (self);

	_update_ip4_address (self);
	return TRUE;
}

void
nm_device_hw_take_down (NMDevice *self, gboolean block)
{
	guint32 tries = 0;

	g_return_if_fail (NM_IS_DEVICE (self));

	if (!nm_device_hw_is_up (self))
		return;

	nm_log_info (LOGD_HW, "(%s): taking down device.", nm_device_get_iface (self));

	if (NM_DEVICE_GET_CLASS (self)->hw_take_down)
		NM_DEVICE_GET_CLASS (self)->hw_take_down (self);

	/* Wait for the device to come up if requested */
	while (block && nm_device_hw_is_up (self) && (tries++ < 50))
		g_usleep (200);
}

static gboolean
nm_device_bring_up (NMDevice *self, gboolean block, gboolean *no_firmware)
{
	gboolean success = FALSE;

	g_return_val_if_fail (NM_IS_DEVICE (self), FALSE);

	if (!nm_device_hw_bring_up (self, block, no_firmware))
		return FALSE;

	if (nm_device_is_up (self))
		return TRUE;

	nm_log_info (LOGD_HW, "(%s): preparing device.", nm_device_get_iface (self));

	if (NM_DEVICE_GET_CLASS (self)->bring_up)
		success = NM_DEVICE_GET_CLASS (self)->bring_up (self);

	return success;
}

static void
nm_device_take_down (NMDevice *self, gboolean block, NMDeviceStateReason reason)
{
	g_return_if_fail (NM_IS_DEVICE (self));

	if (nm_device_get_act_request (self))
		nm_device_deactivate (self, reason);

	if (nm_device_is_up (self)) {
		nm_log_info (LOGD_HW, "(%s): cleaning up...", nm_device_get_iface (self));

		if (NM_DEVICE_GET_CLASS (self)->take_down)
			NM_DEVICE_GET_CLASS (self)->take_down (self);
	}

	nm_device_hw_take_down (self, block);
}

static void
dispose (GObject *object)
{
	NMDevice *self = NM_DEVICE (object);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	gboolean take_down = TRUE;

	if (priv->disposed || !priv->initialized)
		goto out;

	priv->disposed = TRUE;

	/* Don't down can-assume-connection capable devices that are activated with
	 * a connection that can be assumed.
	 */
	if (nm_device_can_assume_connections (self) && (priv->state == NM_DEVICE_STATE_ACTIVATED)) {
		NMConnection *connection;
	    NMSettingIP4Config *s_ip4 = NULL;
		const char *method = NULL;

		connection = nm_device_get_connection (self);
		if (connection) {

			/* Only static or DHCP IPv4 connections can be left up.
			 * All IPv6 connections can be left up, so we don't have
			 * to check that.
			 */
			s_ip4 = nm_connection_get_setting_ip4_config (connection);
			if (s_ip4)
				method = nm_setting_ip4_config_get_method (s_ip4);
			if (   !method
			    || !strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_AUTO)
			    || !strcmp (method, NM_SETTING_IP4_CONFIG_METHOD_MANUAL))
				take_down = FALSE;
		}
	}

	/* Clear any queued transitions */
	queued_state_clear (self);

	/* Clean up and stop DHCP */
	dhcp4_cleanup (self, take_down, FALSE);
	dhcp6_cleanup (self, take_down, FALSE);
	addrconf6_cleanup (self);
	dnsmasq_cleanup (self);

	/* Take the device itself down and clear its IPv4 configuration */
	if (priv->managed && take_down) {
		NMDeviceStateReason ignored = NM_DEVICE_STATE_REASON_NONE;

		nm_device_take_down (self, FALSE, NM_DEVICE_STATE_REASON_REMOVED);
		nm_device_set_ip4_config (self, NULL, FALSE, &ignored);
	}

	/* reset the saved RA value */
	if (priv->ip6_accept_ra_path) {
		nm_utils_do_sysctl (priv->ip6_accept_ra_path,
		                    priv->ip6_accept_ra_save ? "1\n" : "0\n");
	}
	g_free (priv->ip6_accept_ra_path);

	activation_source_clear (self, TRUE, AF_INET);
	activation_source_clear (self, TRUE, AF_INET6);

	clear_act_request (self);

out:
	G_OBJECT_CLASS (nm_device_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMDevice *self = NM_DEVICE (object);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->dhcp_manager)
		g_object_unref (priv->dhcp_manager);

	if (priv->fw_manager)
		g_object_unref (priv->fw_manager);

	g_free (priv->udi);
	g_free (priv->iface);
	g_free (priv->ip_iface);
	g_free (priv->driver);
	g_free (priv->type_desc);
	if (priv->dhcp_anycast_address)
		g_byte_array_free (priv->dhcp_anycast_address, TRUE);

	/* release master reference it still exists */
	nm_device_set_master (self, NULL);

	G_OBJECT_CLASS (nm_device_parent_class)->finalize (object);
}


static void
set_property (GObject *object, guint prop_id,
			  const GValue *value, GParamSpec *pspec)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (object);
 
	switch (prop_id) {
	case PROP_UDI:
		/* construct-only */
		priv->udi = g_strdup (g_value_get_string (value));
		break;
	case PROP_IFACE:
		g_free (priv->iface);
		priv->ifindex = 0;
		priv->iface = g_value_dup_string (value);
		if (priv->iface) {
			priv->ifindex = nm_netlink_iface_to_index (priv->iface);
			if (priv->ifindex <= 0) {
				nm_log_warn (LOGD_HW, "(%s): failed to look up interface index", priv->iface);
			}
		}
		break;
	case PROP_IP_IFACE:
		break;
	case PROP_DRIVER:
		priv->driver = g_strdup (g_value_get_string (value));
		break;
	case PROP_CAPABILITIES:
		priv->capabilities = g_value_get_uint (value);
		break;
	case PROP_IP4_ADDRESS:
		priv->ip4_address = g_value_get_uint (value);
		break;
	case PROP_MANAGED:
		priv->managed = g_value_get_boolean (value);
		break;
	case PROP_FIRMWARE_MISSING:
		priv->firmware_missing = g_value_get_boolean (value);
		break;
	case PROP_DEVICE_TYPE:
		g_return_if_fail (priv->type == NM_DEVICE_TYPE_UNKNOWN);
		priv->type = g_value_get_uint (value);
		break;
	case PROP_TYPE_DESC:
		g_free (priv->type_desc);
		priv->type_desc = g_value_dup_string (value);
		break;
	case PROP_RFKILL_TYPE:
		priv->rfkill_type = g_value_get_uint (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static gboolean
_is_connected (NMDeviceState state)
{
	return (state >= NM_DEVICE_STATE_IP_CONFIG && state <= NM_DEVICE_STATE_DEACTIVATING);
}

static void
get_property (GObject *object, guint prop_id,
			  GValue *value, GParamSpec *pspec)
{
	NMDevice *self = NM_DEVICE (object);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);
	NMDeviceState state;
	const char *ac_path = NULL;

	state = nm_device_get_state (self);

	switch (prop_id) {
	case PROP_UDI:
		g_value_set_string (value, priv->udi);
		break;
	case PROP_IFACE:
		g_value_set_string (value, priv->iface);
		break;
	case PROP_IP_IFACE:
		if (_is_connected (state))
			g_value_set_string (value, nm_device_get_ip_iface (self));
		else
			g_value_set_string (value, NULL);
		break;
	case PROP_IFINDEX:
		g_value_set_int (value, priv->ifindex);
		break;
	case PROP_DRIVER:
		g_value_set_string (value, priv->driver);
		break;
	case PROP_CAPABILITIES:
		g_value_set_uint (value, priv->capabilities);
		break;
	case PROP_IP4_ADDRESS:
		g_value_set_uint (value, priv->ip4_address);
		break;
	case PROP_IP4_CONFIG:
		if (_is_connected (state) && priv->ip4_config)
			g_value_set_boxed (value, nm_ip4_config_get_dbus_path (priv->ip4_config));
		else
			g_value_set_boxed (value, "/");
		break;
	case PROP_DHCP4_CONFIG:
		if (_is_connected (state) && priv->dhcp4_client)
			g_value_set_boxed (value, nm_dhcp4_config_get_dbus_path (priv->dhcp4_config));
		else
			g_value_set_boxed (value, "/");
		break;
	case PROP_IP6_CONFIG:
		if (_is_connected (state) && priv->ip6_config)
			g_value_set_boxed (value, nm_ip6_config_get_dbus_path (priv->ip6_config));
		else
			g_value_set_boxed (value, "/");
		break;
	case PROP_DHCP6_CONFIG:
		if (_is_connected (state) && priv->dhcp6_client)
			g_value_set_boxed (value, nm_dhcp6_config_get_dbus_path (priv->dhcp6_config));
		else
			g_value_set_boxed (value, "/");
		break;
	case PROP_STATE:
		g_value_set_uint (value, priv->state);
		break;
	case PROP_ACTIVE_CONNECTION:
		if (priv->act_request)
			ac_path = nm_act_request_get_active_connection_path (priv->act_request);
		g_value_set_boxed (value, ac_path ? ac_path : "/");
		break;
	case PROP_DEVICE_TYPE:
		g_value_set_uint (value, priv->type);
		break;
	case PROP_MANAGED:
		g_value_set_boolean (value, priv->managed);
		break;
	case PROP_FIRMWARE_MISSING:
		g_value_set_boolean (value, priv->firmware_missing);
		break;
	case PROP_TYPE_DESC:
		g_value_set_string (value, priv->type_desc);
		break;
	case PROP_RFKILL_TYPE:
		g_value_set_uint (value, priv->rfkill_type);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}


static void
nm_device_class_init (NMDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (object_class, sizeof (NMDevicePrivate));

	/* Virtual methods */
	object_class->dispose = dispose;
	object_class->finalize = finalize;
	object_class->set_property = set_property;
	object_class->get_property = get_property;
	object_class->constructor = constructor;
	object_class->constructed = constructed;

	klass->get_type_capabilities = real_get_type_capabilities;
	klass->get_generic_capabilities = real_get_generic_capabilities;
	klass->act_stage1_prepare = real_act_stage1_prepare;
	klass->act_stage2_config = real_act_stage2_config;
	klass->act_stage3_ip4_config_start = real_act_stage3_ip4_config_start;
	klass->act_stage3_ip6_config_start = real_act_stage3_ip6_config_start;
	klass->act_stage4_ip4_config_timeout = real_act_stage4_ip4_config_timeout;
	klass->act_stage4_ip6_config_timeout = real_act_stage4_ip6_config_timeout;

	/* Properties */
	g_object_class_install_property
		(object_class, PROP_UDI,
		 g_param_spec_string (NM_DEVICE_UDI,
		                      "UDI",
		                      "Unique Device Identifier",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_IFACE,
		 g_param_spec_string (NM_DEVICE_IFACE,
		                      "Interface",
		                      "Interface",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_IP_IFACE,
		 g_param_spec_string (NM_DEVICE_IP_IFACE,
		                      "IP Interface",
		                      "IP Interface",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_DRIVER,
		 g_param_spec_string (NM_DEVICE_DRIVER,
		                      "Driver",
		                      "Driver",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_CAPABILITIES,
		 g_param_spec_uint (NM_DEVICE_CAPABILITIES,
		                    "Capabilities",
		                    "Capabilities",
		                    0, G_MAXUINT32, NM_DEVICE_CAP_NONE,
		                    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_IP4_ADDRESS,
		 g_param_spec_uint (NM_DEVICE_IP4_ADDRESS,
		                    "IP4 address",
		                    "IP4 address",
		                    0, G_MAXUINT32, 0, /* FIXME */
		                    G_PARAM_READWRITE));

	g_object_class_install_property
		(object_class, PROP_IP4_CONFIG,
		 g_param_spec_boxed (NM_DEVICE_IP4_CONFIG,
		                     "IP4 Config",
		                     "IP4 Config",
		                     DBUS_TYPE_G_OBJECT_PATH,
		                     G_PARAM_READWRITE));

	g_object_class_install_property
		(object_class, PROP_DHCP4_CONFIG,
		 g_param_spec_boxed (NM_DEVICE_DHCP4_CONFIG,
		                     "DHCP4 Config",
		                     "DHCP4 Config",
		                     DBUS_TYPE_G_OBJECT_PATH,
		                     G_PARAM_READWRITE));

	g_object_class_install_property
		(object_class, PROP_IP6_CONFIG,
		 g_param_spec_boxed (NM_DEVICE_IP6_CONFIG,
		                     "IP6 Config",
		                     "IP6 Config",
		                     DBUS_TYPE_G_OBJECT_PATH,
		                     G_PARAM_READWRITE));

	g_object_class_install_property
		(object_class, PROP_DHCP6_CONFIG,
		 g_param_spec_boxed (NM_DEVICE_DHCP6_CONFIG,
		                     "DHCP6 Config",
		                     "DHCP6 Config",
		                     DBUS_TYPE_G_OBJECT_PATH,
		                     G_PARAM_READWRITE));

	g_object_class_install_property
		(object_class, PROP_STATE,
		 g_param_spec_uint (NM_DEVICE_STATE,
		                    "State",
		                    "State",
		                    0, G_MAXUINT32, NM_DEVICE_STATE_UNKNOWN,
		                    G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_ACTIVE_CONNECTION,
		 g_param_spec_boxed (NM_DEVICE_ACTIVE_CONNECTION,
		                     "ActiveConnection",
		                     "ActiveConnection",
		                     DBUS_TYPE_G_OBJECT_PATH,
		                     G_PARAM_READABLE));

	g_object_class_install_property
		(object_class, PROP_DEVICE_TYPE,
		 g_param_spec_uint (NM_DEVICE_DEVICE_TYPE,
		                    "DeviceType",
		                    "DeviceType",
		                    0, G_MAXUINT32, NM_DEVICE_TYPE_UNKNOWN,
		                    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | NM_PROPERTY_PARAM_NO_EXPORT));

	g_object_class_install_property
		(object_class, PROP_MANAGED,
		 g_param_spec_boolean (NM_DEVICE_MANAGED,
		                       "Managed",
		                       "Managed",
		                       FALSE,
		                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_FIRMWARE_MISSING,
		 g_param_spec_boolean (NM_DEVICE_FIRMWARE_MISSING,
		                       "FirmwareMissing",
		                       "Firmware missing",
		                       FALSE,
		                       G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

	g_object_class_install_property
		(object_class, PROP_TYPE_DESC,
		 g_param_spec_string (NM_DEVICE_TYPE_DESC,
		                      "Type Description",
		                      "Device type description",
		                      NULL,
		                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | NM_PROPERTY_PARAM_NO_EXPORT));

	g_object_class_install_property
		(object_class, PROP_RFKILL_TYPE,
		 g_param_spec_uint (NM_DEVICE_RFKILL_TYPE,
		                    "Rfkill Type",
		                    "Type of rfkill switch (if any) supported by this device",
		                    RFKILL_TYPE_WLAN,
		                    RFKILL_TYPE_MAX,
		                    RFKILL_TYPE_UNKNOWN,
		                    G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | NM_PROPERTY_PARAM_NO_EXPORT));

	g_object_class_install_property
		(object_class, PROP_IFINDEX,
		 g_param_spec_int (NM_DEVICE_IFINDEX,
		                   "Ifindex",
		                   "Ifindex",
		                   0, G_MAXINT, 0,
		                   G_PARAM_READABLE | NM_PROPERTY_PARAM_NO_EXPORT));

	/* Signals */
	signals[STATE_CHANGED] =
		g_signal_new ("state-changed",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              0, NULL, NULL,
		              _nm_marshal_VOID__UINT_UINT_UINT,
		              G_TYPE_NONE, 3,
		              G_TYPE_UINT, G_TYPE_UINT, G_TYPE_UINT);

	signals[DISCONNECT_REQUEST] =
		g_signal_new (NM_DEVICE_DISCONNECT_REQUEST,
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_FIRST,
		              0, NULL, NULL,
		              g_cclosure_marshal_VOID__POINTER,
		              G_TYPE_NONE, 1, G_TYPE_POINTER);

	signals[AUTOCONNECT_ALLOWED] =
		g_signal_new ("autoconnect-allowed",
		              G_OBJECT_CLASS_TYPE (object_class),
		              G_SIGNAL_RUN_LAST,
		              0,
		              autoconnect_allowed_accumulator, NULL,
		              _nm_marshal_BOOLEAN__VOID,
		              G_TYPE_BOOLEAN, 0);

	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass),
	                                 &dbus_glib_nm_device_interface_object_info);

	dbus_g_error_domain_register (NM_DEVICE_ERROR, NULL, NM_TYPE_DEVICE_ERROR);
}

void
nm_device_set_firmware_missing (NMDevice *self, gboolean new_missing)
{
	NMDevicePrivate *priv;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);
	if (priv->firmware_missing != new_missing) {
		priv->firmware_missing = new_missing;
		g_object_notify (G_OBJECT (self), NM_DEVICE_FIRMWARE_MISSING);
	}
}

gboolean
nm_device_get_firmware_missing (NMDevice *self)
{
	return NM_DEVICE_GET_PRIVATE (self)->firmware_missing;
}

static const char *
state_to_string (NMDeviceState state)
{
	switch (state) {
	case NM_DEVICE_STATE_UNMANAGED:
		return "unmanaged";
	case NM_DEVICE_STATE_UNAVAILABLE:
		return "unavailable";
	case NM_DEVICE_STATE_DISCONNECTED:
		return "disconnected";
	case NM_DEVICE_STATE_PREPARE:
		return "prepare";
	case NM_DEVICE_STATE_CONFIG:
		return "config";
	case NM_DEVICE_STATE_NEED_AUTH:
		return "need-auth";
	case NM_DEVICE_STATE_IP_CONFIG:
		return "ip-config";
	case NM_DEVICE_STATE_IP_CHECK:
		return "ip-check";
	case NM_DEVICE_STATE_SECONDARIES:
		return "secondaries";
	case NM_DEVICE_STATE_ACTIVATED:
		return "activated";
	case NM_DEVICE_STATE_DEACTIVATING:
		return "deactivating";
	case NM_DEVICE_STATE_FAILED:
		return "failed";
	default:
		break;
	}
	return "unknown";
}

static const char *
reason_to_string (NMDeviceStateReason reason)
{
	switch (reason) {
	case NM_DEVICE_STATE_REASON_NONE:
		return "none";
	case NM_DEVICE_STATE_REASON_NOW_MANAGED:
		return "managed";
	case NM_DEVICE_STATE_REASON_NOW_UNMANAGED:
		return "unmanaged";
	case NM_DEVICE_STATE_REASON_CONFIG_FAILED:
		return "config-failed";
	case NM_DEVICE_STATE_REASON_IP_CONFIG_UNAVAILABLE:
		return "ip-config-unavailable";
	case NM_DEVICE_STATE_REASON_IP_CONFIG_EXPIRED:
		return "ip-config-expired";
	case NM_DEVICE_STATE_REASON_NO_SECRETS:
		return "no-secrets";
	case NM_DEVICE_STATE_REASON_SUPPLICANT_DISCONNECT:
		return "supplicant-disconnect";
	case NM_DEVICE_STATE_REASON_SUPPLICANT_CONFIG_FAILED:
		return "supplicant-config-failed";
	case NM_DEVICE_STATE_REASON_SUPPLICANT_FAILED:
		return "supplicant-failed";
	case NM_DEVICE_STATE_REASON_SUPPLICANT_TIMEOUT:
		return "supplicant-timeout";
	case NM_DEVICE_STATE_REASON_PPP_START_FAILED:
		return "ppp-start-failed";
	case NM_DEVICE_STATE_REASON_PPP_DISCONNECT:
		return "ppp-disconnect";
	case NM_DEVICE_STATE_REASON_PPP_FAILED:
		return "ppp-failed";
	case NM_DEVICE_STATE_REASON_DHCP_START_FAILED:
		return "dhcp-start-failed";
	case NM_DEVICE_STATE_REASON_DHCP_ERROR:
		return "dhcp-error";
	case NM_DEVICE_STATE_REASON_DHCP_FAILED:
		return "dhcp-failed";
	case NM_DEVICE_STATE_REASON_SHARED_START_FAILED:
		return "sharing-start-failed";
	case NM_DEVICE_STATE_REASON_SHARED_FAILED:
		return "sharing-failed";
	case NM_DEVICE_STATE_REASON_AUTOIP_START_FAILED:
		return "autoip-start-failed";
	case NM_DEVICE_STATE_REASON_AUTOIP_ERROR:
		return "autoip-error";
	case NM_DEVICE_STATE_REASON_AUTOIP_FAILED:
		return "autoip-failed";
	case NM_DEVICE_STATE_REASON_MODEM_BUSY:
		return "modem-busy";
	case NM_DEVICE_STATE_REASON_MODEM_NO_DIAL_TONE:
		return "modem-no-dialtone";
	case NM_DEVICE_STATE_REASON_MODEM_NO_CARRIER:
		return "modem-no-carrier";
	case NM_DEVICE_STATE_REASON_MODEM_DIAL_TIMEOUT:
		return "modem-dial-timeout";
	case NM_DEVICE_STATE_REASON_MODEM_DIAL_FAILED:
		return "modem-dial-failed";
	case NM_DEVICE_STATE_REASON_MODEM_INIT_FAILED:
		return "modem-init-failed";
	case NM_DEVICE_STATE_REASON_GSM_APN_FAILED:
		return "gsm-apn-failed";
	case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_NOT_SEARCHING:
		return "gsm-registration-idle";
	case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_DENIED:
		return "gsm-registration-denied";
	case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_TIMEOUT:
		return "gsm-registration-timeout";
	case NM_DEVICE_STATE_REASON_GSM_REGISTRATION_FAILED:
		return "gsm-registration-failed";
	case NM_DEVICE_STATE_REASON_GSM_PIN_CHECK_FAILED:
		return "gsm-pin-check-failed";
	case NM_DEVICE_STATE_REASON_FIRMWARE_MISSING:
		return "firmware-missing";
	case NM_DEVICE_STATE_REASON_REMOVED:
		return "removed";
	case NM_DEVICE_STATE_REASON_SLEEPING:
		return "sleeping";
	case NM_DEVICE_STATE_REASON_CONNECTION_REMOVED:
		return "connection-removed";
	case NM_DEVICE_STATE_REASON_USER_REQUESTED:
		return "user-requested";
	case NM_DEVICE_STATE_REASON_CARRIER:
		return "carrier-changed";
	case NM_DEVICE_STATE_REASON_CONNECTION_ASSUMED:
		return "connection-assumed";
	case NM_DEVICE_STATE_REASON_SUPPLICANT_AVAILABLE:
		return "supplicant-available";
	case NM_DEVICE_STATE_REASON_MODEM_NOT_FOUND:
		return "modem-not-found";
	case NM_DEVICE_STATE_REASON_BT_FAILED:
		return "bluetooth-failed";
	case NM_DEVICE_STATE_REASON_GSM_SIM_NOT_INSERTED:
		return "gsm-sim-not-inserted";
	case NM_DEVICE_STATE_REASON_GSM_SIM_PIN_REQUIRED:
		return "gsm-sim-pin-required";
	case NM_DEVICE_STATE_REASON_GSM_SIM_PUK_REQUIRED:
		return "gsm-sim-puk-required";
	case NM_DEVICE_STATE_REASON_GSM_SIM_WRONG:
		return "gsm-sim-wrong";
	case NM_DEVICE_STATE_REASON_INFINIBAND_MODE:
		return "infiniband-mode";
	default:
		break;
	}
	return "unknown";
}

void
nm_device_state_changed (NMDevice *device,
                         NMDeviceState state,
                         NMDeviceStateReason reason)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);
	NMDeviceState old_state;
	NMActRequest *req;
	gboolean no_firmware = FALSE;

	g_return_if_fail (NM_IS_DEVICE (device));

	/* Do nothing if state isn't changing, but as a special case allow
	 * re-setting UNAVAILABLE if the device is missing firmware so that we
	 * can retry device initialization.
	 */
	if (   (priv->state == state)
	    && !(state == NM_DEVICE_STATE_UNAVAILABLE && priv->firmware_missing))
		return;

	old_state = priv->state;
	priv->state = state;

	nm_log_info (LOGD_DEVICE, "(%s): device state change: %s -> %s (reason '%s') [%d %d %d]",
	             nm_device_get_iface (device),
	             state_to_string (old_state),
	             state_to_string (state),
	             reason_to_string (reason),
	             old_state,
	             state,
	             reason);

	/* Clear any queued transitions */
	queued_state_clear (device);

	/* Cache the activation request for the dispatcher */
	req = priv->act_request ? g_object_ref (priv->act_request) : NULL;

	/* Handle the new state here; but anything that could trigger
	 * another state change should be done below.
	 */
	switch (state) {
	case NM_DEVICE_STATE_UNMANAGED:
		nm_device_set_firmware_missing (device, FALSE);
		if (old_state > NM_DEVICE_STATE_UNMANAGED)
			nm_device_take_down (device, TRUE, reason);
		break;
	case NM_DEVICE_STATE_UNAVAILABLE:
		if (old_state == NM_DEVICE_STATE_UNMANAGED || priv->firmware_missing) {
			if (!nm_device_bring_up (device, TRUE, &no_firmware) && no_firmware)
				nm_log_warn (LOGD_HW, "(%s): firmware may be missing.", nm_device_get_iface (device));
			nm_device_set_firmware_missing (device, no_firmware ? TRUE : FALSE);
		}
		/* Ensure the device gets deactivated in response to stuff like
		 * carrier changes or rfkill.  But don't deactivate devices that are
		 * about to assume a connection since that defeats the purpose of
		 * assuming the device's existing connection.
		 */
		if (reason != NM_DEVICE_STATE_REASON_CONNECTION_ASSUMED)
			nm_device_deactivate (device, reason);
		break;
	case NM_DEVICE_STATE_DISCONNECTED:
		if (old_state != NM_DEVICE_STATE_UNAVAILABLE)
			nm_device_deactivate (device, reason);
		break;
	default:
		priv->autoconnect_inhibit = FALSE;
		break;
	}

	g_object_notify (G_OBJECT (device), NM_DEVICE_STATE);
	g_signal_emit_by_name (device, "state-changed", state, old_state, reason);

	/* Post-process the event after internal notification */

	switch (state) {
	case NM_DEVICE_STATE_UNAVAILABLE:
		/* If the device can activate now (ie, it's got a carrier, the supplicant
		 * is active, or whatever) schedule a delayed transition to DISCONNECTED
		 * to get things rolling.  The device can't transition immediately because
		 * we can't change states again from the state handler for a variety of
		 * reasons.
		 */
		if (nm_device_is_available (device)) {
			nm_log_dbg (LOGD_DEVICE, "(%s): device is available, will transition to DISCONNECTED",
			            nm_device_get_iface (device));
			nm_device_queue_state (device, NM_DEVICE_STATE_DISCONNECTED, NM_DEVICE_STATE_REASON_NONE);
		} else {
			nm_log_dbg (LOGD_DEVICE, "(%s): device not yet available for transition to DISCONNECTED",
			            nm_device_get_iface (device));
		}
		break;
	case NM_DEVICE_STATE_ACTIVATED:
		nm_log_info (LOGD_DEVICE, "Activation (%s) successful, device activated.",
		             nm_device_get_iface (device));
		nm_utils_call_dispatcher ("up", nm_act_request_get_connection (req), device, NULL, NULL, NULL);
		break;
	case NM_DEVICE_STATE_FAILED:
		nm_log_warn (LOGD_DEVICE, "Activation (%s) failed.", nm_device_get_iface (device));
		/* Schedule the transition to DISCONNECTED.  The device can't transition
		 * immediately because we can't change states again from the state
		 * handler for a variety of reasons.
		 */
		nm_device_queue_state (device, NM_DEVICE_STATE_DISCONNECTED, NM_DEVICE_STATE_REASON_NONE);
		break;
	default:
		break;
	}

	if (old_state == NM_DEVICE_STATE_ACTIVATED)
		nm_utils_call_dispatcher ("down", nm_act_request_get_connection (req), device, NULL, NULL, NULL);

	/* Dispose of the cached activation request */
	if (req)
		g_object_unref (req);
}

static gboolean
queued_set_state (gpointer user_data)
{
	NMDevice *self = NM_DEVICE (user_data);
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (self);

	if (priv->queued_state.id) {
		priv->queued_state.id = 0;

		nm_log_dbg (LOGD_DEVICE, "(%s): running queued state change to %s (id %d)",
			        nm_device_get_iface (self),
			        state_to_string (priv->queued_state.state),
			        priv->queued_state.id);
		nm_device_state_changed (self, priv->queued_state.state, priv->queued_state.reason);
	}
	queued_state_clear (self);
	return FALSE;
}

void
nm_device_queue_state (NMDevice *self,
                       NMDeviceState state,
                       NMDeviceStateReason reason)
{
	NMDevicePrivate *priv;

	g_return_if_fail (self != NULL);
	g_return_if_fail (NM_IS_DEVICE (self));

	priv = NM_DEVICE_GET_PRIVATE (self);

	/* We should only ever have one delayed state transition at a time */
	if (priv->queued_state.id) {
		g_warn_if_fail (priv->queued_state.id == 0);
		queued_state_clear (self);
	}

	priv->queued_state.state = state;
	priv->queued_state.reason = reason;
	priv->queued_state.id = g_idle_add (queued_set_state, self);

	nm_log_dbg (LOGD_DEVICE, "(%s): queued state change to %s (id %d)",
	            nm_device_get_iface (self), state_to_string (state),
	            priv->queued_state.id);
}

NMDeviceState
nm_device_get_state (NMDevice *device)
{
	g_return_val_if_fail (NM_IS_DEVICE (device), NM_DEVICE_STATE_UNKNOWN);

	return NM_DEVICE_GET_PRIVATE (device)->state;
}

gboolean
nm_device_get_managed (NMDevice *device)
{
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	return NM_DEVICE_GET_PRIVATE (device)->managed;
}

void
nm_device_set_managed (NMDevice *device,
                       gboolean managed,
                       NMDeviceStateReason reason)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (device));

	priv = NM_DEVICE_GET_PRIVATE (device);
	if (priv->managed == managed)
		return;

	priv->managed = managed;
	nm_log_info (LOGD_DEVICE, "(%s): now %s",
	             nm_device_get_iface (device),
	             managed ? "managed" : "unmanaged");

	g_object_notify (G_OBJECT (device), NM_DEVICE_MANAGED);

	/* If now managed, jump to unavailable */
	if (managed)
		nm_device_state_changed (device, NM_DEVICE_STATE_UNAVAILABLE, reason);
	else
		nm_device_state_changed (device, NM_DEVICE_STATE_UNMANAGED, reason);
}

gboolean
nm_device_spec_match_list (NMDevice *device, const GSList *specs)
{
	g_return_val_if_fail (device != NULL, FALSE);
	g_return_val_if_fail (NM_IS_DEVICE (device), FALSE);

	if (NM_DEVICE_GET_CLASS (device)->spec_match_list)
		return NM_DEVICE_GET_CLASS (device)->spec_match_list (device, specs);
	return FALSE;
}

NMConnection *
nm_device_connection_match_config (NMDevice *device, const GSList *connections)
{
	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (NM_IS_DEVICE (device), NULL);

	if (NM_DEVICE_GET_CLASS (device)->connection_match_config)
		return NM_DEVICE_GET_CLASS (device)->connection_match_config (NM_DEVICE (device), connections);
	return NULL;
}

void
nm_device_set_dhcp_timeout (NMDevice *device, guint32 timeout)
{
	g_return_if_fail (NM_IS_DEVICE (device));

	NM_DEVICE_GET_PRIVATE (device)->dhcp_timeout = timeout;
}

void
nm_device_set_dhcp_anycast_address (NMDevice *device, guint8 *addr)
{
	NMDevicePrivate *priv;

	g_return_if_fail (NM_IS_DEVICE (device));

	priv = NM_DEVICE_GET_PRIVATE (device);

	if (priv->dhcp_anycast_address) {
		g_byte_array_free (priv->dhcp_anycast_address, TRUE);
		priv->dhcp_anycast_address = NULL;
	}

	if (addr) {
		priv->dhcp_anycast_address = g_byte_array_sized_new (ETH_ALEN);
		g_byte_array_append (priv->dhcp_anycast_address, addr, ETH_ALEN);
	}
}


void
nm_device_clear_autoconnect_inhibit (NMDevice *device)
{
	NMDevicePrivate *priv = NM_DEVICE_GET_PRIVATE (device);
	g_return_if_fail (priv);
	priv->autoconnect_inhibit = FALSE;
}

