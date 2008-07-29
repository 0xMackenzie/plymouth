/* ply-window.h - APIs for putting up a window screen
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
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include "config.h"
#include "ply-window.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>
#include <wchar.h>

#include <linux/kd.h>
#include <linux/vt.h>

#include "ply-buffer.h"
#include "ply-event-loop.h"
#include "ply-frame-buffer.h"
#include "ply-logger.h"
#include "ply-utils.h"

#define KEY_CTRL_P ('\100' ^'P')
#define KEY_CTRL_T ('\100' ^'T')
#define KEY_CTRL_U ('\100' ^'U')
#define KEY_CTRL_W ('\100' ^'W')
#define KEY_CTRL_V ('\100' ^'V')
#define KEY_ESCAPE ('\100' ^'[')
#define KEY_RETURN '\r'
#define KEY_BACKSPACE '\177'

#ifndef CLEAR_SCREEN_SEQUENCE
#define CLEAR_SCREEN_SEQUENCE "\033[2J"
#endif

#ifndef MOVE_CURSOR_SEQUENCE
#define MOVE_CURSOR_SEQUENCE "\033[%d;%df"
#endif

#ifndef HIDE_CURSOR_SEQUENCE
#define HIDE_CURSOR_SEQUENCE "\033[?25l"
#endif

#ifndef SHOW_CURSOR_SEQUENCE
#define SHOW_CURSOR_SEQUENCE "\033[?25h"
#endif

#ifndef COLOR_SEQUENCE_FORMAT
#define COLOR_SEQUENCE_FORMAT "\033[%dm"
#endif

#ifndef FOREGROUND_COLOR_BASE
#define FOREGROUND_COLOR_BASE 30
#endif

#ifndef BACKGROUND_COLOR_BASE
#define BACKGROUND_COLOR_BASE 40
#endif

#ifndef TEXT_PALETTE_SIZE
#define TEXT_PALETTE_SIZE 48
#endif

struct _ply_window
{
  ply_event_loop_t *loop;
  ply_buffer_t     *keyboard_input_buffer;
  ply_buffer_t     *line_buffer;

  struct termios    original_term_attributes;

  ply_frame_buffer_t *frame_buffer;

  char *tty_name;
  int   tty_fd;
  int   vt_number;

  ply_fd_watch_t *tty_fd_watch;
  ply_window_mode_t mode;
  ply_window_color_t foreground_color;
  ply_window_color_t background_color;

  uint8_t original_color_palette[TEXT_PALETTE_SIZE];
  uint8_t color_palette[TEXT_PALETTE_SIZE];

  int number_of_text_rows;
  int number_of_text_columns;

  uint32_t should_force_text_mode : 1;
  uint32_t original_term_attributes_saved : 1;

  ply_window_keyboard_input_handler_t keyboard_input_handler;
  void *keyboard_input_handler_user_data;

  ply_window_backspace_handler_t backspace_handler;
  void *backspace_handler_user_data;

  ply_window_escape_handler_t escape_handler;
  void *escape_handler_user_data;

  ply_window_enter_handler_t enter_handler;
  void *enter_handler_user_data;

  ply_window_draw_handler_t draw_handler;
  void *draw_handler_user_data;

  ply_window_erase_handler_t erase_handler;
  void *erase_handler_user_data;
};

ply_window_t *
ply_window_new (int vt_number)
{
  ply_window_t *window;

  window = calloc (1, sizeof (ply_window_t));
  window->keyboard_input_buffer = ply_buffer_new ();
  window->line_buffer = ply_buffer_new ();
  window->frame_buffer = ply_frame_buffer_new (NULL);
  window->loop = NULL;
  asprintf (&window->tty_name, "/dev/tty%d", vt_number);
  window->tty_fd = -1;
  window->vt_number = vt_number;

  return window;
}

static bool
ply_window_look_up_color_palette (ply_window_t *window)
{
  if (ioctl (window->tty_fd, GIO_CMAP, window->color_palette) < 0)
      return false;

  return true;
}

static bool
ply_window_change_color_palette (ply_window_t *window)
{
  if (ioctl (window->tty_fd, PIO_CMAP, window->color_palette) < 0)
    return false;

  return true;
}

static void
ply_window_save_color_palette (ply_window_t *window)
{
  memcpy (window->original_color_palette, window->color_palette,
          TEXT_PALETTE_SIZE);
}

static void
ply_window_restore_color_palette (ply_window_t *window)
{
  memcpy (window->color_palette, window->original_color_palette,
          TEXT_PALETTE_SIZE);

  ply_window_change_color_palette (window);
}

static void
process_backspace (ply_window_t *window)
{
  ssize_t bytes_to_remove;
  ssize_t previous_character_size;
  const char *bytes;
  size_t size;

  bytes = ply_buffer_get_bytes (window->line_buffer);
  size = ply_buffer_get_size (window->line_buffer);

  bytes_to_remove = MB_CUR_MAX;
  while ((previous_character_size = mbrlen (bytes + size - bytes_to_remove, bytes_to_remove, NULL)) < bytes_to_remove &&
         previous_character_size > 0)
    bytes_to_remove -= previous_character_size;

  if (bytes_to_remove <= size)
    {
      ply_buffer_remove_bytes_at_end (window->line_buffer, bytes_to_remove);

      if (window->backspace_handler != NULL)
        window->backspace_handler (window->backspace_handler_user_data);
    }
}

static void
process_line_erase (ply_window_t *window)
{
  size_t size;

  while ((size = ply_buffer_get_size (window->line_buffer)) > 0)
    process_backspace (window);
}

static void
process_keyboard_input (ply_window_t *window,
                        const char   *keyboard_input,
                        size_t        character_size)
{
  wchar_t key;

  if (mbrtowc (&key, keyboard_input, 1, NULL) > 0)
    {
      switch (key)
        {

          case KEY_CTRL_P:
            ply_trace ("restore text palette to original value!");
            ply_window_restore_color_palette (window);
          return;

          case KEY_CTRL_T:
            ply_trace ("toggle text mode!");
            window->should_force_text_mode = !window->should_force_text_mode;
            ply_window_set_mode (window, window->mode);
            ply_trace ("text mode toggled!");
          return;

          case KEY_CTRL_U:
          case KEY_CTRL_W:
            ply_trace ("erase line!");
            process_line_erase (window);
          return;

          case KEY_CTRL_V:
            ply_trace ("toggle verbose mode!");
            ply_toggle_tracing ();
            ply_trace ("verbose mode toggled!");
          return;

          case KEY_ESCAPE:
            ply_trace ("escape key!");
            if (window->escape_handler != NULL)
              window->escape_handler (window->escape_handler_user_data);
            ply_trace ("end escape key handler");
          return;

          case KEY_BACKSPACE:
            ply_trace ("backspace key!");
            process_backspace (window);
          return;

          case KEY_RETURN:
            ply_trace ("return key!");

            if (window->enter_handler != NULL)
              window->enter_handler (window->enter_handler_user_data,
                                     ply_buffer_get_bytes (window->line_buffer));

            ply_buffer_clear (window->line_buffer);
          return;

          default:
            ply_buffer_append_bytes (window->line_buffer,
                                     keyboard_input, character_size);
          break;
        }
    }

  if (window->keyboard_input_handler != NULL)
    window->keyboard_input_handler (window->keyboard_input_handler_user_data,
                                    keyboard_input, character_size);
}

static void
check_buffer_for_key_events (ply_window_t *window)
{
  const char *bytes;
  size_t size, i;

  bytes = ply_buffer_get_bytes (window->keyboard_input_buffer);
  size = ply_buffer_get_size (window->keyboard_input_buffer);

  i = 0;
  while (i < size)
    {
      ssize_t character_size;
      char *keyboard_input;

      character_size = (ssize_t) mbrlen (bytes + i, size - i, NULL);

      if (character_size < 0)
        break;

      keyboard_input = strndup (bytes + i, character_size);

      process_keyboard_input (window, keyboard_input, character_size);

      free (keyboard_input);

      i += character_size;
    }

  if (i > 0)
    ply_buffer_remove_bytes (window->keyboard_input_buffer, i);
}

static void
on_key_event (ply_window_t *window)
{
  ply_buffer_append_from_fd (window->keyboard_input_buffer, window->tty_fd);

  check_buffer_for_key_events (window);
}

static bool
ply_window_set_unbuffered_input (ply_window_t *window)
{
  struct termios term_attributes;

  tcgetattr (window->tty_fd, &term_attributes);

  if (!window->original_term_attributes_saved)
    {
      window->original_term_attributes = term_attributes;
      window->original_term_attributes_saved = true;
    }

  cfmakeraw (&term_attributes);

  /* Make \n return go to the beginning of the next line */
  term_attributes.c_oflag |= ONLCR;

  if (tcsetattr (window->tty_fd, TCSAFLUSH, &term_attributes) != 0)
    return false;

  return true;
}

