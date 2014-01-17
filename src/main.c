/* main.c - boot messages monitor
 *
 * Copyright (C) 2007 Red Hat, Inc
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include "config.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <math.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sysexits.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <wchar.h>
#include <paths.h>
#include <assert.h>
#include <values.h>

#include <linux/kd.h>
#include <linux/vt.h>

#include "ply-buffer.h"
#include "ply-command-parser.h"
#include "ply-boot-server.h"
#include "ply-boot-splash.h"
#include "ply-device-manager.h"
#include "ply-event-loop.h"
#include "ply-hashtable.h"
#include "ply-list.h"
#include "ply-logger.h"
#include "ply-renderer.h"
#include "ply-terminal-session.h"
#include "ply-trigger.h"
#include "ply-utils.h"
#include "ply-progress.h"

#ifndef PLY_MAX_COMMAND_LINE_SIZE
#define PLY_MAX_COMMAND_LINE_SIZE 512
#endif

#define BOOT_DURATION_FILE     PLYMOUTH_TIME_DIRECTORY "/boot-duration"
#define SHUTDOWN_DURATION_FILE PLYMOUTH_TIME_DIRECTORY "/shutdown-duration"

typedef enum {
  PLY_MODE_BOOT,
  PLY_MODE_SHUTDOWN,
  PLY_MODE_UPDATES
} ply_mode_t;

typedef struct 
{
  const char    *keys;
  ply_trigger_t *trigger;
} ply_keystroke_watch_t;

typedef struct 
{
  enum {PLY_ENTRY_TRIGGER_TYPE_PASSWORD,
        PLY_ENTRY_TRIGGER_TYPE_QUESTION}
        type;
  const char    *prompt;
  ply_trigger_t *trigger;
} ply_entry_trigger_t;

typedef struct
{
  ply_event_loop_t *loop;
  ply_boot_server_t *boot_server;
  ply_boot_splash_t *boot_splash;
  ply_terminal_session_t *session;
  ply_buffer_t *boot_buffer;
  ply_progress_t *progress;
  ply_list_t *keystroke_triggers;
  ply_list_t *entry_triggers;
  ply_buffer_t *entry_buffer;
  ply_list_t *messages;
  ply_command_parser_t *command_parser;
  ply_mode_t mode;
  ply_terminal_t *local_console_terminal;
  ply_device_manager_t *device_manager;

  ply_trigger_t *show_trigger;
  ply_trigger_t *deactivate_trigger;
  ply_trigger_t *quit_trigger;

  double start_time;
  double splash_delay;

  char kernel_command_line[PLY_MAX_COMMAND_LINE_SIZE];
  uint32_t kernel_command_line_is_set : 1;
  uint32_t no_boot_log : 1;
  uint32_t showing_details : 1;
  uint32_t system_initialized : 1;
  uint32_t is_redirected : 1;
  uint32_t is_attached : 1;
  uint32_t should_be_attached : 1;
  uint32_t should_retain_splash : 1;
  uint32_t is_inactive : 1;
  uint32_t is_shown : 1;
  uint32_t should_force_details : 1;

  char *override_splash_path;
  char *system_default_splash_path;
  char *distribution_default_splash_path;
  const char *default_tty;

  int number_of_errors;
} state_t;

static void show_splash (state_t *state);
static ply_boot_splash_t *load_built_in_theme (state_t *state);
static ply_boot_splash_t *load_theme (state_t    *state,
                                      const char *theme_path);
static ply_boot_splash_t *show_theme (state_t    *state,
                                      const char *theme_path);

static void attach_splash_to_seats (state_t           *state,
                                    ply_boot_splash_t *splash);
static bool attach_to_running_session (state_t *state);
static void detach_from_running_session (state_t *state);
static void on_escape_pressed (state_t *state);
static void dump_details_and_quit_splash (state_t *state);
static void update_display (state_t *state);

static void on_error_message (ply_buffer_t *debug_buffer,
                              const void   *bytes,
                              size_t        number_of_bytes);
static ply_buffer_t *debug_buffer;
static char *debug_buffer_path = NULL;
static char *pid_file = NULL;
static void toggle_between_splash_and_details (state_t *state);
#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
static void tell_systemd_to_print_details (state_t *state);
static void tell_systemd_to_stop_printing_details (state_t *state);
#endif
static const char * get_cache_file_for_mode (ply_mode_t mode);
static void on_escape_pressed (state_t *state);
static void on_enter (state_t    *state,
                      const char *line);
static void on_keyboard_input (state_t    *state,
                               const char *keyboard_input,
                               size_t      character_size);
static void on_backspace (state_t *state);

static void
on_session_output (state_t    *state,
                   const char *output,
                   size_t      size)
{
  ply_buffer_append_bytes (state->boot_buffer, output, size);
  if (state->boot_splash != NULL)
    ply_boot_splash_update_output (state->boot_splash,
                                   output, size);
}

static void
on_session_hangup (state_t *state)
{
  ply_trace ("got hang up on terminal session fd");
}

static void
on_update (state_t     *state,
           const char  *status)
{
  ply_trace ("updating status to '%s'", status);
  ply_progress_status_update (state->progress,
                               status);
  if (state->boot_splash != NULL)
    ply_boot_splash_update_status (state->boot_splash,
                                   status);
}

static void
on_change_mode (state_t     *state,
                const char  *mode)
{
  if (state->boot_splash == NULL)
    {
      ply_trace ("no splash set");
      return;
    }

  ply_trace ("updating mode to '%s'", mode);
  if (strcmp (mode, "boot-up") == 0)
    state->mode = PLY_BOOT_SPLASH_MODE_BOOT_UP;
  else if (strcmp (mode, "shutdown") == 0)
    state->mode = PLY_BOOT_SPLASH_MODE_SHUTDOWN;
  else if (strcmp (mode, "updates") == 0)
    state->mode = PLY_BOOT_SPLASH_MODE_UPDATES;
  else
    return;

  if (!ply_boot_splash_show (state->boot_splash, state->mode))
    {
      ply_trace ("failed to update splash");
      return;
    }
}

static void
on_system_update (state_t     *state,
                  int          progress)
{
  if (state->boot_splash == NULL)
    {
      ply_trace ("no splash set");
      return;
    }

  ply_trace ("setting system update to '%i'", progress);
  if (!ply_boot_splash_system_update (state->boot_splash, progress))
    {
      ply_trace ("failed to update splash");
      return;
    }
}

static void
show_messages (state_t *state)
{
  if (state->boot_splash == NULL)
    {
      ply_trace ("not displaying messages, since no boot splash");
      return;
    }

  ply_list_node_t *node = ply_list_get_first_node (state->messages);
  while (node != NULL)
    {
      ply_list_node_t *next_node;
      char *message = ply_list_node_get_data (node);

      ply_trace ("displaying messages");

      ply_boot_splash_display_message (state->boot_splash, message);
      next_node = ply_list_get_next_node (state->messages, node);
      node = next_node;
    }
}

static bool
load_settings (state_t     *state,
               const char  *path,
               char       **theme_path)
{
  ply_key_file_t *key_file = NULL;
  const char *delay_string;
  bool settings_loaded = false;
  const char *splash_string;

  ply_trace ("Trying to load %s", path);
  key_file = ply_key_file_new (path);

  if (!ply_key_file_load (key_file))
    goto out;

  splash_string = ply_key_file_get_value (key_file, "Daemon", "Theme");

  if (splash_string == NULL)
    goto out;

  asprintf (theme_path,
            PLYMOUTH_THEME_PATH "%s/%s.plymouth",
            splash_string, splash_string);

  if (isnan (state->splash_delay))
    {
      delay_string = ply_key_file_get_value (key_file, "Daemon", "ShowDelay");

      if (delay_string != NULL)
        {
          state->splash_delay = atof (delay_string);
          ply_trace ("Splash delay is set to %lf", state->splash_delay);
        }
    }

  settings_loaded = true;
out:
  ply_key_file_free (key_file);

  return settings_loaded;
}

static void
show_detailed_splash (state_t *state)
{
  ply_boot_splash_t *splash;

  if (state->boot_splash != NULL)
    return;

  ply_trace ("Showing detailed splash screen");
  splash = show_theme (state, NULL);

  if (splash == NULL)
    {
      ply_trace ("Could not start detailed splash screen, this could be a problem.");
      return;
    }

  state->boot_splash = splash;
}

static const char *
command_line_get_string_after_prefix (const char *command_line,
                                      const char *prefix)
{
  char *argument;

  argument = strstr (command_line, prefix);

  if (argument == NULL)
    return NULL;

  if (argument == command_line ||
      argument[-1] == ' ')
    return argument + strlen (prefix);

  return NULL;
}

static bool
command_line_has_argument (const char *command_line,
                           const char *argument)
{
    const char *string;

    string = command_line_get_string_after_prefix (command_line, argument);

    if (string == NULL)
      return false;

    if (!isspace ((int) string[0]) && string[0] != '\0')
      return false;

    return true;
}

static void
find_override_splash (state_t *state)
{
  const char *splash_string;

  if (state->override_splash_path != NULL)
      return;

  splash_string = command_line_get_string_after_prefix (state->kernel_command_line,
                                                        "plymouth.splash=");

  if (splash_string != NULL)
    {
      const char *end;
      int length;

      end = splash_string + strcspn (splash_string, " \n");
      length = end - splash_string;

      ply_trace ("Splash is configured to be '%*.*s'", length, length, splash_string);

      asprintf (&state->override_splash_path,
                PLYMOUTH_THEME_PATH "%*.*s/%*.*s.plymouth",
                length, length, splash_string, length, length, splash_string);
    }

  if (isnan (state->splash_delay))
    {
      const char *delay_string;

      delay_string = command_line_get_string_after_prefix (state->kernel_command_line, "plymouth.splash-delay=");

      if (delay_string != NULL)
        state->splash_delay = atof (delay_string);
    }
}

static void
find_system_default_splash (state_t *state)
{
  if (state->system_default_splash_path != NULL)
      return;

  if (!load_settings (state, PLYMOUTH_CONF_DIR "plymouthd.conf", &state->system_default_splash_path))
    {
      ply_trace ("failed to load " PLYMOUTH_CONF_DIR "plymouthd.conf");
      return;
    }

  ply_trace ("System configured theme file is '%s'", state->system_default_splash_path);
}

static void
find_distribution_default_splash (state_t *state)
{
  if (state->distribution_default_splash_path != NULL)
      return;

  if (!load_settings (state, PLYMOUTH_POLICY_DIR "plymouthd.defaults", &state->distribution_default_splash_path))
    {
      ply_trace ("failed to load " PLYMOUTH_POLICY_DIR "plymouthd.defaults");
      return;
    }

  ply_trace ("Distribution default theme file is '%s'", state->distribution_default_splash_path);
}

static void
show_default_splash (state_t *state)
{
  if (state->boot_splash != NULL)
    return;

  ply_trace ("Showing splash screen");
  if (state->override_splash_path != NULL)
    {
      ply_trace ("Trying override splash at '%s'", state->override_splash_path);
      state->boot_splash = show_theme (state, state->override_splash_path);
    }

  if (state->boot_splash == NULL &&
      state->system_default_splash_path != NULL)
    {
      ply_trace ("Trying system default splash");
      state->boot_splash = show_theme (state, state->system_default_splash_path);
    }

  if (state->boot_splash == NULL &&
      state->distribution_default_splash_path != NULL)
    {
      ply_trace ("Trying distribution default splash");
      state->boot_splash = show_theme (state, state->distribution_default_splash_path);
    }

  if (state->boot_splash == NULL)
    {
      ply_trace ("Trying old scheme for default splash");
      state->boot_splash = show_theme (state, PLYMOUTH_THEME_PATH "default.plymouth");
    }

  if (state->boot_splash == NULL)
    {
      ply_trace ("Could not start default splash screen,"
                 "showing text splash screen");
      state->boot_splash = show_theme (state, PLYMOUTH_THEME_PATH "text/text.plymouth");
    }

  if (state->boot_splash == NULL)
    {
      ply_trace ("Could not start text splash screen,"
                 "showing built-in splash screen");
      state->boot_splash = show_theme (state, NULL);
    }

  if (state->boot_splash == NULL)
    {
      ply_error ("plymouthd: could not start boot splash: %m");
      return;
    }
}

static void
cancel_pending_delayed_show (state_t *state)
{
  bool has_open_seats;

  if (isnan (state->splash_delay))
    return;

  ply_event_loop_stop_watching_for_timeout (state->loop,
                                            (ply_event_loop_timeout_handler_t)
                                            show_splash,
                                            state);
  state->splash_delay = NAN;
  has_open_seats = ply_device_manager_has_open_seats (state->device_manager);

  if (state->is_shown && has_open_seats)
    {
      ply_trace ("splash delay cancelled, showing splash immediately");
      show_splash (state);
    }
}

static void
on_ask_for_password (state_t      *state,
                     const char   *prompt,
                     ply_trigger_t *answer)
{
  ply_entry_trigger_t *entry_trigger;

  if (state->boot_splash == NULL)
    {
      /* Waiting to be shown, boot splash will
       * arrive shortly so just sit tight
       */
      if (state->is_shown)
        {
          ply_trace ("splash still coming up, waiting a bit");
          cancel_pending_delayed_show (state);
        }
      else
        {
          /* No splash, client will have to get password */
          ply_trace ("no splash loaded, replying immediately with no password");
          ply_trigger_pull (answer, NULL);
          return;
        }
    }

  entry_trigger = calloc (1, sizeof (ply_entry_trigger_t));
  entry_trigger->type = PLY_ENTRY_TRIGGER_TYPE_PASSWORD;
  entry_trigger->prompt = prompt;
  entry_trigger->trigger = answer;
  ply_trace ("queuing password request with boot splash");
  ply_list_append_data (state->entry_triggers, entry_trigger);
  update_display (state);
}

