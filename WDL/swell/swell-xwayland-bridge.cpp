/* Cockos SWELL (Simple/Small Win32 Emulation Layer for Linux/OSX)
   Copyright (C) 2006 and later, Cockos, Inc.

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
       claim that you wrote the original software. If you use this software
       in a product, an acknowledgment in the product documentation would be
       appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
       misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.


  */

#include <glib.h>
#include <wayland-client.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <cairo-xcb.h>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <drm_fourcc.h>
#include <X11/cursorfont.h>
#include <xcb/composite.h>
#include <xcb/damage.h>
#include <xcb/dri3.h>
#include <xcb/randr.h>
#include <xcb/res.h>
#include <xcb/shape.h>
#include <xcb/sync.h>
#include <xcb/xcb_ewmh.h>
#include <xcb/xcb_icccm.h>
#include <xcb/xcb_image.h>
#include <xcb/xcb_keysyms.h>
#include <xcb/xcb_renderutil.h>
#include <xcb/xfixes.h>
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit
#include <xcb/xtest.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "swell-xwayland-bridge.h"
#include "swell-wayland.h"
#include "swell-internal.h"
#include "../assocarray.h"
#include "../wdlstring.h"
#include "../mutex.h"
static inline xcb_screen_t *swell_xcb_screen(xcb_connection_t *connection)
{
  return xcb_setup_roots_iterator(xcb_get_setup(connection)).data;
}

static inline void swell_xcb_sync(xcb_connection_t *connection)
{
  free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), NULL));
}

static inline bool swell_xcb_translate(xcb_connection_t *connection, xcb_window_t source,
    xcb_window_t destination, int x, int y, int *out_x, int *out_y)
{
  xcb_translate_coordinates_reply_t *reply = xcb_translate_coordinates_reply(
      connection, xcb_translate_coordinates(connection, source, destination, x, y), NULL);
  if (!reply) return false;
  *out_x = reply->dst_x;
  *out_y = reply->dst_y;
  const bool same_screen = reply->same_screen;
  free(reply);
  return same_screen;
}

static inline void swell_xcb_raise(xcb_connection_t *connection, xcb_window_t window)
{
  const uint32_t above = XCB_STACK_MODE_ABOVE;
  xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_STACK_MODE, &above);
}

static inline void swell_xcb_move_resize(
    xcb_connection_t *connection, xcb_window_t window, int x, int y, int width, int height)
{
  const uint32_t values[] = {(uint32_t)x, (uint32_t)y, (uint32_t)width, (uint32_t)height};
  xcb_configure_window(connection, window,
      XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y | XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, values);
}

#ifdef _DEBUG
#define DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)
#else
#define DEBUG_PRINT(...) ((void)0)
#endif

struct WaylandOutput
{
  uint32_t object, xdg = 0, version = 0;
  int scale = 1, reported_scale = 1;
  int32_t logical[2][2] = {};
  unsigned received = 0;
  explicit WaylandOutput(uint32_t id) : object(id) {}
};

struct WaylandConnection
{
  wl_display *display;
  int socket;
  WDL_Mutex mutex;
  WDL_PtrSet<uint32_t> registries, seats;
  WDL_IntKeyedArray<uint32_t> output_managers;
  WDL_PtrList<WaylandOutput> outputs;
  int refs = 1;
  void retain() { wdl_atomic_incr(&refs); }
  void release()
  {
    if (!wdl_atomic_decr(&refs)) delete this;
  }

  WaylandConnection(wl_display *d, int fd) : display(d), socket(fd) {}
  ~WaylandConnection()
  {
    close(socket);
    wl_display_disconnect(display);
    outputs.Empty(true);
  }

  void stop()
  {
    shutdown(socket, SHUT_RDWR);
    shutdown(wl_display_get_fd(display), SHUT_RDWR);
  }
};

struct WaylandMessage
{
  uint32_t header[2];
  WDL_TypedBuf<uint32_t> body;
  WDL_TypedBuf<int> fds;

  ~WaylandMessage() { close_fds(); }

  void close_fds()
  {
    for (int fd : fds) close(fd);
    fds.Resize(0);
  }

  bool read_bytes(int socket, void *data, size_t size)
  {
    while (size)
    {
      union
      {
        cmsghdr align;
        char data[CMSG_SPACE(28 * sizeof(int))];
      } control;
      iovec iov = {data, size};
      msghdr msg = {};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = control.data;
      msg.msg_controllen = sizeof(control.data);
      ssize_t received = recvmsg(socket, &msg, MSG_CMSG_CLOEXEC);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) return false;
      bool good = !(msg.msg_flags & MSG_CTRUNC);
      for (cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg))
      {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        const int *received_fds = (const int *)CMSG_DATA(cmsg);
        const size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (count && (!good || !fds.Add(received_fds, count)))
        {
          for (size_t i = 0; i < count; ++i) close(received_fds[i]);
          good = false;
        }
      }
      if (!good) return false;
      data = (char *)data + received;
      size -= received;
    }
    return true;
  }

  bool read(int socket)
  {
    if (!read_bytes(socket, header, sizeof(header))) return false;
    const size_t size = header[1] >> 16;
    if (size < sizeof(header) || size % 4) return false;
    const int words = (size - sizeof(header)) / sizeof(uint32_t);
    if (words && !body.ResizeOK(words)) return false;
    if (!words) body.Resize(0);
    return read_bytes(socket, body.Get(), size - sizeof(header));
  }

  bool write_bytes(int socket, const void *data, size_t size)
  {
    while (size)
    {
      iovec iov = {(void *)data, size};
      msghdr msg = {};
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      WDL_TypedBuf<char> control;
      if (fds.GetSize())
      {
        if (!control.ResizeOK(CMSG_SPACE(fds.GetSize() * sizeof(int)))) return false;
        msg.msg_control = control.Get();
        msg.msg_controllen = control.GetSize();
        cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(fds.GetSize() * sizeof(int));
        memcpy(CMSG_DATA(cmsg), fds.Get(), fds.GetSize() * sizeof(int));
      }
      ssize_t sent = sendmsg(socket, &msg, MSG_NOSIGNAL);
      if (sent < 0 && errno == EINTR) continue;
      if (sent <= 0) return false;
      close_fds();
      data = (const char *)data + sent;
      size -= sent;
    }
    return true;
  }

  bool write(int socket)
  {
    return write_bytes(socket, header, sizeof(header)) &&
           write_bytes(socket, body.Get(), body.GetSize() * sizeof(uint32_t));
  }
};

static void relay_wayland(WaylandConnection *connection, bool from_server)
{
  const int server = wl_display_get_fd(connection->display);
  const int source = from_server ? server : connection->socket;
  const int destination = from_server ? connection->socket : server;
  WaylandMessage message;
  WDL_PtrSet<uint64_t> hidden_globals;
  WDL_TypedBuf<uint32_t> updates;
  while (message.read(source))
  {
    const uint32_t object = message.header[0], opcode = message.header[1] & 0xffff;
    bool is_registry;
    updates.Resize(0);
    {
      WDL_MutexLock lock(&connection->mutex);
      if (!from_server && object == 1 && opcode == WL_DISPLAY_GET_REGISTRY && message.body.GetSize() == 1)
        connection->registries.Insert(message.body.Get()[0]);
      is_registry = connection->registries.Get(object) != 0;

      /* This is a bit of a nastypoo hack to work around the lack of
       * https://gitlab.freedesktop.org/xorg/xserver/-/commit/0ca92fb4be5bdefcb70edfece31b35d1ae79eb85
       * in Ubuntu 26.04 LTS. Until that's backported, we accept this grossness. */
      if (!from_server && is_registry && opcode == WL_REGISTRY_BIND && message.body.GetSize() == 6 &&
          message.body.Get()[1] == sizeof("wl_seat") && !memcmp(&message.body.Get()[2], "wl_seat", sizeof("wl_seat")))
        connection->seats.Insert(message.body.Get()[message.body.GetSize() - 1]);
      if (from_server && connection->seats.Get(object) && opcode == 0 && message.body.GetSize() == 1)
        message.body.Get()[0] &= ~WL_SEAT_CAPABILITY_POINTER;
      if (!from_server && is_registry && opcode == WL_REGISTRY_BIND && message.body.GetSize() >= 4)
      {
        const uint32_t length = message.body.Get()[1];
        const uint32_t words = length / 4 + !!(length % 4);
        if (length && words == (uint32_t)message.body.GetSize() - 4)
        {
          const char *interface = (const char *)&message.body.Get()[2];
          const uint32_t version = message.body.Get()[words + 2], id = message.body.Get()[words + 3];
          if (length == sizeof("wl_output") && !memcmp(interface, "wl_output", length))
            connection->outputs.Add(new WaylandOutput(id));
          else if (length == sizeof("zxdg_output_manager_v1") &&
                   !memcmp(interface, "zxdg_output_manager_v1", length))
            connection->output_managers.Insert(id, version);
        }
      }
      if (!from_server && opcode == 1 && message.body.GetSize() == 2)
        if (const uint32_t version = connection->output_managers.Get(object))
          for (WaylandOutput *output : connection->outputs)
            if (output->object == message.body.Get()[1])
            {
              output->xdg = message.body.Get()[0];
              output->version = version;
              output->received = 0;
            }
      if (from_server)
        for (WaylandOutput *output : connection->outputs)
        {
          if (object == output->object && opcode == WL_OUTPUT_SCALE && message.body.GetSize() == 1)
            output->scale = max(1, (int)message.body.Get()[0]);
          if (object == output->xdg && opcode <= 1 && message.body.GetSize() == 2)
          {
            output->received |= 1u << opcode;
            for (int i = 0; i < 2; ++i)
            {
              output->logical[opcode][i] = message.body.Get()[i];
              message.body.Get()[i] *= output->scale;
            }
          }
          if (object == output->object && opcode == WL_OUTPUT_DONE &&
              output->scale != output->reported_scale)
          {
            output->reported_scale = output->scale;
            if (!output->xdg || !output->received) continue;
            for (unsigned i = 0; i < 2; ++i)
              if (output->received & (1u << i))
              {
                const uint32_t event[] = {output->xdg, (16u << 16) | i,
                    (uint32_t)output->logical[i][0] * output->scale,
                    (uint32_t)output->logical[i][1] * output->scale};
                updates.Add(event, 4);
              }
            if (output->version < 3)
            {
              const uint32_t done[] = {output->xdg, (8u << 16) | 2};
              updates.Add(done, 2);
            }
          }
        }
      if (from_server && object == 1 && opcode == 1 && message.body.GetSize() == 1)
      {
        const uint32_t id = message.body.Get()[0];
        connection->seats.Delete(id);
        connection->output_managers.Delete(id);
        for (int i = connection->outputs.GetSize() - 1; i >= 0; --i)
        {
          WaylandOutput *output = connection->outputs.Get(i);
          if (output->object == id)
            connection->outputs.Delete(i, true);
          else if (output->xdg == id)
            output->xdg = 0;
        }
      }
    }
    if (from_server && is_registry)
    {
      if (opcode == 0 && message.body.GetSize() >= 2)
      {
        const char hidden[] = "xwayland_shell_v1";
        if (message.body.Get()[1] == sizeof(hidden) &&
            (message.body.GetSize() - 2) * sizeof(uint32_t) >= sizeof(hidden) &&
            !memcmp(&message.body.Get()[2], hidden, sizeof(hidden)))
        {
          hidden_globals.Insert((uint64_t)object << 32 | message.body.Get()[0]);
          continue;
        }
      }
      else if (opcode == 1 && message.body.GetSize() == 1)
      {
        const uint64_t key = (uint64_t)object << 32 | message.body.Get()[0];
        if (hidden_globals.Get(key))
        {
          hidden_globals.Delete(key);
          continue;
        }
      }
    }
    if (updates.GetSize() &&
        !message.write_bytes(destination, updates.Get(), updates.GetSize() * sizeof(uint32_t))) break;
    if (!message.write(destination)) break;
  }
  connection->stop();
  connection->release();
}

static gpointer relay_to_server(gpointer data)
{
  relay_wayland((WaylandConnection *)data, false);
  return NULL;
}

static gpointer relay_from_server(gpointer data)
{
  relay_wayland((WaylandConnection *)data, true);
  return NULL;
}

static char *socket_directory;
static pid_t socket_owner;
static sockaddr_un socket_address;

static void remove_xwayland_socket()
{
  if (!socket_directory || socket_owner != getpid()) return;
  unlink(socket_address.sun_path);
  rmdir(socket_directory);
  g_free(socket_directory);
  socket_directory = NULL;
}

static int create_xwayland_socket(char *display_name, size_t size)
{
  remove_xwayland_socket();
  socket_directory = g_build_filename(g_get_user_runtime_dir(), "swell-xwayland-XXXXXX", NULL);
  if (!g_mkdtemp(socket_directory))
  {
    g_free(socket_directory);
    socket_directory = NULL;
    return -1;
  }
  socket_owner = getpid();
  socket_address.sun_family = AF_UNIX;
  const int length =
      snprintf(socket_address.sun_path, sizeof(socket_address.sun_path), "%s/X", socket_directory);
  if (length < 0 || (size_t)length >= sizeof(socket_address.sun_path) || (size_t)length >= size)
  {
    socket_address.sun_path[0] = 0;
    remove_xwayland_socket();
    return -1;
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0 || bind(fd, (sockaddr *)&socket_address, sizeof(socket_address)) < 0 ||
      listen(fd, SOMAXCONN) < 0)
  {
    if (fd >= 0) close(fd);
    remove_xwayland_socket();
    return -1;
  }
  memcpy(display_name, socket_address.sun_path, length + 1);
  return fd;
}

static void xwayland_child_setup(gpointer data)
{
  if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != *(pid_t *)data) _exit(1);
}

static pid_t xw_spawn_xwayland(char *display_name, size_t size, int *wm_fd)
{
  *wm_fd = -1;
  wl_display *display = wl_display_connect(NULL);
  if (!display) return -1;
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets))
  {
    wl_display_disconnect(display);
    return -1;
  }
  WaylandConnection *connection = new WaylandConnection(display, sockets[0]);
  const int server = wl_display_get_fd(display);
  const int flags = fcntl(server, F_GETFL);
  if (flags < 0 || fcntl(server, F_SETFL, flags & ~O_NONBLOCK) < 0)
  {
    close(sockets[1]);
    connection->release();
    return -1;
  }
  int ready[2] = {-1, -1}, wm[2] = {-1, -1};
  const int listener = create_xwayland_socket(display_name, size);
  if (listener < 0 || pipe2(ready, O_CLOEXEC) < 0 ||
      socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wm) < 0)
  {
    const int fds[] = {listener, ready[0], ready[1], wm[0], wm[1], sockets[1]};
    for (int fd : fds)
      if (fd >= 0) close(fd);
    remove_xwayland_socket();
    connection->release();
    return -1;
  }
  extern char **environ;
  WDL_TypedBuf<const char *> env;
  for (char **p = environ; *p; ++p)
    if (strncmp(*p, "WAYLAND_SOCKET=", 15) && strncmp(*p, "DISPLAY=", 8)) env.Add(*p);
  env.Add("WAYLAND_SOCKET=6");
  env.Add(NULL);
  pid_t parent = getpid(), pid = -1;
  char server_name[32];
  snprintf(server_name, sizeof(server_name), ":%ld", (long)parent);
  const char *args[] = {"Xwayland", server_name, "-displayfd", "3", "-wm", "4", "-listenfd", "5",
      "-rootless", "-nolisten", "tcp", NULL};
  const int source_fds[] = {ready[1], wm[1], listener, sockets[1]};
  const int target_fds[] = {3, 4, 5, 6};
  if (!g_spawn_async_with_pipes_and_fds(NULL, args, env.Get(),
          (GSpawnFlags)(G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH | G_SPAWN_CHILD_INHERITS_STDIN),
          xwayland_child_setup, &parent, -1, -1, -1, source_fds, target_fds, 4, &pid, NULL, NULL,
          NULL, NULL))
    pid = -1;
  close(ready[1]);
  close(wm[1]);
  close(listener);
  close(sockets[1]);
  if (pid > 0)
  {
    connection->retain();
    GThread *to_server = g_thread_try_new("xwayland-send", relay_to_server, connection, NULL);
    if (!to_server) connection->release();
    connection->retain();
    GThread *to_client = g_thread_try_new("xwayland-recv", relay_from_server, connection, NULL);
    if (!to_client) connection->release();
    if (!to_server || !to_client)
    {
      close(ready[0]);
      close(wm[0]);
      connection->stop();
      if (to_server) g_thread_join(to_server);
      if (to_client) g_thread_join(to_client);
      connection->release();
      kill(pid, SIGTERM);
      while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
      {
      }
      remove_xwayland_socket();
      return -1;
    }
    g_thread_unref(to_server);
    g_thread_unref(to_client);
  }
  connection->release();
  char display_number[32];
  size_t length = 0;
  const gint64 deadline = g_get_monotonic_time() + 10000000;
  while (pid > 0 && length < sizeof(display_number))
  {
    const int remaining = (deadline - g_get_monotonic_time()) / 1000;
    if (remaining <= 0) break;
    pollfd fd = {ready[0], POLLIN, 0};
    const int result = poll(&fd, 1, remaining);
    if (result < 0 && errno == EINTR) continue;
    if (result <= 0) break;
    const ssize_t count = read(ready[0], display_number + length, sizeof(display_number) - length);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    length += count;
    if (display_number[length - 1] == '\n')
    {
      display_number[--length] = 0;
      if (!length || strspn(display_number, "0123456789") != length) break;
      close(ready[0]);
      *wm_fd = wm[0];
      atexit(remove_xwayland_socket);
      return pid;
    }
  }
  close(ready[0]);
  close(wm[0]);
  if (pid > 0)
  {
    kill(pid, SIGTERM);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
    {
    }
  }
  remove_xwayland_socket();
  return -1;
}

struct WMAtoms : xcb_ewmh_connection_t
{
  xcb_atom_t WM_DELETE_WINDOW, WM_TAKE_FOCUS, WM_STATE, WM_S0;
  xcb_atom_t _XWAYLAND_ALLOW_COMMITS, _REAPERX_TIMESTAMP, _XEMBED_INFO;
  xcb_atom_t XdndSelection, XdndAware, XdndEnter, XdndPosition;
  xcb_atom_t XdndStatus, XdndDrop, XdndFinished, XdndLeave, XdndActionCopy;
  xcb_atom_t uri_list, dnd_data, targets, motif_hints;
};

#define WM_STATE_WITHDRAWN 0
#define WM_STATE_NORMAL 1
#define WM_STATE_ICONIC 3

struct WMWindowState
{
  bool input_hint;
  bool take_focus;
};

class XWaylandWM
{
public:
  explicit XWaylandWM(xcb_connection_t *connection);
  xcb_connection_t *connection() const { return connection_; }
  xcb_window_t host_focus_window() const { return support_win_; }
  const WMAtoms &atoms() const { return atoms_; }
  bool owns_window(xcb_window_t window, xcb_window_t gui, bool include_child_processes = false);
  enum WindowType
  {
    NORMAL,
    POPUP,
    DRAG_ICON
  };
  WindowType window_type(xcb_window_t window, bool override_redirect);
  bool window_decorated(xcb_window_t window);

  static XWaylandWM *init_bridge_wm();
  static const char *display_name();

  void announce_wm(xcb_window_t support_win);

  bool handle_event(xcb_generic_event_t *ev);

  void track_window(xcb_window_t w);
  void untrack_window(xcb_window_t w);

  void set_active_window(xcb_window_t window);
  void send_take_focus(xcb_window_t w, xcb_timestamp_t t);
  void honour_configure_request(xcb_configure_request_event_t *ev);

  void dnd_init();

  bool dnd_has_pending() const { return dnd_pending_; }

  bool dnd_take_pending_path(char *out, size_t outsz);

  void dnd_release_source_button(xcb_connection_t *connection);

  xcb_window_t dnd_catcher_window() const { return dnd_catcher_; }

  void drop_files(xcb_window_t target, POINT point, const WDL_FastString &uris, unsigned version);