static bool
ply_window_set_buffered_input (ply_window_t *window)
{
  if (!window->original_term_attributes_saved)
    return false;

  if (tcsetattr (window->tty_fd, TCSAFLUSH, &window->original_term_attributes) != 0)
    return false;

  return true;
}

static int
get_active_vt (void)
{
  int console_fd;
  struct vt_stat console_state = { 0 };

  console_fd = open ("/dev/tty0", O_RDONLY | O_NOCTTY);

  if (console_fd < 0)
    goto out;

  if (ioctl (console_fd, VT_GETSTATE, &console_state) < 0)
    goto out;

out:
  if (console_fd >= 0)
    close (console_fd);

  return console_state.v_active;
}

static bool
ply_window_look_up_geometry (ply_window_t *window)
{
    struct winsize window_size;

    ply_trace ("looking up window text geometry");

    if (ioctl (window->tty_fd, TIOCGWINSZ, &window_size) < 0)
      {
        ply_trace ("could not read window text geometry: %m");
        return false;
      }

    window->number_of_text_rows = window_size.ws_row;
    window->number_of_text_columns = window_size.ws_col;

    ply_trace ("window is now %dx%d text cells",
               window->number_of_text_columns,
               window->number_of_text_rows);

    return true;
}

bool
ply_window_open (ply_window_t *window)
{
  assert (window != NULL);
  assert (window->tty_name != NULL);
  assert (window->tty_fd < 0);

  if (window->vt_number == 0)
    {
      window->vt_number = get_active_vt ();
      free (window->tty_name);
      asprintf (&window->tty_name, "/dev/tty%d", window->vt_number);
    }

  window->tty_fd = open (window->tty_name, O_RDWR | O_NOCTTY);

  if (window->tty_fd < 0)
    return false;

  if (!ply_window_set_unbuffered_input (window))
    return false;

  if (!ply_window_set_mode (window, PLY_WINDOW_MODE_TEXT))
    return false;

  if (!ply_window_look_up_geometry (window))
    return false;

  if (!ply_window_look_up_color_palette (window))
    return false;

  ply_window_save_color_palette (window);

  ply_window_hide_text_cursor (window);
  ply_window_set_text_cursor_position (window, 0, 0);

  ply_event_loop_watch_signal (window->loop,
                               SIGWINCH,
                               (ply_event_handler_t)
                               ply_window_look_up_geometry,
                               window);

  if (window->loop != NULL)
    window->tty_fd_watch = ply_event_loop_watch_fd (window->loop, window->tty_fd,
                                                    PLY_EVENT_LOOP_FD_STATUS_HAS_DATA,
                                                    (ply_event_handler_t) on_key_event,
                                                    NULL, window);

  /* We try to open the frame buffer, but it may fail. splash plugins can check
   * to see if it's open and react accordingly
   */
  ply_frame_buffer_open (window->frame_buffer);

  return true;
}

