/* ply-boot-splash.h - APIs for putting up a splash screen
 *
 * Copyright (C) 2007 Red Hat, Inc.
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
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written By: Ray Strode <rstrode@redhat.com>
 */
#ifndef PLY_BOOT_SPLASH_H
#define PLY_BOOT_SPLASH_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#include "ply-answer.h"
#include "ply-event-loop.h"
#include "ply-window.h"
#include "ply-buffer.h"
#include "ply-boot-splash-plugin.h"

typedef struct _ply_boot_splash ply_boot_splash_t;

#ifndef PLY_HIDE_FUNCTION_DECLARATIONS
ply_boot_splash_t *ply_boot_splash_new (const char *module_name,
                                        ply_window_t *window,
                                        ply_buffer_t *boot_buffer);
void ply_boot_splash_free (ply_boot_splash_t *splash);
bool ply_boot_splash_show (ply_boot_splash_t *splash);
void ply_boot_splash_update_status (ply_boot_splash_t *splash,
                                    const char        *status);
void ply_boot_splash_update_output (ply_boot_splash_t *splash,
                                    const char        *output,
                                    size_t             size);
void ply_boot_splash_root_mounted (ply_boot_splash_t *splash);

void ply_boot_splash_ask_for_password (ply_boot_splash_t *splash,
                                       const char        *prompt,
                                       ply_answer_t      *answer);
void ply_boot_splash_hide (ply_boot_splash_t *splash);
void ply_boot_splash_attach_to_event_loop (ply_boot_splash_t *splash,
                                           ply_event_loop_t  *loop);

#endif

#endif /* PLY_BOOT_SPLASH_H */
/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