  void (*on_unhandled_event)(xcb_generic_event_t *ev) = NULL;

private:
  void init_atoms();
  void set_wm_state(xcb_window_t w, int state);
  void read_wm_protocols(xcb_window_t w, WMWindowState &st);
  void read_wm_hints(xcb_window_t w, WMWindowState &st);
  void disallow_commits(xcb_window_t w);

  bool on_map_request(xcb_map_request_event_t *ev);
  bool on_create_notify(xcb_create_notify_event_t *ev);
  bool on_configure_request(xcb_configure_request_event_t *ev);
  bool on_reparent_notify(xcb_reparent_notify_event_t *ev);
  bool on_unmap_notify(xcb_unmap_notify_event_t *ev);
  bool on_destroy_notify(xcb_destroy_notify_event_t *ev);

  bool dnd_handle_event(xcb_generic_event_t *ev);
  void dnd_catcher_show(bool on);
  void dnd_send_status(xcb_window_t src, bool accept);
  void dnd_send_finished(xcb_window_t src, bool accepted);
  void dnd_reset_payload();
  bool drop_handle_event(xcb_generic_event_t *ev);
  void drop_finish();
  static gboolean drop_timeout(gpointer data);

  xcb_connection_t *connection_;
  xcb_window_t root_;
  WMAtoms atoms_ = {};
  xcb_window_t pending_focus_ = XCB_NONE;
  uint16_t focus_sequence_ = 0;
  WDL_KeyedArrayImpl<xcb_window_t, WMWindowState> states_;
  xcb_window_t support_win_ = XCB_NONE;
  xcb_window_t active_window_ = ~0U;

  xcb_window_t dnd_catcher_ = 0;
  int dnd_xfixes_evt_base_ = -1;
  xcb_window_t dnd_source_ = 0;
  bool dnd_requested_ = false;
  char dnd_uri_[8192] = {};
  bool dnd_have_uri_ = false;
  bool dnd_pending_ = false;
  xcb_window_t drop_source_ = XCB_NONE, drop_target_ = XCB_NONE;
  WDL_FastString drop_uris_;
  guint drop_timeout_ = 0;
  bool drop_sent_ = false;
};

XWaylandWM::XWaylandWM(xcb_connection_t *connection)
    : connection_(connection), root_(xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root)
{
  init_atoms();
}

static pid_t s_xwayland_pid = 0;
static char s_xwayland_display[sizeof(sockaddr_un::sun_path)];
static XWaylandWM *g_wm = NULL;

struct XEventSource
{
  GSource source;
  GPollFD poll;
  xcb_connection_t *connection;
  xcb_generic_event_t *event;
};

static gboolean xcb_event_prepare(GSource *source, gint *timeout)
{
  *timeout = -1;
  XEventSource *xcb_source = (XEventSource *)source;
  if (!xcb_source->event) xcb_source->event = xcb_poll_for_queued_event(xcb_source->connection);
  return xcb_source->event != NULL;
}

static gboolean xcb_event_check(GSource *source)
{
  XEventSource *xcb_source = (XEventSource *)source;
  if (!xcb_source->event) xcb_source->event = xcb_poll_for_event(xcb_source->connection);
  return xcb_source->event || (xcb_source->poll.revents & (G_IO_HUP | G_IO_ERR));
}

static gboolean xcb_event_dispatch(GSource *source, GSourceFunc, gpointer)
{
  XEventSource *xcb_source = (XEventSource *)source;
  const gint64 deadline = g_get_monotonic_time() + 2000;
  for (int n = 0; n < 64 && g_wm; ++n)
  {
    xcb_generic_event_t *ev = xcb_source->event;
    xcb_source->event = NULL;
    if (!ev) ev = xcb_poll_for_event(xcb_source->connection);
    if (!ev) break;
    g_wm->handle_event(ev);
    if (g_wm->on_unhandled_event) g_wm->on_unhandled_event(ev);
    free(ev);
    if (g_source_is_destroyed(source)) return G_SOURCE_REMOVE;
    if (g_get_monotonic_time() >= deadline) break;
  }
  if (!g_wm || xcb_connection_has_error(xcb_source->connection)) return G_SOURCE_REMOVE;
  xcb_flush(xcb_source->connection);
  return G_SOURCE_CONTINUE;
}

static void xcb_event_finalize(GSource *source)
{
  free(((XEventSource *)source)->event);
}

static GSource *watch_xcb_connection(xcb_connection_t *connection)
{
  static GSourceFuncs funcs = {
      xcb_event_prepare, xcb_event_check, xcb_event_dispatch, xcb_event_finalize, NULL, NULL};
  GSource *source = g_source_new(&funcs, sizeof(XEventSource));
  XEventSource *xcb_source = (XEventSource *)source;
  xcb_source->connection = connection;
  xcb_source->poll.fd = xcb_get_file_descriptor(connection);
  xcb_source->poll.events = G_IO_IN | G_IO_HUP | G_IO_ERR;
  g_source_add_poll(source, &xcb_source->poll);
  g_source_set_priority(source, G_PRIORITY_HIGH_IDLE + 1);
  g_source_attach(source, NULL);
  return source;
}

const char *XWaylandWM::display_name()
{
  return s_xwayland_display;
}

XWaylandWM *XWaylandWM::init_bridge_wm()
{
  if (s_xwayland_pid > 0) return g_wm;

  int wm_fd;
  s_xwayland_pid = xw_spawn_xwayland(s_xwayland_display, sizeof(s_xwayland_display), &wm_fd);
  if (s_xwayland_pid < 0) return NULL;

  xcb_connection_t *connection = xcb_connect_to_fd(wm_fd, NULL);
  if (xcb_connection_has_error(connection))
  {
    xcb_disconnect(connection);
    kill(s_xwayland_pid, SIGTERM);
    while (waitpid(s_xwayland_pid, NULL, 0) < 0 && errno == EINTR)
    {
    }
    s_xwayland_pid = 0;
    return NULL;
  }

  setenv("DISPLAY", s_xwayland_display, 1);
  free(xcb_composite_query_version_reply(connection, xcb_composite_query_version(connection, 0, 4), NULL));
  g_wm = new XWaylandWM(connection);

  xcb_window_t support = xcb_generate_id(connection);
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, support, g_wm->root_, -2, -2, 1, 1, 0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, 0, NULL);
  xcb_map_window(connection, support);
  g_wm->announce_wm(support);
  g_wm->disallow_commits(support);

  const xcb_font_t font = xcb_generate_id(connection);
  const xcb_cursor_t cursor = xcb_generate_id(connection);
  xcb_open_font(connection, font, 6, "cursor");
  xcb_create_glyph_cursor(connection, cursor, font, font, XC_left_ptr, XC_left_ptr + 1,
      0, 0, 0, 65535, 65535, 65535);
  xcb_change_window_attributes(connection, g_wm->root_, XCB_CW_CURSOR, &cursor);
  xcb_free_cursor(connection, cursor);
  xcb_close_font(connection, font);

  g_source_unref(watch_xcb_connection(connection));

  DEBUG_PRINT("[WM] initialised\n");
  return g_wm;
}

void XWaylandWM::init_atoms()
{
  xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(connection_, &atoms_);
  struct
  {
    const char *name;
    xcb_atom_t *atom;
  } extra[] = {{"WM_DELETE_WINDOW", &atoms_.WM_DELETE_WINDOW}, {"WM_TAKE_FOCUS", &atoms_.WM_TAKE_FOCUS},
      {"WM_STATE", &atoms_.WM_STATE}, {"WM_S0", &atoms_.WM_S0},
      {"_XWAYLAND_ALLOW_COMMITS", &atoms_._XWAYLAND_ALLOW_COMMITS},
      {"_REAPERX_TIMESTAMP", &atoms_._REAPERX_TIMESTAMP}, {"_XEMBED_INFO", &atoms_._XEMBED_INFO},
      {"XdndSelection", &atoms_.XdndSelection}, {"XdndAware", &atoms_.XdndAware},
      {"XdndEnter", &atoms_.XdndEnter}, {"XdndPosition", &atoms_.XdndPosition},
      {"XdndStatus", &atoms_.XdndStatus}, {"XdndDrop", &atoms_.XdndDrop},
      {"XdndFinished", &atoms_.XdndFinished}, {"XdndLeave", &atoms_.XdndLeave},
      {"XdndActionCopy", &atoms_.XdndActionCopy}, {"text/uri-list", &atoms_.uri_list},
      {"XWB_DND_DATA", &atoms_.dnd_data}, {"TARGETS", &atoms_.targets},
      {"_MOTIF_WM_HINTS", &atoms_.motif_hints}};
  xcb_intern_atom_cookie_t requests[sizeof(extra) / sizeof(extra[0])];
  for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
    requests[i] = xcb_intern_atom(connection_, false, strlen(extra[i].name), extra[i].name);
  xcb_ewmh_init_atoms_replies(&atoms_, cookies, NULL);
  for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); ++i)
  {
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection_, requests[i], NULL);
    if (reply) *extra[i].atom = reply->atom;
    free(reply);
  }
}

void XWaylandWM::disallow_commits(xcb_window_t w)
{
  const uint32_t zero = 0;
  xcb_change_property(
      connection_, XCB_PROP_MODE_REPLACE, w, atoms_._XWAYLAND_ALLOW_COMMITS, XCB_ATOM_CARDINAL, 32, 1, &zero);
}

void XWaylandWM::announce_wm(xcb_window_t support_win)
{
  support_win_ = support_win;
  set_active_window(XCB_NONE);
  xcb_ewmh_set_supporting_wm_check(&atoms_, root_, support_win);
  xcb_ewmh_set_supporting_wm_check(&atoms_, support_win, support_win);
  const char *name = "REAPERXBridge";
  xcb_ewmh_set_wm_name(&atoms_, support_win, strlen(name), name);
  xcb_atom_t supported[] = {atoms_._NET_WM_STATE, atoms_._NET_WM_STATE_MODAL, atoms_._NET_WM_STATE_ABOVE,
      atoms_._NET_ACTIVE_WINDOW, atoms_._NET_WM_WINDOW_TYPE, atoms_._NET_WM_WINDOW_TYPE_DIALOG,
      atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU, atoms_._NET_WM_WINDOW_TYPE_NORMAL,
      atoms_._NET_SUPPORTING_WM_CHECK, atoms_._NET_WM_NAME};
  xcb_ewmh_set_supported(&atoms_, 0, sizeof(supported) / sizeof(supported[0]), supported);
  const uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY |
                        XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes(connection_, root_, XCB_CW_EVENT_MASK, &mask);
  const uint32_t support_mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_change_window_attributes(connection_, support_win, XCB_CW_EVENT_MASK, &support_mask);
  xcb_composite_redirect_subwindows(connection_, root_, XCB_COMPOSITE_REDIRECT_MANUAL);
  xcb_set_selection_owner(connection_, support_win, atoms_.WM_S0, XCB_CURRENT_TIME);
  xcb_flush(connection_);
}

void XWaylandWM::set_active_window(xcb_window_t window)
{
  if (active_window_ == window) return;
  active_window_ = window;
  xcb_ewmh_set_active_window(&atoms_, 0, window);
  xcb_flush(connection_);
}

void XWaylandWM::track_window(xcb_window_t w)
{
  if (states_.Exists(w)) return;

  WMWindowState st{};
  st.input_hint = true;

  xcb_get_window_attributes_cookie_t attributes_request = xcb_get_window_attributes(connection_, w);
  read_wm_protocols(w, st);
  read_wm_hints(w, st);

  uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  xcb_get_window_attributes_reply_t *attributes =
      xcb_get_window_attributes_reply(connection_, attributes_request, NULL);
  if (attributes)
  {
    mask |= attributes->your_event_mask;
    free(attributes);
  }
  xcb_change_window_attributes(connection_, w, XCB_CW_EVENT_MASK, &mask);

  states_.Insert(w, st);
}

void XWaylandWM::untrack_window(xcb_window_t w)
{
  states_.Delete(w);
  if (pending_focus_ == w) pending_focus_ = XCB_NONE;
}

bool XWaylandWM::handle_event(xcb_generic_event_t *ev)
{
  if (drop_handle_event(ev)) return true;
  if (dnd_handle_event(ev)) return true;
  switch (ev->response_type & 0x7f)
  {
    case XCB_CREATE_NOTIFY:
      return on_create_notify((xcb_create_notify_event_t *)ev);
    case XCB_MAP_REQUEST:
      return on_map_request((xcb_map_request_event_t *)ev);
    case XCB_CONFIGURE_REQUEST:
      return on_configure_request((xcb_configure_request_event_t *)ev);
    case XCB_REPARENT_NOTIFY:
      return on_reparent_notify((xcb_reparent_notify_event_t *)ev);
    case XCB_UNMAP_NOTIFY:
      return on_unmap_notify((xcb_unmap_notify_event_t *)ev);
    case XCB_DESTROY_NOTIFY:
      return on_destroy_notify((xcb_destroy_notify_event_t *)ev);
    case XCB_PROPERTY_NOTIFY:
    {
      xcb_property_notify_event_t *property = (xcb_property_notify_event_t *)ev;
      if (property->window == support_win_ && property->atom == atoms_._REAPERX_TIMESTAMP)
      {
        if (property->sequence != focus_sequence_) return true;
        WMWindowState *state = states_.GetPtr(pending_focus_);
        if (state != NULL)
        {
          if (state->input_hint)
            xcb_set_input_focus(connection_, XCB_INPUT_FOCUS_PARENT, pending_focus_, property->time);
          if (state->take_focus) send_take_focus(pending_focus_, property->time);
        }
        pending_focus_ = XCB_NONE;
        return true;
      }
      WMWindowState *state = states_.GetPtr(property->window);
      if (state != NULL)
      {
        if (property->atom == atoms_.WM_PROTOCOLS) read_wm_protocols(property->window, *state);
        if (property->atom == XCB_ATOM_WM_HINTS) read_wm_hints(property->window, *state);
      }
      break;
    }
  }
  return false;
}

bool XWaylandWM::on_create_notify(xcb_create_notify_event_t *ev)
{
  if (ev->parent == root_) disallow_commits(ev->window);
  return false;
}

bool XWaylandWM::on_map_request(xcb_map_request_event_t *ev)
{
  track_window(ev->window);
  xcb_map_window(connection_, ev->window);
  set_wm_state(ev->window, WM_STATE_NORMAL);
  const WMWindowState *state = states_.GetPtr(ev->window);
  if (state && (state->input_hint || state->take_focus))
  {
    pending_focus_ = ev->window;
    focus_sequence_ = xcb_change_property(connection_, XCB_PROP_MODE_APPEND, support_win_,
        atoms_._REAPERX_TIMESTAMP, XCB_ATOM_ATOM, 32, 0, NULL)
                          .sequence;
  }
  return true;
}

bool XWaylandWM::on_configure_request(xcb_configure_request_event_t *ev)
{
  honour_configure_request(ev);
  return true;
}

void XWaylandWM::honour_configure_request(xcb_configure_request_event_t *ev)
{
  xcb_configure_window_value_list_t values = {};
  values.x = ev->x;
  values.y = ev->y;
  values.width = ev->width;
  values.height = ev->height;
  values.border_width = ev->border_width;
  values.sibling = ev->sibling;
  values.stack_mode = ev->stack_mode;
  uint16_t mask = ev->value_mask;
  if (!(mask & (XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y)))
  {
    xcb_size_hints_t hints = {};
    if (xcb_icccm_get_wm_normal_hints_reply(
            connection_, xcb_icccm_get_wm_normal_hints(connection_, ev->window), &hints, NULL) &&
        (hints.flags & (XCB_ICCCM_SIZE_HINT_P_POSITION | XCB_ICCCM_SIZE_HINT_US_POSITION)))
    {
      values.x = hints.x;
      values.y = hints.y;
      mask |= XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y;
    }
  }
  if (mask) xcb_configure_window_aux(connection_, ev->window, mask, &values);
  xcb_get_geometry_reply_t *geometry =
      xcb_get_geometry_reply(connection_, xcb_get_geometry(connection_, ev->window), NULL);
  if (geometry)
  {
    xcb_configure_notify_event_t event = {};
    event.response_type = XCB_CONFIGURE_NOTIFY;
    event.event = event.window = ev->window;
    event.x = geometry->x;
    event.y = geometry->y;
    event.width = geometry->width;
    event.height = geometry->height;
    event.border_width = geometry->border_width;
    xcb_send_event(connection_, false, ev->window, XCB_EVENT_MASK_STRUCTURE_NOTIFY, (const char *)&event);
    free(geometry);
  }
  xcb_flush(connection_);
}

bool XWaylandWM::on_reparent_notify(xcb_reparent_notify_event_t *ev)
{
  if (ev->parent == root_)
  {
    disallow_commits(ev->window);
    return false;
  }

  return false;
}

bool XWaylandWM::on_unmap_notify(xcb_unmap_notify_event_t *ev)
{
  WMWindowState *state = states_.GetPtr(ev->window);
  if (!state) return false;

  set_wm_state(ev->window, WM_STATE_WITHDRAWN);
  if (pending_focus_ == ev->window) pending_focus_ = XCB_NONE;

  return false;
}

bool XWaylandWM::on_destroy_notify(xcb_destroy_notify_event_t *ev)
{
  untrack_window(ev->window);
  return false;
}

void XWaylandWM::send_take_focus(xcb_window_t w, xcb_timestamp_t t)
{
  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.window = w;
  event.type = atoms_.WM_PROTOCOLS;
  event.format = 32;
  event.data.data32[0] = atoms_.WM_TAKE_FOCUS;
  event.data.data32[1] = t;
  xcb_send_event(connection_, false, w, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
}

void XWaylandWM::set_wm_state(xcb_window_t w, int state)
{
  const uint32_t data[] = {(uint32_t)state, XCB_NONE};
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, w, atoms_.WM_STATE, atoms_.WM_STATE, 32, 2, data);
}

void XWaylandWM::read_wm_protocols(xcb_window_t w, WMWindowState &st)
{
  st.take_focus = false;
  xcb_icccm_get_wm_protocols_reply_t protocols;
  if (!xcb_icccm_get_wm_protocols_reply(
          connection_, xcb_icccm_get_wm_protocols(connection_, w, atoms_.WM_PROTOCOLS), &protocols, NULL))
    return;
  for (uint32_t i = 0; i < protocols.atoms_len; ++i)
    if (protocols.atoms[i] == atoms_.WM_TAKE_FOCUS) st.take_focus = true;
  xcb_icccm_get_wm_protocols_reply_wipe(&protocols);
}

void XWaylandWM::read_wm_hints(xcb_window_t w, WMWindowState &st)
{
  xcb_icccm_wm_hints_t hints;
  st.input_hint = true;
  if (xcb_icccm_get_wm_hints_reply(connection_, xcb_icccm_get_wm_hints(connection_, w), &hints, NULL) &&
      (hints.flags & XCB_ICCCM_WM_HINT_INPUT))
    st.input_hint = hints.input;
}

static uint32_t client_pid(xcb_connection_t *connection, xcb_window_t window)
{
  const xcb_res_client_id_spec_t spec = {window, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
  xcb_res_query_client_ids_reply_t *reply =
      xcb_res_query_client_ids_reply(connection, xcb_res_query_client_ids(connection, 1, &spec), NULL);
  if (!reply) return 0;
  uint32_t pid = 0;
  for (xcb_res_client_id_value_iterator_t it = xcb_res_query_client_ids_ids_iterator(reply); it.rem;
       xcb_res_client_id_value_next(&it))
  {
    if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
        xcb_res_client_id_value_value_length(it.data))
    {
      pid = *xcb_res_client_id_value_value(it.data);
      break;
    }
  }
  free(reply);
  return pid;
}

