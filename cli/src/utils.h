/* nmcli - command-line tool to control NetworkManager
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
 * (C) Copyright 2010 - 2012 Red Hat, Inc.
 */

#ifndef NMC_UTILS_H
#define NMC_UTILS_H

#include <glib.h>

#include "nmcli.h"

/* === Functions === */
int matches (const char *cmd, const char *pattern);
int next_arg (int *argc, char ***argv);
char *ssid_to_printable (const char *str, gsize len);
char *nmc_ip4_address_as_string (guint32 ip, GError **error);
char *nmc_ip6_address_as_string (const struct in6_addr *ip, GError **error);
int nmc_string_screen_width (const char *start, const char *end);
void set_val_str (NmcOutputField fields_array[], guint32 index, const char *value);
void set_val_arr (NmcOutputField fields_array[], guint32 index, const char **value);
GArray *parse_output_fields (const char *fields_str, const NmcOutputField fields_array[], GError **error);
gboolean nmc_terse_option_check (NMCPrintOutput print_output, const char *fields, GError **error);
void print_fields (const NmcPrintFields fields, const NmcOutputField field_values[]);
gboolean nmc_is_nm_running (NmCli *nmc, GError **error);
gboolean nmc_versions_match (NmCli *nmc);

#endif /* NMC_UTILS_H */
