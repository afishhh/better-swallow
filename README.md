## A better swallow

A simple program that implements devour-like swallowing except it's actually smart.

### Building

```
c++ -O3 -Wall -Wextra -std=c++20 -lX11 -lXRes main.cc
```
or
```
mkdir build && cd build
cmake .. && cmake --build .
```

### Usage

```command
alias bs=better-swallow
bs mpv video.mp4
bs glxgears
```

### How it works

1. Attempt to find the swallower by traversing the process hierarchy upwards.
    - If that fails, get the currently focused window like devour. (fallback)
2. Run the program provided on the command-line.
3. If the child creates a window, unmap the swallower.
4. If the child unmaps all it's windows or exits, map back the swallower.

This method of finding the swallower is, in my opinion, significantly better than just grabbing the currently focused window like devour does.
It means that you can run `sleep 2; better-swallow glxgears` and have `glxgears` still swallow the terminal instead of whatever other window you moved your mouse over by the time the sleep completed.
