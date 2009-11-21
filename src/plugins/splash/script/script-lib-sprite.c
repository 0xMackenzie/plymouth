/* script-lib-sprite.c - script library controling sprites
 *
 * Copyright (C) 2009 Charlie Brej <cbrej@cs.man.ac.uk>
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
 * Written by: Charlie Brej <cbrej@cs.man.ac.uk>
 */
#include "ply-image.h"
#include "ply-utils.h"
#include "ply-logger.h"
#include "ply-key-file.h"
#include "ply-pixel-buffer.h"
#include "ply-pixel-display.h"
#include "script.h"
#include "script-parse.h"
#include "script-execute.h"
#include "script-object.h"
#include "script-lib-image.h"
#include "script-lib-sprite.h"
#include <assert.h>
#include <stdlib.h>
#include <math.h>

#include "script-lib-sprite.script.h"

static void sprite_free (script_obj_t *obj)
{
  sprite_t *sprite = obj->data.native.object_data;
  sprite->remove_me = true;
}

static script_return_t sprite_new (script_state_t *state,
                                   void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;
  script_obj_t *reply;

  sprite_t *sprite = calloc (1, sizeof (sprite_t));

  sprite->x = 0;
  sprite->y = 0;
  sprite->z = 0;
  sprite->opacity = 1.0;
  sprite->old_x = 0;
  sprite->old_y = 0;
  sprite->old_z = 0;
  sprite->old_width = 0;
  sprite->old_height = 0;
  sprite->old_opacity = 1.0;
  sprite->refresh_me = false;
  sprite->remove_me = false;
  sprite->image = NULL;
  sprite->image_obj = NULL;
  ply_list_append_data (data->sprite_list, sprite);

  reply = script_obj_new_native (sprite, data->class);
  return script_return_obj (reply);
}

static script_return_t sprite_set_image (script_state_t *state,
                                         void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;
  sprite_t *sprite = script_obj_as_native_of_class (state->this, data->class);
  script_obj_t *script_obj_image = script_obj_hash_get_element (state->local,
                                                                "image");
  script_obj_deref (&script_obj_image);
  ply_pixel_buffer_t *image = script_obj_as_native_of_class_name (script_obj_image,
                                                                  "image");

  if (image && sprite)
    {
      script_obj_unref (sprite->image_obj);
      script_obj_ref (script_obj_image);
      sprite->image = image;
      sprite->image_obj = script_obj_image;
      sprite->refresh_me = true;
    }
  script_obj_unref (script_obj_image);

  return script_return_obj_null ();
}

static script_return_t sprite_set_x (script_state_t *state,
                                     void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;
  sprite_t *sprite = script_obj_as_native_of_class (state->this, data->class);

  if (sprite)
    sprite->x = script_obj_hash_get_number (state->local, "value");
  return script_return_obj_null ();
}

static script_return_t sprite_set_y (script_state_t *state,
                                     void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;
  sprite_t *sprite = script_obj_as_native_of_class (state->this, data->class);

  if (sprite)
    sprite->y = script_obj_hash_get_number (state->local, "value");
  return script_return_obj_null ();
}

static script_return_t sprite_set_z (script_state_t *state,
                                     void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;
  sprite_t *sprite = script_obj_as_native_of_class (state->this, data->class);

  if (sprite)
    sprite->z = script_obj_hash_get_number (state->local, "value");
  return script_return_obj_null ();
}

static script_return_t sprite_set_opacity (script_state_t *state,
                                           void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;
  sprite_t *sprite = script_obj_as_native_of_class (state->this, data->class);

  if (sprite)
    sprite->opacity = script_obj_hash_get_number (state->local, "value");
  return script_return_obj_null ();
}

static script_return_t sprite_window_get_width (script_state_t *state,
                                                void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;

  return script_return_obj (script_obj_new_number (ply_pixel_display_get_width (data->display)));
}

static script_return_t sprite_window_get_height (script_state_t *state,
                                                 void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;

  return script_return_obj (script_obj_new_number (ply_pixel_display_get_height (data->display)));
}