static void
on_ask_question (state_t      *state,
                 const char   *prompt,
                 ply_trigger_t *answer)
{
  ply_entry_trigger_t *entry_trigger;

  entry_trigger = calloc (1, sizeof (ply_entry_trigger_t));
  entry_trigger->type = PLY_ENTRY_TRIGGER_TYPE_QUESTION;
  entry_trigger->prompt = prompt;
  entry_trigger->trigger = answer;
  ply_trace ("queuing question with boot splash");
  ply_list_append_data (state->entry_triggers, entry_trigger);
  update_display (state);
}

static void
on_display_message (state_t       *state,
                    const char    *message)
{
  if (state->boot_splash != NULL)
    {
        ply_trace ("displaying message %s", message);
        ply_boot_splash_display_message (state->boot_splash, message);
    }
  else
    ply_trace ("not displaying message %s as no splash", message);
  ply_list_append_data (state->messages, strdup(message));
}

static void
on_hide_message (state_t       *state,
                 const char    *message)
{
  ply_list_node_t *node;
  
  ply_trace ("hiding message %s", message);
  
  node = ply_list_get_first_node (state->messages);
  while (node != NULL)
    {
      ply_list_node_t *next_node;
      char *list_message;

      list_message = ply_list_node_get_data (node);
      next_node = ply_list_get_next_node (state->messages, node);

      if (strcmp (list_message, message) == 0)
        {
          free (list_message);
          ply_list_remove_node (state->messages, node);
          ply_boot_splash_hide_message (state->boot_splash, message);
        }
      node = next_node;
    }
}

