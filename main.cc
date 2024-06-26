#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/extensions/XRes.h>

#include <barrier>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <string_view>
#include <sys/mman.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#ifndef USE_VFORK
#  ifdef __linux__
#    define USE_VFORK
#  endif
#else
#  if !defined(__linux__)
#    error You are compiling on a system that is not Linux but explicitly enabled USE_VFORK, make sure your system shares virtual memory with child processes in vfork or this program will not work!
#  endif
#endif

Display *dpy;

pid_t ppid(int pid) {
  std::ifstream status("/proc/" + std::to_string(pid) + "/status");
  std::string line;

  while (std::getline(status, line)) {
    if (line.starts_with("PPid:"))
      return std::stoi(line.data() + 6);
  }

  throw std::runtime_error("could not get parent pid of " +
                           std::to_string(pid));
}

std::optional<pid_t> window_to_pid(Window window) {
  XResClientIdSpec spec;
  spec.client = window;
  spec.mask = XRES_CLIENT_ID_XID;

  long count;
  XResClientIdValue *output;
  XResQueryClientIds(dpy, 1, &spec, &count, &output);

  std::optional<pid_t> pid;

  for (auto i = 0; i < count; ++i)
    if (output[i].spec.mask == XRES_CLIENT_ID_PID_MASK) {
      pid = *(pid_t *)output[i].value;
      break;
    }

  XResClientIdsDestroy(count, output);

  return pid;
}

void collect_candidate_windows(Window window,
                               std::unordered_multimap<pid_t, Window> &out) {
  if (auto pid = window_to_pid(window)) {
    XTextProperty text;
    XGetWMName(dpy, window, &text);
    // Only grab windows with names to filter out garbage
    // More heuristics could be implemented in the future
    if (text.value)
      out.emplace(*pid, window);
  }

  Window root, parent;
  Window *children;
  unsigned n;
  XQueryTree(dpy, window, &root, &parent, &children, &n);

  if (children != NULL) {
    for (unsigned i = 0; i < n; i++)
      collect_candidate_windows(children[i], out);
    XFree(children);
  }
}

int main(int argc, char **argv) {
  char const *program_name = "better-swallow";

  if (argc >= 1)
    program_name = argv[0];

  if (argc <= 1) {
    std::cerr << "usage: " << program_name << " <command> [args...]\n";
    return 1;
  }

  dpy = XOpenDisplay(NULL);

  pid_t child_pid;
  std::barrier sync(2);

  std::jthread worker([&child_pid, &sync](std::stop_token const &token) {
    {
      int major_opcode;
      int first_event;
      int first_error;
      if (!XQueryExtension(dpy, "X-Resource", &major_opcode, &first_event,
                           &first_error)) {
        std::cerr << "\x1b[31merror\x1b[0m: X-Resource extension not supported "
                     "by X server\n";
        exit(1);
      }
    }

    Atom swallow_atom = XInternAtom(dpy, "_BETTER_SWALLOW", False);

    XTextProperty prop;
    XGetTextProperty(dpy, XDefaultRootWindow(dpy), &prop, swallow_atom);
    bool has_patch =
        prop.value && !std::strcmp((char *)prop.value, "supported");

    Window swallower;
    {
      std::unordered_multimap<pid_t, Window> pid_to_window;
      collect_candidate_windows(XDefaultRootWindow(dpy), pid_to_window);
      bool found_reliable_parent = false;

      pid_t parent = getppid();

      while (true) {
        auto [begin, end] = pid_to_window.equal_range(parent);

        if (begin != end) {
          // Don't risk grabbing the wrong window when there are mutiple
          if (std::next(begin) != end)
            break;

          swallower = begin->second;
          found_reliable_parent = true;
          break;
        } else {
          parent = ppid(parent);
          if (parent == 1)
            break;
        }
      }

      if (!found_reliable_parent) {
        std::cerr << "Failed to find swallower through reliable method, "
                     "falling back to input focus\n";

        int rev;
        XGetInputFocus(dpy, &swallower, &rev);
      }
    }

    XSelectInput(dpy, XDefaultRootWindow(dpy), SubstructureNotifyMask);
    XSync(dpy, true);

    sync.arrive_and_wait();

    if (has_patch) {
      XEvent event;
      event.xclient.type = ClientMessage;
      event.xclient.message_type = swallow_atom;
      event.xclient.format = 32;
      event.xclient.window = swallower;
      event.xclient.data.l[0] = child_pid;
      XSendEvent(dpy, XDefaultRootWindow(dpy), False,
                 SubstructureRedirectMask | SubstructureNotifyMask, &event);
      XSync(dpy, true);
      sync.arrive_and_wait();
      return; // From here on, dwm will manage the swallowing itself
    } else
      sync.arrive_and_wait();

    std::unordered_set<Window> child_windows;
    while (true) {
      if (token.stop_requested())
        break;
      else if (XEventsQueued(dpy, QueuedAfterFlush) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      XEvent event;
      XNextEvent(dpy, &event);

      if (event.type == MapNotify) {
        if (window_to_pid(event.xmap.window) == child_pid) {
          if (child_windows.empty())
            XUnmapWindow(dpy, swallower);
          child_windows.insert(event.xmap.window);
        }
      } else if (event.type == UnmapNotify) {
        if (child_windows.erase(event.xmap.window))
          if (child_windows.empty())
            XMapWindow(dpy, swallower);
      }

      XFreeEventData(dpy, &event.xcookie);
    }

    if (!child_windows.empty())
      XMapWindow(dpy, swallower);

    XCloseDisplay(dpy);
  });

  // NOTE: Why is this here?
  //       Theoretically, there is a practically impossible race condition in
  //       the no-vfork implementation where the child process creates a window
  //       before we register out swallow in dwm. With the below vfork
  //       implementation, the swallow definition is registered before the child
  //       process is executed so this becomes impossible. (unless the X server
  //       does something funny with the event order, which I don't think is
  //       ever a problem)
#ifdef USE_VFORK
  // For good measure. (don't want to have child_pid be put in a register)
  __asm__("" ::: "memory");
  int ret = vfork();
  if (ret < 0) {
    perror("vfork");
    exit(1);
  } else if (ret == 0) {
    // NOTE: This and the sync calls are undefined behaviour :)
    //       POSIX decided that vfork will result in "undefined behaviour"
    //       whenever the program modifies anything else
    //       than a pid_t variable for the result of the fork.
    //       This means that this code is linux specific.
    //       The same behaviour can be implemented using fork() and shared
    //       memory but that complicates the implementation for no added
    //       benefit.
    child_pid = getpid();
    close(ConnectionNumber(dpy));
    sync.arrive_and_wait();
    sync.arrive_and_wait();
    execvp(argv[1], argv + 1);
    perror("execvp");
    exit(255);
  }
#else
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addclose(&fa, ConnectionNumber(dpy));
  if (posix_spawnp(&child_pid, argv[1], &fa, nullptr, argv + 1, environ)) {
    perror("posix_spawnp failed");
    exit(1);
  }
  sync.arrive_and_wait();
  sync.arrive_and_wait();
  posix_spawn_file_actions_destroy(&fa);
#endif

  int status;
  while (true) {
    auto ret = waitpid(child_pid, &status, 0);
    if (ret == child_pid)
      break;
    if (ret != EINTR) {
      perror("waitpid failed");
      exit(1);
    }
  }

  worker.request_stop();

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  else
    return 1;
}