bool XWaylandWM::owns_window(xcb_window_t window, xcb_window_t gui, bool include_child_processes)
{
  if (!window || !gui) return false;
  uint32_t window_pid = 0, gui_pid = 0;
  xcb_get_property_cookie_t a = xcb_ewmh_get_wm_pid(&atoms_, window);
  xcb_get_property_cookie_t b = xcb_ewmh_get_wm_pid(&atoms_, gui);
  xcb_ewmh_get_wm_pid_reply(&atoms_, a, &window_pid, NULL);
  xcb_ewmh_get_wm_pid_reply(&atoms_, b, &gui_pid, NULL);
  const uint32_t client_mask = ~xcb_get_setup(connection_)->resource_id_mask;
  if ((!window_pid || !gui_pid) && (window & client_mask) == (gui & client_mask)) return true;
  if (!window_pid) window_pid = client_pid(connection_, window);
  if (!gui_pid) gui_pid = client_pid(connection_, gui);
  if (window_pid && window_pid == gui_pid) return true;
  if (!include_child_processes) return false;
  for (int depth = 0; window_pid > 1 && gui_pid > 1 && depth < 64; ++depth)
  {
    char path[64], buf[512];
    snprintf(path, sizeof(path), "/proc/%u/stat", window_pid);
    FILE *fp = fopen(path, "r");
    if (!fp) break;
    const size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[len] = 0;
    const char *p = strrchr(buf, ')');
    if (!p || sscanf(p + 1, " %*c %u", &window_pid) != 1) break;
    if (window_pid == gui_pid) return true;
  }
  return false;
}

XWaylandWM::WindowType XWaylandWM::window_type(xcb_window_t window, bool override_redirect)
{
  xcb_ewmh_get_atoms_reply_t types;
  xcb_atom_t type = XCB_NONE;
  if (xcb_ewmh_get_wm_window_type_reply(
          &atoms_, xcb_ewmh_get_wm_window_type(&atoms_, window), &types, NULL))
  {
    for (uint32_t i = 0; i < types.atoms_len; ++i)
    {
      const xcb_atom_t candidate = types.atoms[i];
      if (candidate == atoms_._NET_WM_WINDOW_TYPE_DND)
      {
        type = candidate;
        break;
      }
      if (!type &&
          (candidate == atoms_._NET_WM_WINDOW_TYPE_NORMAL || candidate == atoms_._NET_WM_WINDOW_TYPE_DIALOG ||
              candidate == atoms_._NET_WM_WINDOW_TYPE_UTILITY ||
              candidate == atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU ||
              candidate == atoms_._NET_WM_WINDOW_TYPE_MENU ||
              candidate == atoms_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU ||
              candidate == atoms_._NET_WM_WINDOW_TYPE_TOOLTIP))
        type = candidate;
    }
    xcb_ewmh_get_atoms_reply_wipe(&types);
  }
  if (type == atoms_._NET_WM_WINDOW_TYPE_DND) return DRAG_ICON;
  if (override_redirect) return POPUP;
  if (type == atoms_._NET_WM_WINDOW_TYPE_POPUP_MENU || type == atoms_._NET_WM_WINDOW_TYPE_MENU ||
      type == atoms_._NET_WM_WINDOW_TYPE_DROPDOWN_MENU || type == atoms_._NET_WM_WINDOW_TYPE_TOOLTIP)
    return POPUP;
  if (type && type != atoms_._NET_WM_WINDOW_TYPE_NORMAL) return NORMAL;
  return window_decorated(window) ? NORMAL : POPUP;
}

bool XWaylandWM::window_decorated(xcb_window_t window)
{
  xcb_get_property_reply_t *motif = xcb_get_property_reply(connection_,
      xcb_get_property(connection_, false, window, atoms_.motif_hints, atoms_.motif_hints, 0, 5), NULL);
  bool undecorated = false;
  if (motif && motif->format == 32 && motif->value_len >= 3)
  {
    const uint32_t *values = (const uint32_t *)xcb_get_property_value(motif);
    undecorated = (values[0] & (1U << 1)) && !values[2];
  }
  free(motif);
  return !undecorated;
}

void XWaylandWM::dnd_init()
{
  if (dnd_catcher_) return;
  const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection_, &xcb_xfixes_id);
  if (!extension || !extension->present) return;
  xcb_xfixes_query_version_reply_t *version =
      xcb_xfixes_query_version_reply(connection_, xcb_xfixes_query_version(connection_, 5, 0), NULL);
  if (!version) return;
  free(version);
  dnd_xfixes_evt_base_ = extension->first_event;
  xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(connection_)).data;
  dnd_catcher_ = xcb_generate_id(connection_);
  const uint32_t override_redirect = true;
  xcb_create_window(connection_, XCB_COPY_FROM_PARENT, dnd_catcher_, root_, 0, 0, screen->width_in_pixels,
      screen->height_in_pixels, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
      XCB_CW_OVERRIDE_REDIRECT, &override_redirect);
  const uint32_t ver = 5;
  xcb_change_property(
      connection_, XCB_PROP_MODE_REPLACE, dnd_catcher_, atoms_.XdndAware, XCB_ATOM_ATOM, 32, 1, &ver);
  xcb_xfixes_select_selection_input(connection_, root_, atoms_.XdndSelection,
      XCB_XFIXES_SELECTION_EVENT_MASK_SET_SELECTION_OWNER |
          XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_WINDOW_DESTROY |
          XCB_XFIXES_SELECTION_EVENT_MASK_SELECTION_CLIENT_CLOSE);
  xcb_flush(connection_);
}

void XWaylandWM::dnd_catcher_show(bool on)
{
  if (!dnd_catcher_) return;
  if (on)
  {
    const uint32_t above = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(connection_, dnd_catcher_, XCB_CONFIG_WINDOW_STACK_MODE, &above);
    xcb_map_window(connection_, dnd_catcher_);
  }
  else
  {
    xcb_unmap_window(connection_, dnd_catcher_);
    dnd_source_ = XCB_NONE;
  }
  xcb_flush(connection_);
}

void XWaylandWM::dnd_send_status(xcb_window_t src, bool accepted)
{
  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.window = src;
  event.type = atoms_.XdndStatus;
  event.format = 32;
  event.data.data32[0] = dnd_catcher_;
  event.data.data32[1] = accepted ? 1 : 0;
  event.data.data32[4] = accepted ? atoms_.XdndActionCopy : XCB_NONE;
  xcb_send_event(connection_, false, src, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
  xcb_flush(connection_);
}

void XWaylandWM::dnd_send_finished(xcb_window_t src, bool accepted)
{
  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.window = src;
  event.type = atoms_.XdndFinished;
  event.format = 32;
  event.data.data32[0] = dnd_catcher_;
  event.data.data32[1] = accepted ? 1 : 0;
  event.data.data32[2] = accepted ? atoms_.XdndActionCopy : XCB_NONE;
  xcb_send_event(connection_, false, src, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
  xcb_flush(connection_);
}

void XWaylandWM::dnd_reset_payload()
{
  dnd_requested_ = false;
  dnd_have_uri_ = false;
  dnd_pending_ = false;
  dnd_uri_[0] = 0;
}

bool XWaylandWM::dnd_handle_event(xcb_generic_event_t *ev)
{
  if (!dnd_catcher_ || !connection_) return false;

  if (dnd_xfixes_evt_base_ >= 0 &&
      (ev->response_type & 0x7f) == dnd_xfixes_evt_base_ + XCB_XFIXES_SELECTION_NOTIFY)
  {
    xcb_xfixes_selection_notify_event_t *se = (xcb_xfixes_selection_notify_event_t *)ev;
    if (se->selection == atoms_.XdndSelection)
    {
      dnd_reset_payload();
      dnd_catcher_show(se->owner != XCB_NONE && se->owner != drop_source_);
    }
    return true;
  }

  if ((ev->response_type & 0x7f) == XCB_CLIENT_MESSAGE &&
      ((xcb_client_message_event_t *)ev)->window == dnd_catcher_)
  {
    const xcb_atom_t mt = ((xcb_client_message_event_t *)ev)->type;
    const xcb_window_t src = (xcb_window_t)((xcb_client_message_event_t *)ev)->data.data32[0];

    if (mt == atoms_.XdndEnter)
    {
      dnd_source_ = src;
      return true;
    }
    if (mt == atoms_.XdndPosition)
    {
      dnd_send_status(src, true);

      if (!dnd_requested_)
      {
        dnd_requested_ = true;
        dnd_source_ = src;
        xcb_convert_selection(connection_, dnd_catcher_, atoms_.XdndSelection, atoms_.uri_list,
            atoms_.dnd_data, XCB_CURRENT_TIME);
        xcb_flush(connection_);
      }
      return true;
    }
    if (mt == atoms_.XdndLeave)
    {
      dnd_catcher_show(false);
      dnd_reset_payload();
      return true;
    }
    if (mt == atoms_.XdndDrop)
    {
      dnd_send_finished(src, dnd_have_uri_ && !dnd_pending_);
      dnd_catcher_show(false);
      dnd_reset_payload();
      return true;
    }
    return true;
  }

  if ((ev->response_type & 0x7f) == XCB_SELECTION_NOTIFY &&
      ((xcb_selection_notify_event_t *)ev)->requestor == dnd_catcher_)
  {
    if (!dnd_source_ || !dnd_requested_) return true;
    if (((xcb_selection_notify_event_t *)ev)->property != XCB_NONE)
    {
      xcb_get_property_reply_t *reply = xcb_get_property_reply(connection_,
          xcb_get_property(
              connection_, true, dnd_catcher_, atoms_.dnd_data, atoms_.uri_list, 0, sizeof(dnd_uri_) / 4),
          NULL);
      if (reply && reply->format == 8 && !reply->bytes_after)
      {
        size_t len = xcb_get_property_value_length(reply);
        if (len < sizeof(dnd_uri_))
        {
          memcpy(dnd_uri_, xcb_get_property_value(reply), len);
          dnd_uri_[len] = 0;
          dnd_have_uri_ = dnd_pending_ = true;
        }
      }
      free(reply);
    }
    return true;
  }

  return false;
}

bool XWaylandWM::dnd_take_pending_path(char *out, size_t outsz)
{
  if (out && outsz) out[0] = 0;
  if (!dnd_pending_) return false;
  dnd_pending_ = false;
  if (!out || !outsz) return false;
  swell_wayland_first_uri_path(dnd_uri_, strlen(dnd_uri_), out, outsz);
  dnd_have_uri_ = out[0] != 0;
  return dnd_have_uri_;
}

void XWaylandWM::dnd_release_source_button(xcb_connection_t *connection)
{
  if (!connection) return;
  xcb_query_pointer_reply_t *pointer =
      xcb_query_pointer_reply(connection, xcb_query_pointer(connection, root_), NULL);
  if (pointer)
  {
    xcb_test_fake_input(
        connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, root_, pointer->root_x, pointer->root_y, 0);
    free(pointer);
  }
  xcb_test_fake_input(connection, XCB_BUTTON_RELEASE, 1, XCB_CURRENT_TIME, root_, 0, 0, 0);
  free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), NULL));
}

void XWaylandWM::drop_finish()
{
  if (drop_source_)
  {
    if (!drop_sent_ && drop_target_)
    {
      xcb_client_message_event_t event = {};
      event.response_type = XCB_CLIENT_MESSAGE;
      event.format = 32;
      event.window = drop_target_;
      event.type = atoms_.XdndLeave;
      event.data.data32[0] = drop_source_;
      xcb_send_event(connection_, false, drop_target_, 0, (const char *)&event);
    }
    xcb_destroy_window(connection_, drop_source_);
    xcb_flush(connection_);
  }
  if (drop_timeout_) g_source_remove(drop_timeout_);
  drop_timeout_ = 0;
  drop_source_ = drop_target_ = XCB_NONE;
  drop_uris_.Set("");
  drop_sent_ = false;
}

gboolean XWaylandWM::drop_timeout(gpointer data)
{
  XWaylandWM *wm = (XWaylandWM *)data;
  wm->drop_timeout_ = 0;
  wm->drop_finish();
  return G_SOURCE_REMOVE;
}

void XWaylandWM::drop_files(xcb_window_t target, POINT point, const WDL_FastString &uris, unsigned version)
{
  drop_finish();
  if (!target || !uris.GetLength() || (uint64_t)uris.GetLength() > xcb_get_maximum_request_length(connection_) * 4ULL - 24)
    return;
  drop_source_ = xcb_generate_id(connection_);
  drop_target_ = target;
  drop_uris_ = uris;
  xcb_create_window(connection_, XCB_COPY_FROM_PARENT, drop_source_, root_, 0, 0, 1, 1, 0,
      XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, NULL);
  const uint32_t supported = 5;
  xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, drop_source_, atoms_.XdndAware,
      XCB_ATOM_ATOM, 32, 1, &supported);
  xcb_set_selection_owner(connection_, drop_source_, atoms_.XdndSelection, XCB_CURRENT_TIME);
  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.format = 32;
  event.window = target;
  event.type = atoms_.XdndEnter;
  event.data.data32[0] = drop_source_;
  event.data.data32[1] = min(version, supported) << 24;
  event.data.data32[2] = atoms_.uri_list;
  xcb_send_event(connection_, false, target, 0, (const char *)&event);
  event.type = atoms_.XdndPosition;
  event.data.data32[1] = 0;
  event.data.data32[2] = ((uint32_t)point.x << 16) | (point.y & 0xffff);
  event.data.data32[3] = XCB_CURRENT_TIME;
  event.data.data32[4] = atoms_.XdndActionCopy;
  xcb_send_event(connection_, false, target, 0, (const char *)&event);
  xcb_flush(connection_);
  drop_timeout_ = g_timeout_add_seconds(10, drop_timeout, this);
}

bool XWaylandWM::drop_handle_event(xcb_generic_event_t *ev)
{
  if (!drop_source_) return false;
  const uint8_t type = ev->response_type & 0x7f;
  if (type == XCB_SELECTION_REQUEST)
  {
    const auto *request = (xcb_selection_request_event_t *)ev;
    if (request->owner != drop_source_ || request->selection != atoms_.XdndSelection) return false;
    xcb_selection_notify_event_t reply = {};
    reply.response_type = XCB_SELECTION_NOTIFY;
    reply.requestor = request->requestor;
    reply.selection = request->selection;
    reply.target = request->target;
    reply.time = request->time;
    const xcb_atom_t property = request->property ? request->property : request->target;
    if (request->target == atoms_.uri_list)
    {
      xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request->requestor, property,
          atoms_.uri_list, 8, drop_uris_.GetLength(), drop_uris_.Get());
      reply.property = property;
    }
    else if (request->target == atoms_.targets)
    {
      const xcb_atom_t targets[] = {atoms_.targets, atoms_.uri_list};
      xcb_change_property(connection_, XCB_PROP_MODE_REPLACE, request->requestor, property,
          XCB_ATOM_ATOM, 32, 2, targets);
      reply.property = property;
    }
    xcb_send_event(connection_, false, request->requestor, 0, (const char *)&reply);
    return true;
  }
  if (type == XCB_SELECTION_CLEAR && ((xcb_selection_clear_event_t *)ev)->owner == drop_source_)
  {
    drop_finish();
    return true;
  }
  if (type == XCB_DESTROY_NOTIFY && ((xcb_destroy_notify_event_t *)ev)->window == drop_target_)
  {
    drop_target_ = XCB_NONE;
    drop_finish();
    return false;
  }
  if (type != XCB_CLIENT_MESSAGE) return false;
  const auto *event = (xcb_client_message_event_t *)ev;
  if (event->window != drop_source_ || event->data.data32[0] != drop_target_) return false;
  if (event->type == atoms_.XdndStatus && !drop_sent_)
  {
    if (!(event->data.data32[1] & 1))
      drop_finish();
    else
    {
      xcb_client_message_event_t reply = {};
      reply.response_type = XCB_CLIENT_MESSAGE;
      reply.format = 32;
      reply.window = drop_target_;
      reply.type = atoms_.XdndDrop;
      reply.data.data32[0] = drop_source_;
      reply.data.data32[2] = XCB_CURRENT_TIME;
      xcb_send_event(connection_, false, drop_target_, 0, (const char *)&reply);
      drop_sent_ = true;
    }
    return true;
  }
  if (event->type == atoms_.XdndFinished)
  {
    drop_finish();
    return true;
  }
  return false;
}

class CapturePixmap
{
public:
  xcb_connection_t *connection = NULL;
  xcb_screen_t *screen = NULL;
  xcb_pixmap_t pixmap = 0;
  int width = 0, height = 0;
  xcb_render_pictforminfo_t format = {};
  cairo_format_t image_format = CAIRO_FORMAT_INVALID;

  CapturePixmap() = default;
  CapturePixmap(const CapturePixmap &) = delete;
  CapturePixmap &operator=(const CapturePixmap &) = delete;
  ~CapturePixmap() { reset(); }
  void reset();
  bool update(xcb_connection_t *conn, xcb_window_t window);
  cairo_surface_t *surface();
};

class CaptureImage
{
  xcb_connection_t *connection = NULL;
  xcb_image_t *image = NULL;
  xcb_shm_segment_info_t shm = {};
  cairo_surface_t *surface = NULL;
  bool attached = false, disabled = false;

  bool create(const CapturePixmap &pixmap);

public:
  ~CaptureImage() { reset(); }
  void reset();
  cairo_surface_t *read(const CapturePixmap &pixmap, bool refresh);
};

void CapturePixmap::reset()
{
  if (pixmap) xcb_free_pixmap(connection, pixmap);
  pixmap = 0;
  width = height = 0;
  connection = NULL;
  screen = NULL;
  format = {};
  image_format = CAIRO_FORMAT_INVALID;
}

bool CapturePixmap::update(xcb_connection_t *conn, xcb_window_t window)
{
  if (!conn || xcb_connection_has_error(conn)) return false;
  if (connection != conn) reset();
  connection = conn;
  const xcb_pixmap_t next = xcb_generate_id(conn);
  xcb_composite_name_window_pixmap(conn, window, next);
  xcb_get_geometry_cookie_t geometry_cookie = xcb_get_geometry(conn, next);
  xcb_get_window_attributes_cookie_t attributes_cookie = {};
  if (!format.id) attributes_cookie = xcb_get_window_attributes(conn, window);
  xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(conn, geometry_cookie, NULL);
  if (!format.id)
  {
    xcb_get_window_attributes_reply_t *attributes =
        xcb_get_window_attributes_reply(conn, attributes_cookie, NULL);
    const xcb_render_query_pict_formats_reply_t *formats = xcb_render_util_query_formats(conn);
    xcb_render_pictvisual_t *visual =
        attributes && formats ? xcb_render_util_find_visual_format(formats, attributes->visual) : NULL;
    if (visual)
    {
      xcb_render_pictforminfo_t match = {};
      match.id = visual->format;
      xcb_render_pictforminfo_t *info = xcb_render_util_find_format(formats, XCB_PICT_FORMAT_ID, &match, 0);
      if (info) format = *info;
    }
    free(attributes);
    if (format.type == XCB_RENDER_PICT_TYPE_DIRECT && (format.depth == 24 || format.depth == 32) &&
        format.direct.red_shift == 16 && format.direct.red_mask == 0xff && format.direct.green_shift == 8 &&
        format.direct.green_mask == 0xff && format.direct.blue_shift == 0 &&
        format.direct.blue_mask == 0xff &&
        (!format.direct.alpha_mask || (format.direct.alpha_shift == 24 && format.direct.alpha_mask == 0xff)))
      image_format = format.direct.alpha_mask ? CAIRO_FORMAT_ARGB32 : CAIRO_FORMAT_RGB24;
  }
  if (!geometry || !format.id || geometry->depth != format.depth)
  {
    free(geometry);
    xcb_free_pixmap(conn, next);
    return false;
  }
  for (xcb_screen_iterator_t i = xcb_setup_roots_iterator(xcb_get_setup(conn)); i.rem; xcb_screen_next(&i))
    if (i.data->root == geometry->root)
    {
      screen = i.data;
      break;
    }
  if (pixmap) xcb_free_pixmap(conn, pixmap);
  pixmap = next;
  width = geometry->width;
  height = geometry->height;
  free(geometry);
  return screen != NULL;
}

cairo_surface_t *CapturePixmap::surface()
{
  return cairo_xcb_surface_create_with_xrender_format(connection, screen, pixmap, &format, width, height);
}