static uint32_t extract_rgb_color (script_state_t *state)
{
  uint8_t red =   CLAMP (255 * script_obj_hash_get_number (state->local, "red"),   0, 255);
  uint8_t green = CLAMP (255 * script_obj_hash_get_number (state->local, "green"), 0, 255);
  uint8_t blue =  CLAMP (255 * script_obj_hash_get_number (state->local, "blue"),  0, 255);

  return (uint32_t) red << 16 | green << 8 | blue;
}

static script_return_t sprite_window_set_background_top_color (script_state_t *state,
                                                               void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;

  data->background_color_start = extract_rgb_color (state);
  data->full_refresh = true;
  return script_return_obj_null ();
}

static script_return_t sprite_window_set_background_bottom_color (script_state_t *state,
                                                                  void           *user_data)
{
  script_lib_sprite_data_t *data = user_data;

  data->background_color_end = extract_rgb_color (state);
  data->full_refresh = true;
  return script_return_obj_null ();
}

void script_lib_sprite_draw_area (script_lib_sprite_data_t *data,
                                 ply_pixel_buffer_t       *pixel_buffer,
                                 int                       x,
                                 int                       y,
                                 int                       width,
                                 int                       height)
{
  ply_rectangle_t clip_area;

  clip_area.x = x;
  clip_area.y = y;
  clip_area.width = width;
  clip_area.height = height;

  if (data->background_color_start == data->background_color_end)
    ply_pixel_buffer_fill_with_hex_color (pixel_buffer,
                                          &clip_area,
                                          data->background_color_start);
  else
    ply_pixel_buffer_fill_with_gradient (pixel_buffer,
                                         &clip_area,
                                         data->background_color_start,
                                         data->background_color_end);
  ply_list_node_t *node;
  for (node = ply_list_get_first_node (data->sprite_list);
       node;
       node = ply_list_get_next_node (data->sprite_list, node))
    {
      sprite_t *sprite = ply_list_node_get_data (node);
      ply_rectangle_t sprite_area;
      
      if (!sprite->image) continue;
      if (sprite->remove_me) continue;
      if (sprite->opacity < 0.011) continue;
      
      ply_pixel_buffer_get_size (sprite->image, &sprite_area);
      
      sprite_area.x = sprite->x;
      sprite_area.y = sprite->y;

      if (sprite_area.x >= (x + width)) continue;
      if (sprite_area.y >= (y + height)) continue;

      if ((sprite_area.x + (int) sprite_area.width) <= x) continue;
      if ((sprite_area.y + (int) sprite_area.height) <= y) continue;
      ply_pixel_buffer_fill_with_argb32_data_at_opacity_with_clip (pixel_buffer,
                                                                   &sprite_area,
                                                                   &clip_area,
                                                                   0, 0,
                                                                   ply_pixel_buffer_get_argb32_data (sprite->image),
                                                                   sprite->opacity);
    }
}

void draw_area (script_lib_sprite_data_t *data,
                int                       x,
                int                       y,
                int                       width,
                int                       height)
{
  ply_pixel_display_draw_area (data->display, x, y, width, height);
}

script_lib_sprite_data_t *script_lib_sprite_setup (script_state_t      *state,
                                                   ply_pixel_display_t *display)
{
  script_lib_sprite_data_t *data = malloc (sizeof (script_lib_sprite_data_t));

  data->class = script_obj_native_class_new (sprite_free, "sprite", data);
  data->sprite_list = ply_list_new ();
  data->display = display;

  script_obj_t *sprite_hash = script_obj_hash_get_element (state->global, "Sprite");
  script_add_native_function (sprite_hash,
                              "_New",
                              sprite_new,
                              data,
                              NULL);
  script_add_native_function (sprite_hash,
                              "SetImage",
                              sprite_set_image,
                              data,
                              "image",
                              NULL);
  script_add_native_function (sprite_hash,
                              "SetX",
                              sprite_set_x,
                              data,
                              "value",
                              NULL);
  script_add_native_function (sprite_hash,
                              "SetY",
                              sprite_set_y,
                              data,
                              "value",
                              NULL);
  script_add_native_function (sprite_hash,
                              "SetZ",
                              sprite_set_z,
                              data,
                              "value",
                              NULL);
  script_add_native_function (sprite_hash,
                              "SetOpacity",
                              sprite_set_opacity,
                              data,
                              "value",
                              NULL);
  script_obj_unref (sprite_hash);

  
  script_obj_t *window_hash = script_obj_hash_get_element (state->global, "Window");
  script_add_native_function (window_hash,
                              "GetWidth",
                              sprite_window_get_width,
                              data,
                              NULL);
  script_add_native_function (window_hash,
                              "GetHeight",
                              sprite_window_get_height,
                              data,
                              NULL);
  script_add_native_function (window_hash,
                              "SetBackgroundTopColor",
                              sprite_window_set_background_top_color,
                              data,
                              "red",
                              "green",
                              "blue",
                              NULL);
  script_add_native_function (window_hash,
                              "SetBackgroundBottomColor",
                              sprite_window_set_background_bottom_color,
                              data,
                              "red",
                              "green",
                              "blue",
                              NULL);
  script_obj_unref (window_hash);

  data->script_main_op = script_parse_string (script_lib_sprite_string, "script-lib-sprite.script");
  data->background_color_start = 0x000000;
  data->background_color_end   = 0x000000;
  data->full_refresh = true;
  script_return_t ret = script_execute (state, data->script_main_op);
  script_obj_unref (ret.object);
  return data;
}

