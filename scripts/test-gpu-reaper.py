#!/usr/bin/env python3
"""Linux/X11 GPU checks in an isolated REAPER profile with relocated VST3 bundles."""
import argparse
import ctypes as c
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
from x11_file_drop import drop_files


class XError(c.Structure):
    _fields_ = [("type", c.c_int), ("display", c.c_void_p), ("resource", c.c_ulong),
                ("serial", c.c_ulong), ("code", c.c_ubyte), ("request", c.c_ubyte), ("minor", c.c_ubyte)]


class WindowAttributes(c.Structure):
    _fields_ = [("x", c.c_int), ("y", c.c_int), ("width", c.c_int), ("height", c.c_int),
                ("border", c.c_int), ("depth", c.c_int), ("visual", c.c_void_p), ("root", c.c_ulong),
                ("window_class", c.c_int), ("bit_gravity", c.c_int), ("win_gravity", c.c_int),
                ("backing_store", c.c_int), ("backing_planes", c.c_ulong), ("backing_pixel", c.c_ulong),
                ("save_under", c.c_int), ("colormap", c.c_ulong), ("map_installed", c.c_int),
                ("map_state", c.c_int), ("all_event_masks", c.c_long), ("your_event_mask", c.c_long),
                ("do_not_propagate_mask", c.c_long), ("override_redirect", c.c_int), ("screen", c.c_void_p)]


