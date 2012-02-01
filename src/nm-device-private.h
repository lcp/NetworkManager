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
 * Copyright (C) 2007 - 2008 Novell, Inc.
 * Copyright (C) 2007 - 2011 Red Hat, Inc.
 */

#ifndef NM_DEVICE_PRIVATE_H
#define NM_DEVICE_PRIVATE_H

#include "nm-device.h"

/* This file should only be used by subclasses of NMDevice */

enum NMActStageReturn {
	NM_ACT_STAGE_RETURN_FAILURE = 0,
	NM_ACT_STAGE_RETURN_SUCCESS,
	NM_ACT_STAGE_RETURN_POSTPONE,
	NM_ACT_STAGE_RETURN_STOP         /* This activation chain is done */
};

void nm_device_set_ip_iface (NMDevice *self, const char *iface);

void nm_device_activate_schedule_stage3_ip_config_start (NMDevice *device);

gboolean nm_device_hw_bring_up (NMDevice *self, gboolean wait, gboolean *no_firmware);

void nm_device_hw_take_down (NMDevice *self, gboolean block);

gboolean nm_device_ip_config_should_fail (NMDevice *self, gboolean ip6);

void nm_device_set_firmware_missing (NMDevice *self, gboolean missing);

guint32 nm_device_get_capabilities (NMDevice *dev);
guint32 nm_device_get_type_capabilities (NMDevice *dev);

void nm_device_activate_schedule_stage1_device_prepare (NMDevice *device);
void nm_device_activate_schedule_stage2_device_config (NMDevice *device);

void nm_device_activate_schedule_ip4_config_result(NMDevice *device, NMIP4Config *config);
void nm_device_activate_schedule_ip4_config_timeout (NMDevice *device);

void nm_device_activate_schedule_ip6_config_result (NMDevice *device, NMIP6Config *config);
void nm_device_activate_schedule_ip6_config_timeout (NMDevice *device);

gboolean nm_device_activate_ip4_state_in_conf (NMDevice *device);
gboolean nm_device_activate_ip6_state_in_conf (NMDevice *device);

void nm_device_set_dhcp_timeout (NMDevice *device, guint32 timeout);
void nm_device_set_dhcp_anycast_address (NMDevice *device, guint8 *addr);

gboolean nm_device_dhcp4_renew (NMDevice *device, gboolean release);

#endif	/* NM_DEVICE_PRIVATE_H */
