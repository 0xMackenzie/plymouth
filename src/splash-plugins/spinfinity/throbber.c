/* throbber.c - boot throbber
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

#include "throbber.h"
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

struct _throbber
{
  ply_array_t *frames;
  ply_event_loop_t *loop;
  char *image_dir;
  char *frames_prefix;

  ply_window_t            *window;
  ply_frame_buffer_t      *frame_buffer;
  ply_frame_buffer_area_t  frame_area;

  long x, y;
  long width, height;
  double start_time, now;
};

throbber_t *
throbber_new (const char *image_dir,
              const char *frames_prefix)
{
  throbber_t *throbber;

  assert (image_dir != NULL);
  assert (frames_prefix != NULL);

  throbber = calloc (1, sizeof (throbber_t));

  throbber->frames = ply_array_new ();
  throbber->frames_prefix = strdup (frames_prefix);
  throbber->image_dir = strdup (image_dir);
  throbber->width = 82;
  throbber->height = 47;
  throbber->frame_area.width = 0;
  throbber->frame_area.height = 0;
  throbber->frame_area.x = 700;
  throbber->frame_area.y = 700;

  return throbber;
}

static void
throbber_remove_frames (throbber_t *throbber)
{
  int i;
  ply_image_t **frames;

  frames = (ply_image_t **) ply_array_steal_elements (throbber->frames);
  for (i = 0; frames[i] != NULL; i++)
    ply_image_free (frames[i]);
  free (frames);
}

void
throbber_free (throbber_t *throbber)
{
  if (throbber == NULL)
    return;

  throbber_remove_frames (throbber);
  ply_array_free (throbber->frames);

  free (throbber->frames_prefix);
  free (throbber->image_dir);
  free (throbber);
}

static void
animate_at_time (throbber_t *throbber,
                 double      time)
{
  int number_of_frames;
  int frame_number;
  ply_image_t * const * frames;
  uint32_t *frame_data;

  number_of_frames = ply_array_get_size (throbber->frames);

  if (number_of_frames == 0)
    return;

  frame_number = (.5 * sin (time) + .5) * number_of_frames;

  ply_frame_buffer_pause_updates (throbber->frame_buffer);
  if (throbber->frame_area.width > 0)
    ply_frame_buffer_fill_with_color (throbber->frame_buffer, &throbber->frame_area,
                                      0.0, 0.43, .71, 1.0);

  frames = (ply_image_t * const *) ply_array_get_elements (throbber->frames);

  throbber->frame_area.x = throbber->x;
  throbber->frame_area.y = throbber->y;
  throbber->frame_area.width = ply_image_get_width (frames[frame_number]);
  throbber->frame_area.height = ply_image_get_height (frames[frame_number]);
  frame_data = ply_image_get_data (frames[frame_number]);

  ply_frame_buffer_fill_with_argb32_data (throbber->frame_buffer,
                                          &throbber->frame_area, 0, 0,
                                          frame_data);
  ply_frame_buffer_unpause_updates (throbber->frame_buffer);
}

static void
on_timeout (throbber_t *throbber)
{
  double sleep_time;
  throbber->now = ply_get_timestamp ();

#ifdef REAL_TIME_ANIMATION
  animate_at_time (throbber,
                   throbber->now - throbber->start_time);
#else
  static double time = 0.0;
  time += 1.0 / FRAMES_PER_SECOND;
  animate_at_time (throbber, time);
#endif

  sleep_time = 1.0 / FRAMES_PER_SECOND;
  sleep_time = MAX (sleep_time - (ply_get_timestamp () - throbber->now),
                    0.005);

  ply_event_loop_watch_for_timeout (throbber->loop,
                                    sleep_time,
                                    (ply_event_loop_timeout_handler_t)
                                    on_timeout, throbber);
}

static bool
throbber_add_frame (throbber_t *throbber,
                    const char *filename)
{
  ply_image_t *image;

  image = ply_image_new (filename);

  if (!ply_image_load (image))
    {
      ply_image_free (image);
      return false;
    }

  ply_array_add_element (throbber->frames, image);

  throbber->width = MAX (throbber->width, ply_image_get_width (image));
  throbber->height = MAX (throbber->width, ply_image_get_height (image));

  return true;
}

static bool
throbber_add_frames (throbber_t *throbber)
{
  struct dirent **entries;
  int number_of_entries;
  int i;
  bool load_finished;

  entries = NULL;

  number_of_entries = scandir (throbber->image_dir, &entries, NULL, versionsort);

  if (number_of_entries < 0)
    return false;

  load_finished = false;
  for (i = 0; i < number_of_entries; i++)
    {
      if (strncmp (entries[i]->d_name,
                   throbber->frames_prefix,
                   strlen (throbber->frames_prefix)) == 0
          && (strlen (entries[i]->d_name) > 4)
          && strcmp (entries[i]->d_name + strlen (entries[i]->d_name) - 4, ".png") == 0)
        {
          char *filename;

          filename = NULL;
          asprintf (&filename, "%s/%s", throbber->image_dir, entries[i]->d_name);

          if (!throbber_add_frame (throbber, filename))
            goto out;

          free (filename);
        }

      free (entries[i]);
      entries[i] = NULL;
    }
  load_finished = true;

out:
  if (!load_finished)
    {
      throbber_remove_frames (throbber);

      while (entries[i] != NULL)
        {
          free (entries[i]);
          i++;
        }
    }
  free (entries);

  return load_finished;
}

bool
throbber_start (throbber_t         *throbber,
                ply_event_loop_t   *loop,
                ply_window_t       *window,
                long                x,
                long                y)
{
  assert (throbber != NULL);
  assert (throbber->loop == NULL);

  if (ply_array_get_size (throbber->frames) == 0)
    {
      if (!throbber_add_frames (throbber))
        return false;
    }

  throbber->loop = loop;
  throbber->window = window;
  throbber->frame_buffer = ply_window_get_frame_buffer (window);;

  throbber->x = x;
  throbber->y = y;

  throbber->start_time = ply_get_timestamp ();

  ply_event_loop_watch_for_timeout (throbber->loop,
                                    1.0 / FRAMES_PER_SECOND,
                                    (ply_event_loop_timeout_handler_t)
                                    on_timeout, throbber);

  return true;
}

void
throbber_stop (throbber_t *throbber)
{
  if (throbber->frame_area.width > 0)
    ply_frame_buffer_fill_with_color (throbber->frame_buffer, &throbber->frame_area,
                                      0.0, 0.43, .71, 1.0);
  throbber->frame_buffer = NULL;
  throbber->window = NULL;

  if (throbber->loop != NULL)
    {
      ply_event_loop_stop_watching_for_timeout (throbber->loop,
                                                (ply_event_loop_timeout_handler_t)
                                                on_timeout, throbber);
      throbber->loop = NULL;
    }
}

long
throbber_get_width (throbber_t *throbber)
{
  return throbber->width;
}

long
throbber_get_height (throbber_t *throbber)
{
  return throbber->height;
}

/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