void
ply_window_close (ply_window_t *window)
{
  ply_window_restore_color_palette (window);
  ply_window_set_text_cursor_position (window, 0, 0);

  if (ply_frame_buffer_device_is_open (window->frame_buffer))
    ply_frame_buffer_close (window->frame_buffer);

  if (window->tty_fd_watch != NULL)
    {
      ply_event_loop_stop_watching_fd (window->loop, window->tty_fd_watch);
      window->tty_fd_watch = NULL;
    }

  if (window->loop != NULL)
    ply_event_loop_stop_watching_signal (window->loop, SIGWINCH);

  ply_window_set_buffered_input (window);

  close (window->tty_fd);
  window->tty_fd = -1;
}

bool
ply_window_set_mode (ply_window_t      *window,
                     ply_window_mode_t  mode)
{
  assert (window != NULL);
  assert (mode == PLY_WINDOW_MODE_TEXT || mode == PLY_WINDOW_MODE_GRAPHICS);

  if (ply_is_tracing ())
    window->should_force_text_mode = true;

  switch (mode)
    {
      case PLY_WINDOW_MODE_TEXT:
        if (ioctl (window->tty_fd, KDSETMODE, KD_TEXT) < 0)
          return false;
        break;

      case PLY_WINDOW_MODE_GRAPHICS:
        if (!ply_frame_buffer_device_is_open (window->frame_buffer)
            && !ply_frame_buffer_open (window->frame_buffer))
          return false;

        if (ioctl (window->tty_fd, KDSETMODE,
                   window->should_force_text_mode? KD_TEXT : KD_GRAPHICS) < 0)
          return false;
        break;
    }
  ply_window_set_unbuffered_input (window);

  window->mode = mode;
  return true;
}

