/* ply-boot-server.c - listens for and processes boot-status events
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
#include "ply-boot-server.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "ply-event-loop.h"
#include "ply-list.h"
#include "ply-logger.h"
#include "ply-utils.h"

typedef struct 
{
  int fd;
  ply_fd_watch_t *watch;
  ply_boot_server_t *server;
} ply_boot_connection_t;

struct _ply_boot_server
{
  ply_event_loop_t *loop;
  ply_list_t *connections;
  ply_list_t *cached_answers;
  int socket_fd;

  ply_boot_server_update_handler_t update_handler;
  ply_boot_server_newroot_handler_t newroot_handler;
  ply_boot_server_system_initialized_handler_t system_initialized_handler;
  ply_boot_server_error_handler_t error_handler;
  ply_boot_server_show_splash_handler_t show_splash_handler;
  ply_boot_server_hide_splash_handler_t hide_splash_handler;
  ply_boot_server_ask_for_password_handler_t ask_for_password_handler;
  ply_boot_server_quit_handler_t quit_handler;
  void *user_data;

  uint32_t is_listening : 1;
};

ply_boot_server_t *
ply_boot_server_new (ply_boot_server_update_handler_t  update_handler,
                     ply_boot_server_ask_for_password_handler_t ask_for_password_handler,
                     ply_boot_server_show_splash_handler_t show_splash_handler,
                     ply_boot_server_hide_splash_handler_t hide_splash_handler,
                     ply_boot_server_newroot_handler_t newroot_handler,
                     ply_boot_server_system_initialized_handler_t initialized_handler,
                     ply_boot_server_error_handler_t   error_handler,
                     ply_boot_server_quit_handler_t    quit_handler,
                     void                             *user_data)
{
  ply_boot_server_t *server;

  server = calloc (1, sizeof (ply_boot_server_t));
  server->connections = ply_list_new ();
  server->cached_answers = ply_list_new ();
  server->loop = NULL;
  server->is_listening = false;
  server->update_handler = update_handler;
  server->ask_for_password_handler = ask_for_password_handler;
  server->newroot_handler = newroot_handler;
  server->error_handler = error_handler;
  server->system_initialized_handler = initialized_handler;
  server->show_splash_handler = show_splash_handler;
  server->hide_splash_handler = hide_splash_handler;
  server->quit_handler = quit_handler;
  server->user_data = user_data;

  return server;
}

void
ply_boot_server_free (ply_boot_server_t *server)
{
  if (server == NULL)
    return;

  ply_list_free (server->connections);
  ply_list_free (server->cached_answers);
  free (server);
}

static ply_boot_connection_t *
ply_boot_connection_new (ply_boot_server_t *server,
                         int                fd)
{
  ply_boot_connection_t *connection;

  connection = calloc (1, sizeof (ply_boot_server_t));
  connection->fd = fd;
  connection->server = server;
  connection->watch = NULL;

  return connection;
}

static void
ply_boot_connection_free (ply_boot_connection_t *connection)
{
  if (connection == NULL)
    return;

  close (connection->fd);
  free (connection);
}

bool
ply_boot_server_listen (ply_boot_server_t *server)
{
  assert (server != NULL);

  server->socket_fd =
      ply_listen_to_unix_socket (PLY_BOOT_PROTOCOL_SOCKET_PATH + 1, true);

  if (server->socket_fd < 0)
    return false;

  return true;
}

void
ply_boot_server_stop_listening (ply_boot_server_t *server)
{
  assert (server != NULL);
}

static bool
ply_boot_connection_read_request (ply_boot_connection_t  *connection,
                                  char                  **command,
                                  char                  **argument)
{
  uint8_t header[2];

  assert (connection != NULL);
  assert (connection->fd >= 0);

  if (!ply_read (connection->fd, header, sizeof (header)))
    return false;

  *command = calloc (2, sizeof (char));
  *command[0] = header[0];

  *argument = NULL;
  if (header[1] == '\002')
    {
      uint8_t argument_size;

      if (!ply_read (connection->fd, &argument_size, sizeof (uint8_t)))
        return false;

      *argument = calloc (argument_size, sizeof (char));

      if (!ply_read (connection->fd, *argument, argument_size))
        return false;
    }
  return true;
}

static bool
ply_boot_connection_is_from_root (ply_boot_connection_t *connection)
{
  uid_t uid;

  if (!ply_get_credentials_from_fd (connection->fd, NULL, &uid, NULL))
    return false;

  return uid == 0;
}

static void
ply_boot_connection_on_password_answer (ply_boot_connection_t *connection,
                                        const char            *password,
                                        ply_answer_t          *answer)
{

  uint8_t size;

  /* splash plugin isn't able to ask for password,
   * punt to client
   */
  if (password == NULL)
    {
      if (!ply_write (connection->fd,
                      PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NO_ANSWER,
                      strlen (PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NO_ANSWER)))
        ply_error ("could not write bytes: %m");
    }
  else
    {
      /* FIXME: support up to 4 billion
      */
      if (strlen (password) > 255)
          ply_error ("password to long to fit in buffer");

      size = (uint8_t) strlen (password);

      if (!ply_write (connection->fd,
                      PLY_BOOT_PROTOCOL_RESPONSE_TYPE_ANSWER,
                      strlen (PLY_BOOT_PROTOCOL_RESPONSE_TYPE_ANSWER)) ||
          !ply_write (connection->fd,
                      &size, sizeof (uint8_t)) ||
          !ply_write (connection->fd,
                      password, size))
          ply_error ("could not write bytes: %m");
    }

  ply_list_append_data (connection->server->cached_answers, answer);
}

