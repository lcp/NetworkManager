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
 * Copyright (C) 2011 Red Hat, Inc.
 */

#ifndef WIFI_UTILS_PRIVATE_H
#define WIFI_UTILS_PRIVATE_H

#include <glib.h>

#include "NetworkManager.h"
#include "wifi-utils.h"

struct WifiData {
	char *iface;
	int ifindex;
	NMDeviceWifiCapabilities caps;
	gboolean can_scan_ssid;

	NM80211Mode (*get_mode) (WifiData *data);

	gboolean (*set_mode) (WifiData *data, const NM80211Mode mode);

	/* Return current frequency in MHz (really associated BSS frequency) */
	guint32 (*get_freq) (WifiData *data);

	/* Return first supported frequency in the zero-terminated list */
	guint32 (*find_freq) (WifiData *data, const guint32 *freqs);

	/* If SSID is empty/blank (zero-length or all \0s) return NULL */
	GByteArray * (*get_ssid) (WifiData *data);

	/* Return current bitrate in Kbps */
	guint32 (*get_rate) (WifiData *data);

	gboolean (*get_bssid) (WifiData *data, struct ether_addr *out_bssid);

	/* Return a signal strength percentage 0 - 100% for the current BSSID;
	 * return -1 on errors or if not associated.
	 */
	int (*get_qual) (WifiData *data);

	void (*deinit) (WifiData *data);

	/* OLPC Mesh-only functions */

	guint32 (*get_mesh_channel) (WifiData *data);

	/* channel == 0 means "auto channel" */
	gboolean (*set_mesh_channel) (WifiData *data, guint32 channel);

	/* ssid == NULL means "auto SSID" */
	gboolean (*set_mesh_ssid) (WifiData *data, const GByteArray *ssid);
};

gpointer wifi_data_new (const char *iface, int ifindex, gsize len);
void wifi_data_free (WifiData *data);

#endif  /* WIFI_UTILS_PRIVATE_H */
