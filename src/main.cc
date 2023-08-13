#include <X11/X.h>
#include <X11/Xlib.h>
#include <X11/extensions/XRes.h>

#include <barrier>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <spawn.h>
#include <sys/wait.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <unordered_set>

int main(int argc, char **argv) {
  char const *program_name = "bswallow";

  if (argc >= 1)
    program_name = argv[0];

  if (argc <= 1) {
    std::cerr << "usage: " << program_name << " <command> [args...]\n";
    return 1;
  }

  pid_t child_pid;
  std::unordered_set<Window> child_windows;
  Window swallower;

  std::barrier sync(2, [&child_pid, argv] {
    if (posix_spawnp(&child_pid, argv[1], nullptr, nullptr, argv + 1, environ))
      throw std::system_error(std::error_code(errno, std::generic_category()),
                              "posix_spawnp");
  });

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

    int rev;
    XGetInputFocus(display, &swallower, &rev);
    XSelectInput(display, XDefaultRootWindow(display), SubstructureNotifyMask);

    XSync(display, true);

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
        XResClientIdSpec spec;
        spec.client = event.xmap.window;
        spec.mask = XRES_CLIENT_ID_XID;

        long count;
        XResClientIdValue *output;
        XResQueryClientIds(display, 1, &spec, &count, &output);

        bool found_child_pid = false;

        for (auto i = 0; i < count; ++i)
          if (output[i].spec.mask == XRES_CLIENT_ID_PID_MASK &&
              *(pid_t *)output[i].value == child_pid)
            found_child_pid = true;

        if (found_child_pid) {
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

  int status;
  while (true) {
    auto ret = waitpid(child_pid, &status, 0);
    if (ret == child_pid)
      break;
    if (ret != EINTR)
      throw std::system_error(std::error_code(errno, std::generic_category()),
                              "waitpid");
  }

  worker.request_stop();

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  else
    return 1;
}