bool CaptureImage::create(const CapturePixmap &pixmap)
{
  connection = pixmap.connection;
  if (pixmap.image_format == CAIRO_FORMAT_INVALID || pixmap.width <= 0 || pixmap.height <= 0) return false;
  const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection, &xcb_shm_id);
  if (!extension || !extension->present) return false;
  xcb_shm_query_version_reply_t *version =
      xcb_shm_query_version_reply(connection, xcb_shm_query_version(connection), NULL);
  const bool supported =
      version && (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2));
  free(version);
  if (!supported) return false;
  image = xcb_image_create_native(connection, pixmap.width, pixmap.height, XCB_IMAGE_FORMAT_Z_PIXMAP,
      pixmap.format.depth, NULL, ~0U, NULL);
  const unsigned int endian = 1;
  if (!image || image->bpp != 32 || !image->stride || image->stride > INT_MAX ||
      (size_t)image->height > SIZE_MAX / image->stride ||
      image->size != (size_t)image->height * image->stride ||
      image->byte_order != (*(const char *)&endian ? XCB_IMAGE_ORDER_LSB_FIRST : XCB_IMAGE_ORDER_MSB_FIRST))
    return false;
  int fd = memfd_create("swell-plugin", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  if (fd < 0) return false;
  if (ftruncate(fd, image->size) < 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0)
  {
    close(fd);
    return false;
  }
  void *mapping = mmap(NULL, image->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED)
  {
    close(fd);
    return false;
  }
  image->data = (uint8_t *)mapping;
  shm.shmaddr = image->data;
  shm.shmseg = xcb_generate_id(connection);
  xcb_generic_error_t *error =
      xcb_request_check(connection, xcb_shm_attach_fd_checked(connection, shm.shmseg, fd, false));
  attached = !error && !xcb_connection_has_error(connection);
  free(error);
  if (!attached) return false;
  surface = cairo_image_surface_create_for_data(
      image->data, pixmap.image_format, image->width, image->height, image->stride);
  return cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
}

void CaptureImage::reset()
{
  if (surface) cairo_surface_destroy(surface);
  surface = NULL;
  if (attached) xcb_shm_detach(connection, shm.shmseg);
  attached = false;
  if (image)
  {
    if (image->data) munmap(image->data, image->size);
    xcb_image_destroy(image);
  }
  image = NULL;
  shm = {};
  connection = NULL;
}

cairo_surface_t *CaptureImage::read(const CapturePixmap &pixmap, bool refresh)
{
  if (disabled) return NULL;
  if (image &&
      (connection != pixmap.connection || image->width != pixmap.width || image->height != pixmap.height))
    reset();
  if (image && !refresh) return cairo_surface_reference(surface);
  if (!image && !create(pixmap))
  {
    reset();
    disabled = true;
    return NULL;
  }
  cairo_surface_flush(surface);
  if (!xcb_image_shm_get(connection, pixmap.pixmap, image, shm, 0, 0, ~0U))
  {
    reset();
    disabled = true;
    return NULL;
  }
  cairo_surface_mark_dirty(surface);
  return cairo_surface_reference(surface);
}

class CaptureGL
{
  xcb_connection_t *connection = NULL;
  xcb_sync_fence_t fence = 0;
  EGLContext context = EGL_NO_CONTEXT;
  GLuint texture = 0, framebuffer = 0;
  bool disabled = false;

public:
  ~CaptureGL() { reset(); }
  void reset();
  bool draw(const CapturePixmap &pixmap, int width, int height, int x, int y);
};

struct CaptureDmaBuf
{
  xcb_dri3_buffers_from_pixmap_reply_t *reply = NULL;
  int *fds = NULL;

  ~CaptureDmaBuf()
  {
    if (fds)
      for (int i = 0; i < reply->nfd; ++i) close(fds[i]);
    free(reply);
  }

  bool get(xcb_connection_t *connection, xcb_pixmap_t pixmap, xcb_sync_fence_t &fence)
  {
    if (!connection || xcb_connection_has_error(connection)) return false;
    xcb_generic_error_t *error = NULL;
    if (!fence)
    {
      const xcb_query_extension_reply_t *extension = xcb_get_extension_data(connection, &xcb_dri3_id);
      if (!extension || !extension->present) return false;
      xcb_dri3_query_version_reply_t *version =
          xcb_dri3_query_version_reply(connection, xcb_dri3_query_version(connection, 1, 2), &error);
      bool supported =
          !error && version &&
          (version->major_version > 1 || (version->major_version == 1 && version->minor_version >= 2));
      free(version);
      free(error);
      if (!supported) return false;
      xcb_sync_fence_t id = xcb_generate_id(connection);
      error = xcb_request_check(connection, xcb_sync_create_fence_checked(connection, pixmap, id, true));
      supported = !error && !xcb_connection_has_error(connection);
      free(error);
      if (!supported) return false;
      fence = id;
    }
    xcb_sync_reset_fence(connection, fence);
    xcb_sync_trigger_fence(connection, fence);
    error = NULL;
    reply = xcb_dri3_buffers_from_pixmap_reply(
        connection, xcb_dri3_buffers_from_pixmap(connection, pixmap), &error);
    if (reply) fds = xcb_dri3_buffers_from_pixmap_reply_fds(connection, reply);
    bool supported = !error && reply && reply->nfd > 0 && reply->nfd <= 4;
    free(error);
    return supported;
  }

  EGLImageKHR import(EGLDisplay display)
  {
    if (!epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import")) return EGL_NO_IMAGE_KHR;
    const bool modifiers = epoxy_has_egl_extension(display, "EGL_EXT_image_dma_buf_import_modifiers");
    if (!modifiers && reply->modifier != DRM_FORMAT_MOD_INVALID && reply->modifier != DRM_FORMAT_MOD_LINEAR)
      return EGL_NO_IMAGE_KHR;
    const uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(reply);
    const uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(reply);
    static const EGLint planes[][5] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
            EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
            EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT}};
    EGLint attrs[6 + 4 * 10 + 1] = {EGL_WIDTH, reply->width, EGL_HEIGHT, reply->height,
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)DRM_FORMAT_XRGB8888};
    int count = 6;
    for (int i = 0; i < reply->nfd; ++i)
    {
      if (strides[i] > INT_MAX || offsets[i] > INT_MAX) return EGL_NO_IMAGE_KHR;
      attrs[count++] = planes[i][0];
      attrs[count++] = fds[i];
      attrs[count++] = planes[i][1];
      attrs[count++] = offsets[i];
      attrs[count++] = planes[i][2];
      attrs[count++] = strides[i];
      if (modifiers && reply->modifier != DRM_FORMAT_MOD_INVALID)
      {
        attrs[count++] = planes[i][3];
        attrs[count++] = reply->modifier;
        attrs[count++] = planes[i][4];
        attrs[count++] = reply->modifier >> 32;
      }
    }
    attrs[count] = EGL_NONE;
    return eglCreateImageKHR(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
  }
};

void CaptureGL::reset()
{
  if (fence && connection)
  {
    xcb_sync_destroy_fence(connection, fence);
    xcb_flush(connection);
  }
  fence = 0;
  connection = NULL;
  if (context != EGL_NO_CONTEXT && context == eglGetCurrentContext())
  {
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
  }
  context = EGL_NO_CONTEXT;
  texture = framebuffer = 0;
}

