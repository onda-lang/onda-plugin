"""Send native XDND file offers to an editor without moving the desktop pointer."""
import ctypes as c
from pathlib import Path
import time


class ClientData(c.Union):
    _fields_ = [("bytes", c.c_char * 20), ("shorts", c.c_short * 10), ("longs", c.c_long * 5)]


class ClientMessage(c.Structure):
    _fields_ = [("type", c.c_int), ("serial", c.c_ulong), ("send_event", c.c_int),
                ("display", c.c_void_p), ("window", c.c_ulong), ("message_type", c.c_ulong),
                ("format", c.c_int), ("data", ClientData)]


class SelectionRequest(c.Structure):
    _fields_ = [("type", c.c_int), ("serial", c.c_ulong), ("send_event", c.c_int),
                ("display", c.c_void_p), ("owner", c.c_ulong), ("requestor", c.c_ulong),
                ("selection", c.c_ulong), ("target", c.c_ulong), ("property", c.c_ulong),
                ("time", c.c_ulong)]


class SelectionNotify(c.Structure):
    _fields_ = [("type", c.c_int), ("serial", c.c_ulong), ("send_event", c.c_int),
                ("display", c.c_void_p), ("requestor", c.c_ulong), ("selection", c.c_ulong),
                ("target", c.c_ulong), ("property", c.c_ulong), ("time", c.c_ulong)]


class Event(c.Union):
    _fields_ = [("type", c.c_int), ("client", ClientMessage), ("request", SelectionRequest),
                ("selection", SelectionNotify), ("padding", c.c_long * 24)]


def drop_files(x11, target, files, x, y, timeout=5):
    xlib, display = x11.x, x11.display
    signatures = {
        "XCreateSimpleWindow": ([c.c_void_p, c.c_ulong, c.c_int, c.c_int, c.c_uint, c.c_uint, c.c_uint, c.c_ulong, c.c_ulong], c.c_ulong),
        "XDestroyWindow": ([c.c_void_p, c.c_ulong], c.c_int),
        "XSetSelectionOwner": ([c.c_void_p, c.c_ulong, c.c_ulong, c.c_ulong], c.c_int),
        "XChangeProperty": ([c.c_void_p, c.c_ulong, c.c_ulong, c.c_ulong, c.c_int, c.c_int, c.c_void_p, c.c_int], c.c_int),
        "XSendEvent": ([c.c_void_p, c.c_ulong, c.c_int, c.c_long, c.POINTER(Event)], c.c_int),
        "XPending": ([c.c_void_p], c.c_int),
        "XNextEvent": ([c.c_void_p, c.POINTER(Event)], c.c_int),
    }
    for name, (args, result) in signatures.items():
        function = getattr(xlib, name)
        function.argtypes, function.restype = args, result
    atoms = {name: xlib.XInternAtom(display, name.encode(), 0) for name in [
        "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndDrop",
        "XdndFinished", "XdndLeave", "XdndSelection", "XdndActionCopy", "text/uri-list",
        "text/plain;charset=utf-8",
    ]}
    actual, bits, count, remaining = c.c_ulong(), c.c_int(), c.c_ulong(), c.c_ulong()
    data = c.POINTER(c.c_ubyte)()
    xlib.XGetWindowProperty(display, target, atoms["XdndAware"], 0, 1, 0, 0,
                           c.byref(actual), c.byref(bits), c.byref(count), c.byref(remaining), c.byref(data))
    version = min(5, c.cast(data, c.POINTER(c.c_ulong))[0]) if count.value and bits.value == 32 else 0
    if data:
        xlib.XFree(data)
    if version < 3:
        raise AssertionError("The native editor does not advertise XDND file drops")
    source = xlib.XCreateSimpleWindow(display, x11.root, 0, 0, 1, 1, 0, 0, 0)
    payload = ("\r\n".join(Path(file).resolve().as_uri() for file in files) + "\r\n").encode()
    uri = atoms["text/uri-list"]

    def send(name, values):
        event = Event()
        event.client.type, event.client.window = 33, target
        event.client.message_type, event.client.format = atoms[name], 32
        event.client.data.longs[:] = values
        xlib.XSendEvent(display, target, 0, 0, c.byref(event))
        xlib.XFlush(display)

    root_x, root_y, child = c.c_int(), c.c_int(), c.c_ulong()
    xlib.XTranslateCoordinates(display, target, x11.root, x, y, c.byref(root_x), c.byref(root_y), c.byref(child))
    position = ((root_x.value & 0xffff) << 16) | (root_y.value & 0xffff)
    dropped = False
    try:
        xlib.XSetSelectionOwner(display, atoms["XdndSelection"], source, 0)
        # Nautilus offers its text representation before its native file list.
        send("XdndEnter", [source, version << 24, atoms["text/plain;charset=utf-8"], uri, 0])
        deadline, next_position = time.monotonic() + timeout, 0
        while time.monotonic() < deadline:
            if not dropped and time.monotonic() >= next_position:
                send("XdndPosition", [source, 0, position, 0, atoms["XdndActionCopy"]])
                next_position = time.monotonic() + 0.02
            if not xlib.XPending(display):
                time.sleep(0.001)
                continue
            event = Event()
            xlib.XNextEvent(display, c.byref(event))
            if event.type == 30:
                request = event.request
                assert request.target == uri, "Native file drops must request the URI list when text is also offered"
                assert request.time == 0, "Native file drops must request data with the XDND event timestamp"
                prop = request.property or request.target
                xlib.XChangeProperty(display, request.requestor, prop, uri, 8, 0, payload, len(payload))
                reply = Event()
                reply.selection = SelectionNotify(31, 0, 1, display, request.requestor,
                                                   request.selection, request.target, prop, request.time)
                xlib.XSendEvent(display, request.requestor, 0, 0, c.byref(reply))
                xlib.XFlush(display)
            elif event.type == 33 and event.client.message_type == atoms["XdndStatus"] and not dropped:
                if event.client.data.longs[1] & 1:
                    send("XdndDrop", [source, 0, 0, 0, 0])
                    dropped = True
            elif event.type == 33 and event.client.message_type == atoms["XdndFinished"]:
                return
        raise AssertionError(f"Native file drop did not finish (accepted={dropped}, target={target:#x})")
    finally:
        if not dropped:
            send("XdndLeave", [source, 0, 0, 0, 0])
        xlib.XDestroyWindow(display, source)
        xlib.XFlush(display)
