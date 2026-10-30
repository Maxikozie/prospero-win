/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* The two battery state bits Wine's system.c reads from FreeBSD's
 * dev/acpica/acpiio.h (BSD-2-Clause facts, restated). The console has no
 * hw.acpi.battery sysctls, so Wine reports no battery; the header exists so
 * that the file compiles. */
#ifndef _ORBIS_WINE_DEV_ACPICA_ACPIIO_H
#define _ORBIS_WINE_DEV_ACPICA_ACPIIO_H

#define ACPI_BATT_STAT_DISCHARG   0x0001
#define ACPI_BATT_STAT_CHARGING   0x0002
#define ACPI_BATT_STAT_CRITICAL   0x0004
#define ACPI_BATT_STAT_INVALID    (ACPI_BATT_STAT_DISCHARG | ACPI_BATT_STAT_CHARGING)
#define ACPI_BATT_STAT_BST_MASK   (ACPI_BATT_STAT_INVALID | ACPI_BATT_STAT_CRITICAL)
#define ACPI_BATT_STAT_NOT_PRESENT ACPI_BATT_STAT_BST_MASK

#endif /* _ORBIS_WINE_DEV_ACPICA_ACPIIO_H */