bool CaptureGL::draw(const CapturePixmap &pixmap, int width, int height, int x, int y)
{
  if (disabled || pixmap.image_format == CAIRO_FORMAT_INVALID) return false;
  const EGLContext current = eglGetCurrentContext();
  const EGLDisplay display = eglGetCurrentDisplay();
  if (current == EGL_NO_CONTEXT || display == EGL_NO_DISPLAY) return false;
  if (context != EGL_NO_CONTEXT && context != current)
  {
    disabled = true;
    return false;
  }
  context = current;
  connection = pixmap.connection;
  CaptureDmaBuf buffer;
  const int w = pixmap.width, h = pixmap.height;
  bool success =
      buffer.get(connection, pixmap.pixmap, fence) && buffer.reply->width == w && buffer.reply->height == h &&
      buffer.reply->bpp == 32 && buffer.reply->depth == pixmap.format.depth &&
      epoxy_gl_version() >= (epoxy_is_desktop_gl() ? 32 : 30) && epoxy_has_gl_extension("GL_OES_EGL_image");
  if (success)
  {
    EGLImageKHR image = buffer.import(display);
    success = image != EGL_NO_IMAGE_KHR;
    if (success)
    {
      if (!texture)
      {
        glGenTextures(1, &texture);
        glGenFramebuffers(1, &framebuffer);
      }
      glBindTexture(GL_TEXTURE_2D, texture);
      glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
      eglDestroyImageKHR(display, image);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
      glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
      success = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
      if (success)
      {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDrawBuffer(GL_BACK);
        glDisable(GL_SCISSOR_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        const int copy_width = min(width, w - x), copy_height = min(height, h - y);
        if (copy_width > 0 && copy_height > 0)
          glBlitFramebuffer(x, y, x + copy_width, y + copy_height, 0, height, copy_width,
              height - copy_height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, 0);
      success = success && glGetError() == GL_NO_ERROR;
    }
  }
  if (!success)
  {
    reset();
    disabled = true;
  }
  return success;
}

static void xw_size(HWND hwnd);

struct Capture
{
  CapturePixmap pixmap;
  CaptureImage image;
  CaptureGL gl;
  cairo_device_t *cairo_device = NULL;
  xcb_connection_t *connection = NULL;
  xcb_window_t parent_win = 0, plugin_win = 0, gui_win = 0;
  uint32_t sequence = 0;
  bool pixmap_dirty = true;
  SWELL_WaylandView *view = NULL;
  HWND hwnd = NULL;
  xcb_damage_damage_t damage = 0;
  SWELL_WaylandView *popup_canvas = NULL;
  bool pointer_aligned = false;
  cairo_rectangle_int_t monitor_bounds = {};
  int canvas_x = 0, canvas_y = 0, native_x = 0, native_y = 0;
  int source_x = 0, source_y = 0;
  xcb_window_t root_popup = 0;
  uint32_t canvas_buttons = 0, view_buttons = 0;
  bool canvas_plugin_cursor = false;

  struct PopupWin
  {
    xcb_window_t x11_win = 0;
    CapturePixmap pixmap;
    CaptureImage image;
    bool pixmap_dirty = true;
    int x = 0, y = 0, w = 0, h = 0;
    xcb_damage_damage_t damage = 0;
    WDL_TypedBuf<xcb_rectangle_t> shape, input;
  };
  WDL_PtrList_DeleteOnDestroy<PopupWin> popups;

  struct ModalWin
  {
    Capture *cap = NULL;
    xcb_window_t x11_win = 0;
    CapturePixmap pixmap;
    CaptureImage image;
    bool pixmap_dirty = true;
    SWELL_WaylandView *view = NULL;
    uint32_t buttons = 0;
    xcb_damage_damage_t damage = 0;
  };
  WDL_PtrList_DeleteOnDestroy<ModalWin> modals;
};

struct XWaylandBridge
{
  xcb_connection_t *connection = NULL;
  xcb_window_t parent = 0;
  xcb_atom_t monitor = XCB_NONE;
  Capture *cap = NULL;
  GSource *event_source = NULL;
  SWELL_WaylandView *view = NULL;
  HWND host = NULL;
  int source_x = 0, source_y = 0;
  bool placed = false, has_last_size_pos = false, external_gl = false;
  RECT last_size_pos = {};
};

static WDL_KeyedArray<xcb_window_t, Capture *> g_captures;
static WDL_KeyedArray<HWND, XWaylandBridge *> g_bridges;
static Capture *g_active_capture = NULL;
static int g_damage_event_base = -1, g_shape_event_base = -1, g_cursor_event_base = -1;

static xcb_connection_t *wm_connection()
{
  return g_wm ? g_wm->connection() : NULL;
}

static const char *cursor_font_name(const xcb_xfixes_get_cursor_image_and_name_reply_t *image)
{
  struct CursorShape
  {
    unsigned glyph;
    const char *name;
    int width = 0, height = 0, xhot = 0, yhot = 0;
    WDL_TypedBuf<uint32_t> pixels;
    CursorShape(unsigned g, const char *n) : glyph(g), name(n) {}
  };
  static CursorShape shapes[] = {
    {XC_left_ptr, "left_ptr"}, {XC_arrow, "arrow"}, {XC_top_left_arrow, "top_left_arrow"},
    {XC_hand1, "hand1"}, {XC_hand2, "hand2"}, {XC_xterm, "xterm"}, {XC_watch, "watch"},
    {XC_cross, "cross"}, {XC_crosshair, "crosshair"}, {XC_fleur, "fleur"},
    {XC_question_arrow, "question_arrow"}, {XC_sb_h_double_arrow, "sb_h_double_arrow"},
    {XC_sb_v_double_arrow, "sb_v_double_arrow"}, {XC_top_side, "top_side"},
    {XC_bottom_side, "bottom_side"}, {XC_left_side, "left_side"}, {XC_right_side, "right_side"},
    {XC_top_left_corner, "top_left_corner"}, {XC_top_right_corner, "top_right_corner"},
    {XC_bottom_left_corner, "bottom_left_corner"}, {XC_bottom_right_corner, "bottom_right_corner"}
  };
  static bool initialized;
  if (!initialized)
  {
    initialized = true;
    xcb_connection_t *connection = wm_connection();
    const xcb_font_t font = xcb_generate_id(connection);
    xcb_generic_error_t *error =
        xcb_request_check(connection, xcb_open_font_checked(connection, font, 6, "cursor"));
    if (error)
    {
      free(error);
      return NULL;
    }
    xcb_query_font_reply_t *reply = xcb_query_font_reply(connection, xcb_query_font(connection, font), NULL);
    if (reply && !reply->min_byte1 && !reply->max_byte1)
      for (auto &shape : shapes)
      {
        const unsigned index = shape.glyph + 1 - reply->min_char_or_byte2;
        if (index >= reply->char_infos_len) continue;
        const xcb_charinfo_t &metrics = xcb_query_font_char_infos(reply)[index];
        shape.xhot = wdl_max(0, -metrics.left_side_bearing);
        shape.yhot = wdl_max(0, metrics.ascent);
        shape.width = shape.xhot + wdl_max(0, metrics.right_side_bearing);
        shape.height = shape.yhot + wdl_max(0, metrics.descent);
        if (!shape.width || !shape.height) continue;
        for (int mask = 0; mask < 2; ++mask)
        {
          const xcb_pixmap_t pixmap = xcb_generate_id(connection);
          const xcb_gcontext_t gc = xcb_generate_id(connection);
          xcb_create_pixmap(connection, 1, pixmap, swell_xcb_screen(connection)->root,
              shape.width, shape.height);
          const uint32_t values[] = {0, font};
          xcb_create_gc(connection, gc, pixmap, XCB_GC_FOREGROUND | XCB_GC_FONT, values);
          const xcb_rectangle_t rect = {0, 0, (uint16_t)shape.width, (uint16_t)shape.height};
          xcb_poly_fill_rectangle(connection, pixmap, gc, 1, &rect);
          const uint32_t foreground = 1;
          xcb_change_gc(connection, gc, XCB_GC_FOREGROUND, &foreground);
          const uint8_t text[] = {1, 0, 0, (uint8_t)(shape.glyph + mask)};
          xcb_poly_text_16(connection, pixmap, gc, shape.xhot, shape.yhot, sizeof(text), text);
          xcb_image_t *bits = xcb_image_get(
              connection, pixmap, 0, 0, shape.width, shape.height, 1, XCB_IMAGE_FORMAT_Z_PIXMAP);
          xcb_free_gc(connection, gc);
          xcb_free_pixmap(connection, pixmap);
          if (!bits)
          {
            shape.pixels.Resize(0);
            break;
          }
          if (!mask && !shape.pixels.ResizeOK(shape.width * shape.height))
          {
            xcb_image_destroy(bits);
            break;
          }
          for (int y = 0; y < shape.height; ++y)
            for (int x = 0; x < shape.width; ++x)
            {
              uint32_t &pixel = shape.pixels.Get()[(size_t)y * shape.width + x];
              const bool set = xcb_image_get_pixel(bits, x, y);
              if (!mask)
                pixel = set ? 0xff000000 : 0xffffffff;
              else if (!set)
                pixel = 0;
            }
          xcb_image_destroy(bits);
        }
      }
    free(reply);
    xcb_close_font(connection, font);
  }
  for (const auto &shape : shapes)
    if (shape.pixels.GetSize() && image->width == shape.width && image->height == shape.height &&
        image->xhot == shape.xhot && image->yhot == shape.yhot &&
        !memcmp(xcb_xfixes_get_cursor_image_and_name_cursor_image(image), shape.pixels.Get(),
            shape.pixels.GetSize() * sizeof(uint32_t)))
      return shape.name;
  return NULL;
}

static void update_view_cursor(SWELL_WaylandView *view, bool plugin_cursor = true)
{
  if (g_cursor_event_base < 0 || !swell_wayland_view_has_pointer(view)) return;
  if (!plugin_cursor)
  {
    swell_wayland_view_set_cursor(view, "default", NULL, 0, 0, 0, 0);
    return;
  }
  xcb_xfixes_get_cursor_image_and_name_reply_t *image = xcb_xfixes_get_cursor_image_and_name_reply(
      wm_connection(), xcb_xfixes_get_cursor_image_and_name(wm_connection()), NULL);
  if (!image) return;
  WDL_FastString name;
  name.SetRaw(xcb_xfixes_get_cursor_image_and_name_name(image), image->nbytes);
  if (!name.GetLength())
    if (const char *standard = cursor_font_name(image)) name.Set(standard);
  swell_wayland_view_set_cursor(view, name.Get(), xcb_xfixes_get_cursor_image_and_name_cursor_image(image),
      image->width, image->height, image->xhot, image->yhot);
  free(image);
}

static void invalidate_view(SWELL_WaylandView *view, int x, int y, int w, int h)
{
  if (!view) return;
  RECT r = {x, y, x + w, y + h};
  swell_wayland_view_invalidate(view, &r);
}

static void set_active_capture(Capture *c)
{
  g_active_capture = c;
  if (g_wm) g_wm->set_active_window(c ? (c->gui_win ? c->gui_win : c->plugin_win) : XCB_NONE);
}

static Capture *capture_for_hwnd(HWND hwnd)
{
  XWaylandBridge *bridge = g_bridges.Get(hwnd);
  return bridge ? bridge->cap : NULL;
}

static xcb_damage_damage_t create_damage(xcb_window_t window)
{
  if (g_damage_event_base < 0) return 0;
  xcb_damage_damage_t damage = xcb_generate_id(wm_connection());
  xcb_damage_create(wm_connection(), damage, window, XCB_DAMAGE_REPORT_LEVEL_BOUNDING_BOX);
  return damage;
}

static void forward_modifiers(xcb_connection_t *connection, int state)
{
  xcb_xkb_latch_lock_state(
      connection, XCB_XKB_ID_USE_CORE_KBD, 0xff, state & 0xff, true, (state >> 13) & 3, 0, false, 0);
}

static void fake_motion(xcb_connection_t *connection, int x, int y, int state = -1)
{
  if (state >= 0) forward_modifiers(connection, state);
  xcb_test_fake_input(
      connection, XCB_MOTION_NOTIFY, 0, XCB_CURRENT_TIME, swell_xcb_screen(connection)->root, x, y, 0);
}

static void fake_button(xcb_connection_t *connection, int button, bool press)
{
  xcb_test_fake_input(
      connection, press ? XCB_BUTTON_PRESS : XCB_BUTTON_RELEASE, button, XCB_CURRENT_TIME, XCB_NONE, 0, 0, 0);
}

static void forward_key(xcb_connection_t *connection, xcb_window_t window, int keycode, int state, bool press)
{
  xcb_key_press_event_t event = {};
  event.response_type = press ? XCB_KEY_PRESS : XCB_KEY_RELEASE;
  event.event = window;
  event.root = swell_xcb_screen(connection)->root;
  event.detail = keycode;
  event.state = state;
  event.same_screen = true;
  xcb_send_event(connection, false, window, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
  xcb_flush(connection);
}

static xcb_window_t first_child(xcb_connection_t *connection, xcb_window_t window)
{
  xcb_query_tree_reply_t *tree =
      xcb_query_tree_reply(connection, xcb_query_tree(connection, window), NULL);
  xcb_window_t child =
      tree && xcb_query_tree_children_length(tree) ? xcb_query_tree_children(tree)[0] : XCB_NONE;
  free(tree);
  return child;
}

static void update_gui_window(Capture *c)
{
  xcb_window_t gui = first_child(c->connection, c->plugin_win);
  if (!gui) gui = c->plugin_win;
  if (gui == c->gui_win) return;
  auto old = g_captures.Get(c->gui_win);
  if (c->gui_win != c->plugin_win && old != NULL && old == c) g_captures.Delete(c->gui_win);
  c->gui_win = gui;
  g_captures.Insert(gui, c);
}

static void register_capture(Capture *c)
{
  g_captures.Insert(c->parent_win, c);
  g_captures.Insert(c->plugin_win, c);
  if (c->gui_win) g_captures.Insert(c->gui_win, c);
}

static void unregister_capture(Capture *c)
{
  for (int i = g_captures.GetSize() - 1; i >= 0; --i)
    if (g_captures.Enumerate(i) == c) g_captures.DeleteByIndex(i);
}

static Capture *find_capture(xcb_window_t w)
{
  return g_captures.Get(w);
}

static bool update_pixmap(Capture *c)
{
  if (!c || !c->connection || !c->plugin_win) return false;

  if (!c->pixmap.update(c->connection, c->plugin_win)) return false;
  c->pixmap_dirty = false;
  return true;
}

static void refresh_pixmap(Capture *c)
{
  if (!c) return;
  c->pixmap_dirty = true;
  if (c->view) swell_wayland_view_invalidate(c->view);
}

static cairo_surface_t *read_pixels(Capture *c, CapturePixmap &pixmap, CaptureImage &image, bool refresh)
{
  cairo_surface_t *surface = image.read(pixmap, refresh);
  if (!surface)
  {
    surface = pixmap.surface();
    if (!c->cairo_device) c->cairo_device = cairo_device_reference(cairo_surface_get_device(surface));
  }
  return surface;
}

static xcb_window_t s_pointer_window;
static bool s_pointer_parked;

static void park_pointer(int x, int y)
{
  if (!wm_connection()) return;
  if (!s_pointer_window)
  {
    const uint32_t values[] = {true, XCB_EVENT_MASK_BUTTON_PRESS | XCB_EVENT_MASK_BUTTON_RELEASE};
    s_pointer_window = xcb_generate_id(wm_connection());
    xcb_create_window(wm_connection(), XCB_COPY_FROM_PARENT, s_pointer_window,
        swell_xcb_screen(wm_connection())->root, x, y, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
        XCB_COPY_FROM_PARENT, XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
  }
  const uint32_t position[] = {(uint32_t)x, (uint32_t)y};
  xcb_configure_window(
      wm_connection(), s_pointer_window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, position);
  swell_xcb_raise(wm_connection(), s_pointer_window);
  xcb_map_window(wm_connection(), s_pointer_window);
  fake_motion(wm_connection(), x, y);
  swell_xcb_sync(wm_connection());
  s_pointer_parked = true;
}

static void resume_pointer()
{
  if (!s_pointer_parked) return;
  xcb_unmap_window(wm_connection(), s_pointer_window);
  swell_xcb_sync(wm_connection());
  s_pointer_parked = false;
}

static void raise_window(xcb_window_t window)
{
  for (const auto &kv : g_captures)
  {
    const Capture *c = kv.val;
    if (kv.key != c->parent_win) continue;
    if (c->popups.GetSize()) return;
  }
  swell_xcb_raise(wm_connection(), window);
}

static void forward_motion(Capture *c, int x, int y, int state)
{
  resume_pointer();
  raise_window(c->parent_win);
  int rx = 0, ry = 0;
  swell_xcb_translate(wm_connection(), c->plugin_win, swell_xcb_screen(c->connection)->root, x, y, &rx, &ry);
  fake_motion(c->connection, rx, ry, state);
}

static void update_popup_shape(Capture::PopupWin &popup)
{
  xcb_connection_t *connection = wm_connection();
  const auto bounding = xcb_shape_get_rectangles(connection, popup.x11_win, XCB_SHAPE_SK_BOUNDING);
  const auto input = xcb_shape_get_rectangles(connection, popup.x11_win, XCB_SHAPE_SK_INPUT);
  xcb_shape_get_rectangles_reply_t *reply = xcb_shape_get_rectangles_reply(connection, bounding, NULL);
  popup.shape.Resize(0);
  if (reply)
  {
    const xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(reply);
    popup.shape.Add(rects, xcb_shape_get_rectangles_rectangles_length(reply));
  }
  free(reply);
  reply = xcb_shape_get_rectangles_reply(connection, input, NULL);
  popup.input.Resize(0);
  if (reply)
  {
    const xcb_rectangle_t *rects = xcb_shape_get_rectangles_rectangles(reply);
    const int count = xcb_shape_get_rectangles_rectangles_length(reply);
    for (const auto &b : popup.shape)
      for (int i = 0; i < count; ++i)
      {
        const xcb_rectangle_t &r = rects[i];
        const int left = wdl_max(0, wdl_max(b.x, r.x)), top = wdl_max(0, wdl_max(b.y, r.y));
        const int right = wdl_min(popup.w, wdl_min(b.x + b.width, r.x + r.width));
        const int bottom = wdl_min(popup.h, wdl_min(b.y + b.height, r.y + r.height));
        if (right > left && bottom > top)
          popup.input.Add({(int16_t)left, (int16_t)top, (uint16_t)(right - left), (uint16_t)(bottom - top)});
      }
  }
  free(reply);
}

static bool has_input_popup(const Capture *c)
{
  if (!c->popup_canvas) return false;
  RECT rect;
  swell_wayland_view_rect(c->popup_canvas, &rect);
  for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
  {
    const Capture::PopupWin &p = *c->popups.Get(popup_idx);
    if (p.x < rect.right && p.x + p.w > rect.left && p.y < rect.bottom && p.y + p.h > rect.top &&
        p.input.GetSize())
      return true;
  }
  return false;
}

static uint32_t capture_buttons(const Capture *c)
{
  uint32_t buttons = c->view_buttons | c->canvas_buttons;
  for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
  {
    const Capture::ModalWin &m = *c->modals.Get(modal_idx);
    buttons |= m.buttons;
  }
  return buttons;
}

static bool popup_contains_input(const Capture *c, int x, int y)
{
  for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
  {
    const Capture::PopupWin &p = *c->popups.Get(popup_idx);
    if (x >= p.x && x < p.x + p.w && y >= p.y && y < p.y + p.h)
      for (const auto &r : p.input)
        if (x >= p.x + r.x && x < p.x + r.x + r.width &&
            y >= p.y + r.y && y < p.y + r.y + r.height)
          return true;
  }
  return false;
}

static void update_canvas_cursor(Capture *c, bool force = false)
{
  POINT point;
  if (!swell_wayland_view_has_pointer(c->popup_canvas, &point)) return;
  point.x += c->canvas_x;
  point.y += c->canvas_y;
  RECT editor;
  swell_wayland_view_rect(!c->modals.GetSize() ? c->view : c->modals.GetLast()->view, &editor);
  if (c->modals.GetSize()) OffsetRect(&editor, -editor.left, -editor.top);
  const bool plugin_cursor = popup_contains_input(c, point.x, point.y) ||
      capture_buttons(c) || PtInRect(&editor, point);
  if (plugin_cursor != c->canvas_plugin_cursor || force)
  {
    c->canvas_plugin_cursor = plugin_cursor;
    update_view_cursor(c->popup_canvas, plugin_cursor);
  }
}

static void canvas_update_input(Capture *c)
{
  if (!c->popup_canvas) return;
  update_canvas_cursor(c);
  const bool dragging = capture_buttons(c);
  const bool input = has_input_popup(c);
  if (!dragging || input)
  {
    swell_wayland_view_set_input(c->popup_canvas, input);
    return;
  }
  RECT canvas, editor;
  swell_wayland_view_rect(c->popup_canvas, &canvas);
  swell_wayland_view_rect(!c->modals.GetSize() ? c->view : c->modals.GetLast()->view, &editor);
  if (c->modals.GetSize()) OffsetRect(&editor, -editor.left, -editor.top);
  if (!IntersectRect(&editor, &editor, &canvas))
  {
    swell_wayland_view_set_input(c->popup_canvas, true);
    return;
  }
  OffsetRect(&editor, -canvas.left, -canvas.top);
  const int width = canvas.right - canvas.left, height = canvas.bottom - canvas.top;
  const RECT rects[] = {{0, 0, width, editor.top}, {0, editor.top, editor.left, editor.bottom},
      {editor.right, editor.top, width, editor.bottom}, {0, editor.bottom, width, height}};
  swell_wayland_view_set_input_region(c->popup_canvas, rects, 4);
}

static void leave_pointer(Capture *c, xcb_window_t window, int state)
{
  if ((state & (XCB_BUTTON_MASK_1 | XCB_BUTTON_MASK_2 | XCB_BUTTON_MASK_3 | XCB_BUTTON_MASK_4 |
                   XCB_BUTTON_MASK_5)) ||
      has_input_popup(c))
    return;
  xcb_motion_notify_event_t motion = {};
  motion.response_type = XCB_MOTION_NOTIFY;
  motion.event = window;
  motion.root = swell_xcb_screen(c->connection)->root;
  motion.state = state;
  motion.same_screen = true;
  int x, y;
  if (swell_xcb_translate(c->connection, motion.root, motion.event, 0, 0, &x, &y))
  {
    motion.event_x = x;
    motion.event_y = y;
    xcb_send_event(c->connection, true, motion.event, XCB_EVENT_MASK_POINTER_MOTION, (const char *)&motion);
    swell_xcb_sync(c->connection);
  }
  park_pointer(0, 0);
}

bool xw_bridge_raise_modals()
{
  bool any = false;
  for (const auto &kv : g_captures)
  {
    Capture *c = kv.val;
    if (kv.key != c->parent_win) continue;
    for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
    {
      Capture::ModalWin &m = *c->modals.Get(modal_idx);
      if (m.view)
      {
        raise_window(m.x11_win);
        swell_wayland_view_present(m.view);
        any = true;
      }
    }
  }
  if (any && wm_connection()) xcb_flush(wm_connection());
  return any;
}

static xcb_window_t input_focus_in(xcb_connection_t *connection, xcb_window_t parent)
{
  xcb_get_input_focus_reply_t *reply =
      xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), NULL);
  const xcb_window_t focus = reply ? reply->focus : XCB_NONE;
  free(reply);
  xcb_window_t window = focus;
  for (int depth = 0; window > XCB_INPUT_FOCUS_POINTER_ROOT && depth < 64; ++depth)
  {
    if (window == parent) return focus;
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(connection, xcb_query_tree(connection, window), NULL);
    window = tree ? tree->parent : XCB_NONE;
    free(tree);
  }
  return XCB_NONE;
}

static void restore_input_focus(
    xcb_connection_t *connection, xcb_window_t window, xcb_window_t fallback = XCB_NONE)
{
  if (!input_focus_in(connection, window))
    xcb_set_input_focus(connection, XCB_INPUT_FOCUS_PARENT, fallback ? fallback : window, XCB_CURRENT_TIME);
}

static void send_scroll(xcb_connection_t *connection, int x, int y)
{
  while (x)
  {
    const int button = x < 0 ? 6 : 7;
    fake_button(connection, button, true);
    fake_button(connection, button, false);
    x += x < 0 ? 1 : -1;
  }
  while (y)
  {
    const int button = y < 0 ? 4 : 5;
    fake_button(connection, button, true);
    fake_button(connection, button, false);
    y += y < 0 ? 1 : -1;
  }
}

static void update_xembed(Capture *c)
{
  const xcb_atom_t atom = g_wm->atoms()._XEMBED_INFO;
  xcb_get_property_reply_t *reply = xcb_get_property_reply(
      c->connection, xcb_get_property(c->connection, false, c->plugin_win, atom, atom, 0, 2), NULL);
  if (reply && reply->type == atom && reply->format == 32 && reply->value_len >= 2)
  {
    const bool mapped = ((const uint32_t *)xcb_get_property_value(reply))[1] & 1;
    if (mapped)
      xcb_map_window(c->connection, c->plugin_win);
    else
    {
      xcb_unmap_window(c->connection, c->plugin_win);
      c->pixmap.reset();
    }
    refresh_pixmap(c);
    xcb_flush(c->connection);
  }
  free(reply);
}

static Capture *setup_capture(
    xcb_connection_t *connection, xcb_window_t parent_win, xcb_window_t plugin_win, HWND hwnd)
{
  const uint32_t events =
      XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
  xcb_void_cookie_t subscription =
      xcb_change_window_attributes_checked(wm_connection(), plugin_win, XCB_CW_EVENT_MASK, &events);
  xcb_query_tree_reply_t *tree =
      xcb_query_tree_reply(wm_connection(), xcb_query_tree(wm_connection(), plugin_win), NULL);
  xcb_generic_error_t *error = xcb_request_check(wm_connection(), subscription);
  const bool valid = tree && tree->parent == parent_win && !error;
  free(tree);
  free(error);
  if (!valid) return NULL;

  Capture *c = new Capture();
  c->connection = connection;
  c->parent_win = parent_win;
  c->plugin_win = plugin_win;
  c->sequence = subscription.sequence;
  c->hwnd = hwnd;

  xcb_xfixes_change_save_set(connection, XCB_SET_MODE_INSERT, XCB_XFIXES_SAVE_SET_TARGET_ROOT,
      XCB_XFIXES_SAVE_SET_MAPPING_UNMAP, plugin_win);

  xcb_composite_redirect_window(connection, plugin_win, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
  xcb_map_window(connection, plugin_win);

  update_pixmap(c);

  c->damage = create_damage(plugin_win);

  xcb_flush(connection);

  register_capture(c);
  DEBUG_PRINT("[XW] setup parent=0x%lx plugin=0x%lx (auto-composite, pixmap=0x%lx)\n",
      (unsigned long)parent_win, (unsigned long)plugin_win, (unsigned long)c->pixmap.pixmap);
  return c;
}

static void canvas_remove_popup(Capture *c, xcb_window_t window);
static void modal_remove(Capture *c, xcb_window_t window);
static void on_window_configured(xcb_window_t window, int width, int height);
static void dismiss_popups(Capture *c);

static int mouse_button(UINT msg, bool &pressed)
{
  pressed = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_MBUTTONDOWN ||
            msg == WM_MBUTTONDBLCLK || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK;
  switch (msg)
  {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
      return 1;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
      return 2;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
      return 3;
  }
  return 0;
}

static bool editor_draw(SWELL_WaylandView *view, void *data, cairo_t *cr)
{
  Capture *c = capture_for_hwnd((HWND)data);
  const bool refresh = c && c->pixmap_dirty;
  if (refresh) update_pixmap(c);
  if (!c || !c->pixmap.pixmap)
  {
    if (cr) return false;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDrawBuffer(GL_BACK);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    return glGetError() == GL_NO_ERROR;
  }
  if (!cr)
  {
    RECT r;
    swell_wayland_view_rect(view, &r);
    if (c->gl.draw(c->pixmap, r.right - r.left, r.bottom - r.top, c->source_x, c->source_y)) return true;
    c->pixmap_dirty = true;
    return false;
  }
  cairo_surface_t *surface = read_pixels(c, c->pixmap, c->image, refresh);
  if (!surface) return false;
  const bool success = cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
  if (success)
  {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(cr, surface, -c->source_x, -c->source_y);
    cairo_paint(cr);
    if (c->pixmap.image_format == CAIRO_FORMAT_ARGB32)
    {
      cairo_set_operator(cr, CAIRO_OPERATOR_DEST_OVER);
      cairo_set_source_rgb(cr, 0, 0, 0);
      cairo_rectangle(cr, -c->source_x, -c->source_y, c->pixmap.width, c->pixmap.height);
      cairo_fill(cr);
    }
  }
  cairo_surface_destroy(surface);
  return success;
}

static void editor_reset_gl(SWELL_WaylandView *, void *data)
{
  if (Capture *c = capture_for_hwnd((HWND)data))
  {
    c->gl.reset();
    c->pixmap_dirty = true;
  }
}

static bool editor_mouse(SWELL_WaylandView *view, void *data, UINT msg, int x, int y, int wheel, unsigned state)
{
  Capture *c = capture_for_hwnd((HWND)data);
  if (!c || !c->connection) return false;
  x += c->source_x;
  y += c->source_y;
  if (msg == SWELL_WAYLAND_MOUSE_LEAVE)
  {
    leave_pointer(c, c->gui_win ? c->gui_win : c->plugin_win, state);
    return true;
  }
  if (msg == WM_MOUSEMOVE && g_wm && g_wm->dnd_has_pending())
  {
    char path[8192];
    g_wm->dnd_take_pending_path(path, sizeof(path));
    if (path[0])
    {
      const xcb_window_t parent = c->parent_win;
      const char *list[] = {path};
      c->view_buttons = 0;
      canvas_update_input(c);
      SWELL_InitiateDragDropOfFileList(c->hwnd, NULL, list, 1, NULL);
      if (find_capture(parent) != c) return true;
      g_wm->dnd_release_source_button(c->connection);
    }
  }
  bool pressed = false;
  const int button = mouse_button(msg, pressed);
  if (button && pressed)
  {
    if (xw_bridge_raise_modals()) return true;
    set_active_capture(c);
    const xcb_window_t parent = c->parent_win;
    SetFocus(c->hwnd);
    if (find_capture(parent) != c) return true;
    restore_input_focus(c->connection, c->parent_win, g_wm->host_focus_window());
  }
  forward_motion(c, x, y, state);
  if (button)
  {
    fake_button(c->connection, button, pressed);
    if (pressed)
      c->view_buttons |= 1u << button;
    else
      c->view_buttons &= ~(1u << button);
    canvas_update_input(c);
  }
  else if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
  {
    send_scroll(
        c->connection, msg == WM_MOUSEHWHEEL ? wheel / 120 : 0, msg == WM_MOUSEWHEEL ? -wheel / 120 : 0);
  }
  xcb_flush(c->connection);
  if (msg == SWELL_WAYLAND_MOUSE_ENTER) update_view_cursor(view);
  return true;
}

static bool editor_key(
    SWELL_WaylandView *, void *data, bool down, uint32_t code, uint32_t sym, unsigned state)
{
  return xw_bridge_forward_key((HWND)data, code, sym, state, down);
}

static HWND capture_toplevel(const Capture *c)
{
  HWND h = c->hwnd;
  while (h && h->m_parent) h = h->m_parent;
  return h;
}

static void editor_focus(SWELL_WaylandView *, void *data, bool active)
{
  Capture *c = capture_for_hwnd((HWND)data);
  if (!c) return;
  if (active)
  {
    if (g_active_capture && g_active_capture != c &&
        (g_active_capture->modals.GetSize() ||
            (capture_toplevel(g_active_capture) == capture_toplevel(c) && GetFocus() != c->hwnd)))
      return;
    set_active_capture(c);
    raise_window(c->parent_win);
    xcb_flush(wm_connection());
    if (g_wm && !c->modals.GetSize() && !c->popups.GetSize())
    {
      const xcb_window_t focus = input_focus_in(c->connection, c->parent_win);
      if (focus && focus != c->parent_win)
      {
        xcb_set_input_focus(
            c->connection, XCB_INPUT_FOCUS_PARENT, g_wm->host_focus_window(), XCB_CURRENT_TIME);
        xcb_flush(c->connection);
      }
    }
    xw_bridge_raise_modals();
  }
  else if (g_active_capture == c && !c->modals.GetSize() && !c->popups.GetSize())
    set_active_capture(NULL);
}

static const SWELL_WaylandViewCallbacks editor_callbacks = {
    editor_draw, editor_mouse, editor_key, NULL, editor_focus, editor_reset_gl, NULL};

static void cleanup_capture(Capture *c)
{
  if (!c) return;
  for (int button = 1; button < 32; ++button)
    if (c->view_buttons & (1u << button)) fake_button(c->connection, button, false);
  if (c->view_buttons)
  {
    xcb_flush(c->connection);
    swell_wayland_view_cancel_input(c->view);
  }
  if (g_active_capture == c) set_active_capture(NULL);
  unregister_capture(c);
  while (c->popups.GetSize()) canvas_remove_popup(c, c->popups.GetLast()->x11_win);
  while (c->modals.GetSize()) modal_remove(c, c->modals.GetLast()->x11_win);
  if (c->damage && wm_connection()) xcb_damage_destroy(wm_connection(), c->damage);
  if (wm_connection()) xcb_flush(wm_connection());
  swell_wayland_view_reset_gl(c->view);
  c->gl.reset();
  c->pixmap.reset();
  c->image.reset();
  if (c->cairo_device)
  {
    cairo_device_finish(c->cairo_device);
    cairo_device_destroy(c->cairo_device);
  }
  delete c;
}

static void refresh_native_offset(Capture *c)
{
  if (!c || !c->connection || !c->view) return;
  const bool modal = c->modals.GetSize();
  const xcb_window_t origin = modal ? c->modals.GetLast()->x11_win : c->plugin_win;
  int x = 0, y = 0;
  swell_xcb_translate(wm_connection(), origin, swell_xcb_screen(c->connection)->root, 0, 0, &x, &y);
  RECT r = {};
  if (!modal) swell_wayland_view_rect(c->view, &r);
  c->native_x = r.left - (modal ? 0 : c->source_x) - x;
  c->native_y = r.top - (modal ? 0 : c->source_y) - y;
}

static void popup_position(Capture *c, bool align_pointer = false)
{
  if (!c->popup_canvas) return;
  xcb_connection_t *connection = wm_connection();
  RECT r;
  swell_wayland_view_rect(c->popup_canvas, &r);
  const bool moved = c->canvas_x != r.left || c->canvas_y != r.top;
  const int dx = c->monitor_bounds.x + c->native_x - r.left;
  const int dy = c->monitor_bounds.y + c->native_y - r.top;
  if (align_pointer && (dx || dy))
  {
    const xcb_window_t window = !c->modals.GetSize() ? c->parent_win : c->modals.GetLast()->x11_win;
    const xcb_get_geometry_cookie_t geometry_request = xcb_get_geometry(connection, window);
    const xcb_query_pointer_cookie_t pointer_request = xcb_query_pointer(connection, window);
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(connection, geometry_request, NULL);
    xcb_query_pointer_reply_t *pointer = xcb_query_pointer_reply(connection, pointer_request, NULL);
    if (geometry)
    {
      const uint32_t position[] = {(uint32_t)(geometry->x + dx), (uint32_t)(geometry->y + dy)};
      xcb_configure_window(connection, window, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, position);
      for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
      {
        Capture::PopupWin &p = *c->popups.Get(popup_idx);
        if (!c->pointer_aligned)
        {
          const uint32_t pos[] = {(uint32_t)(p.x - c->native_x + dx), (uint32_t)(p.y - c->native_y + dy)};
          xcb_configure_window(connection, p.x11_win, XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y, pos);
        }
        else
        {
          p.x -= dx;
          p.y -= dy;
        }
      }
      c->native_x -= dx;
      c->native_y -= dy;
      if (!c->pointer_aligned && pointer && g_active_capture == c)
        fake_motion(connection, pointer->root_x + dx, pointer->root_y + dy);
      xcb_flush(connection);
    }
    free(geometry);
    free(pointer);
  }
  if (align_pointer) c->pointer_aligned = true;
  c->canvas_x = r.left;
  c->canvas_y = r.top;
  if (moved) canvas_update_input(c);
}

static void invalidate_popup(Capture *c, int x, int y, int w, int h)
{
  popup_position(c);
  invalidate_view(c->popup_canvas, x - c->canvas_x, y - c->canvas_y, w, h);
}

static bool canvas_draw(SWELL_WaylandView *, void *data, cairo_t *cr)
{
  Capture *c = (Capture *)data;
  if (!cr || !c || !c->connection) return false;
  popup_position(c);
  cairo_set_source_rgba(cr, 0, 0, 0, 0);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
  double x1, y1, x2, y2;
  cairo_clip_extents(cr, &x1, &y1, &x2, &y2);
  for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
  {
    Capture::PopupWin &p = *c->popups.Get(popup_idx);
    const bool refresh = p.pixmap_dirty;
    if (refresh && p.pixmap.update(c->connection, p.x11_win))
    {
      if (p.w != p.pixmap.width || p.h != p.pixmap.height)
      {
        p.w = p.pixmap.width;
        p.h = p.pixmap.height;
        update_popup_shape(p);
        canvas_update_input(c);
        swell_wayland_view_invalidate(c->popup_canvas);
      }
      p.pixmap_dirty = false;
    }
    const int x = p.x - c->canvas_x, y = p.y - c->canvas_y;
    if (!p.pixmap.pixmap || x + p.w <= x1 || x >= x2 || y + p.h <= y1 || y >= y2) continue;
    cairo_surface_t *surface = read_pixels(c, p.pixmap, p.image, refresh);
    if (!surface) continue;
    cairo_save(cr);
    cairo_rectangle(cr, x, y, p.w, p.h);
    cairo_clip(cr);
    if (g_shape_event_base >= 0)
    {
      for (const auto &r : p.shape) cairo_rectangle(cr, x + r.x, y + r.y, r.width, r.height);
      cairo_clip(cr);
    }
    cairo_set_source_surface(cr, surface, x, y);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_NEAREST);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(surface);
  }
  return true;
}

static bool canvas_mouse(SWELL_WaylandView *, void *data, UINT msg, int x, int y, int wheel, unsigned state)
{
  Capture *c = (Capture *)data;
  if (!c || !c->connection) return false;
  if (msg == SWELL_WAYLAND_MOUSE_LEAVE) return true;
  popup_position(c, msg == WM_MOUSEMOVE && capture_buttons(c));
  x += c->canvas_x;
  y += c->canvas_y;
  bool pressed = false;
  const int button = mouse_button(msg, pressed);
  if (button && pressed && !popup_contains_input(c, x, y))
  {
    dismiss_popups(c);
    return true;
  }
  resume_pointer();
  fake_motion(c->connection, x - c->native_x, y - c->native_y, state);
  if (button)
  {
    fake_button(c->connection, button, pressed);
    if (pressed)
      c->canvas_buttons |= 1u << button;
    else
      c->canvas_buttons &= ~(1u << button);
    canvas_update_input(c);
  }
  else if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
  {
    send_scroll(
        c->connection, msg == WM_MOUSEHWHEEL ? wheel / 120 : 0, msg == WM_MOUSEWHEEL ? -wheel / 120 : 0);
  }
  xcb_flush(c->connection);
  update_canvas_cursor(c, msg == SWELL_WAYLAND_MOUSE_ENTER);
  return true;
}

static bool canvas_key(
    SWELL_WaylandView *, void *data, bool down, uint32_t code, uint32_t sym, unsigned state)
{
  return xw_bridge_forward_key(((Capture *)data)->hwnd, code, sym, state, down);
}

static void canvas_close(SWELL_WaylandView *, void *data)
{
  dismiss_popups((Capture *)data);
}

static const SWELL_WaylandViewCallbacks canvas_callbacks = {
    canvas_draw, canvas_mouse, canvas_key, canvas_close, NULL, NULL, NULL};
static cairo_rectangle_int_t capture_monitor(
    xcb_connection_t *connection, SWELL_WaylandView *view, xcb_atom_t *monitor_name = NULL)
{
  xcb_screen_t *screen = swell_xcb_screen(connection);
  cairo_rectangle_int_t bounds = {0, 0, screen->width_in_pixels, screen->height_in_pixels};
  xcb_get_geometry_reply_t *geometry =
      xcb_get_geometry_reply(connection, xcb_get_geometry(connection, screen->root), NULL);
  if (geometry)
  {
    bounds.width = geometry->width;
    bounds.height = geometry->height;
    free(geometry);
  }
  xcb_randr_get_monitors_reply_t *reply = xcb_randr_get_monitors_reply(
      connection, xcb_randr_get_monitors(connection, screen->root, true), NULL);
  if (!reply) return bounds;
  RECT output = {0, 0, bounds.width, bounds.height};
  if (view) swell_wayland_view_output(view, &output);
  cairo_rectangle_int_t selected = bounds;
  xcb_atom_t selected_name = XCB_NONE;
  int best = -1;
  for (xcb_randr_monitor_info_iterator_t it = xcb_randr_get_monitors_monitors_iterator(reply); it.rem;
      xcb_randr_monitor_info_next(&it))
  {
    const int left = max(bounds.x, (int)it.data->x), top = max(bounds.y, (int)it.data->y);
    const int right = min(bounds.x + bounds.width, (int)it.data->x + it.data->width);
    const int bottom = min(bounds.y + bounds.height, (int)it.data->y + it.data->height);
    if (right <= left || bottom <= top) continue;
    cairo_rectangle_int_t candidate = {left, top, right - left, bottom - top};
    if (monitor_name && it.data->name == *monitor_name)
    {
      selected = candidate;
      selected_name = it.data->name;
      break;
    }
    const int distance = abs(candidate.width - (output.right - output.left)) +
                         abs(candidate.height - (output.bottom - output.top));
    if (best < 0 || distance < best)
    {
      best = distance;
      selected = candidate;
      selected_name = it.data->name;
    }
  }
  free(reply);
  if (monitor_name) *monitor_name = selected_name;
  return selected;
}

static void create_popup_canvas(Capture *c)
{
  if (c->popup_canvas) return;
  c->pointer_aligned = false;
  SWELL_WaylandView *parent = !c->modals.GetSize() ? c->view : c->modals.GetLast()->view;
  if (!parent) return;
  c->monitor_bounds = capture_monitor(c->connection, parent);
  const cairo_rectangle_int_t &bounds = c->monitor_bounds;
  c->canvas_x = bounds.x + c->native_x;
  c->canvas_y = bounds.y + c->native_y;
  RECT r = {c->canvas_x, c->canvas_y, c->canvas_x + bounds.width, c->canvas_y + bounds.height};
  c->popup_canvas = swell_wayland_view_create(c->hwnd, parent, &r, SWELL_WAYLAND_POPUP, &canvas_callbacks, c);
}

static void canvas_remove_popup(Capture *c, xcb_window_t window)
{
  if (!c) return;
  for (int i = c->popups.GetSize() - 1; i >= 0; --i)
  {
    Capture::PopupWin *popup = c->popups.Get(i);
    if (popup->x11_win != window) continue;
    if (popup->damage && wm_connection()) xcb_damage_destroy(wm_connection(), popup->damage);
    c->popups.Delete(i, true);
  }
  if (c->root_popup == window) c->root_popup = XCB_NONE;
  if (!c->popups.GetSize())
  {
    for (int button = 1; button < 32; ++button)
      if (c->canvas_buttons & (1u << button)) fake_button(c->connection, button, false);
    if (c->canvas_buttons) xcb_flush(c->connection);
    c->canvas_buttons = 0;
    SWELL_WaylandView *view = c->popup_canvas;
    c->popup_canvas = NULL;
    if (view) swell_wayland_view_destroy(view);
  }
  else if (c->popup_canvas)
  {
    canvas_update_input(c);
    swell_wayland_view_invalidate(c->popup_canvas);
  }
}

static void canvas_add_popup(Capture *c, xcb_window_t window, xcb_get_geometry_reply_t *attr)
{
  if (!c) return;
  refresh_native_offset(c);
  int x, y;
  if (!swell_xcb_translate(c->connection, window, swell_xcb_screen(c->connection)->root, 0, 0, &x, &y))
    return;
  x += c->native_x;
  y += c->native_y;
  Capture::PopupWin *popup = NULL;
  for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
  {
    Capture::PopupWin &p = *c->popups.Get(popup_idx);
    if (p.x11_win == window)
    {
      popup = &p;
      break;
    }
  }
  if (popup && popup->pixmap.pixmap && popup->x == x && popup->y == y && popup->w == attr->width &&
      popup->h == attr->height)
  {
    canvas_update_input(c);
    if (c->popup_canvas) swell_wayland_view_show(c->popup_canvas, true);
    return;
  }
  xcb_composite_redirect_window(c->connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
  xcb_flush(c->connection);
  if (!popup)
  {
    popup = new Capture::PopupWin;
    c->popups.Add(popup);
    if (wm_connection() && g_damage_event_base >= 0) popup->damage = create_damage(window);
    if (wm_connection() && g_shape_event_base >= 0) xcb_shape_select_input(wm_connection(), window, true);
  }
  popup->x11_win = window;
  popup->pixmap_dirty = true;
  popup->x = x;
  popup->y = y;
  popup->w = attr->width;
  popup->h = attr->height;
  update_popup_shape(*popup);
  if (!popup->shape.GetSize())
  {
    canvas_remove_popup(c, window);
    return;
  }
  if (!c->popup_canvas) create_popup_canvas(c);
  if (c->root_popup == XCB_NONE && popup->input.GetSize()) c->root_popup = window;
  canvas_update_input(c);
  if (!c->view)
  {
    dismiss_popups(c);
    return;
  }
  if (c->popup_canvas)
  {
    swell_wayland_view_show(c->popup_canvas, true);
    swell_wayland_view_invalidate(c->popup_canvas);
  }
}

static void modal_request_close(Capture *c, xcb_window_t window)
{
  xcb_client_message_event_t event = {};
  event.response_type = XCB_CLIENT_MESSAGE;
  event.window = window;
  event.type = g_wm->atoms().WM_PROTOCOLS;
  event.format = 32;
  event.data.data32[0] = g_wm->atoms().WM_DELETE_WINDOW;
  event.data.data32[1] = XCB_CURRENT_TIME;
  xcb_send_event(c->connection, false, window, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
  xcb_flush(c->connection);
}

static void modal_close(SWELL_WaylandView *, void *data)
{
  Capture::ModalWin *m = (Capture::ModalWin *)data;
  if (!has_input_popup(m->cap)) modal_request_close(m->cap, m->x11_win);
}

static bool modal_draw(SWELL_WaylandView *, void *data, cairo_t *cr)
{
  Capture::ModalWin *m = (Capture::ModalWin *)data;
  if (!cr || !m->cap->connection) return false;
  const bool refresh = m->pixmap_dirty;
  if (refresh && m->pixmap.update(m->cap->connection, m->x11_win)) m->pixmap_dirty = false;
  if (!m->pixmap.pixmap) return false;
  cairo_surface_t *surface = read_pixels(m->cap, m->pixmap, m->image, refresh);
  if (!surface) return false;
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_surface(cr, surface, 0, 0);
  cairo_paint(cr);
  cairo_surface_destroy(surface);
  return true;
}

static void modal_motion(Capture::ModalWin *m, int x, int y, unsigned state)
{
  resume_pointer();
  raise_window(m->x11_win);
  int rx = 0, ry = 0;
  swell_xcb_translate(
      wm_connection(), m->x11_win, swell_xcb_screen(m->cap->connection)->root, x, y, &rx, &ry);
  fake_motion(m->cap->connection, rx, ry, state);
}

static bool modal_mouse(SWELL_WaylandView *view, void *data, UINT msg, int x, int y, int wheel, unsigned state)
{
  Capture::ModalWin *m = (Capture::ModalWin *)data;
  if (!m->cap->connection) return false;
  if (msg == SWELL_WAYLAND_MOUSE_LEAVE)
  {
    leave_pointer(m->cap, m->x11_win, state);
    return true;
  }
  bool pressed = false;
  const int button = mouse_button(msg, pressed);
  if (button && pressed) restore_input_focus(m->cap->connection, m->x11_win);
  modal_motion(m, x, y, state);
  if (button)
  {
    fake_button(m->cap->connection, button, pressed);
    if (pressed)
      m->buttons |= 1u << button;
    else
      m->buttons &= ~(1u << button);
    canvas_update_input(m->cap);
  }
  else if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL)
  {
    send_scroll(
        m->cap->connection, msg == WM_MOUSEHWHEEL ? wheel / 120 : 0, msg == WM_MOUSEWHEEL ? -wheel / 120 : 0);
  }
  xcb_flush(m->cap->connection);
  if (msg == SWELL_WAYLAND_MOUSE_ENTER) update_view_cursor(view);
  return true;
}

static bool modal_key(SWELL_WaylandView *, void *data, bool down, uint32_t code, uint32_t sym, unsigned state)
{
  Capture::ModalWin *m = (Capture::ModalWin *)data;
  if (m->cap->root_popup) return xw_bridge_forward_key(m->cap->hwnd, code, sym, state, down);
  forward_key(m->cap->connection, m->x11_win, code, state, down);
  return true;
}

static void modal_focus(SWELL_WaylandView *, void *data, bool active)
{
  if (active) set_active_capture(((Capture::ModalWin *)data)->cap);
}

static void modal_resize(SWELL_WaylandView *, void *data, int width, int height)
{
  Capture::ModalWin *m = (Capture::ModalWin *)data;
  const uint32_t size[] = {(uint32_t)width, (uint32_t)height};
  xcb_configure_window(
      m->cap->connection, m->x11_win, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, size);
  xcb_flush(m->cap->connection);
}

static const SWELL_WaylandViewCallbacks modal_callbacks = {
    modal_draw, modal_mouse, modal_key, modal_close, modal_focus, NULL, modal_resize};

static void modal_update_title(Capture::ModalWin &m)
{
  if (!m.view) return;
  const xcb_atom_t atoms[] = {g_wm->atoms()._NET_WM_NAME, XCB_ATOM_WM_NAME};
  for (xcb_atom_t atom : atoms)
  {
    xcb_get_property_reply_t *reply = xcb_get_property_reply(m.cap->connection,
        xcb_get_property(m.cap->connection, false, m.x11_win, atom, XCB_ATOM_ANY, 0, 1024), NULL);
    if (reply && reply->format == 8 && reply->type != XCB_ATOM_NONE)
    {
      const char *value = (const char *)xcb_get_property_value(reply);
      const int length = xcb_get_property_value_length(reply);
      char *title = reply->type == XCB_ATOM_STRING
                        ? g_convert(value, length, "UTF-8", "ISO-8859-1", NULL, NULL, NULL)
                        : g_utf8_make_valid(value, length);
      swell_wayland_view_set_title(m.view, title);
      g_free(title);
      free(reply);
      return;
    }
    free(reply);
  }
  swell_wayland_view_set_title(m.view, "");
}

static void modal_update_size_limits(Capture::ModalWin &m)
{
  if (!m.view) return;
  xcb_size_hints_t hints = {};
  xcb_icccm_get_wm_normal_hints_reply(
      m.cap->connection, xcb_icccm_get_wm_normal_hints(m.cap->connection, m.x11_win), &hints, NULL);
  int min_width = 0, min_height = 0, max_width = 0, max_height = 0;
  if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE)
  {
    min_width = max(0, hints.min_width);
    min_height = max(0, hints.min_height);
  }
  if (hints.flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE)
  {
    max_width = max(min_width, hints.max_width);
    max_height = max(min_height, hints.max_height);
  }
  swell_wayland_view_set_size_limits(m.view, min_width, min_height, max_width, max_height);
}

static void modal_create_view(Capture::ModalWin &m, int width, int height)
{
  RECT r = {0, 0, width, height};
  m.view =
      swell_wayland_view_create(m.cap->hwnd, m.cap->view, &r, SWELL_WAYLAND_TOPLEVEL, &modal_callbacks, &m);
  if (m.view)
  {
    swell_wayland_view_set_decorated(m.view, g_wm->window_decorated(m.x11_win));
    modal_update_size_limits(m);
    modal_update_title(m);
    swell_wayland_view_show(m.view, true);
    swell_wayland_view_focus(m.view);
  }
}

static void create_modal(Capture *c, xcb_window_t window, xcb_get_geometry_reply_t *attr)
{
  for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
  {
    Capture::ModalWin &m = *c->modals.Get(modal_idx);
    if (m.x11_win == window) return;
  }
  xcb_composite_redirect_window(c->connection, window, XCB_COMPOSITE_REDIRECT_AUTOMATIC);
  xcb_flush(c->connection);
  Capture::ModalWin &m = *c->modals.Add(new Capture::ModalWin);
  m.cap = c;
  m.x11_win = window;
  if (wm_connection() && g_damage_event_base >= 0) m.damage = create_damage(window);
  if (c->view) modal_create_view(m, attr->width, attr->height);
}

static void modal_remove(Capture *c, xcb_window_t window)
{
  for (int i = 0; i < c->modals.GetSize(); ++i)
  {
    Capture::ModalWin *modal = c->modals.Get(i);
    if (modal->x11_win != window) continue;
    for (int button = 1; button < 32; ++button)
      if (modal->buttons & (1u << button)) fake_button(c->connection, button, false);
    if (modal->buttons) xcb_flush(c->connection);
    if (i == c->modals.GetSize() - 1)
      while (c->popups.GetSize()) canvas_remove_popup(c, c->popups.GetLast()->x11_win);
    if (modal->damage && wm_connection()) xcb_damage_destroy(wm_connection(), modal->damage);
    if (modal->view) swell_wayland_view_destroy(modal->view);
    c->modals.Delete(i, true);
    break;
  }
}

static void handle_new_window(
    xcb_window_t win, Capture *state, xcb_get_geometry_reply_t *attr, bool override_redirect)
{
  if (!state || !state->connection || !win) return;
  auto bridge = g_bridges.Get(state->hwnd);
  if (bridge != NULL && bridge->external_gl) return;

  for (int popup_idx = 0; popup_idx < state->popups.GetSize(); ++popup_idx)
  {
    Capture::PopupWin &p = *state->popups.Get(popup_idx);
    if (p.x11_win == win)
    {
      canvas_add_popup(state, win, attr);
      return;
    }
  }
  for (int modal_idx = 0; modal_idx < state->modals.GetSize(); ++modal_idx)
  {
    Capture::ModalWin &m = *state->modals.Get(modal_idx);
    if (m.x11_win == win) return;
  }

  const XWaylandWM::WindowType type = g_wm->window_type(win, override_redirect);
  if (type == XWaylandWM::DRAG_ICON)
  {
    DEBUG_PRINT(
        "[DNDCOORD] ignoring XDND icon window 0x%lx (not compositing as popup)\n", (unsigned long)win);
    return;
  }

  if (type == XWaylandWM::POPUP)
  {
    canvas_add_popup(state, win, attr);
  }
  else
  {
    create_modal(state, win, attr);
  }
}

static void place_container(xcb_connection_t *connection, xcb_window_t container, SWELL_WaylandView *view,
    int width, int height, xcb_atom_t *monitor)
{
  const cairo_rectangle_int_t bounds = capture_monitor(connection, view, monitor);
  swell_xcb_move_resize(wm_connection(), container, bounds.x + wdl_max((bounds.width - width) / 2, 0),
      bounds.y + wdl_max((bounds.height - height) / 2, 0), width, height);
  xcb_flush(wm_connection());
}

static bool try_create_plugin(HWND hwnd)
{
  XWaylandBridge *bridge = g_bridges.Get(hwnd);
  if (!bridge) return true;
  if (bridge->external_gl) return true;
  if (!bridge->placed)
  {
    xw_size(hwnd);
    if (g_bridges.Get(hwnd) != bridge) return true;
  }
  if (!bridge->cap && bridge->connection && bridge->parent)
  {
    const xcb_window_t plugin = first_child(bridge->connection, bridge->parent);
    if (plugin)
    {
      xcb_get_geometry_reply_t *geometry =
          xcb_get_geometry_reply(bridge->connection, xcb_get_geometry(bridge->connection, plugin), NULL);
      if (!geometry) return false;
      place_container(bridge->connection, bridge->parent, bridge->view, geometry->width, geometry->height,
          &bridge->monitor);
      free(geometry);
      xcb_flush(bridge->connection);
      Capture *c = setup_capture(bridge->connection, bridge->parent, plugin, hwnd);
      if (!c) return false;
      c->view = bridge->view;
      c->source_x = bridge->source_x;
      c->source_y = bridge->source_y;
      bridge->cap = c;
      update_gui_window(c);
      if (c->view)
      {
        swell_wayland_view_reset_gl(c->view);
        swell_wayland_view_invalidate(c->view);
      }
    }
  }
  if (bridge->placed && bridge->cap) KillTimer(hwnd, 1);
  return true;
}

static Capture *find_window_owner(xcb_window_t window, bool &known_owner)
{
  for (unsigned int depth = 0; window && depth < 64; ++depth)
  {
    if (Capture *c = find_capture(window)) return c;
    for (const auto &kv : g_bridges)
      if (kv.val->parent == window)
      {
        known_owner = true;
        try_create_plugin(kv.key);
        return find_capture(window);
      }
    for (const auto &kv : g_captures)
    {
      Capture *c = kv.val;
      if (kv.key != c->parent_win) continue;
      for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
      {
        const Capture::PopupWin &p = *c->popups.Get(popup_idx);
        if (p.x11_win == window) return c;
      }
      for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
      {
        const Capture::ModalWin &m = *c->modals.Get(modal_idx);
        if (m.x11_win == window) return c;
      }
    }
    xcb_window_t transient = XCB_NONE;
    xcb_icccm_get_wm_transient_for_reply(
        wm_connection(), xcb_icccm_get_wm_transient_for(wm_connection(), window), &transient, NULL);
    if (transient)
    {
      known_owner = transient != swell_xcb_screen(wm_connection())->root;
      window = transient;
      continue;
    }
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(wm_connection(), xcb_query_tree(wm_connection(), window), NULL);
    window = tree ? tree->parent : XCB_NONE;
    free(tree);
  }
  return NULL;
}

static void on_popup_mapped(xcb_window_t w)
{
  const xcb_setup_t *setup = xcb_get_setup(wm_connection());
  if ((w & ~setup->resource_id_mask) == setup->resource_id_base) return;
  for (const auto &kv : g_bridges)
    if (kv.val->parent == w) return;
  xcb_get_window_attributes_cookie_t attributes_request = xcb_get_window_attributes(wm_connection(), w);
  xcb_get_geometry_cookie_t geometry_request = xcb_get_geometry(wm_connection(), w);
  xcb_query_tree_cookie_t tree_request = xcb_query_tree(wm_connection(), w);
  xcb_get_window_attributes_reply_t *attributes =
      xcb_get_window_attributes_reply(wm_connection(), attributes_request, NULL);
  xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(wm_connection(), geometry_request, NULL);
  xcb_query_tree_reply_t *tree = xcb_query_tree_reply(wm_connection(), tree_request, NULL);
  const bool top_level = tree && tree->parent == swell_xcb_screen(wm_connection())->root;
  free(tree);
  if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE ||
      attributes->_class != XCB_WINDOW_CLASS_INPUT_OUTPUT || !geometry || !top_level)
  {
    free(attributes);
    free(geometry);
    return;
  }
  const bool override_redirect = attributes->override_redirect;
  xcb_get_geometry_reply_t attr = *geometry;
  free(attributes);
  free(geometry);

  bool known_owner = false;
  if (Capture *owner = find_window_owner(w, known_owner))
  {
    handle_new_window(w, owner, &attr, override_redirect);
    return;
  }

  if (known_owner) return;

  if (g_active_capture && g_wm->owns_window(w, g_active_capture->gui_win))
  {
    handle_new_window(w, g_active_capture, &attr, override_redirect);
    return;
  }

  for (auto &kv : g_captures)
  {
    Capture *cand = kv.val;
    if (!cand || kv.key != cand->parent_win) continue;
    if (cand && g_wm->owns_window(w, cand->gui_win))
    {
      handle_new_window(w, cand, &attr, override_redirect);
      return;
    }
  }

  if (g_active_capture && g_wm->owns_window(w, g_active_capture->gui_win, true))
    handle_new_window(w, g_active_capture, &attr, override_redirect);
}

static void on_popup_unmapped(xcb_window_t w)
{
  for (auto &kv : g_captures)
  {
    Capture *c = kv.val;
    if (!c || kv.key != c->parent_win) continue;
    for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
    {
      const Capture::PopupWin &popup = *c->popups.Get(popup_idx);
      if (popup.x11_win == w)
      {
        canvas_remove_popup(c, w);
        return;
      }
    }
    for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
    {
      const Capture::ModalWin &modal = *c->modals.Get(modal_idx);
      if (modal.x11_win == w)
      {
        modal_remove(c, w);
        return;
      }
    }
  }
}

static void on_window_configured(xcb_window_t window, int width, int height)
{
  for (const auto &kv : g_captures)
  {
    Capture *c = kv.val;
    if (kv.key != c->parent_win) continue;
    for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
    {
      Capture::PopupWin &p = *c->popups.Get(popup_idx);
      if (p.x11_win != window) continue;
      int x, y;
      if (!swell_xcb_translate(c->connection, window, swell_xcb_screen(c->connection)->root, 0, 0, &x, &y))
        return;
      refresh_native_offset(c);
      const int old_x = p.x, old_y = p.y, old_w = p.w, old_h = p.h;
      p.x = x + c->native_x;
      p.y = y + c->native_y;
      if (p.w != width || p.h != height)
      {
        p.w = width;
        p.h = height;
        p.pixmap_dirty = true;
        update_popup_shape(p);
      }
      canvas_update_input(c);
      invalidate_popup(c, old_x, old_y, old_w, old_h);
      invalidate_popup(c, p.x, p.y, p.w, p.h);
      return;
    }
    for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
    {
      Capture::ModalWin &m = *c->modals.Get(modal_idx);
      if (m.x11_win != window) continue;
      RECT r = {};
      if (m.view) swell_wayland_view_rect(m.view, &r);
      const bool resized = !m.view || r.right - r.left != width || r.bottom - r.top != height;
      if (resized || m.pixmap.width != width || m.pixmap.height != height)
      {
        m.pixmap_dirty = true;
        r.right = r.left + width;
        r.bottom = r.top + height;
        if (m.view)
        {
          if (resized)
          {
            swell_wayland_view_move(m.view, &r);
            canvas_update_input(c);
          }
          swell_wayland_view_invalidate(m.view);
        }
      }
      return;
    }
  }
}

static void handle_damage_notify(xcb_damage_notify_event_t *event)
{
  xcb_damage_subtract(wm_connection(), event->damage, XCB_NONE, XCB_NONE);
  if (Capture *c = find_capture(event->drawable))
  {
    c->pixmap_dirty = true;
    if (c->view)
    {
      if (c->pixmap.width != event->geometry.width || c->pixmap.height != event->geometry.height)
        swell_wayland_view_invalidate(c->view);
      else
        invalidate_view(c->view, event->area.x - c->source_x, event->area.y - c->source_y, event->area.width,
            event->area.height);
    }
    return;
  }
  for (const auto &kv : g_captures)
  {
    Capture *c = kv.val;
    if (kv.key != c->parent_win) continue;
    for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
    {
      Capture::PopupWin &p = *c->popups.Get(popup_idx);
      if (p.x11_win == event->drawable)
      {
        p.pixmap_dirty = true;
        invalidate_popup(c, p.x + event->area.x, p.y + event->area.y, event->area.width, event->area.height);
        return;
      }
    }
    for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
    {
      Capture::ModalWin &m = *c->modals.Get(modal_idx);
      if (m.x11_win == event->drawable)
      {
        m.pixmap_dirty = true;
        invalidate_view(m.view, event->area.x, event->area.y, event->area.width, event->area.height);
        return;
      }
    }
  }
}

static void handle_xdnd_from_plugin(xcb_client_message_event_t *event)
{
  const WMAtoms &atoms = g_wm->atoms();
  xcb_client_message_event_t reply = {};
  reply.response_type = XCB_CLIENT_MESSAGE;
  reply.window = event->data.data32[0];
  if (!reply.window) return;
  if (event->type == atoms.XdndPosition)
    reply.type = atoms.XdndStatus;
  else if (event->type == atoms.XdndDrop)
    reply.type = atoms.XdndFinished;
  else
    return;
  reply.format = 32;
  reply.data.data32[0] = event->window;
  xcb_send_event(wm_connection(), false, reply.window, XCB_EVENT_MASK_NO_EVENT, (const char *)&reply);
  xcb_flush(wm_connection());
}

static void bridge_handle_event(xcb_generic_event_t *ev)
{
  const uint8_t type = ev->response_type & 0x7f;
  if ((ev->response_type & 0x80) && (type == XCB_KEY_PRESS || type == XCB_KEY_RELEASE))
  {
    const xcb_key_press_event_t *key = (xcb_key_press_event_t *)ev;
    for (const auto &item : g_bridges)
      if (item.val->parent == key->event)
      {
        swell_wayland_view_return_key(item.val->view, key->detail, key->state, type == XCB_KEY_PRESS);
        return;
      }
  }
  xcb_window_t dnd_catcher = g_wm ? g_wm->dnd_catcher_window() : 0;
  if (dnd_catcher &&
      ((type == XCB_MAP_NOTIFY && ((xcb_map_notify_event_t *)ev)->window == dnd_catcher) ||
          (type == XCB_UNMAP_NOTIFY && ((xcb_unmap_notify_event_t *)ev)->window == dnd_catcher) ||
          (type == XCB_CLIENT_MESSAGE && ((xcb_client_message_event_t *)ev)->window == dnd_catcher)))
    return;

  if (g_cursor_event_base >= 0 && type == g_cursor_event_base + XCB_XFIXES_CURSOR_NOTIFY)
  {
    for (const auto &item : g_bridges) update_view_cursor(item.val->view);
    for (const auto &item : g_captures)
    {
      Capture *c = item.val;
      if (item.key != c->parent_win) continue;
      update_canvas_cursor(c, true);
      for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
      {
        const Capture::ModalWin &modal = *c->modals.Get(modal_idx);
        update_view_cursor(modal.view);
      }
    }
    return;
  }

  if (g_damage_event_base >= 0 && type == g_damage_event_base + XCB_DAMAGE_NOTIFY)
  {
    handle_damage_notify((xcb_damage_notify_event_t *)ev);
    return;
  }

  if (g_shape_event_base >= 0 && type == g_shape_event_base + XCB_SHAPE_NOTIFY)
  {
    xcb_shape_notify_event_t *shape = (xcb_shape_notify_event_t *)ev;
    if (shape->shape_kind != XCB_SHAPE_SK_BOUNDING && shape->shape_kind != XCB_SHAPE_SK_INPUT) return;
    for (auto &kv : g_captures)
    {
      Capture *c = kv.val;
      if (kv.key != c->parent_win) continue;
      for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
      {
        Capture::PopupWin &p = *c->popups.Get(popup_idx);
        if (p.x11_win != shape->affected_window) continue;
        update_popup_shape(p);
        if (!p.shape.GetSize())
        {
          canvas_remove_popup(c, p.x11_win);
          return;
        }
        if (!c->popup_canvas)
        {
          create_popup_canvas(c);
          if (c->popup_canvas) swell_wayland_view_show(c->popup_canvas, true);
        }
        canvas_update_input(c);
        if (shape->shape_kind == XCB_SHAPE_SK_BOUNDING) invalidate_popup(c, p.x, p.y, p.w, p.h);
        return;
      }
    }
    if (shape->shape_kind == XCB_SHAPE_SK_BOUNDING) on_popup_mapped(shape->affected_window);
    return;
  }

  if (type == XCB_PROPERTY_NOTIFY)
  {
    xcb_property_notify_event_t *property = (xcb_property_notify_event_t *)ev;
    if (property->atom == g_wm->atoms()._XEMBED_INFO)
    {
      Capture *c = find_capture(property->window);
      if (c && c->plugin_win == property->window) update_xembed(c);
      return;
    }
    if (property->atom == XCB_ATOM_WM_NAME || property->atom == g_wm->atoms()._NET_WM_NAME ||
        property->atom == XCB_ATOM_WM_NORMAL_HINTS || property->atom == g_wm->atoms().motif_hints)
      for (const auto &kv : g_captures)
      {
        Capture *c = kv.val;
        if (kv.key != c->parent_win) continue;
        for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
        {
          Capture::ModalWin &m = *c->modals.Get(modal_idx);
          if (m.x11_win == property->window)
          {
            if (property->atom == g_wm->atoms().motif_hints)
              swell_wayland_view_set_decorated(m.view, g_wm->window_decorated(m.x11_win));
            else if (property->atom == XCB_ATOM_WM_NORMAL_HINTS)
              modal_update_size_limits(m);
            else
              modal_update_title(m);
            return;
          }
        }
      }
    return;
  }

  if (type == XCB_CLIENT_MESSAGE)
  {
    handle_xdnd_from_plugin((xcb_client_message_event_t *)ev);
    return;
  }

  if (type == XCB_DESTROY_NOTIFY || type == XCB_REPARENT_NOTIFY)
  {
    const xcb_window_t window = ((xcb_destroy_notify_event_t *)ev)->window;
    for (auto &kv : g_captures)
    {
      Capture *c = kv.val;
      if (kv.key != c->parent_win || c->plugin_win != window ||
          (int32_t)(ev->full_sequence - c->sequence) < 0)
        continue;
      if (type == XCB_REPARENT_NOTIFY)
      {
        if (((xcb_reparent_notify_event_t *)ev)->parent == c->parent_win) continue;
        xcb_xfixes_change_save_set(c->connection, XCB_SET_MODE_DELETE, XCB_XFIXES_SAVE_SET_TARGET_ROOT,
            XCB_XFIXES_SAVE_SET_MAPPING_UNMAP, window);
        xcb_flush(c->connection);
      }
      HWND hwnd = c->hwnd;
      XWaylandBridge *bridge = (XWaylandBridge *)hwnd->m_private_data;
      swell_wayland_view_reset_gl(c->view);
      bridge->cap = NULL;
      cleanup_capture(c);
      if (bridge->view) swell_wayland_view_invalidate(bridge->view);
      if (!bridge->external_gl) SetTimer(hwnd, 1, 100, NULL);
      return;
    }
  }
  if (type == XCB_MAP_NOTIFY || type == XCB_DESTROY_NOTIFY || type == XCB_REPARENT_NOTIFY)
  {
    const xcb_window_t parent = ((xcb_map_notify_event_t *)ev)->event;
    Capture *c = find_capture(parent);
    if (c && c->plugin_win == parent)
    {
      update_gui_window(c);
      return;
    }
  }

  if (type != XCB_MAP_NOTIFY && type != XCB_UNMAP_NOTIFY && type != XCB_CONFIGURE_NOTIFY) return;
  Capture *c = find_capture(((xcb_map_notify_event_t *)ev)->window);
  if (c) return;
  switch (type)
  {
    case XCB_MAP_NOTIFY:
      on_popup_mapped(((xcb_map_notify_event_t *)ev)->window);
      break;
    case XCB_UNMAP_NOTIFY:
      on_popup_unmapped(((xcb_unmap_notify_event_t *)ev)->window);
      break;
    case XCB_CONFIGURE_NOTIFY:
      on_window_configured(((xcb_configure_notify_event_t *)ev)->window,
          ((xcb_configure_notify_event_t *)ev)->width, ((xcb_configure_notify_event_t *)ev)->height);
      break;
    default:
      break;
  }
}

static void dismiss_popups(Capture *c)
{
  if (!c || !c->connection || !c->popups.GetSize()) return;
  park_pointer(0, 0);
  if (c->root_popup)
  {
    int levels = 0;
    for (int popup_idx = 0; popup_idx < c->popups.GetSize(); ++popup_idx)
    {
      const Capture::PopupWin &p = *c->popups.Get(popup_idx);
      if (p.input.GetSize()) ++levels;
    }
    xcb_key_symbols_t *symbols = xcb_key_symbols_alloc(c->connection);
    xcb_keycode_t *codes = symbols ? xcb_key_symbols_get_keycode(symbols, XKB_KEY_Escape) : NULL;
    const int keycode = codes ? codes[0] : 0;
    free(codes);
    if (symbols) xcb_key_symbols_free(symbols);
    if (keycode)
      for (int i = 0; i < wdl_max(levels, 1); ++i)
      {
        forward_key(c->connection, c->root_popup, keycode, 0, true);
        forward_key(c->connection, c->root_popup, keycode, 0, false);
      }
    xcb_set_input_focus(
        c->connection, XCB_INPUT_FOCUS_POINTER_ROOT, swell_xcb_screen(c->connection)->root, XCB_CURRENT_TIME);
    xcb_flush(c->connection);
  }
}

bool xw_bridge_swell_on_button_event_escape()
{
  bool any = xw_bridge_raise_modals();
  for (auto &kv : g_captures)
  {
    Capture *c = kv.val;
    if (!c || kv.key != c->parent_win) continue;
    if (has_input_popup(c))
    {
      dismiss_popups(c);
      any = true;
    }
  }
  return any;
}

static bool capture_owned_by(const Capture *c, HWND hwnd)
{
  HWND owner = c->hwnd;
  while (owner && owner != hwnd) owner = owner->m_parent ? owner->m_parent : owner->m_owner;
  return owner != NULL;
}

bool xw_bridge_popup_blocks_close(HWND hwnd)
{
  for (auto &kv : g_captures)
  {
    Capture *c = kv.val;
    if (kv.key != c->parent_win) continue;
    if (capture_owned_by(c, hwnd) && has_input_popup(c)) return true;
  }
  return false;
}

bool xw_bridge_close_owned(HWND hwnd)
{
  bool any = false;
  for (auto &kv : g_captures)
  {
    Capture *c = kv.val;
    if (!c || kv.key != c->parent_win) continue;
    if (!capture_owned_by(c, hwnd)) continue;
    if (c->popups.GetSize())
    {
      any = true;
      dismiss_popups(c);
      while (c->popups.GetSize()) canvas_remove_popup(c, c->popups.GetLast()->x11_win);
    }
    if (c->modals.GetSize() && c->connection)
    {
      for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
      {
        Capture::ModalWin &md = *c->modals.Get(modal_idx);
        if (!md.x11_win) continue;
        any = true;
        modal_request_close(c, md.x11_win);
      }
      xcb_flush(c->connection);
    }
  }
  return any;
}

void init_private_xwayland()
{
  if (!XWaylandWM::init_bridge_wm()) return;

  const xcb_query_extension_reply_t *damage = xcb_get_extension_data(wm_connection(), &xcb_damage_id);
  if (damage && damage->present)
  {
    free(xcb_damage_query_version_reply(
        wm_connection(), xcb_damage_query_version(wm_connection(), 1, 1), NULL));
    g_damage_event_base = damage->first_event;
  }
  const xcb_query_extension_reply_t *shape = xcb_get_extension_data(wm_connection(), &xcb_shape_id);
  if (shape && shape->present)
  {
    free(xcb_shape_query_version_reply(wm_connection(), xcb_shape_query_version(wm_connection()), NULL));
    g_shape_event_base = shape->first_event;
  }

  g_wm->on_unhandled_event = bridge_handle_event;
  g_wm->dnd_init();
  const xcb_query_extension_reply_t *fixes = xcb_get_extension_data(wm_connection(), &xcb_xfixes_id);
  if (fixes && fixes->present)
  {
    g_cursor_event_base = fixes->first_event;
    xcb_xfixes_select_cursor_input(wm_connection(), swell_xcb_screen(wm_connection())->root,
        XCB_XFIXES_CURSOR_NOTIFY_MASK_DISPLAY_CURSOR);
    xcb_flush(wm_connection());
  }
}

static void detach_view(XWaylandBridge *bridge)
{
  Capture *c = bridge->cap;
  if (c)
  {
    for (int button = 1; button < 32; ++button)
      if (c->view_buttons & (1u << button)) fake_button(c->connection, button, false);
    c->view_buttons = 0;
    dismiss_popups(c);
    while (c->popups.GetSize()) canvas_remove_popup(c, c->popups.GetLast()->x11_win);
    for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
    {
      Capture::ModalWin &m = *c->modals.Get(modal_idx);
      for (int button = 1; button < 32; ++button)
        if (m.buttons & (1u << button)) fake_button(c->connection, button, false);
      m.buttons = 0;
      SWELL_WaylandView *view = m.view;
      m.view = NULL;
      if (view) swell_wayland_view_destroy(view);
    }
    xcb_flush(c->connection);
  }
  if (bridge->view) swell_wayland_view_destroy(bridge->view);
  bridge->view = NULL;
  bridge->placed = false;
  if (c) c->view = NULL;
}

static void xw_destroy(HWND hwnd)
{
  XWaylandBridge *bridge = g_bridges.Get(hwnd);
  if (!bridge) return;
  g_source_destroy(bridge->event_source);
  g_source_unref(bridge->event_source);
  KillTimer(hwnd, 1);
  const xcb_window_t plugin = bridge->cap ? bridge->cap->plugin_win : XCB_NONE;
  if (bridge->cap)
  {
    cleanup_capture(bridge->cap);
    bridge->cap = NULL;
  }
  if (bridge->view) swell_wayland_view_destroy(bridge->view);
  if (bridge->connection)
  {
    xcb_query_tree_reply_t *tree =
        xcb_query_tree_reply(bridge->connection, xcb_query_tree(bridge->connection, bridge->parent), NULL);
    if (tree)
    {
      const xcb_window_t *children = xcb_query_tree_children(tree);
      const int count = xcb_query_tree_children_length(tree);
      int child_index = 0;
      while (child_index < count && children[child_index] != plugin) ++child_index;
      if (plugin && child_index == count)
        xcb_xfixes_change_save_set(bridge->connection, XCB_SET_MODE_DELETE, XCB_XFIXES_SAVE_SET_TARGET_ROOT,
            XCB_XFIXES_SAVE_SET_MAPPING_UNMAP, plugin);
      for (int i = 0; i < count; ++i)
        xcb_xfixes_change_save_set(bridge->connection, XCB_SET_MODE_INSERT, XCB_XFIXES_SAVE_SET_TARGET_ROOT,
            XCB_XFIXES_SAVE_SET_MAPPING_UNMAP, children[i]);
      swell_xcb_sync(bridge->connection);
      free(tree);
    }
    xcb_render_util_disconnect(bridge->connection);
    xcb_flush(bridge->connection);
    xcb_disconnect(bridge->connection);
  }
  hwnd->m_private_data = 0;
  g_bridges.Delete(hwnd);
  delete bridge;
}

static void xw_size(HWND hwnd)
{
  XWaylandBridge *bridge = g_bridges.Get(hwnd);
  if (!bridge) return;
  if (bridge->external_gl) return;
  if (!IsWindowVisible(hwnd))
  {
    detach_view(bridge);
    return;
  }
  HWND top = hwnd->m_parent;
  while (top && !top->m_oswindow) top = top->m_parent;
  if (!top)
  {
    detach_view(bridge);
    return;
  }
  hwnd->Retain();
  top->Retain();
  POINT origin = {0, 0};
  ClientToScreen(hwnd, &origin);
  HWND current = hwnd->m_parent;
  while (current && !current->m_oswindow) current = current->m_parent;
  if (hwnd->m_hashaddestroy || top->m_hashaddestroy || current != top ||
      !g_bridges.Exists(hwnd) || bridge->external_gl)
  {
    top->Release();
    hwnd->Release();
    return;
  }
  const RECT size = hwnd->m_position;
  const int width = wdl_max(size.right - size.left, 1), height = wdl_max(size.bottom - size.top, 1);
  RECT visible = {origin.x, origin.y, origin.x + width, origin.y + height};
  bool valid = true;
  for (HWND parent = hwnd->m_parent; parent;)
  {
    parent->Retain();
    RECT client;
    GetClientRect(parent, &client);
    POINT point = {0, 0};
    if (!parent->m_hashaddestroy && !hwnd->m_hashaddestroy) ClientToScreen(parent, &point);
    valid = !parent->m_hashaddestroy && !hwnd->m_hashaddestroy;
    if (valid)
    {
      visible.left = max(visible.left, point.x + client.left);
      visible.top = max(visible.top, point.y + client.top);
      visible.right = min(visible.right, point.x + client.right);
      visible.bottom = min(visible.bottom, point.y + client.bottom);
    }
    HWND next = valid && parent != top ? parent->m_parent : NULL;
    parent->Release();
    if (!valid) break;
    parent = next;
  }
  current = hwnd->m_parent;
  while (current && !current->m_oswindow) current = current->m_parent;
  if (!valid || top->m_hashaddestroy || current != top || !g_bridges.Exists(hwnd) ||
      bridge->external_gl)
  {
    top->Release();
    hwnd->Release();
    return;
  }
  const bool empty = visible.right <= visible.left || visible.bottom <= visible.top;
  bridge->source_x = visible.left - origin.x;
  bridge->source_y = visible.top - origin.y;
  RECT r = {visible.left - top->m_position.left, visible.top - top->m_position.top,
      visible.right - top->m_position.left, visible.bottom - top->m_position.top};
  if (bridge->host != top)
  {
    detach_view(bridge);
    bridge->host = top;
  }
  if (!empty && !bridge->view)
    bridge->view =
        swell_wayland_view_create(hwnd, NULL, &r, SWELL_WAYLAND_SUBSURFACE, &editor_callbacks, hwnd);
  else if (!empty)
    swell_wayland_view_move(bridge->view, &r);
  if (empty && bridge->cap && bridge->cap->view_buttons)
  {
    for (int button = 1; button < 32; ++button)
      if (bridge->cap->view_buttons & (1u << button)) fake_button(bridge->connection, button, false);
    bridge->cap->view_buttons = 0;
    xcb_flush(bridge->connection);
  }
  if (bridge->view) swell_wayland_view_show(bridge->view, !empty && IsWindowVisible(hwnd));
  const bool changed = !bridge->has_last_size_pos || memcmp(&size, &bridge->last_size_pos, sizeof(size));
  if (bridge->cap)
  {
    Capture *c = bridge->cap;
    c->view = bridge->view;
    c->source_x = bridge->source_x;
    c->source_y = bridge->source_y;
    if (changed)
    {
      refresh_pixmap(c);
      if (c->popup_canvas)
      {
        const uint32_t dimensions[] = {(uint32_t)width, (uint32_t)height};
        xcb_configure_window(wm_connection(), c->parent_win,
            XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, dimensions);
        xcb_flush(wm_connection());
      }
      else
      {
        place_container(c->connection, c->parent_win, c->view, width, height, &bridge->monitor);
        xcb_flush(c->connection);
      }
    }
    for (int modal_idx = 0; modal_idx < c->modals.GetSize(); ++modal_idx)
    {
      Capture::ModalWin &m = *c->modals.Get(modal_idx);
      if (!m.view && c->view)
      {
        xcb_get_geometry_reply_t *geometry =
            xcb_get_geometry_reply(c->connection, xcb_get_geometry(c->connection, m.x11_win), NULL);
        if (geometry)
        {
          modal_create_view(m, geometry->width, geometry->height);
          free(geometry);
        }
      }
    }
    refresh_native_offset(c);
    canvas_update_input(c);
  }
  bridge->placed = empty || (bridge->view && swell_wayland_view_surface(bridge->view));
  bridge->last_size_pos = size;
  bridge->has_last_size_pos = true;
  top->Release();
  hwnd->Release();
}

static void xw_drop_files(HWND hwnd, HDROP files)
{
  auto found = g_bridges.Get(hwnd);
  if (!found || !g_wm) return;
  XWaylandBridge *bridge = found;
  POINT point;
  DragQueryPoint(files, &point);
  xcb_connection_t *connection = wm_connection();
  const xcb_window_t root = swell_xcb_screen(connection)->root;
  if (!swell_xcb_translate(connection, bridge->parent, root, point.x, point.y, &point.x, &point.y)) return;
  xcb_window_t target = XCB_NONE, window = bridge->parent;
  unsigned version = 0;
  while (window)
  {
    xcb_get_property_reply_t *property = xcb_get_property_reply(connection,
        xcb_get_property(connection, false, window, g_wm->atoms().XdndAware, XCB_ATOM_ATOM, 0, 1), NULL);
    if (property && property->format == 32 && xcb_get_property_value_length(property) == 4)
    {
      target = window;
      version = *(const uint32_t *)xcb_get_property_value(property);
    }
    free(property);
    xcb_translate_coordinates_reply_t *position = xcb_translate_coordinates_reply(connection,
        xcb_translate_coordinates(connection, root, window, point.x, point.y), NULL);
    window = position ? position->child : XCB_NONE;
    free(position);
  }
  if (!target || version < 3) return;
  WDL_FastString uris;
  WDL_TypedBuf<char> path;
  const UINT count = DragQueryFile(files, 0xffffffff, NULL, 0);
  for (UINT i = 0; i < count; ++i)
  {
    const UINT length = DragQueryFile(files, i, NULL, 0);
    if (!path.Resize(length + 1)) return;
    DragQueryFile(files, i, path.Get(), length + 1);
    gchar *uri = g_filename_to_uri(path.Get(), NULL, NULL);
    if (uri)
    {
      uris.Append(uri);
      uris.Append("\r\n");
      g_free(uri);
    }
  }
  g_wm->drop_files(target, point, uris, version);
}

static LRESULT xw_bridge_proc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM)
{
  switch (msg)
  {
    case WM_DROPFILES:
      xw_drop_files(hwnd, (HDROP)wParam);
      break;
    case WM_DESTROY:
      xw_destroy(hwnd);
      break;
    case WM_TIMER:
      try_create_plugin(hwnd);
      break;
    case WM_MOVE:
    case WM_SIZE:
      xw_size(hwnd);
      break;
  }
  return 0;
}

