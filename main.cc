#include <X11/X.h>
#include <X11/Xutil.h>
#include <X11/extensions/XRes.h>

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

Display *dpy;
// Used to signal to the worker thread that it should shut down.
int stop_pipe[2];

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

// Memory shared with the child process after fork.
struct shared_memory {
  pid_t child_pid{};
  pthread_barrier_t sync;

  shared_memory() {
    pthread_barrierattr_t attr;
    pthread_barrierattr_init(&attr);
    pthread_barrierattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_barrier_init(&sync, &attr, 2);
  }
  shared_memory(shared_memory const &) = delete;
  shared_memory(shared_memory &&) = delete;

  void arrive_and_wait() { pthread_barrier_wait(&sync); }

  static shared_memory *create() {
    void *memory = mmap(nullptr, sizeof(shared_memory), PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (memory == MAP_FAILED) {
      perror("mmap failed");
      exit(1);
    }

    return new (memory) shared_memory;
  }
};

int main(int argc, char **argv) {
  char const *program_name = "better-swallow";

  if (argc >= 1)
    program_name = argv[0];

  if (argc <= 1) {
    std::cerr << "usage: " << program_name << " <command> [args...]\n";
    return 1;
  }

  dpy = XOpenDisplay(NULL);
  if (!dpy) {
    execvp(argv[1], argv + 1);
    perror("execvp failed");
    exit(1);
  }

  shared_memory &sh = *shared_memory::create();
  if (pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) < 0) {
    perror("pipe2 failed");
    exit(1);
  }

  std::thread worker([&sh]() {
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
    bool has_patch = prop.value && !strcmp((char *)prop.value, "supported");

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

    sh.arrive_and_wait();

    if (has_patch) {
      XEvent event;
      event.xclient.type = ClientMessage;
      event.xclient.message_type = swallow_atom;
      event.xclient.format = 32;
      event.xclient.window = swallower;
      event.xclient.data.l[0] = sh.child_pid;
      XSendEvent(dpy, XDefaultRootWindow(dpy), False,
                 SubstructureRedirectMask | SubstructureNotifyMask, &event);
      XSync(dpy, true);
      sh.arrive_and_wait();
      return; // From here on, dwm will manage the swallowing itself
    } else
      sh.arrive_and_wait();

    pollfd fds[2];
    fds[0] = {.fd = ConnectionNumber(dpy), .events = POLLIN, .revents = 0};
    fds[1] = {.fd = stop_pipe[0], .events = 0, .revents = 0};

    std::unordered_set<Window> child_windows;
    while (true) {
      if (XEventsQueued(dpy, QueuedAfterFlush) == 0) {
        int n = poll(fds, 2, -1);
        if (n < 0 && errno != EINTR) {
          perror("poll failed");
          exit(1);
        }

        if (fds[1].revents & POLLHUP)
          break;

        continue;
      }

      XEvent event;
      XNextEvent(dpy, &event);

      if (event.type == MapNotify) {
        if (window_to_pid(event.xmap.window) == sh.child_pid) {
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

  pid_t forkret = fork();
  if (forkret < 0) {
    perror("fork failed");
    exit(1);
  } else if (forkret == 0) {
    close(ConnectionNumber(dpy));
    sh.child_pid = getpid();
    sh.arrive_and_wait();
    sh.arrive_and_wait();
    execvp(argv[1], argv + 1);
    perror("execvp failed");
    exit(255);
  }

  int status;
  while (true) {
    auto ret = waitpid(forkret, &status, 0);
    if (ret == sh.child_pid)
      break;
    if (ret != EINTR) {
      perror("waitpid failed");
      exit(1);
    }
  }

  if (close(stop_pipe[1]) < 0) {
    perror("close failed");
    exit(1);
  }

  try {
    worker.join();
  } catch (std::invalid_argument const &) {
    // Thread is not joinable (already finished execution)
  }

  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  else
    return 1;
}