static void
on_watch_for_keystroke (state_t      *state,
                     const char    *keys,
                     ply_trigger_t *trigger)
{
  ply_keystroke_watch_t *keystroke_trigger =
                                  calloc (1, sizeof (ply_keystroke_watch_t));
  ply_trace ("watching for keystroke");
  keystroke_trigger->keys = keys;
  keystroke_trigger->trigger = trigger;
  ply_list_append_data (state->keystroke_triggers, keystroke_trigger);
}

static void
on_ignore_keystroke (state_t      *state,
                     const char    *keys)
{
  ply_list_node_t *node;

  ply_trace ("ignoring for keystroke");

  for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                    node = ply_list_get_next_node (state->keystroke_triggers, node))
    {
      ply_keystroke_watch_t* keystroke_trigger = ply_list_node_get_data (node);
      if ((!keystroke_trigger->keys && !keys) ||
          (keystroke_trigger->keys && keys && strcmp(keystroke_trigger->keys, keys)==0))
        {
          ply_trigger_pull (keystroke_trigger->trigger, NULL);
          ply_list_remove_node (state->keystroke_triggers, node);
          return;
        }
    }
}

static void
on_progress_pause (state_t *state)
{
  ply_trace ("pausing progress");
  ply_progress_pause (state->progress);
}

static void
on_progress_unpause (state_t *state)
{
  ply_trace ("unpausing progress");
  ply_progress_unpause (state->progress);
}

static void
on_newroot (state_t    *state,
            const char *root_dir)
{
  ply_trace ("new root mounted at \"%s\", switching to it", root_dir);
  chdir(root_dir);
  chroot(".");
  chdir("/");
  ply_progress_load_cache (state->progress, get_cache_file_for_mode (state->mode));
  if (state->boot_splash != NULL)
    ply_boot_splash_root_mounted (state->boot_splash);
}

static const char *
get_cache_file_for_mode (ply_mode_t mode)
{
  const char *filename;

  switch ((int)mode)
    {
    case PLY_MODE_BOOT:
      filename = BOOT_DURATION_FILE;
      break;
    case PLY_MODE_SHUTDOWN:
      filename = SHUTDOWN_DURATION_FILE;
      break;
    case PLY_MODE_UPDATES:
      filename = NULL;
      break;
    default:
      ply_error ("Unhandled case in %s line %d\n", __FILE__, __LINE__);
      abort ();
      break;
    }

  ply_trace ("returning cache file '%s'", filename);
  return filename;
}

static const char *
get_log_file_for_mode (ply_mode_t mode)
{
  const char *filename;

  switch ((int)mode)
    {
    case PLY_MODE_BOOT:
      filename = PLYMOUTH_LOG_DIRECTORY "/boot.log";
      break;
    case PLY_MODE_SHUTDOWN:
    case PLY_MODE_UPDATES:
      filename = _PATH_DEVNULL;
      break;
    default:
      ply_error ("Unhandled case in %s line %d\n", __FILE__, __LINE__);
      abort ();
      break;
    }

  ply_trace ("returning log file '%s'", filename);
  return filename;
}

static const char *
get_log_spool_file_for_mode (ply_mode_t mode)
{
  const char *filename;

  switch ((int)mode)
    {
    case PLY_MODE_BOOT:
      filename = PLYMOUTH_SPOOL_DIRECTORY "/boot.log";
      break;
    case PLY_MODE_SHUTDOWN:
    case PLY_MODE_UPDATES:
      filename = NULL;
      break;
    default:
      ply_error ("Unhandled case in %s line %d\n", __FILE__, __LINE__);
      abort ();
      break;
    }

  ply_trace ("returning spool file '%s'", filename);
  return filename;
}

static void
spool_error (state_t *state)
{
  const char *logfile;
  const char *logspool;

  ply_trace ("spooling error for viewer");

  logfile = get_log_file_for_mode (state->mode);
  logspool = get_log_spool_file_for_mode (state->mode);

  if (logfile != NULL && logspool != NULL)
    {
      unlink (logspool);

      ply_create_file_link (logfile, logspool);
    }
}

static void
prepare_logging (state_t *state)
{
  const char *logfile;

  if (!state->system_initialized)
    {
      ply_trace ("not preparing logging yet, system not initialized");
      return;
    }

  if (state->session == NULL)
    {
      ply_trace ("not preparing logging, no session");
      return;
    }

  logfile = get_log_file_for_mode (state->mode);
  if (logfile != NULL)
    {
      ply_trace ("opening log '%s'", logfile);
      ply_terminal_session_open_log (state->session, logfile);

      if (state->number_of_errors > 0)
        spool_error (state);
    }
}

static void
on_system_initialized (state_t *state)
{
  ply_trace ("system now initialized, opening log");
  state->system_initialized = true;

  prepare_logging (state);
}

static void
on_error (state_t *state)
{
  ply_trace ("encountered error during boot up");

  if (state->system_initialized && state->number_of_errors == 0)
    spool_error (state);
  else
    ply_trace ("not spooling because number of errors %d", state->number_of_errors);

  state->number_of_errors++;
}

static bool
plymouth_should_ignore_show_splash_calls (state_t *state)
{
  const char *init_string;
  size_t length;

  ply_trace ("checking if plymouth should be running");
  if (state->mode != PLY_MODE_BOOT || command_line_has_argument (state->kernel_command_line, "plymouth.force-splash"))
      return false;

  if (command_line_has_argument (state->kernel_command_line, "plymouth.ignore-show-splash"))
      return true;

  init_string = command_line_get_string_after_prefix (state->kernel_command_line, "init=");

  if (init_string)
    {
      length = strcspn (init_string, " \n");
      if (length > 2 && ply_string_has_prefix (init_string + length - 2, "sh"))
        return true;
    }

  return false;
}

static bool
plymouth_should_show_default_splash (state_t *state)
{
  ply_trace ("checking if plymouth should show default splash");

  const char const *strings[] = {
      "single", "1", "s", "S", "-S", NULL
  };
  int i;

  if (state->should_force_details)
    return false;

  for (i = 0; strings[i] != NULL; i++)
    {
      if (command_line_has_argument (state->kernel_command_line, strings[i]))
        {
          ply_trace ("no default splash because kernel command line has option \"%s\"", strings[i]);
          return false;
        }
    }

  if (command_line_has_argument (state->kernel_command_line, "splash=verbose"))
    {
      ply_trace ("no default splash because kernel command line has option \"splash=verbose\"");
      return false;
    }

  if (command_line_has_argument (state->kernel_command_line, "rhgb"))
    {
      ply_trace ("using default splash because kernel command line has option \"rhgb\"");
      return true;
    }

  if (command_line_has_argument (state->kernel_command_line, "splash"))
    {
      ply_trace ("using default splash because kernel command line has option \"splash\"");
      return true;
    }

  if (command_line_has_argument (state->kernel_command_line, "splash=silent"))
    {
      ply_trace ("using default splash because kernel command line has option \"splash=slient\"");
      return true;
    }

  ply_trace ("no default splash because kernel command line lacks \"splash\" or \"rhgb\"");
  return false;
}

static void
on_show_splash (state_t       *state,
                ply_trigger_t *show_trigger)
{
  bool has_open_seats;

  if (state->is_shown)
    {
      ply_trace ("show splash called while already shown");
      return;
    }

  if (state->is_inactive)
    {
      ply_trace ("show splash called while inactive");
      return;
    }

  if (plymouth_should_ignore_show_splash_calls (state))
    {
      ply_trace ("show splash called while ignoring show splash calls");
      dump_details_and_quit_splash (state);
      return;
    }

  state->show_trigger = show_trigger;
  state->is_shown = true;
  has_open_seats = ply_device_manager_has_open_seats (state->device_manager);

  if (!state->is_attached && state->should_be_attached && has_open_seats)
    attach_to_running_session (state);

  if (has_open_seats)
    {
      ply_trace ("at least one seat already open, so loading splash");
      show_splash (state);
    }
  else
    {
      ply_trace ("no seats available to show splash on, waiting...");
    }
}