int
ply_window_get_number_of_text_rows (ply_window_t *window)
{
  return window->number_of_text_rows;
}

int
ply_window_get_number_of_text_columns (ply_window_t *window)
{
  return window->number_of_text_columns;
}

void
ply_window_set_text_cursor_position (ply_window_t *window,
                                     int           column,
                                     int           row)
{
  char *sequence;

  sequence = NULL;
  asprintf (&sequence, MOVE_CURSOR_SEQUENCE, row, column);
  write (window->tty_fd, sequence, strlen (sequence));
  free (sequence);
}

void
ply_window_clear_screen (ply_window_t *window)
{
  if (ply_frame_buffer_device_is_open (window->frame_buffer))
    ply_frame_buffer_fill_with_color (window->frame_buffer, NULL, 0.0, 0.0, 0.0, 1.0);

  write (window->tty_fd, CLEAR_SCREEN_SEQUENCE, strlen (CLEAR_SCREEN_SEQUENCE));

  ply_window_set_text_cursor_position (window, 0, 0);
}

void
ply_window_set_background_color (ply_window_t       *window,
                                 ply_window_color_t  color)
{
  char *sequence;

  sequence = NULL;
  asprintf (&sequence, COLOR_SEQUENCE_FORMAT,
            BACKGROUND_COLOR_BASE + color);
  write (window->tty_fd, sequence, strlen (sequence));
  free (sequence);

  window->background_color = color;
}

void
ply_window_set_foreground_color (ply_window_t       *window,
                                 ply_window_color_t  color)
{
  char *sequence;

  sequence = NULL;
  asprintf (&sequence, COLOR_SEQUENCE_FORMAT,
            FOREGROUND_COLOR_BASE + color);
  write (window->tty_fd, sequence, strlen (sequence));
  free (sequence);

  window->foreground_color = color;
}

ply_window_color_t
ply_window_get_background_color (ply_window_t *window)
{
  return window->background_color;
}

ply_window_color_t
ply_window_get_foreground_color (ply_window_t *window)
{
  return window->foreground_color;
}

void
ply_window_draw_area (ply_window_t *window,
                      int           x,
                      int           y,
                      int           width,
                      int           height)
{
  if (window->draw_handler != NULL)
    window->draw_handler (window->draw_handler_user_data,
                          x, y, width, height);
}