class X11:
    def __init__(self):
        self.x = c.CDLL("libX11.so.6")
        self.xtest = c.CDLL("libXtst.so.6")
        self.xtest.XTestFakeKeyEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
        self.xtest.XTestFakeMotionEvent.argtypes = [c.c_void_p, c.c_int, c.c_int, c.c_int, c.c_ulong]
        self.xtest.XTestFakeButtonEvent.argtypes = [c.c_void_p, c.c_uint, c.c_int, c.c_ulong]
        self.errors = []
        handler_type = c.CFUNCTYPE(c.c_int, c.c_void_p, c.POINTER(XError))
        def error_handler(_display, error):
            # Other applications can destroy a root child while it is being
            # enumerated. Keep looking; unexpected X11 errors still fail below.
            if error.contents.code != 3:  # BadWindow
                self.errors.append((error.contents.code, error.contents.request))
            return 0
        self.error_handler = handler_type(error_handler)
        self.x.XSetErrorHandler.argtypes = [handler_type]
        self.x.XSetErrorHandler(self.error_handler)
        signatures = {
            "XOpenDisplay": ([c.c_char_p], c.c_void_p),
            "XDefaultRootWindow": ([c.c_void_p], c.c_ulong),
            "XQueryTree": ([c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.POINTER(c.c_ulong)), c.POINTER(c.c_uint)], c.c_int),
            "XFetchName": ([c.c_void_p, c.c_ulong, c.POINTER(c.c_char_p)], c.c_int),
            "XInternAtom": ([c.c_void_p, c.c_char_p, c.c_int], c.c_ulong),
            "XGetWindowProperty": ([c.c_void_p, c.c_ulong, c.c_ulong, c.c_long, c.c_long, c.c_int, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_int), c.POINTER(c.c_ulong), c.POINTER(c.c_ulong), c.POINTER(c.POINTER(c.c_ubyte))], c.c_int),
            "XGetGeometry": ([c.c_void_p, c.c_ulong, c.POINTER(c.c_ulong), c.POINTER(c.c_int), c.POINTER(c.c_int), c.POINTER(c.c_uint), c.POINTER(c.c_uint), c.POINTER(c.c_uint), c.POINTER(c.c_uint)], c.c_int),
            "XGetWindowAttributes": ([c.c_void_p, c.c_ulong, c.POINTER(WindowAttributes)], c.c_int),
            "XKeysymToKeycode": ([c.c_void_p, c.c_ulong], c.c_uint),
            "XResizeWindow": ([c.c_void_p, c.c_ulong, c.c_uint, c.c_uint], c.c_int),
            "XMoveWindow": ([c.c_void_p, c.c_ulong, c.c_int, c.c_int], c.c_int),
            "XRaiseWindow": ([c.c_void_p, c.c_ulong], c.c_int),
            "XSetInputFocus": ([c.c_void_p, c.c_ulong, c.c_int, c.c_ulong], c.c_int),
            "XTranslateCoordinates": ([c.c_void_p, c.c_ulong, c.c_ulong, c.c_int, c.c_int, c.POINTER(c.c_int), c.POINTER(c.c_int), c.POINTER(c.c_ulong)], c.c_int),
            "XFlush": ([c.c_void_p], c.c_int),
            "XFree": ([c.c_void_p], c.c_int),
            "XCloseDisplay": ([c.c_void_p], c.c_int),
        }
        for name, (args, result) in signatures.items():
            function = getattr(self.x, name)
            function.argtypes, function.restype = args, result
        self.display = self.x.XOpenDisplay(None)
        if not self.display:
            raise RuntimeError("A desktop X11 DISPLAY is required")
        self.root = self.x.XDefaultRootWindow(self.display)
        self.pid_atom = self.x.XInternAtom(self.display, b"_NET_WM_PID", 0)

    def close(self):
        self.x.XCloseDisplay(self.display)

    def children(self, window):
        root, parent, count = c.c_ulong(), c.c_ulong(), c.c_uint()
        children = c.POINTER(c.c_ulong)()
        self.x.XQueryTree(self.display, window, c.byref(root), c.byref(parent), c.byref(children), c.byref(count))
        result = list(children[:count.value])
        if children:
            self.x.XFree(children)
        return result

    def name(self, window):
        name = c.c_char_p()
        self.x.XFetchName(self.display, window, c.byref(name))
        result = name.value.decode(errors="replace") if name.value else ""
        if name:
            self.x.XFree(name)
        return result

    def size(self, window):
        root, x, y, width, height, border, depth = c.c_ulong(), c.c_int(), c.c_int(), c.c_uint(), c.c_uint(), c.c_uint(), c.c_uint()
        self.x.XGetGeometry(self.display, window, c.byref(root), c.byref(x), c.byref(y), c.byref(width), c.byref(height), c.byref(border), c.byref(depth))
        return width.value, height.value

    def pid(self, window):
        actual, bits, count, remaining = c.c_ulong(), c.c_int(), c.c_ulong(), c.c_ulong()
        data = c.POINTER(c.c_ubyte)()
        self.x.XGetWindowProperty(self.display, window, self.pid_atom, 0, 1, 0, 0,
                                 c.byref(actual), c.byref(bits), c.byref(count), c.byref(remaining), c.byref(data))
        value = c.cast(data, c.POINTER(c.c_ulong))[0] if count.value else None
        if data:
            self.x.XFree(data)
        return value

    def visible(self, window):
        attributes = WindowAttributes()
        return bool(self.x.XGetWindowAttributes(self.display, window, c.byref(attributes))) and attributes.map_state == 2

    def peer(self, window):
        # REAPER creates its floating frame, then an embedding viewport, then
        # JUCE attaches the native editor child. Never return the frame itself:
        # it can exist and paint its toolbar before the plugin has attached.
        viewports = [child for child in self.children(window) if min(self.size(child)) > 100]
        peers = [child for viewport in viewports for child in self.children(viewport)
                 if min(self.size(child)) > 100 and self.visible(child)]
        return peers[-1] if peers else None

    def key(self, symbol):
        code = self.x.XKeysymToKeycode(self.display, symbol)
        # XTest updates the server's key state as well as delivering events;
        # JUCE and the VST3 host both consult that state during key handling.
        self.xtest.XTestFakeKeyEvent(self.display, code, 1, 0)
        self.x.XFlush(self.display)
        time.sleep(0.025)
        self.xtest.XTestFakeKeyEvent(self.display, code, 0, 0)
        self.x.XFlush(self.display)

    def click(self, window, x, y):
        root_x, root_y, child = c.c_int(), c.c_int(), c.c_ulong()
        self.x.XTranslateCoordinates(self.display, window, self.root, x, y, c.byref(root_x), c.byref(root_y), c.byref(child))
        self.x.XRaiseWindow(self.display, window)
        self.x.XSetInputFocus(self.display, window, 1, 0)
        self.x.XFlush(self.display)
        time.sleep(0.1)
        self.xtest.XTestFakeMotionEvent(self.display, -1, root_x.value, root_y.value, 0)
        self.x.XFlush(self.display)
        time.sleep(0.025)
        self.xtest.XTestFakeButtonEvent(self.display, 1, 1, 0)
        self.x.XFlush(self.display)
        time.sleep(0.025)
        self.xtest.XTestFakeButtonEvent(self.display, 1, 0, 0)
        self.x.XFlush(self.display)

    def drag_file(self, source, target):
        def position(window, x, y):
            root_x, root_y, child = c.c_int(), c.c_int(), c.c_ulong()
            self.x.XTranslateCoordinates(self.display, window, self.root, x, y,
                                         c.byref(root_x), c.byref(root_y), c.byref(child))
            return root_x.value, root_y.value
        start = position(source, 100, 125)
        width, height = self.size(target)
        end = position(target, width // 2, height // 2)
        self.x.XSetInputFocus(self.display, source, 1, 0)
        self.xtest.XTestFakeMotionEvent(self.display, -1, *start, 0)
        self.xtest.XTestFakeButtonEvent(self.display, 1, 1, 0)
        self.x.XFlush(self.display)
        time.sleep(0.1)
        for step in range(1, 41):
            x, y = [round(a + (b - a) * step / 40) for a, b in zip(start, end)]
            self.xtest.XTestFakeMotionEvent(self.display, -1, x, y, 0)
            self.x.XFlush(self.display)
            time.sleep(0.015)
        time.sleep(0.2)
        self.xtest.XTestFakeButtonEvent(self.display, 1, 0, 0)
        self.x.XFlush(self.display)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="GPU-enabled CMake build directory")
    parser.add_argument("--reaper", default="reaper")
    parser.add_argument("--file-drops", action="store_true", help="Check native source drops into empty plugin editors")
    parser.add_argument("--nautilus", action="store_true", help="Send file drops using the real Nautilus file manager")
    args = parser.parse_args()
    if args.nautilus and not args.file_drops:
        parser.error("--nautilus requires --file-drops")
    build = args.build.resolve()
    root = Path(tempfile.mkdtemp(prefix="onda-gpu-reaper-"))
    products = ("OndaSynth", "OndaFX")
    source_directory = root / "drop-files" if args.nautilus else root
    source_directory.mkdir(exist_ok=True)
    source = source_directory / ("drop source_日本.onda" if args.file_drops else "host.onda")
    source.write_text("params:\n  freq = 440.0 {20.0, 20000.0}\ninit:\n  phase = 0.0\nsample:\n  out1 = sin(phase) * 0.00001\n  out2 = sin(phase) * 0.00001\n  phase = (phase + freq * TWO_PI / SR) % TWO_PI\nevent note_on(id: i32, channel: i32, key: i32, velocity):\n  phase = 0.0\nevent note_off(id: i32, channel: i32, key: i32, velocity):\n  phase = 0.0\n")
    plugins = root / "plugins"
    plugins.mkdir()
    for product in products:
        bundle = next((build / f"{product}_artefacts").glob(f"*/VST3/{product}.vst3"))
        relocated = plugins / bundle.name
        shutil.copytree(bundle, relocated)
        if not (relocated / "Contents/Resources/onda_ui/libonda_ui.so").is_file():
            raise RuntimeError("Build the GPU editor before running the DAW test")
        subprocess.run([str(build / "onda_vst3_smoke"), "--preset", str(relocated), str(source), str(root / f"{product}.vstpreset")], check=True)
    configuration = root / "reaper.ini"
    # REAPER appends ~/.vst3 if absent and selects the last duplicate plugin.
    # Keep the relocated test bundles last, ahead of installed Onda versions.
    configuration.write_text(f"[REAPER]\nvstpath=~/.vst3;{plugins}\nloadlastproj=0\nverchk=0\n")
    environment = {**os.environ, "ONDA_GPU_REAPER_TEST_DIR": str(root)}
    environment.pop("ONDA_GPU_UI_LIBRARY", None)
    if args.file_drops:
        environment["ONDA_GPU_REAPER_DROP_TEST"] = "1"
    script = Path(__file__).resolve().parent.parent / "tests/GpuReaper.lua"
    log = (root / "reaper.log").open("w")
    process = subprocess.Popen([args.reaper, "-newinst", "-nosplash", "-cfgfile", str(configuration), str(script)], env=environment, stdout=log, stderr=log)
    x11 = X11()
    sequence = 0

    def wait(check, description, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if x11.errors:
                raise RuntimeError(f"Native X11 errors: {x11.errors}")
            value = check()
            if value:
                return value
            if process.poll() is not None:
                raise RuntimeError(f"REAPER exited before {description}; see {root}/reaper.log")
            time.sleep(0.05)
        raise RuntimeError(f"Timed out waiting for {description}; artifacts: {root}")

    def response(identity):
        try:
            lines = (root / "response").read_text().splitlines()
        except FileNotFoundError:
            return None
        return lines if lines and lines[0] == identity else None

    def command(action):
        nonlocal sequence
        sequence += 1
        temporary = root / "request.tmp"
        temporary.write_text(f"{sequence}\n{action}\n")
        temporary.replace(root / "request")
        return wait(lambda: response(str(sequence)), f"DAW action {action}")

    def window(product):
        return next((w for w in x11.children(x11.root) if x11.pid(w) == process.pid and x11.visible(w) and product in x11.name(w) and "Track" in x11.name(w)), None)

    def evaluation_dialog():
        return next((w for w in x11.children(x11.root) if x11.pid(w) == process.pid and x11.visible(w) and x11.name(w).startswith('About REAPER')), None)

    def loaded_libraries():
        maps = Path(f"/proc/{process.pid}/maps").read_text()
        return sorted({line.split()[-1] for line in maps.splitlines() if "libonda_ui.so" in line})

    def rendered(peer):
        # Both empty and loaded screens draw the panel's top border. The native
        # loading overlay has a plain background instead.
        width, height = x11.size(peer)
        capture = subprocess.run(["import", "-window", str(peer), "-depth", "8", "RGB:-"], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        pixels = capture.stdout
        if capture.returncode != 0 or len(pixels) != width * height * 3:
            return False
        offset = (12 * width + width // 2) * 3
        return pixels[offset + 1] > 35 and pixels[offset + 2] > 60

    def evaluation_enabled(dialog):
        # SWELL draws its controls in one native window, without separate
        # accessible button handles. In the fresh profile's default theme the
        # countdown label is grey; enabled Still Evaluating text is dark.
        width, height = x11.size(dialog)
        pixels = subprocess.check_output(['import', '-window', str(dialog), '-depth', '8', 'RGB:-'])
        if len(pixels) != width * height * 3:
            return False
        dark = sum(max(pixels[(y * width + x) * 3:(y * width + x) * 3 + 3]) < 80
                   for y in range(height - 37, height - 9) for x in range(width - 155, width - 15))
        # A newly mapped, unpainted window is entirely dark; it is not ready.
        return 40 < dark < 1000

    completed = False
    file_manager = None
    try:
        ready = wait(lambda: response("ready"), "both exported plugins")
        if any(x11.pid(w) == process.pid and 'EVALUATION LICENSE' in x11.name(w) for w in x11.children(x11.root)):
            dialog = wait(evaluation_dialog, 'ordinary REAPER evaluation dialog')
            x11.x.XMoveWindow(x11.display, dialog, 20, 20)
            x11.x.XFlush(x11.display)
            wait(lambda: evaluation_enabled(dialog), 'enabled Still Evaluating button')
            # Click once; repeated clicks can re-enter SWELL's dialog teardown.
            subprocess.run(['import', '-window', str(dialog), str(root / 'evaluation-before-click.png')], check=True)
            width, height = x11.size(dialog)
            x11.click(dialog, width - 80, height - 25)
            # An inactive SWELL window can consume the first click for
            # activation. Allow teardown to finish before a second click.
            settle = time.monotonic() + 1.0
            while evaluation_dialog() and time.monotonic() < settle:
                time.sleep(0.05)
            if evaluation_dialog():
                x11.click(dialog, width - 80, height - 25)
            wait(lambda: not evaluation_dialog(), 'evaluation dialog dismissal')
        if args.nautilus:
            file_manager = subprocess.Popen(['dbus-run-session', '--', 'nautilus', '--new-window', str(source_directory)],
                env={**os.environ, 'GDK_BACKEND':'x11', 'XDG_CONFIG_HOME':str(root/'file-manager-config'),
                     'XDG_DATA_HOME':str(root/'file-manager-data'), 'GTK_A11Y':'none'},
                stdout=log, stderr=log, start_new_session=True)
            file_window = wait(lambda: next((w for w in x11.children(x11.root)
                if x11.name(w) == 'drop-files' and x11.visible(w)), None), 'Nautilus source folder')
            x11.x.XMoveWindow(x11.display, file_window, 0, 20)
            x11.x.XResizeWindow(x11.display, file_window, 500, 700)
            x11.x.XFlush(x11.display)
            time.sleep(1)
            subprocess.run(['import', '-window', str(file_window), str(root/'file-manager.png')], check=True)
        for product in products:
            command(product)
            handle = wait(lambda: window(product), f"{product} GPU editor")
            peer = wait(lambda: x11.peer(handle), f"{product} attached native peer")
            x11.x.XMoveWindow(x11.display, handle, 20, 20)
            x11.x.XRaiseWindow(x11.display, handle)
            x11.x.XSetInputFocus(x11.display, peer, 1, 0)
            x11.x.XFlush(x11.display)
            wait(lambda: rendered(peer), f"{product} rendered header")
            libraries = wait(loaded_libraries, "bundled UI library mappings")
            assert all(str(plugins) in path for path in libraries), libraries
            if args.file_drops:
                # Move the host after the native peer was attached. Drop
                # coordinates must use its current position, not JUCE's cache.
                x11.x.XMoveWindow(x11.display, handle, 550, 20)
                x11.x.XFlush(x11.display)
                width, height = x11.size(peer)
                if args.nautilus:
                    x11.x.XRaiseWindow(x11.display, file_window)
                    x11.x.XFlush(x11.display)
                    x11.drag_file(file_window, peer)
                else:
                    drop_files(x11, peer, [source], width // 2, height // 2)
                parameter_index = 2 if product == 'OndaSynth' else 4
                expected_loaded = (440.0 - 20.0) / (20000.0 - 20.0)
                wait(lambda: abs(float(command('query')[parameter_index]) - expected_loaded) < 1e-6,
                     f"{product} loading a dropped patch from the empty editor", timeout=5)
                subprocess.run(['import', '-window', str(handle), str(root / f'{product}-dropped.png')], check=True)
                print(f"{product}: native source drop loaded {source.name}", flush=True)
                continue
            x11.x.XResizeWindow(x11.display, handle, 720, 840)
            x11.x.XFlush(x11.display)
            time.sleep(0.2)
            width, _ = x11.size(peer)
            time.sleep(0.1)
            # Choose the slider layout and focus its only numeric field. Tab
            # starts at header actions now that all controls are focusable.
            x11.click(peer, width - 100, 337)
            x11.click(peer, 178, 378)
            time.sleep(0.1)
            for symbol in [ord('8'), ord('8'), ord('0'), 0xff0d]:
                x11.key(symbol)
                time.sleep(0.05)
            time.sleep(0.15)
            subprocess.run(["import", "-window", str(handle), str(root / f"{product}-edited.png")], check=True)
            x11.click(peer, width - 112, 48)  # Settings action in the loaded header.
            x11.key(0xff09)  # Read-only sample rate is skipped; focus block size.
            x11.key(0xff0d)  # Enter opens the block-size dropdown above the panel.
            time.sleep(0.1)
            subprocess.run(["import", "-window", str(handle), str(root / f"{product}-settings.png")], check=True)
            x11.key(0xff1b)  # Escape closes the dropdown, then the settings popup.
            x11.key(0xff1b)
        if args.file_drops:
            result = {"passed": True, "reaper": ready[1], "products": products,
                      "loaded_ui_libraries": libraries,
                      "checks": ["native X11 source drops into empty editors", "moved host windows",
                                 "spaces and Unicode in file paths"],
                      "drop_source": "Nautilus" if args.nautilus else "XDND with text and URI offers"}
        else:
            expected = (880.0 - 20.0) / (20000.0 - 20.0)
            initial_edit = command('query')
            (root / 'edited-parameters.json').write_text(json.dumps(initial_edit))
            edited = wait(lambda: (values if all(abs(float(values[i]) - expected) < 1e-6 for i in (2, 4)) else None)
                          if (values := command("query")) else None, "UI changes reaching REAPER", timeout=5)
            automated = command("automate")
            assert all(abs(float(automated[i]) - 0.25) < 1e-6 for i in (2, 4)), automated
            time.sleep(0.2)
            for product in products:
                command(product)
                handle = wait(lambda: window(product), f"{product} editor for resize")
                wait(lambda: x11.peer(handle), f"{product} attached native peer for resize")
                x11.x.XResizeWindow(x11.display, handle, 960, 748)
                x11.x.XFlush(x11.display)
                time.sleep(0.2)
                subprocess.run(["import", "-window", str(handle), str(root / f"{product}-automation-resized.png")], check=True)
            for _ in range(2):
                command("hide")
                command("show")
            for product in products:
                handle = wait(lambda: window(product), f"{product} reopened editor")
                x11.x.XRaiseWindow(x11.display, handle)
                x11.x.XFlush(x11.display)
                wait(lambda: (peer := x11.peer(handle)) and rendered(peer), f"{product} reopened surface")
            libraries = wait(loaded_libraries, "bundled UI library mappings")
            assert all(str(plugins) in path for path in libraries), libraries
            result = {"passed": True, "reaper": ready[1], "products": products, "edited_parameters": edited[2::2],
                      "automated_parameters": automated[2::2], "loaded_ui_libraries": libraries,
                      "checks": ["relocated bundles", "native GPU editor", "native input", "UI to DAW parameters", "DAW automation", "resize", "hide/show", "editor destruction"]}
        command('done')
        time.sleep(0.1)
        if process.poll() is not None:
            raise RuntimeError(f'REAPER exited while closing plugin editors; see {root}/reaper.log')
        completed = True
    except BaseException:
        for product in products:
            handle = window(product)
            if handle:
                subprocess.run(["import", "-window", str(handle), str(root / f"{product}-failure.png")])
        raise
    finally:
        try:
            if not completed and (root / 'response').exists():
                command("done")
        except RuntimeError:
            pass
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            x11.close()
            if file_manager:
                try:
                    os.killpg(file_manager.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                file_manager.wait(timeout=5)
            log.close()
    (root / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(f"REAPER GPU checks passed. Evidence: {root}/result.json")


if __name__ == "__main__":
    main()