static void
on_seat_removed (state_t    *state,
                 ply_seat_t *seat)
{
  ply_keyboard_t *keyboard;

  keyboard = ply_seat_get_keyboard (seat);

  ply_trace ("no longer listening for keystrokes");
  ply_keyboard_remove_input_handler (keyboard,
                                     (ply_keyboard_input_handler_t)
                                     on_keyboard_input);
  ply_trace ("no longer listening for escape");
  ply_keyboard_remove_escape_handler (keyboard,
                                      (ply_keyboard_escape_handler_t)
                                      on_escape_pressed);
  ply_trace ("no longer listening for backspace");
  ply_keyboard_remove_backspace_handler (keyboard,
                                         (ply_keyboard_backspace_handler_t)
                                         on_backspace);
  ply_trace ("no longer listening for enter");
  ply_keyboard_remove_enter_handler (keyboard,
                                     (ply_keyboard_enter_handler_t)
                                     on_enter);

  if (state->boot_splash != NULL)
   ply_boot_splash_detach_from_seat (state->boot_splash, seat);
}

static void
show_splash (state_t *state)
{
  if (state->boot_splash != NULL)
    return;

  if (!isnan (state->splash_delay))
    {
      double now, running_time;

      now = ply_get_timestamp ();
      running_time = now - state->start_time;
      if (state->splash_delay > running_time)
        {
          double time_left = state->splash_delay - running_time;

          ply_trace ("delaying show splash for %lf seconds",
                     time_left);
          ply_event_loop_stop_watching_for_timeout (state->loop,
                                                    (ply_event_loop_timeout_handler_t)
                                                    show_splash,
                                                    state);
          ply_event_loop_watch_for_timeout (state->loop,
                                            time_left,
                                            (ply_event_loop_timeout_handler_t)
                                            show_splash,
                                            state);
          return;
        }
    }

  if (plymouth_should_show_default_splash (state))
    {
      show_default_splash (state);
      state->showing_details = false;
    }
  else
    {
      show_detailed_splash (state);
      state->showing_details = true;
    }

  if (state->show_trigger != NULL)
    {
      ply_trace ("telling boot server about completed show operation");
      ply_trigger_pull (state->show_trigger, NULL);
      state->show_trigger = NULL;
    }
}

static void
on_seat_added (state_t    *state,
               ply_seat_t *seat)
{
  ply_keyboard_t *keyboard;

  if (state->is_shown)
    {
      if (state->boot_splash == NULL)
        {
          ply_trace ("seat added before splash loaded, so loading splash now");
          show_splash (state);
        }
      else
        {
          ply_trace ("seat added after splash loaded, so attaching to splash");
          ply_boot_splash_attach_to_seat (state->boot_splash, seat);
        }
    }

  keyboard = ply_seat_get_keyboard (seat);

  ply_trace ("listening for keystrokes");
  ply_keyboard_add_input_handler (keyboard,
                                  (ply_keyboard_input_handler_t)
                                  on_keyboard_input, state);
  ply_trace ("listening for escape");
  ply_keyboard_add_escape_handler (keyboard,
                                   (ply_keyboard_escape_handler_t)
                                   on_escape_pressed, state);
  ply_trace ("listening for backspace");
  ply_keyboard_add_backspace_handler (keyboard,
                                      (ply_keyboard_backspace_handler_t)
                                      on_backspace, state);
  ply_trace ("listening for enter");
  ply_keyboard_add_enter_handler (keyboard,
                                  (ply_keyboard_enter_handler_t)
                                  on_enter, state);

}

static void
load_devices (state_t                    *state,
              ply_device_manager_flags_t  flags)
{
  state->device_manager = ply_device_manager_new (state->default_tty, flags);
  state->local_console_terminal = ply_device_manager_get_default_terminal (state->device_manager);

  ply_device_manager_watch_seats (state->device_manager,
                                  (ply_seat_added_handler_t)
                                  on_seat_added,
                                  (ply_seat_removed_handler_t)
                                  on_seat_removed,
                                  state);
}

static void
quit_splash (state_t *state)
{
  ply_trace ("quiting splash");
  if (state->boot_splash != NULL)
    {
      ply_trace ("freeing splash");
      ply_boot_splash_free (state->boot_splash);
      state->boot_splash = NULL;
    }

  if (state->local_console_terminal != NULL)
    {
      if (!state->should_retain_splash)
        {
          ply_trace ("Not retaining splash, so deallocating VT");
          ply_terminal_deactivate_vt (state->local_console_terminal);
        }
      state->local_console_terminal = NULL;
    }

  detach_from_running_session (state);
}

static void
hide_splash (state_t *state)
{
  state->is_shown = false;

  cancel_pending_delayed_show (state);

  if (state->boot_splash == NULL)
    return;

  ply_boot_splash_hide (state->boot_splash);

  if (state->local_console_terminal != NULL)
    ply_terminal_set_mode (state->local_console_terminal, PLY_TERMINAL_MODE_TEXT);
}

static void
dump_details_and_quit_splash (state_t *state)
{
  state->showing_details = false;
  toggle_between_splash_and_details (state);

  ply_device_manager_deactivate_renderers (state->device_manager);
  hide_splash (state);
  quit_splash (state);
}

static void
on_hide_splash (state_t *state)
{
  if (state->is_inactive)
    return;

  if (state->boot_splash == NULL)
    return;

  ply_trace ("hiding boot splash");
  dump_details_and_quit_splash (state);
}

#ifdef PLY_ENABLE_DEPRECATED_GDM_TRANSITION
static void
tell_gdm_to_transition (void)
{
  int fd;

  fd = creat ("/var/spool/gdm/force-display-on-active-vt", 0644);
  close (fd);
}
#endif

static void
quit_program (state_t *state)
{
  ply_trace ("cleaning up devices");
  ply_device_manager_free (state->device_manager);

  ply_trace ("exiting event loop");
  ply_event_loop_exit (state->loop, 0);

  if (pid_file != NULL)
    {
      unlink (pid_file);
      free (pid_file);
      pid_file = NULL;
    }

#ifdef PLY_ENABLE_DEPRECATED_GDM_TRANSITION
  if (state->should_retain_splash &&
      state->mode == PLY_MODE_BOOT)
    {
      tell_gdm_to_transition ();
    }
#endif

  if (state->deactivate_trigger != NULL)
    {
      ply_trigger_pull (state->deactivate_trigger, NULL);
      state->deactivate_trigger = NULL;
    }
  if (state->quit_trigger != NULL)
    {
      ply_trigger_pull (state->quit_trigger, NULL);
      state->quit_trigger = NULL;
    }
}

static void
deactivate_splash (state_t *state)
{
  assert (!state->is_inactive);

  ply_device_manager_deactivate_renderers (state->device_manager);

  detach_from_running_session (state);

  if (state->local_console_terminal != NULL)
    {
      ply_trace ("deactivating terminal");
      ply_terminal_stop_watching_for_vt_changes (state->local_console_terminal);
      ply_terminal_set_buffered_input (state->local_console_terminal);
      ply_terminal_ignore_mode_changes (state->local_console_terminal, true);
      ply_terminal_close (state->local_console_terminal);
    }

  /* do not let any tty opened where we could write after deactivate */
  if (command_line_has_argument (state->kernel_command_line, "plymouth.debug"))
    {
      ply_logger_close_file (ply_logger_get_error_default ());
    }

  state->is_inactive = true;

  ply_trigger_pull (state->deactivate_trigger, NULL);
  state->deactivate_trigger = NULL;
}

static void
on_boot_splash_idle (state_t *state)
{
  ply_trace ("boot splash idle");

  /* In the case where we've received both a deactivate command and a
   * quit command, the quit command takes precedence.
   */
  if (state->quit_trigger != NULL)
    {
      if (!state->should_retain_splash)
        {
          ply_trace ("hiding splash");
          ply_device_manager_deactivate_renderers (state->device_manager);
          hide_splash (state);
        }

      ply_trace ("quitting splash");
      quit_splash (state);
      ply_trace ("quitting program");
      quit_program (state);
    }
  else if (state->deactivate_trigger != NULL)
    {
      ply_trace ("deactivating splash");
      deactivate_splash (state);
    }
}