static void update_children(HWND hwnd, bool attach)
{
  WDL_PtrList<HWND__> windows;
  for (const auto &kv : g_bridges)
    if ((kv.key == hwnd || IsChild(hwnd, kv.key)) && windows.Add(kv.key)) kv.key->Retain();
  for (int i = 0; i < windows.GetSize(); ++i)
  {
    HWND child = windows.Get(i);
    XWaylandBridge *bridge = g_bridges.Get(child);
    if (!child->m_hashaddestroy && bridge && (child == hwnd || IsChild(hwnd, child)))
    {
      if (attach)
      {
        xw_size(child);
        try_create_plugin(child);
      }
      else
        detach_view(bridge);
    }
    child->Release();
  }
}

void xw_bridge_attach_children(HWND hwnd)
{
  update_children(hwnd, true);
}

void xw_bridge_detach_children(HWND hwnd)
{
  update_children(hwnd, false);
}

bool xw_bridge_set_external_gl(HWND hwnd, bool enabled)
{
  XWaylandBridge *bridge = g_bridges.Get(hwnd);
  if (!bridge) return false;
  if (bridge->external_gl == enabled) return true;
  bridge->external_gl = enabled;
  if (enabled)
  {
    KillTimer(hwnd, 1);
    detach_view(bridge);
  }
  else
  {
    xw_size(hwnd);
    if (!g_bridges.Exists(hwnd)) return false;
    SetTimer(hwnd, 1, 100, NULL);
    try_create_plugin(hwnd);
  }
  return g_bridges.Exists(hwnd);
}