void
ply_window_erase_area (ply_window_t *window,
                       int           x,
                       int           y,
                       int           width,
                       int           height)
{
  if (window->erase_handler != NULL)
    window->erase_handler (window->erase_handler_user_data,
                           x, y, width, height);
}

uint32_t
ply_window_get_color_hex_value (ply_window_t       *window,
                                ply_window_color_t  color)
{
  uint8_t red, green, blue;
  uint32_t hex_value;

  assert (window != NULL);
  assert (color >= 0 && color <= PLY_WINDOW_COLOR_WHITE);

  red = (uint8_t) *(window->color_palette + 3 * color);
  green = (uint8_t) *(window->color_palette + 3 * color + 1);
  blue = (uint8_t) *(window->color_palette + 3 * color + 2);

  hex_value = red << 16 | green << 8 | blue;

  return hex_value;
}

void
ply_window_set_color_hex_value (ply_window_t       *window,
                                ply_window_color_t  color,
                                uint32_t            hex_value)
{
  uint8_t red, green, blue;

  assert (window != NULL);
  assert (color >= 0 && color <= PLY_WINDOW_COLOR_WHITE);

  red = (uint8_t) ((hex_value >> 16) & 0xff);
  green = (uint8_t) ((hex_value >> 8) & 0xff);
  blue = (uint8_t) (hex_value & 0xff);

  *(window->color_palette + 3 * color) = red;
  *(window->color_palette + 3 * color + 1) = green;
  *(window->color_palette + 3 * color + 2) = blue;

  ply_window_change_color_palette (window);
}

void
ply_window_hide_text_cursor (ply_window_t *window)
{
  write (window->tty_fd, HIDE_CURSOR_SEQUENCE, strlen (HIDE_CURSOR_SEQUENCE));
}

void
ply_window_show_text_cursor (ply_window_t *window)
{
  write (window->tty_fd, SHOW_CURSOR_SEQUENCE, strlen (SHOW_CURSOR_SEQUENCE));
}

static void
ply_window_detach_from_event_loop (ply_window_t *window)
{
  assert (window != NULL);
  window->loop = NULL;
  window->tty_fd_watch = NULL;
}

void
ply_window_free (ply_window_t *window)
{
  if (window == NULL)
    return;

  if (window->loop != NULL)
    ply_event_loop_stop_watching_for_exit (window->loop,
                                           (ply_event_loop_exit_handler_t)
                                           ply_window_detach_from_event_loop,
                                           window);

  ply_window_close (window);

  ply_buffer_free (window->keyboard_input_buffer);
  ply_buffer_free (window->line_buffer);

  ply_frame_buffer_free (window->frame_buffer);

  free (window);
}

void
ply_window_set_keyboard_input_handler (ply_window_t *window,
                                       ply_window_keyboard_input_handler_t input_handler,
                                       void       *user_data)
{
  assert (window != NULL);

  window->keyboard_input_handler = input_handler;
  window->keyboard_input_handler_user_data = user_data;
}

void
ply_window_set_backspace_handler (ply_window_t *window,
                                  ply_window_backspace_handler_t backspace_handler,
                                  void         *user_data)
{
  assert (window != NULL);

  window->backspace_handler = backspace_handler;
  window->backspace_handler_user_data = user_data;
}

void
ply_window_set_escape_handler (ply_window_t *window,
                               ply_window_escape_handler_t escape_handler,
                               void       *user_data)
{
  assert (window != NULL);

  window->escape_handler = escape_handler;
  window->escape_handler_user_data = user_data;
}

void
ply_window_set_enter_handler (ply_window_t *window,
                              ply_window_enter_handler_t enter_handler,
                              void         *user_data)
{
  assert (window != NULL);

  window->enter_handler = enter_handler;
  window->enter_handler_user_data = user_data;
}

void
ply_window_set_draw_handler (ply_window_t *window,
                             ply_window_draw_handler_t draw_handler,
                             void         *user_data)
{
  assert (window != NULL);

  window->draw_handler = draw_handler;
  window->draw_handler_user_data = user_data;
}