static void
on_deactivate (state_t       *state,
               ply_trigger_t *deactivate_trigger)
{
  if (state->is_inactive)
    {
      ply_trigger_pull (deactivate_trigger, NULL);
      return;
    }

  if (state->deactivate_trigger != NULL)
    {
      ply_trigger_add_handler (state->deactivate_trigger,
                               (ply_trigger_handler_t)
                               ply_trigger_pull,
                               deactivate_trigger);
      return;
    }

  state->deactivate_trigger = deactivate_trigger;

  ply_trace ("deactivating");
  ply_device_manager_deactivate_keyboards (state->device_manager);

  if (state->boot_splash != NULL)
    {
      ply_boot_splash_become_idle (state->boot_splash,
                                   (ply_boot_splash_on_idle_handler_t)
                                   on_boot_splash_idle,
                                   state);
    }
  else
    {
      ply_trace ("deactivating splash");
      deactivate_splash (state);
    }
}

static void
on_reactivate (state_t *state)
{
  if (!state->is_inactive)
    return;

  if (state->local_console_terminal != NULL)
    {
      ply_terminal_open (state->local_console_terminal);
      ply_terminal_watch_for_vt_changes (state->local_console_terminal);
      ply_terminal_set_unbuffered_input (state->local_console_terminal);
      ply_terminal_ignore_mode_changes (state->local_console_terminal, false);
    }

  if ((state->session != NULL) && state->should_be_attached)
    {
      ply_trace ("reactivating terminal session");
      attach_to_running_session (state);
    }

  ply_device_manager_activate_keyboards (state->device_manager);
  ply_device_manager_activate_renderers (state->device_manager);

  state->is_inactive = false;

  update_display (state);
}

static void
on_quit (state_t       *state,
         bool           retain_splash,
         ply_trigger_t *quit_trigger)
{
  if (state->quit_trigger != NULL)
    {
      ply_trigger_add_handler (state->quit_trigger,
                               (ply_trigger_handler_t)
                               ply_trigger_pull,
                               quit_trigger);
      return;
    }

  if (state->system_initialized)
    ply_progress_save_cache (state->progress,
                             get_cache_file_for_mode (state->mode));

  state->quit_trigger = quit_trigger;
  state->should_retain_splash = retain_splash;

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
  tell_systemd_to_stop_printing_details (state);
#endif

  ply_trace ("time to quit, closing log");
  if (state->session != NULL)
    ply_terminal_session_close_log (state->session);

  ply_device_manager_deactivate_keyboards (state->device_manager);

  ply_trace ("unloading splash");
  if (state->is_inactive && !retain_splash)
    {
      /* We've been deactivated and X failed to start
       */
      dump_details_and_quit_splash (state);
      quit_program (state);
    }
  else if (state->boot_splash != NULL)
    {
      ply_boot_splash_become_idle (state->boot_splash,
                                   (ply_boot_splash_on_idle_handler_t)
                                   on_boot_splash_idle,
                                   state);
    }
  else
    quit_program (state);
}

static bool
on_has_active_vt (state_t *state)
{
  if (state->local_console_terminal != NULL)
    return ply_terminal_is_active (state->local_console_terminal);
  else
    return false;
}

static ply_boot_server_t *
start_boot_server (state_t *state)
{
  ply_boot_server_t *server;

  server = ply_boot_server_new ((ply_boot_server_update_handler_t) on_update,
                                (ply_boot_server_change_mode_handler_t) on_change_mode,
                                (ply_boot_server_system_update_handler_t) on_system_update,
                                (ply_boot_server_ask_for_password_handler_t) on_ask_for_password,
                                (ply_boot_server_ask_question_handler_t) on_ask_question,
                                (ply_boot_server_display_message_handler_t) on_display_message,
                                (ply_boot_server_hide_message_handler_t) on_hide_message,
                                (ply_boot_server_watch_for_keystroke_handler_t) on_watch_for_keystroke,
                                (ply_boot_server_ignore_keystroke_handler_t) on_ignore_keystroke,
                                (ply_boot_server_progress_pause_handler_t) on_progress_pause,
                                (ply_boot_server_progress_unpause_handler_t) on_progress_unpause,
                                (ply_boot_server_show_splash_handler_t) on_show_splash,
                                (ply_boot_server_hide_splash_handler_t) on_hide_splash,
                                (ply_boot_server_newroot_handler_t) on_newroot,
                                (ply_boot_server_system_initialized_handler_t) on_system_initialized,
                                (ply_boot_server_error_handler_t) on_error,
                                (ply_boot_server_deactivate_handler_t) on_deactivate,
                                (ply_boot_server_reactivate_handler_t) on_reactivate,
                                (ply_boot_server_quit_handler_t) on_quit,
                                (ply_boot_server_has_active_vt_handler_t) on_has_active_vt,
                                state);

  if (!ply_boot_server_listen (server))
    {
      ply_save_errno ();
      ply_boot_server_free (server);
      ply_restore_errno ();
      return NULL;
    }

  ply_boot_server_attach_to_event_loop (server, state->loop);

  return server;
}


static void
update_display (state_t *state)
{
  if (!state->boot_splash) return;
  
  ply_list_node_t *node;
  node = ply_list_get_first_node (state->entry_triggers);
  if (node)
    {
      ply_entry_trigger_t* entry_trigger = ply_list_node_get_data (node);
      if (entry_trigger->type == PLY_ENTRY_TRIGGER_TYPE_PASSWORD)
        {
          int bullets = ply_utf8_string_get_length (ply_buffer_get_bytes (state->entry_buffer),
                                                    ply_buffer_get_size (state->entry_buffer));
          bullets = MAX(0, bullets);
          ply_boot_splash_display_password (state->boot_splash, 
                                            entry_trigger->prompt,
                                            bullets);
        }
      else if (entry_trigger->type == PLY_ENTRY_TRIGGER_TYPE_QUESTION)
        {
          ply_boot_splash_display_question (state->boot_splash,
                                            entry_trigger->prompt,
                                            ply_buffer_get_bytes (state->entry_buffer));
        }
      else {
          ply_trace("unkown entry type");
        }
    }
  else
    {
      ply_boot_splash_display_normal (state->boot_splash);
    }

}

static void
toggle_between_splash_and_details (state_t *state)
{
  ply_trace ("toggling between splash and details");
  if (state->boot_splash != NULL)
    {
      ply_trace ("hiding and freeing current splash");
      hide_splash (state);
      ply_boot_splash_free (state->boot_splash);
      state->boot_splash = NULL;
    }

  if (!state->showing_details)
    {
      show_detailed_splash (state);
      state->showing_details = true;
    }
  else
    {
      show_default_splash (state);
      state->showing_details = false;
    }
}

static void
on_escape_pressed (state_t *state)
{
  ply_trace ("escape key pressed");
  toggle_between_splash_and_details (state);
}

static void
on_keyboard_input (state_t                  *state,
                   const char               *keyboard_input,
                   size_t                    character_size)
{
  ply_list_node_t *node;
  node = ply_list_get_first_node (state->entry_triggers);
  if (node)
    {               /* \x3 (ETX) is Ctrl+C and \x4 (EOT) is Ctrl+D */
      if (character_size == 1 && ( keyboard_input[0] == '\x3' || keyboard_input[0] == '\x4' ))
        {
          ply_entry_trigger_t* entry_trigger = ply_list_node_get_data (node);
          ply_trigger_pull (entry_trigger->trigger, "\x3");
          ply_buffer_clear (state->entry_buffer);
          ply_list_remove_node (state->entry_triggers, node);
          free (entry_trigger);
        }
      else
        {
          ply_buffer_append_bytes (state->entry_buffer, keyboard_input, character_size);
        }
      update_display (state);
    }
  else
    {
      for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                        node = ply_list_get_next_node (state->keystroke_triggers, node))
        {
          ply_keystroke_watch_t* keystroke_trigger = ply_list_node_get_data (node);
          if (!keystroke_trigger->keys || strstr(keystroke_trigger->keys, keyboard_input))  /* assume strstr works on utf8 arrays */
            {
              ply_trigger_pull (keystroke_trigger->trigger, keyboard_input);
              ply_list_remove_node (state->keystroke_triggers, node);
              free(keystroke_trigger);
              return;
            }
        }
      return;
    }
}