static int
sprite_compare_z(void *data_a, void *data_b)
{
 sprite_t *sprite_a = data_a;
 sprite_t *sprite_b = data_b;
 return sprite_a->z - sprite_b->z;
}

void script_lib_sprite_refresh (script_lib_sprite_data_t *data)
{
  ply_list_node_t *node;
  
  ply_list_sort (data->sprite_list, &sprite_compare_z);
  
  node = ply_list_get_first_node (data->sprite_list);

  while (node)
    {
      sprite_t *sprite = ply_list_node_get_data (node);
      ply_list_node_t *next_node = ply_list_get_next_node (data->sprite_list,
                                                           node);
      if (sprite->remove_me)
        {
          if (sprite->image)
            draw_area (data,
                       sprite->old_x,
                       sprite->old_y,
                       sprite->old_width,
                       sprite->old_height);
          ply_list_remove_node (data->sprite_list, node);
          script_obj_unref (sprite->image_obj);
          free (sprite);
        }
      node = next_node;
    }

  for (node = ply_list_get_first_node (data->sprite_list);
       node;
       node = ply_list_get_next_node (data->sprite_list, node))
    {
      sprite_t *sprite = ply_list_node_get_data (node);
      if (!sprite->image) continue;
      if ((sprite->x != sprite->old_x)
          || (sprite->y != sprite->old_y)
          || (sprite->z != sprite->old_z)
          || (fabs (sprite->old_opacity - sprite->opacity) > 0.01)      /* People can't see the difference between */
          || sprite->refresh_me)
        {
          ply_rectangle_t size;
          ply_pixel_buffer_get_size (sprite->image, &size);
          draw_area (data, sprite->x, sprite->y, size.width, size.height);
          draw_area (data,
                     sprite->old_x,
                     sprite->old_y,
                     sprite->old_width,
                     sprite->old_height);
          sprite->old_x = sprite->x;
          sprite->old_y = sprite->y;
          sprite->old_z = sprite->z;
          sprite->old_width = size.width;
          sprite->old_height = size.height;
          sprite->old_opacity = sprite->opacity;
          sprite->refresh_me = false;
        }
    }

  if (data->full_refresh)
    {
      draw_area (data, 0, 0,
                 ply_pixel_display_get_width (data->display),
                 ply_pixel_display_get_height (data->display));
      data->full_refresh = false;
      return;
    }
}

void script_lib_sprite_destroy (script_lib_sprite_data_t *data)
{
  ply_list_node_t *node = ply_list_get_first_node (data->sprite_list);

  while (node)
    {
      sprite_t *sprite = ply_list_node_get_data (node);
      ply_list_node_t *next_node = ply_list_get_next_node (data->sprite_list,
                                                           node);
      ply_list_remove_node (data->sprite_list, node);
      script_obj_unref (sprite->image_obj);
      free (sprite);
      node = next_node;
    }

  ply_list_free (data->sprite_list);
  script_parse_op_free (data->script_main_op);
  script_obj_native_class_destroy (data->class);
  free (data);
}