void
ply_window_set_erase_handler (ply_window_t *window,
                              ply_window_erase_handler_t erase_handler,
                              void         *user_data)
{
  assert (window != NULL);

  window->erase_handler = erase_handler;
  window->erase_handler_user_data = user_data;
}

void
ply_window_attach_to_event_loop (ply_window_t     *window,
                                 ply_event_loop_t *loop)
{
  assert (window != NULL);
  assert (loop != NULL);
  assert (window->loop == NULL);

  window->loop = loop;

  ply_event_loop_watch_for_exit (loop, (ply_event_loop_exit_handler_t)
                                 ply_window_detach_from_event_loop,
                                 window);
}

ply_frame_buffer_t *
ply_window_get_frame_buffer (ply_window_t *window)
{
  return window->frame_buffer;
}

static bool
switch_to_vt (int tty_fd,
              int vt_number)
{
  if (ioctl (tty_fd, VT_ACTIVATE, vt_number) < 0)
    return false;

  ioctl (tty_fd, VT_WAITACTIVE, vt_number);

  return get_active_vt () == vt_number;
}

bool
ply_window_take_console (ply_window_t *window)
{
  assert (window != NULL);

  return switch_to_vt (window->tty_fd, window->vt_number);
}

bool
ply_window_give_console (ply_window_t *window,
                         int           vt_number)
{
  char *tty_name;
  int   tty_fd;
  bool vt_switched;

  assert (window != NULL);
  assert (vt_number > 0);
  assert (vt_number != window->vt_number);

  tty_name = NULL;
  asprintf (&tty_name, "/dev/tty%d", vt_number);
  tty_fd = open (tty_name, O_RDONLY | O_NOCTTY);

  if (tty_fd < 0)
    {
      free (tty_name);
      return false;
    }
  free (tty_name);

  vt_switched = switch_to_vt (tty_fd, vt_number);

  close (tty_fd);

  return vt_switched;
}

#ifdef PLY_WINDOW_ENABLE_TEST

#include <stdio.h>

#include "ply-event-loop.h"
#include "ply-window.h"

static void
on_timeout (ply_window_t     *window,
            ply_event_loop_t *loop)
{
  ply_event_loop_exit (loop, 0);
}

static void
on_keypress (ply_window_t *window,
             const char   *keyboard_input)
{
  printf ("key '%c' (0x%x) was pressed\n",
          keyboard_input[0], (unsigned int) keyboard_input[0]);
}

int
main (int    argc,
      char **argv)
{
  ply_event_loop_t *loop;
  ply_window_t *window;
  int exit_code;
  int vt_number;

  exit_code = 0;

  loop = ply_event_loop_new ();

  vt_number = 0;
  if (argc > 1)
    vt_number = atoi (argv[1]);

  if (vt_number <= 0)
    vt_number = 1;

  window = ply_window_new (vt_number);
  ply_window_attach_to_event_loop (window, loop);
  ply_window_set_keyboard_input_handler (window,
                                         (ply_window_keyboard_input_handler_t)
                                         on_keypress, window);

  if (!ply_window_open (window))
    {
      ply_save_errno ();
      perror ("could not open window");
      ply_restore_errno ();
      return errno;
    }

  if (!ply_window_take_console (window))
    {
      ply_save_errno ();
      ply_error ("could not move console to vt %d: %m", vt_number);
      ply_restore_errno ();
      return errno;
    }

  if (!ply_window_set_mode (window, PLY_WINDOW_MODE_TEXT))
    {
      ply_save_errno ();
      perror ("could not set window for graphics mode");
      ply_restore_errno ();
    }

  ply_event_loop_watch_for_timeout (loop,
                                    15.0,
                                   (ply_event_loop_timeout_handler_t)
                                   on_timeout,
                                   window);
  exit_code = ply_event_loop_run (loop);

  ply_window_close (window);
  ply_window_free (window);

  return exit_code;
}

#endif /* PLY_WINDOW_ENABLE_TEST */
/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