static void
on_backspace (state_t                  *state)
{
  ssize_t bytes_to_remove;
  ssize_t previous_character_size;
  const char *bytes;
  size_t size;
  ply_list_node_t *node = ply_list_get_first_node (state->entry_triggers);
  if (!node) return;

  bytes = ply_buffer_get_bytes (state->entry_buffer);
  size = ply_buffer_get_size (state->entry_buffer);

  bytes_to_remove = MIN(size, PLY_UTF8_CHARACTER_SIZE_MAX);
  while ((previous_character_size = ply_utf8_character_get_size (bytes + size - bytes_to_remove, bytes_to_remove)) < bytes_to_remove)
    {
      if (previous_character_size > 0)
        bytes_to_remove -= previous_character_size;
      else
        bytes_to_remove--;
    }

  ply_buffer_remove_bytes_at_end (state->entry_buffer, bytes_to_remove);
  update_display (state);
}

static void
on_enter (state_t                  *state,
          const char               *line)
{
  ply_list_node_t *node;
  node = ply_list_get_first_node (state->entry_triggers);
  if (node)
    {
      ply_entry_trigger_t* entry_trigger = ply_list_node_get_data (node);
      const char* reply_text = ply_buffer_get_bytes (state->entry_buffer);
      ply_trigger_pull (entry_trigger->trigger, reply_text);
      ply_buffer_clear (state->entry_buffer);
      ply_list_remove_node (state->entry_triggers, node);
      free (entry_trigger);
      update_display (state);
    }
  else
    {
      for (node = ply_list_get_first_node (state->keystroke_triggers); node;
                        node = ply_list_get_next_node (state->keystroke_triggers, node))
        {
          ply_keystroke_watch_t* keystroke_trigger = ply_list_node_get_data (node);
          if (!keystroke_trigger->keys || strstr(keystroke_trigger->keys, "\n"))  /* assume strstr works on utf8 arrays */
            {
              ply_trigger_pull (keystroke_trigger->trigger, line);
              ply_list_remove_node (state->keystroke_triggers, node);
              free(keystroke_trigger);
              return;
            }
        }
      return;
    }
}

static void
attach_splash_to_seats (state_t           *state,
                        ply_boot_splash_t *splash)
{

  ply_list_t *seats;
  ply_list_node_t *node;

  seats = ply_device_manager_get_seats (state->device_manager);
  node = ply_list_get_first_node (seats);
  while (node != NULL)
    {
      ply_seat_t *seat;
      ply_list_node_t *next_node;

      seat = ply_list_node_get_data (node);
      next_node = ply_list_get_next_node (seats, node);

      ply_boot_splash_attach_to_seat (splash, seat);

      node = next_node;
    }
}

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
static void
tell_systemd_to_print_details (state_t *state)
{
  ply_trace ("telling systemd to start printing details");
  if (kill (1, SIGRTMIN + 20) < 0)
    ply_trace ("could not tell systemd to print details: %m");
}

static void
tell_systemd_to_stop_printing_details (state_t *state)
{
  ply_trace ("telling systemd to stop printing details");
  if (kill (1, SIGRTMIN + 21) < 0)
    ply_trace ("could not tell systemd to stop printing details: %m");
}
#endif

static ply_boot_splash_t *
load_built_in_theme (state_t *state)
{
  ply_boot_splash_t *splash;
  bool is_loaded;

  ply_trace ("Loading built-in theme");

  splash = ply_boot_splash_new ("",
                                PLYMOUTH_PLUGIN_PATH,
                                state->boot_buffer);

  is_loaded = ply_boot_splash_load_built_in (splash);

  if (!is_loaded)
    {
      ply_save_errno ();
      ply_boot_splash_free (splash);
      ply_restore_errno ();
      return NULL;
    }

  ply_trace ("attaching plugin to event loop");
  ply_boot_splash_attach_to_event_loop (splash, state->loop);

  ply_trace ("attaching progress to plugin");
  ply_boot_splash_attach_progress (splash, state->progress);

  return splash;
}

static ply_boot_splash_t *
load_theme (state_t    *state,
            const char *theme_path)
{
  ply_boot_splash_t *splash;
  bool is_loaded;

  ply_trace ("Loading boot splash theme '%s'",
             theme_path);

  splash = ply_boot_splash_new (theme_path,
                                PLYMOUTH_PLUGIN_PATH,
                                state->boot_buffer);

  is_loaded = ply_boot_splash_load (splash);

  if (!is_loaded)
    {
      ply_save_errno ();
      ply_boot_splash_free (splash);
      ply_restore_errno ();
      return NULL;
    }

  ply_trace ("attaching plugin to event loop");
  ply_boot_splash_attach_to_event_loop (splash, state->loop);

  ply_trace ("attaching progress to plugin");
  ply_boot_splash_attach_progress (splash, state->progress);

  return splash;
}

static ply_boot_splash_t *
show_theme (state_t           *state,
            const char        *theme_path)
{
  ply_boot_splash_mode_t splash_mode;
  ply_boot_splash_t *splash;

  if (theme_path != NULL)
    splash = load_theme (state, theme_path);
  else
    splash = load_built_in_theme (state);

  if (splash == NULL)
    return NULL;

  attach_splash_to_seats (state, splash);
  ply_device_manager_activate_renderers (state->device_manager);

  if (state->mode == PLY_MODE_SHUTDOWN)
    splash_mode = PLY_BOOT_SPLASH_MODE_SHUTDOWN;
  else
    splash_mode = PLY_BOOT_SPLASH_MODE_BOOT_UP;

  if (!ply_boot_splash_show (splash, splash_mode))
    {
      ply_save_errno ();
      ply_boot_splash_free (splash);
      ply_restore_errno ();
      return NULL;
    }

#ifdef PLY_ENABLE_SYSTEMD_INTEGRATION
  if (state->is_attached)
    tell_systemd_to_print_details (state);
#endif

  ply_device_manager_activate_keyboards (state->device_manager);
  show_messages (state);
  update_display (state);

  return splash;
}

static bool
attach_to_running_session (state_t *state)
{
  ply_terminal_session_t *session;
  ply_terminal_session_flags_t flags;
  bool should_be_redirected;

  flags = 0;

  should_be_redirected = !state->no_boot_log;

  if (should_be_redirected)
    flags |= PLY_TERMINAL_SESSION_FLAGS_REDIRECT_CONSOLE;

 if (state->session == NULL)
   {
     ply_trace ("creating new terminal session");
     session = ply_terminal_session_new (NULL);

     ply_terminal_session_attach_to_event_loop (session, state->loop);
   }
 else
   {
     session = state->session;
     ply_trace ("session already created");
   }

  if (!ply_terminal_session_attach (session, flags,
                                 (ply_terminal_session_output_handler_t)
                                 on_session_output,
                                 (ply_terminal_session_hangup_handler_t)
                                 (should_be_redirected? on_session_hangup: NULL),
                                 -1, state))
    {
      ply_save_errno ();
      ply_terminal_session_free (session);
      ply_buffer_free (state->boot_buffer);
      state->boot_buffer = NULL;
      ply_restore_errno ();

      state->is_redirected = false;
      state->is_attached = false;
      return false;
    }

  state->is_redirected = should_be_redirected;
  state->is_attached = true;
  state->session = session;

  return true;
}

