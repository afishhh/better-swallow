#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/extensions/XRes.h>

#include <barrier>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

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

// TODO: Can we assume there is only one pid per window? I think so
std::optional<pid_t> window_to_pid(Display *display, Window window) {
  XResClientIdSpec spec;
  spec.client = window;
  spec.mask = XRES_CLIENT_ID_XID;

  long count;
  XResClientIdValue *output;
  XResQueryClientIds(display, 1, &spec, &count, &output);

  std::optional<pid_t> pid;

  for (auto i = 0; i < count; ++i)
    if (output[i].spec.mask == XRES_CLIENT_ID_PID_MASK) {
      pid = *(pid_t *)output[i].value;
      break;
    }

  XResClientIdsDestroy(count, output);

  return pid;
}

void collect_candidate_windows(Display *display, Window window,
                               std::unordered_multimap<pid_t, Window> &out) {
  if (auto pid = window_to_pid(display, window)) {
    XTextProperty text;
    XGetWMName(display, window, &text);
    // Only grab windows with names to filter out garbage
    // More heuristics could be implemented in the future
    if (text.value)
      out.emplace(*pid, window);
  }

  Window root, parent;
  Window *children;
  unsigned n;
  XQueryTree(display, window, &root, &parent, &children, &n);

  if (children != NULL) {
    for (unsigned i = 0; i < n; i++)
      collect_candidate_windows(display, children[i], out);
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

  pid_t child_pid;
  std::unordered_set<Window> child_windows;
  Window swallower;

  std::barrier sync(2);

  std::jthread worker([&](std::stop_token const &token) {
    auto *display = XOpenDisplay(0);

    {
      int major_opcode;
      int first_event;
      int first_error;
      if (!XQueryExtension(display, "X-Resource", &major_opcode, &first_event,
                           &first_error)) {
        std::cerr << "\x1b[31merror\x1b[0m: X-Resource extension not supported "
                     "by X server\n";
        exit(1);
      }
    }

    {
      std::unordered_multimap<pid_t, Window> pid_to_window;
      collect_candidate_windows(display, XDefaultRootWindow(display),
                                pid_to_window);
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
        XGetInputFocus(display, &swallower, &rev);
      }
    }

    XSelectInput(display, XDefaultRootWindow(display), SubstructureNotifyMask);

    XSync(display, true);

    sync.arrive_and_wait();
    sync.arrive_and_wait();

    while (true) {
      if (token.stop_requested())
        break;
      else if (XEventsQueued(display, QueuedAfterFlush) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }

      XEvent event;
      XNextEvent(display, &event);

      if (event.type == MapNotify) {
        if (window_to_pid(display, event.xmap.window) == child_pid) {
          if (child_windows.empty())
            XUnmapWindow(display, swallower);
          child_windows.insert(event.xmap.window);
        }
      } else if (event.type == UnmapNotify) {
        if (child_windows.erase(event.xmap.window))
          if (child_windows.empty())
            XMapWindow(display, swallower);
      }

      XFreeEventData(display, &event.xcookie);
    }

    if (!child_windows.empty())
      XMapWindow(display, swallower);

    XCloseDisplay(display);
  });

  sync.arrive_and_wait();

  if (posix_spawnp(&child_pid, argv[1], nullptr, nullptr, argv + 1, environ)) {
    perror("posix_spawnp failed");
    exit(1);
  }

  sync.arrive_and_wait();

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