HWND xw_bridge_create(HWND parent, void **wref, const RECT *rect, const char *bridge_class_name)
{
  *wref = NULL;
  if (!g_wm) init_private_xwayland();
  xcb_connection_t *connection = g_wm ? xcb_connect(XWaylandWM::display_name(), NULL) : NULL;
  if (!connection || xcb_connection_has_error(connection))
  {
    if (connection) xcb_disconnect(connection);
    HWND hwnd = new HWND__(parent, 0, rect, NULL, false, NULL);
    hwnd->m_classname = bridge_class_name;
    return hwnd;
  }
  const auto composite = xcb_composite_query_version(connection, 0, 4);
  const auto fixes = xcb_xfixes_query_version(connection, 5, 0);
  const auto randr = xcb_randr_query_version(connection, 1, 5);
  const auto xkb = xcb_xkb_use_extension(connection, 1, 0);
  free(xcb_composite_query_version_reply(connection, composite, NULL));
  free(xcb_xfixes_query_version_reply(connection, fixes, NULL));
  free(xcb_randr_query_version_reply(connection, randr, NULL));
  free(xcb_xkb_use_extension_reply(connection, xkb, NULL));
  xcb_screen_t *screen = swell_xcb_screen(connection);
  const int width = wdl_max(rect->right - rect->left, 1), height = wdl_max(rect->bottom - rect->top, 1);
  const xcb_window_t container = xcb_generate_id(connection);
  const uint32_t attributes[] = {screen->white_pixel, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_KEY_RELEASE};
  xcb_create_window(connection, XCB_COPY_FROM_PARENT, container, screen->root, 0, 0, width, height, 0,
      XCB_WINDOW_CLASS_INPUT_OUTPUT, XCB_COPY_FROM_PARENT, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, attributes);
  xcb_icccm_wm_hints_t hints = {};
  xcb_icccm_wm_hints_set_input(&hints, false);
  xcb_icccm_set_wm_hints(connection, container, &hints);
  xcb_map_window(connection, container);
  xcb_flush(connection);
  HWND hwnd = new HWND__(parent, 0, rect, NULL, true, xw_bridge_proc);
  hwnd->m_classname = bridge_class_name;
  XWaylandBridge *bridge = new XWaylandBridge();
  bridge->connection = connection;
  bridge->event_source = watch_xcb_connection(connection);
  bridge->parent = container;
  hwnd->m_private_data = (INT_PTR)bridge;
  g_bridges.Insert(hwnd, bridge);
  *wref = (void *)(uintptr_t)container;
  hwnd->Retain();
  xw_size(hwnd);
  if (hwnd->m_hashaddestroy || !g_bridges.Exists(hwnd))
  {
    *wref = NULL;
    hwnd->Release();
    return NULL;
  }
  place_container(connection, container, bridge->view, width, height, &bridge->monitor);
  xcb_flush(connection);
  SetTimer(hwnd, 1, 100, NULL);
  hwnd->Release();
  return hwnd;
}