static void
detach_from_running_session (state_t *state)
{
  if (state->session == NULL)
    return;

  if (!state->is_attached)
    return;

  ply_trace ("detaching from terminal session");
  ply_terminal_session_detach (state->session);
  state->is_redirected = false;
  state->is_attached = false;
}

static bool
get_kernel_command_line (state_t *state)
{
  int fd;
  const char *remaining_command_line;
  char *key;

  if (state->kernel_command_line_is_set)
    return true;

  ply_trace ("opening /proc/cmdline");
  fd = open ("/proc/cmdline", O_RDONLY);

  if (fd < 0)
    {
      ply_trace ("couldn't open it: %m");
      return false;
    }

  ply_trace ("reading kernel command line");
  if (read (fd, state->kernel_command_line, sizeof (state->kernel_command_line)) < 0)
    {
      ply_trace ("couldn't read it: %m");
      close (fd);
      return false;
    }


  /* we now use plymouth.argument for kernel commandline arguments.
   * It used to be plymouth:argument. This bit just rewrites all : to be .
   */
  remaining_command_line = state->kernel_command_line;
  while ((key = strstr (remaining_command_line, "plymouth:")) != NULL)
    {
      char *colon;

      colon = key + strlen ("plymouth");
      *colon = '.';

      remaining_command_line = colon + 1;
    }
  ply_trace ("Kernel command line is: '%s'", state->kernel_command_line);

  close (fd);

  state->kernel_command_line_is_set = true;
  return true;
}

static void
check_verbosity (state_t *state)
{
  const char *stream;
  const char *path;

  ply_trace ("checking if tracing should be enabled");

  stream = command_line_get_string_after_prefix (state->kernel_command_line,
                                                 "plymouth.debug=stream:");

  path = command_line_get_string_after_prefix (state->kernel_command_line,
                                               "plymouth.debug=file:");
  if (stream != NULL || path != NULL ||
      command_line_has_argument (state->kernel_command_line, "plymouth.debug"))
    {
      int fd;

      ply_trace ("tracing should be enabled!");
      if (!ply_is_tracing ())
        ply_toggle_tracing ();

      if (path != NULL && debug_buffer_path == NULL)
        {
          char *end;

          debug_buffer_path = strdup (path);
          end = debug_buffer_path + strcspn (debug_buffer_path, " \n");
          *end = '\0';
        }

        if (debug_buffer == NULL)
          debug_buffer = ply_buffer_new ();

      if (stream != NULL)
        {
          char *end;
          char *stream_copy;

          stream_copy = strdup (stream);
          end = stream_copy + strcspn (stream_copy, " \n");
          *end = '\0';

          ply_trace ("streaming debug output to %s instead of screen", stream_copy);
          fd = open (stream_copy, O_RDWR | O_NOCTTY | O_CREAT, 0600);

          if (fd < 0)
            {
              ply_trace ("could not stream output to %s: %m", stream_copy);
            }
          else
            {
              ply_logger_set_output_fd (ply_logger_get_error_default (), fd);
            }
          free (stream_copy);
        } else {
            const char* device;
            char *file;

            device = state->default_tty;

            ply_trace ("redirecting debug output to %s", device);

            if (strncmp (device, "/dev/", strlen ("/dev/")) == 0)
                file = strdup (device);
              else
                asprintf (&file, "/dev/%s", device);

            fd = open (file, O_RDWR | O_APPEND);

            if (fd < 0)
              {
                 ply_trace ("could not redirected debug output to %s: %m", device);
              }
            else {
                ply_logger_set_output_fd (ply_logger_get_error_default (), fd);
            }

            free (file);
        }
    }
  else
    ply_trace ("tracing shouldn't be enabled!");

  if (debug_buffer != NULL)
    {
      if (debug_buffer_path == NULL)
        debug_buffer_path = strdup (PLYMOUTH_LOG_DIRECTORY "/plymouth-debug.log");

      ply_logger_add_filter (ply_logger_get_error_default (),
                             (ply_logger_filter_handler_t)
                             on_error_message,
                             debug_buffer);

    }
}

static void
check_logging (state_t *state)
{
  ply_trace ("checking if console messages should be redirected and logged");

  if (command_line_has_argument (state->kernel_command_line, "plymouth.nolog"))
    {
      ply_trace ("logging won't be enabled!");
      state->no_boot_log = true;
    }
  else
    {
      ply_trace ("logging will be enabled!");
      state->no_boot_log = false;
    }
}

static bool
redirect_standard_io_to_dev_null (void)
{
  int fd;

  fd = open ("/dev/null", O_RDWR | O_APPEND);

  if (fd < 0)
    return false;

  dup2 (fd, STDIN_FILENO);
  dup2 (fd, STDOUT_FILENO);
  dup2 (fd, STDERR_FILENO);

  close (fd);

  return true;
}
static const char *
find_fallback_tty (state_t *state)
{
  static const char *tty_list[] =
    {
      "/dev/ttyS0",
      "/dev/hvc0",
      "/dev/xvc0",
      "/dev/ttySG0",
      NULL
    };
  int i;

  for (i = 0; tty_list[i] != NULL; i++)
    {
      if (ply_character_device_exists (tty_list[i]))
        return tty_list[i];
    }

  return state->default_tty;
}

static bool
initialize_environment (state_t *state)
{
  ply_trace ("initializing minimal work environment");

  if (!get_kernel_command_line (state))
    return false;

  if (!state->default_tty)
    {
      if (getenv ("DISPLAY") != NULL && access (PLYMOUTH_PLUGIN_PATH "renderers/x11.so", F_OK) == 0)
          state->default_tty = "/dev/tty";
    }
  if (!state->default_tty)
    {
      if (state->mode == PLY_MODE_SHUTDOWN)
        {
          state->default_tty = SHUTDOWN_TTY;
        }
      else
        state->default_tty = BOOT_TTY;

      ply_trace ("checking if '%s' exists", state->default_tty);
      if (!ply_character_device_exists (state->default_tty))
        {
          ply_trace ("nope, forcing details mode");
          state->should_force_details = true;

          state->default_tty = find_fallback_tty (state);
          ply_trace ("going to go with '%s'", state->default_tty);
        }
    }

  check_verbosity (state);
  check_logging (state);

  ply_trace ("source built on %s", __DATE__);

  state->keystroke_triggers = ply_list_new ();
  state->entry_triggers = ply_list_new ();
  state->entry_buffer = ply_buffer_new();
  state->messages = ply_list_new ();

  redirect_standard_io_to_dev_null ();

  ply_trace ("Making sure " PLYMOUTH_RUNTIME_DIR " exists");
  if (!ply_create_directory (PLYMOUTH_RUNTIME_DIR))
    ply_trace ("could not create " PLYMOUTH_RUNTIME_DIR ": %m");

  ply_trace ("initialized minimal work environment");
  return true;
}

static void
on_error_message (ply_buffer_t *debug_buffer,
                  const void   *bytes,
                  size_t        number_of_bytes)
{
  ply_buffer_append_bytes (debug_buffer, bytes, number_of_bytes);
}

static void
dump_debug_buffer_to_file (void)
{
  int fd;
  const char *bytes;
  size_t size;

  fd = open (debug_buffer_path,
             O_WRONLY | O_CREAT | O_TRUNC, 0600);

  if (fd < 0)
    return;

  size = ply_buffer_get_size (debug_buffer);
  bytes = ply_buffer_get_bytes (debug_buffer);
  ply_write (fd, bytes, size);
  close (fd);
}

 #include <termios.h>
 #include <unistd.h>
