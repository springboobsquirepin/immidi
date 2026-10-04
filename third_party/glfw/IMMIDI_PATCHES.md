# Changes to GLFW 3.4 for ImMidi

GLFW reports dropped files only when they are dropped. To show where dropped files will land in the
playlist while they are dragged, this copy adds a drag-over callback (X11, Wayland, Cocoa):

- `include/GLFW/glfw3.h`: `GLFWdragfun` and `glfwSetDragCallback()`.
- `src/internal.h`, `src/input.c`: the callback slot, `_glfwInputDrag()` and the setter.
- `src/x11_*`: the `XdndLeave` atom; `XdndPosition` reports the position, `XdndLeave` and `XdndDrop`
  end the drag.
- `src/wl_window.c`: the data device's enter/motion report the position, leave and drop end the drag.
- `src/cocoa_window.m`: `draggingEntered:`/`draggingUpdated:` report the position,
  `draggingExited:` and `performDragOperation:` end the drag.

Windows is handled in ImMidi itself (`src/ui/FileDropTarget.cpp`, an OLE `IDropTarget`), because
GLFW's Win32 backend only uses `WM_DROPFILES`, which has no drag-over notifications.

## Finishing drops on Wayland

GLFW 3.4 binds `wl_data_device_manager` version 1, never selects a drag-and-drop action and keeps a drop's
offer until the next drag enters. Compositors that wait for the destination (KDE Plasma's KWin) then never
end the drag for its source: a file manager like Dolphin keeps its drag cursor until the next click, which
it then takes as the end of a selection. This copy:

- `src/wl_init.c`, `src/wl_platform.h`: binds `wl_data_device_manager` version 3 when the compositor has it
  (and remembers the version).
- `src/wl_window.c`:
  - the offer and data source listeners handle the version 3 events (`source_actions`, `action`,
    `dnd_drop_performed`, `dnd_finished`);
  - entering a drag accepts `text/uri-list` with the copy action only (with "move", the source would delete
    what was dropped);
  - after reading a drop, the offer is finished (`wl_data_offer.finish`) and destroyed.
- `src/wl_window.c`: a drag entering a surface that is not a GLFW window (e.g. libdecor's title bar)
  no longer dereferences a null window, and destroying the window under a drag drops the drag's offer.

## Window icons on Wayland

GLFW 3.4 cannot set a window icon on Wayland. This copy implements `glfwSetWindowIcon()` there with the
`xdg-toplevel-icon-v1` protocol (wayland-protocols, staging), which KDE Plasma 6.3 and later support.
Compositors without it report `GLFW_FEATURE_UNAVAILABLE`, as before.

- `src/wayland/xdg-toplevel-icon-v1.xml` and the headers generated from it (see below).
- `src/wl_platform.h`: the protocol's interface names, the manager in the library state, and the
  window's icon buffers.
- `src/wl_init.c`: binds `xdg_toplevel_icon_manager_v1` (without a listener: its `icon_size` events are
  not needed) and destroys it at termination.
- `src/wl_window.c`: `_glfwSetWindowIconWayland()` turns the images into wl_shm buffers (square ones only)
  and sends them with `set_icon`; the icon is sent again when the window's toplevel is created (with
  xdg-shell before its first commit, with libdecor after the frame is mapped). The buffers stay until the
  window is destroyed or gets another icon, as the protocol requires them to outlive the icon object.

## Wayland protocol headers

`src/wayland/` holds the protocol XML files of GLFW 3.4 (`deps/wayland`), `xdg-toplevel-icon-v1.xml`
from wayland-protocols 1.47, and the headers generated from them (with wayland-scanner 1.24), so
building needs no `wayland-scanner`:

    for x in *.xml; do n=${x%.xml}
      wayland-scanner client-header $x $n-client-protocol.h
      wayland-scanner private-code  $x $n-client-protocol-code.h
    done
