#!/bin/sh
# vim: ft=sh ts=2 sts=2 sw=2 et ai
# -*- Mode: sh; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*-
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
#
# Copyright (C) 2011 Red Hat, Inc.
#

#
# Call Get() method on org.freedesktop.DBus.Properties interface to get Hostname
# property of /org/freedesktop/NetworkManager/Settings object
#

SERVICE_NAME="org.freedesktop.NetworkManager"
OBJECT_PATH="/org/freedesktop/NetworkManager/Settings"
METHOD="org.freedesktop.DBus.Properties.Get"


dbus-send --system --print-reply --dest=$SERVICE_NAME $OBJECT_PATH $METHOD \
          string:"org.freedesktop.NetworkManager.Settings" string:"Hostname" | \
sed  -n 's/.*"\([^"]*\)".*/\1/p'


# The same with glib's gdbus
# gdbus call --system --dest $SERVICE_NAME --object-path $OBJECT_PATH --method $METHOD \
#      "org.freedesktop.NetworkManager.Settings" "Hostname"


# The same with qt's qdbus
# qdbus --system $SERVICE_NAME $OBJECT_PATH $METHOD \
#      "org.freedesktop.NetworkManager.Settings" "Hostname"