bool xw_bridge_key_wanted_by_host(HWND hwnd, uint32_t code, unsigned state)
{
  XWaylandBridge *bridge = g_bridges.Get(hwnd);
  if (!bridge || bridge->external_gl) return false;
  Capture *c = bridge->cap;
  if (c && (c->popups.GetSize() || c->modals.GetSize())) return false;
  void *hook = GetProp(hwnd, "SWELL_XBRIDGE_KBHOOK_CHECK");
  HWND parent = hook ? GetParent(hwnd) : NULL;
  if (!parent) return false;
  if (input_focus_in(bridge->connection, bridge->parent)) return false;
  hwnd->Retain();
  const bool handled = SendMessage(parent, (UINT)(INT_PTR)hook, code, state) != 0;
  const bool valid = !hwnd->m_hashaddestroy && g_bridges.Get(hwnd) == bridge && GetFocus() == hwnd;
  hwnd->Release();
  return handled || !valid;
}

bool xw_bridge_forward_key(HWND hwnd, uint32_t code, uint32_t sym, unsigned state, bool down)
{
  auto bridge = g_bridges.Get(hwnd);
  if (bridge != NULL && bridge->external_gl) return false;
  Capture *c = capture_for_hwnd(hwnd);
  if (!c || !c->connection) return false;
  xcb_window_t target = c->root_popup && c->popups.GetSize() ? c->root_popup :
    c->gui_win ? c->gui_win : c->plugin_win;
  if (!target) return false;
  if (c->root_popup && sym == XKB_KEY_Escape)
  {
    if (down) dismiss_popups(c);
    return true;
  }
  if (!c->root_popup)
  {
    const xcb_window_t focus = input_focus_in(c->connection, c->parent_win);
    if (focus && focus != c->parent_win) target = focus;
  }
  forward_key(c->connection, target, code, state, down);
  return true;
}
