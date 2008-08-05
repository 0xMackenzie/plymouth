/* entry.c - boot entry
 *
 * Copyright (C) 2007, 2008 Red Hat, Inc.
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
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include "config.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <values.h>
#include <unistd.h>
#include <wchar.h>

#include "ply-entry.h"
#include "ply-event-loop.h"
#include "ply-array.h"
#include "ply-logger.h"
#include "ply-frame-buffer.h"
#include "ply-image.h"
#include "ply-utils.h"
#include "ply-window.h"

#include <linux/kd.h>

#ifndef FRAMES_PER_SECOND
#define FRAMES_PER_SECOND 30
#endif

struct _ply_entry
{
  ply_event_loop_t *loop;

  ply_window_t            *window;
  ply_frame_buffer_t      *frame_buffer;
  ply_frame_buffer_area_t  area;
  ply_image_t             *text_field_image;
  ply_image_t             *bullet_image;

  int number_of_bullets;
  int max_number_of_visible_bullets;

  uint32_t is_hidden : 1;
};

ply_entry_t *
ply_entry_new (const char *image_dir)
{
  ply_entry_t *entry;
  char *image_path;

  assert (image_dir != NULL);

  entry = calloc (1, sizeof (ply_entry_t));

  image_path = NULL;
  asprintf (&image_path, "%s/entry.png", image_dir);
  entry->text_field_image = ply_image_new (image_path);
  free (image_path);

  image_path = NULL;
  asprintf (&image_path, "%s/bullet.png", image_dir);
  entry->bullet_image = ply_image_new (image_path);
  free (image_path);

  entry->is_hidden = true;

  return entry;
}

void
ply_entry_free (ply_entry_t *entry)
{
  if (entry == NULL)
    return;

  free (entry);
}

static int
get_max_number_of_visible_bullets (ply_entry_t *entry)
{
  long bullet_width, text_field_width;

  bullet_width = ply_image_get_width (entry->bullet_image);
  text_field_width = ply_image_get_width (entry->text_field_image);

  return (int) (text_field_width / bullet_width);
}

bool
ply_entry_load (ply_entry_t *entry)
{

  if (!ply_image_load (entry->text_field_image))
    return false;

  if (!ply_image_load (entry->bullet_image))
    return false;

  entry->area.width = ply_image_get_width (entry->text_field_image);
  entry->area.height = ply_image_get_height (entry->text_field_image);

  entry->max_number_of_visible_bullets = get_max_number_of_visible_bullets (entry);

  return true;
}

static void
erase_entry_area (ply_entry_t *entry)
{
  ply_window_erase_area (entry->window,
                         entry->area.x, entry->area.y,
                         entry->area.width, entry->area.height);
}

void
ply_entry_draw (ply_entry_t *entry)
{
  ply_frame_buffer_area_t bullet_area;
  uint32_t *text_field_data, *bullet_data;
  int i, number_of_visible_bullets;

  if (entry->is_hidden)
    return;

  ply_frame_buffer_pause_updates (entry->frame_buffer);

  text_field_data = ply_image_get_data (entry->text_field_image);

  erase_entry_area (entry);

  ply_frame_buffer_fill_with_argb32_data (entry->frame_buffer,
                                          &entry->area, 0, 0,
                                          text_field_data);

  bullet_data = ply_image_get_data (entry->bullet_image);
  bullet_area.width = ply_image_get_width (entry->bullet_image);
  bullet_area.height = ply_image_get_height (entry->bullet_image);

  if (entry->number_of_bullets < entry->max_number_of_visible_bullets)
    number_of_visible_bullets = entry->number_of_bullets;
  else
    {
      number_of_visible_bullets = entry->max_number_of_visible_bullets;

      /* We've got more bullets than we can show in the available space, so
       * draw a little half bullet to indicate some bullets are offscreen
       */
      bullet_area.x = entry->area.x;
      bullet_area.y = entry->area.y + entry->area.height / 2.0 - bullet_area.height / 2.0;

      ply_frame_buffer_fill_with_argb32_data (entry->frame_buffer,
                                              &bullet_area, bullet_area.width / 2.0, 0,
                                              bullet_data);
    }

  for (i = 0; i < number_of_visible_bullets; i++)
    {
      bullet_area.x = entry->area.x + i * bullet_area.width + bullet_area.width / 2.0;
      bullet_area.y = entry->area.y + entry->area.height / 2.0 - bullet_area.height / 2.0;

      ply_frame_buffer_fill_with_argb32_data (entry->frame_buffer,
                                              &bullet_area, 0, 0,
                                              bullet_data);
    }
  ply_frame_buffer_unpause_updates (entry->frame_buffer);
}

void
ply_entry_add_bullet (ply_entry_t *entry)
{
  entry->number_of_bullets++;
  ply_entry_draw (entry);
}

void
ply_entry_remove_bullet (ply_entry_t *entry)
{
  entry->number_of_bullets--;
  ply_entry_draw (entry);
}

void
ply_entry_show (ply_entry_t      *entry,
                ply_event_loop_t *loop,
                ply_window_t     *window,
                long              x,
                long              y)
{
  assert (entry != NULL);
  assert (entry->loop == NULL);

  entry->loop = loop;
  entry->window = window;
  entry->frame_buffer = ply_window_get_frame_buffer (window);;

  entry->area.x = x;
  entry->area.y = y;

  entry->is_hidden = false;

  ply_entry_draw (entry);
}

void
ply_entry_hide (ply_entry_t *entry)
{
  erase_entry_area (entry);

  entry->frame_buffer = NULL;
  entry->window = NULL;
  entry->loop = NULL;

  entry->is_hidden = true;
}

bool
ply_entry_is_hidden (ply_entry_t *entry)
{
  return entry->is_hidden;
}

long
ply_entry_get_width (ply_entry_t *entry)
{
  return entry->area.width;
}

long
ply_entry_get_height (ply_entry_t *entry)
{
  return entry->area.height;
}

/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
