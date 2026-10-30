/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* A data import between application PRXs (milestone d1): pw_bind_counter
 * through this module's GOT, which only the loader can fill. A data import
 * faulted on the PS5 (FW 12.02), so the title runs this stage last among
 * the binding stages. */
extern int pw_bind_counter;

int pw_bind_data_read(void) { return pw_bind_counter; }
const void *pw_bind_data_address(void) { return &pw_bind_counter; }