static void
on_crash (int signum)
{
    struct termios term_attributes;
    int fd;

    fd = open ("/dev/tty1", O_RDWR | O_NOCTTY);
    if (fd < 0) fd = open ("/dev/hvc0", O_RDWR | O_NOCTTY);

    ioctl (fd, KDSETMODE, KD_TEXT);

    tcgetattr (fd, &term_attributes);

    term_attributes.c_iflag |= BRKINT | IGNPAR | ICRNL | IXON;
    term_attributes.c_oflag |= OPOST;
    term_attributes.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;

    tcsetattr (fd, TCSAFLUSH, &term_attributes);

    close (fd);

    if (debug_buffer != NULL)
      {
        dump_debug_buffer_to_file ();
        sleep (30);
      }

    if (pid_file != NULL)
      {
        unlink (pid_file);
        free (pid_file);
        pid_file = NULL;
      }

    signal (signum, SIG_DFL);
    raise(signum);
}

static void
write_pid_file (const char *filename)
{
  FILE *fp;

  fp = fopen (filename, "w");
  if (fp == NULL)
    {
      ply_error ("could not write pid file %s: %m", filename);
    }
  else
    {
      fprintf (fp, "%d\n", (int) getpid ());
      fclose (fp);
    }
}

int
main (int    argc,
      char **argv)
{
  state_t state = { 0 };
  int exit_code;
  bool should_help = false;
  bool no_daemon = false;
  bool debug = false;
  bool attach_to_session;
  ply_daemon_handle_t *daemon_handle = NULL;
  char *mode_string = NULL;
  char *kernel_command_line = NULL;
  char *tty = NULL;
  ply_device_manager_flags_t device_manager_flags = PLY_DEVICE_MANAGER_FLAGS_NONE;

  state.start_time = ply_get_timestamp ();
  state.command_parser = ply_command_parser_new ("plymouthd", "Splash server");

  state.loop = ply_event_loop_get_default ();

  ply_command_parser_add_options (state.command_parser,
                                  "help", "This help message", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "attach-to-session", "Redirect console messages from screen to log", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "no-daemon", "Do not daemonize", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "debug", "Output debugging information", PLY_COMMAND_OPTION_TYPE_FLAG,
                                  "debug-file", "File to output debugging information to", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "mode", "Mode is one of: boot, shutdown", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "pid-file", "Write the pid of the daemon to a file", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "kernel-command-line", "Fake kernel command line to use", PLY_COMMAND_OPTION_TYPE_STRING,
                                  "tty", "TTY to use instead of default", PLY_COMMAND_OPTION_TYPE_STRING,
                                  NULL);

  if (!ply_command_parser_parse_arguments (state.command_parser, state.loop, argv, argc))
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      ply_error_without_new_line ("%s", help_string);

      free (help_string);
      return EX_USAGE;
    }

  ply_command_parser_get_options (state.command_parser,
                                  "help", &should_help,
                                  "attach-to-session", &attach_to_session,
                                  "mode", &mode_string,
                                  "no-daemon", &no_daemon,
                                  "debug", &debug,
                                  "debug-file", &debug_buffer_path,
                                  "pid-file", &pid_file,
                                  "tty", &tty,
                                  "kernel-command-line", &kernel_command_line,
                                  NULL);

  if (should_help)
    {
      char *help_string;

      help_string = ply_command_parser_get_help_string (state.command_parser);

      if (argc < 2)
        fprintf (stderr, "%s", help_string);
      else
        printf ("%s", help_string);

      free (help_string);
      return 0;
    }

  if (debug && !ply_is_tracing ())
    ply_toggle_tracing ();

  if (mode_string != NULL)
    {
      if (strcmp (mode_string, "shutdown") == 0)
        state.mode = PLY_MODE_SHUTDOWN;
      else if (strcmp (mode_string, "updates") == 0)
        state.mode = PLY_MODE_UPDATES;
      else
        state.mode = PLY_MODE_BOOT;

      free (mode_string);
    }

  if (tty != NULL)
    {
      state.default_tty = tty;
    }

  if (kernel_command_line != NULL)
    {
      strncpy (state.kernel_command_line, kernel_command_line, sizeof (state.kernel_command_line));
      state.kernel_command_line[sizeof (state.kernel_command_line) - 1] = '\0';
      state.kernel_command_line_is_set = true;
    }

  if (geteuid () != 0)
    {
      ply_error ("plymouthd must be run as root user");
      return EX_OSERR;
    }

  chdir ("/");
  signal (SIGPIPE, SIG_IGN);

  if (! no_daemon)
    {
      daemon_handle = ply_create_daemon ();

      if (daemon_handle == NULL)
        {
          ply_error ("plymouthd: cannot daemonize: %m");
          return EX_UNAVAILABLE;
        }
    }

  if (debug)
    debug_buffer = ply_buffer_new ();

  signal (SIGABRT, on_crash);
  signal (SIGSEGV, on_crash);

  /* before do anything we need to make sure we have a working
   * environment.
   */
  if (!initialize_environment (&state))
    {
      if (errno == 0)
        {
          if (daemon_handle != NULL)
            ply_detach_daemon (daemon_handle, 0);
          return 0;
        }

      ply_error ("plymouthd: could not setup basic operating environment: %m");
      if (daemon_handle != NULL)
        ply_detach_daemon (daemon_handle, EX_OSERR);
      return EX_OSERR;
    }

  /* Make the first byte in argv be '@' so that we can survive systemd's killing
   * spree when going from initrd to /, and so we stay alive all the way until
   * the power is killed at shutdown.
   * http://www.freedesktop.org/wiki/Software/systemd/RootStorageDaemons
   */
  argv[0][0] = '@';

  state.boot_server = start_boot_server (&state);

  if (state.boot_server == NULL)
    {
      ply_trace ("plymouthd is already running");

      if (daemon_handle != NULL)
        ply_detach_daemon (daemon_handle, EX_OK);
      return EX_OK;
    }

  state.boot_buffer = ply_buffer_new ();

  if (attach_to_session)
    {
      state.should_be_attached = attach_to_session;
      if (!attach_to_running_session (&state))
        {
          ply_trace ("could not redirect console session: %m");
          if (! no_daemon)
            ply_detach_daemon (daemon_handle, EX_UNAVAILABLE);
          return EX_UNAVAILABLE;
        }
    }

  state.progress = ply_progress_new ();
  state.splash_delay = NAN;

  ply_progress_load_cache (state.progress,
                           get_cache_file_for_mode (state.mode));

  if (pid_file != NULL)
    write_pid_file (pid_file);

  if (daemon_handle != NULL
      && !ply_detach_daemon (daemon_handle, 0))
    {
      ply_error ("plymouthd: could not tell parent to exit: %m");
      return EX_UNAVAILABLE;
    }

  find_override_splash (&state);
  find_system_default_splash (&state);
  find_distribution_default_splash (&state);

  if (command_line_has_argument (state.kernel_command_line, "plymouth.ignore-serial-consoles"))
    device_manager_flags |= PLY_DEVICE_MANAGER_FLAGS_IGNORE_SERIAL_CONSOLES;

  if (command_line_has_argument (state.kernel_command_line, "plymouth.ignore-udev") ||
      (getenv ("DISPLAY") != NULL))
    device_manager_flags |= PLY_DEVICE_MANAGER_FLAGS_IGNORE_UDEV;

  load_devices (&state, device_manager_flags);

  ply_trace ("entering event loop");
  exit_code = ply_event_loop_run (state.loop);
  ply_trace ("exited event loop");

  ply_boot_splash_free (state.boot_splash);
  state.boot_splash = NULL;

  ply_command_parser_free (state.command_parser);

  ply_boot_server_free (state.boot_server);
  state.boot_server = NULL;

  ply_trace ("freeing terminal session");
  ply_terminal_session_free (state.session);

  ply_buffer_free (state.boot_buffer);
  ply_progress_free (state.progress);

  ply_trace ("exiting with code %d", exit_code);
  
  if (debug_buffer != NULL)
    {
      dump_debug_buffer_to_file ();
      ply_buffer_free (debug_buffer);
    }

  ply_free_error_log();

  return exit_code;
}
/* vim: set sts=4 ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