static void
ply_boot_connection_on_request (ply_boot_connection_t *connection)
{
  ply_boot_server_t *server;
  char *command, *argument;

  assert (connection != NULL);
  assert (connection->fd >= 0);

  server = connection->server;
  assert (server != NULL);

  if (!ply_boot_connection_read_request (connection,
                                         &command, &argument))
    return;

  if (!ply_boot_connection_is_from_root (connection))
    {
      ply_error ("request came from non-root user");

      if (!ply_write (connection->fd,
                      PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NAK,
                      strlen (PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NAK)))
        ply_error ("could not write bytes: %m");

      return;
    }

  if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_UPDATE) == 0)
    {
      if (server->update_handler != NULL)
        server->update_handler (server->user_data, argument, server);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_SYSTEM_INITIALIZED) == 0)
    {
      if (server->system_initialized_handler != NULL)
        server->system_initialized_handler (server->user_data, server);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_ERROR) == 0)
    {
      if (server->error_handler != NULL)
        server->error_handler (server->user_data, server);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_SHOW_SPLASH) == 0)
    {
      ply_trace ("got show splash request");
      if (server->show_splash_handler != NULL)
        server->show_splash_handler (server->user_data, server);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_HIDE_SPLASH) == 0)
    {
      ply_trace ("got hide splash request");
      if (server->hide_splash_handler != NULL)
        server->hide_splash_handler (server->user_data, server);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_QUIT) == 0)
    {
      if (server->quit_handler != NULL)
        server->quit_handler (server->user_data, server);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_PASSWORD) == 0)
    {
      ply_answer_t *answer;

      answer = ply_answer_new ((ply_answer_handler_t)
                               ply_boot_connection_on_password_answer,
                               connection);

      if (server->ask_for_password_handler != NULL)
        server->ask_for_password_handler (server->user_data,
                                          answer,
                                          server);
      /* will reply later
       */
      return;
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_CACHED_PASSWORD) == 0)
    {
      ply_list_node_t *node;
      ply_buffer_t *buffer;
      size_t buffer_size;
      uint8_t size;

      buffer = ply_buffer_new ();

      node = ply_list_get_first_node (server->cached_answers);

      /* Add each answer separated by their NUL terminators into
       * a buffer that we write out to the client
       */
      while (node != NULL)
        {
          ply_list_node_t *next_node;
          ply_answer_t *answer;
          char *answer_string;

          next_node = ply_list_get_next_node (server->cached_answers, node);
          answer = ply_list_node_get_data (node);
          answer_string = ply_answer_get_string (answer);
          ply_buffer_append_bytes (buffer,
                                   answer_string,
                                   strlen (answer_string) + 1);
          free (answer_string);

          node = next_node;
        }

      buffer_size = ply_buffer_get_size (buffer);

      /* splash plugin doesn't have any cached passwords
      */
      if (buffer_size == 0)
        {
          if (!ply_write (connection->fd,
                          PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NO_ANSWER,
                          strlen (PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NO_ANSWER)))
              ply_error ("could not write bytes: %m");
        }
      else
        {
          /* FIXME: This is likely too small, we need to add another
           * layer of indirection that says how many bytes the size
           * is.
           */
          if (buffer_size > 255)
            ply_error ("passwords too long to fit in buffer");

          size = buffer_size;

          if (!ply_write (connection->fd,
                          PLY_BOOT_PROTOCOL_RESPONSE_TYPE_MULTIPLE_ANSWERS,
                          strlen (PLY_BOOT_PROTOCOL_RESPONSE_TYPE_MULTIPLE_ANSWERS)) ||
              !ply_write (connection->fd,
                          &size, sizeof (uint8_t)) ||
              !ply_write (connection->fd,
                          ply_buffer_get_bytes (buffer), size))
              ply_error ("could not write bytes: %m");
        }

      ply_buffer_free (buffer);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_NEWROOT) == 0)
    {
      if (server->newroot_handler != NULL)
        server->newroot_handler(server->user_data, argument, server);
    }
  else if (strcmp (command, PLY_BOOT_PROTOCOL_REQUEST_TYPE_PING) != 0)
    {
      ply_error ("received unknown command '%s' from client", command);

      if (!ply_write (connection->fd,
                      PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NAK,
                      strlen (PLY_BOOT_PROTOCOL_RESPONSE_TYPE_NAK)))
        ply_error ("could not write bytes: %m");

      return;
    }

  if (!ply_write (connection->fd, 
                  PLY_BOOT_PROTOCOL_RESPONSE_TYPE_ACK,
                  strlen (PLY_BOOT_PROTOCOL_RESPONSE_TYPE_ACK)))
    {
      ply_error ("could not write bytes: %m");
    }
}

