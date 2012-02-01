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

#ifndef WIFI_UTILS_H
#define WIFI_UTILS_H

#include <net/ethernet.h>
#include <glib.h>

#include "NetworkManager.h"

typedef struct WifiData WifiData;

gboolean wifi_utils_is_wifi (const char *iface);

WifiData *wifi_utils_init (const char *iface, int ifindex, gboolean check_scan);

void wifi_utils_deinit (WifiData *data);

NMDeviceWifiCapabilities wifi_utils_get_caps (WifiData *data);

gboolean wifi_utils_can_scan_ssid (WifiData *data);

NM80211Mode wifi_utils_get_mode (WifiData *data);

gboolean wifi_utils_set_mode (WifiData *data, const NM80211Mode mode);

/* Returns frequency in MHz */
guint32 wifi_utils_get_freq (WifiData *data);

/* Return the first supported frequency in the zero-terminated list */
guint32 wifi_utils_find_freq (WifiData *data, const guint32 *freqs);

/* Caller must free returned byte array */
GByteArray *wifi_utils_get_ssid (WifiData *data);

/* Caller must free returned byte array */
gboolean wifi_utils_get_bssid (WifiData *data, struct ether_addr *out_bssid);

/* Returns current bitrate in Kbps */
guint32 wifi_utils_get_rate (WifiData *data);

/* Returns quality 0 - 100% on succes, or -1 on error */
int wifi_utils_get_qual (WifiData *data);


/* OLPC Mesh-only functions */
guint32 wifi_utils_get_mesh_channel (WifiData *data);

gboolean wifi_utils_set_mesh_channel (WifiData *data, guint32 channel);

gboolean wifi_utils_set_mesh_ssid (WifiData *data, const GByteArray *ssid);

#endif  /* WIFI_UTILS_H */
