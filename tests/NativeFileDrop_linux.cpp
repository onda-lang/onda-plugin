#include "NativeFileDrop.h"
#include "ProcessorTestSupport.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <iostream>
#include <memory>

namespace {

Window findDropWindow(Display *display, Window window, Atom aware) {
  Window root{}, parent{}, *children{};
  unsigned count{};
  Window result{};
  if (XQueryTree(display, window, &root, &parent, &children, &count)) {
    for (unsigned i = 0; i < count && result == 0; ++i)
      result = findDropWindow(display, children[i], aware);
    if (children)
      XFree(children);
  }
  if (result)
    return result;
  Atom type{};
  int format{};
  unsigned long size{}, remaining{};
  unsigned char *data{};
  XGetWindowProperty(display, window, aware, 0, 1, False, XA_ATOM, &type,
                     &format, &size, &remaining, &data);
  if (data)
    XFree(data);
  return type != None ? window : 0;
}

} // namespace

bool dropNativeFile(juce::WebBrowserComponent &browser, const juce::File &file,
                    juce::Point<int> clientPosition) {
  const std::unique_ptr<Display, decltype(&XCloseDisplay)> display{
      XOpenDisplay(nullptr), XCloseDisplay};
  if (!display || !browser.getPeer())
    return false;
  auto *d = display.get();
  const auto atom = [&](const char *name) {
    return XInternAtom(d, name, False);
  };
  const auto host =
      reinterpret_cast<Window>(browser.getPeer()->getNativeHandle());
  const auto target = findDropWindow(d, host, atom("XdndAware"));
  if (!target || target == host) {
    std::cerr << "Could not find the native GTK drop destination\n";
    return false;
  }
  const auto source =
      XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
  const juce::ScopeGuard cleanup{[&] { XDestroyWindow(d, source); }};
  const auto selection = atom("XdndSelection");
  const auto uriType = atom("text/uri-list");
  const auto status = atom("XdndStatus");
  const auto finished = atom("XdndFinished");
  XSetSelectionOwner(d, selection, source, CurrentTime);
  const auto send = [&](Atom message, long second, long third, long fourth,
                        long fifth) {
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.display = d;
    event.xclient.window = target;
    event.xclient.message_type = message;
    event.xclient.format = 32;
    event.xclient.data.l[0] = static_cast<long>(source);
    event.xclient.data.l[1] = second;
    event.xclient.data.l[2] = third;
    event.xclient.data.l[3] = fourth;
    event.xclient.data.l[4] = fifth;
    XSendEvent(d, target, False, NoEventMask, &event);
    XFlush(d);
  };
  const auto position = browser.localPointToGlobal(clientPosition);
  send(atom("XdndEnter"), 5L << 24, static_cast<long>(uriType), 0, 0);
  send(atom("XdndPosition"), 0,
       (static_cast<long>(position.x) << 16) | position.y, CurrentTime,
       static_cast<long>(atom("XdndActionCopy")));
  bool accepted{}, dropped{}, complete{}, successful{};
  const auto uri = juce::URL(file).toString(false) + "\r\n";
  const auto delivered = test::waitForMessage([&] {
    while (XPending(d)) {
      XEvent event{};
      XNextEvent(d, &event);
      if (event.type == ClientMessage && event.xclient.message_type == status &&
          !dropped) {
        accepted = (event.xclient.data.l[1] & 1) != 0;
        if (!accepted) {
          complete = true;
          break;
        }
        dropped = true;
        send(atom("XdndDrop"), 0, CurrentTime, 0, 0);
      } else if (event.type == SelectionRequest) {
        const auto &request = event.xselectionrequest;
        XEvent response{};
        response.xselection = {SelectionNotify,
                               0,
                               True,
                               d,
                               request.requestor,
                               request.selection,
                               request.target,
                               None,
                               request.time};
        if (request.target == uriType) {
          const auto property =
              request.property != None ? request.property : request.target;
          XChangeProperty(
              d, request.requestor, property, uriType, 8, PropModeReplace,
              reinterpret_cast<const unsigned char *>(uri.toRawUTF8()),
              static_cast<int>(uri.getNumBytesAsUTF8()));
          response.xselection.property = property;
        }
        XSendEvent(d, request.requestor, False, NoEventMask, &response);
        XFlush(d);
      } else if (event.type == ClientMessage &&
                 event.xclient.message_type == finished) {
        complete = true;
        successful = (event.xclient.data.l[1] & 1) != 0;
      }
    }
    return complete;
  });
  if (!delivered || !successful)
    std::cerr << "Native browser drop failed (accepted=" << accepted
              << ", dropped=" << dropped << ", complete=" << complete << ")\n";
  return delivered && successful;
}