static void
ply_boot_connection_on_hangup (ply_boot_connection_t *connection)
{
  ply_list_node_t *node;
  ply_boot_server_t *server;

  assert (connection != NULL);
  assert (connection->server != NULL);

  server = connection->server;

  node = ply_list_find_node (server->connections, connection);

  assert (node != NULL);

  ply_boot_connection_free (connection);
  ply_list_remove_node (server->connections, node);
}

static void
ply_boot_server_on_new_connection (ply_boot_server_t *server)
{
  ply_boot_connection_t *connection;
  int fd;

  assert (server != NULL);

  fd = accept (server->socket_fd, NULL, NULL);

  if (fd < 0)
    return;

  connection = ply_boot_connection_new (server, fd);

  connection->watch = 
      ply_event_loop_watch_fd (server->loop, fd,
                               PLY_EVENT_LOOP_FD_STATUS_HAS_DATA,
                               (ply_event_handler_t)
                               ply_boot_connection_on_request,
                               (ply_event_handler_t)
                               ply_boot_connection_on_hangup,
                               connection);

  ply_list_append_data (server->connections, connection);
}

static void
ply_boot_server_on_hangup (ply_boot_server_t *server)
{
  assert (server != NULL);
}

static void
ply_boot_server_detach_from_event_loop (ply_boot_server_t *server)
{
  assert (server != NULL);
  server->loop = NULL;
}

void
ply_boot_server_attach_to_event_loop (ply_boot_server_t *server,
                                      ply_event_loop_t  *loop)
{
  assert (server != NULL);
  assert (loop != NULL);
  assert (server->loop == NULL);
  assert (server->socket_fd >= 0);

  server->loop = loop;

  ply_event_loop_watch_fd (loop, server->socket_fd,
                           PLY_EVENT_LOOP_FD_STATUS_HAS_DATA,
                           (ply_event_handler_t)
                           ply_boot_server_on_new_connection,
                           (ply_event_handler_t)
                           ply_boot_server_on_hangup,
                           server);
  ply_event_loop_watch_for_exit (loop, (ply_event_loop_exit_handler_t) 
                                 ply_boot_server_detach_from_event_loop,
                                 server); 
}

#ifdef PLY_BOOT_SERVER_ENABLE_TEST

#include <stdio.h>

#include "ply-event-loop.h"
#include "ply-boot-server.h"

static void 
on_update (ply_event_loop_t  *loop,
           const char        *status)
{
  printf ("new status is '%s'\n", status);
}

static void
on_newroot (ply_event_loop_t *loop)
{
  printf ("got newroot request\n");
}

static void
on_system_initialized (ply_event_loop_t *loop)
{
  printf ("got sysinit done request\n");
}

static void
on_show_splash (ply_event_loop_t *loop)
{
  printf ("got show splash request\n");
}

static void
on_hide_splash (ply_event_loop_t *loop)
{
  printf ("got hide splash request\n");
}

static void
on_quit (ply_event_loop_t *loop)
{
  printf ("got quit request, quiting...\n");
  ply_event_loop_exit (loop, 0);
}

static char *
on_ask_for_password (ply_event_loop_t *loop)
{
  printf ("got password request, returning 'password'...\n");

  return strdup ("password");
}

int
main (int    argc,
      char **argv)
{
  ply_event_loop_t *loop;
  ply_boot_server_t *server;
  int exit_code;

  exit_code = 0;

  loop = ply_event_loop_new ();

  server = ply_boot_server_new ((ply_boot_server_update_handler_t) on_update,
                                (ply_boot_server_ask_for_password_handler_t) on_ask_for_password,
                                (ply_boot_server_show_splash_handler_t) on_show_splash,
                                (ply_boot_server_hide_splash_handler_t) on_hide_splash,
                                (ply_boot_server_newroot_handler_t) on_newroot,
                                (ply_boot_server_system_initialized_handler_t) on_system_initialized,
                                (ply_boot_server_quit_handler_t) on_quit,
                                loop);

  if (!ply_boot_server_listen (server))
    {
      perror ("could not start boot status daemon");
      return errno;
    }

  ply_boot_server_attach_to_event_loop (server, loop);
  exit_code = ply_event_loop_run (loop);
  ply_boot_server_free (server);

  return exit_code;
}

#endif /* PLY_BOOT_SERVER_ENABLE_TEST */
/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
