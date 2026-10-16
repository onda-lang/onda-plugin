#include "NativeFileDrop.h"

#include <windows.h>

#include <shlguid.h>
#include <shlobj.h>
#include <wrl.h>

#include <chrono>
#include <iostream>

namespace {

// Drive a real OLE file drop over WebView2. A timer supplies mouse movement
// while DoDragDrop pumps its modal loop; no physical button press is needed.
class FileDragSource final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IDropSource>,
      private juce::Timer {
public:
  explicit FileDragSource(POINT position) : position_(position) {
    startTimer(25);
  }

  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD) override {
    if (escape || std::chrono::steady_clock::now() >= deadline_)
      return DRAGDROP_S_CANCEL;
    return accepted_ ? DRAGDROP_S_DROP : S_OK;
  }

  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
    accepted_ = (effect & DROPEFFECT_COPY) != 0;
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

  void stop() { stopTimer(); }

private:
  void timerCallback() override {
    SetCursorPos(position_.x + (++moves_ % 2), position_.y);
  }

  POINT position_{};
  int moves_{};
  bool accepted_{};
  const std::chrono::steady_clock::time_point deadline_{
      std::chrono::steady_clock::now() + std::chrono::seconds(5)};
};

} // namespace

bool dropNativeFile(juce::WebBrowserComponent &browser,
                    const juce::File &file) {
  using Microsoft::WRL::ComPtr;
  ComPtr<IShellItem> item;
  const auto path = file.getFullPathName();
  if (FAILED(SHCreateItemFromParsingName(path.toWideCharPointer(), nullptr,
                                         IID_PPV_ARGS(item.GetAddressOf()))))
    return false;
  ComPtr<IShellItemArray> items;
  if (FAILED(SHCreateShellItemArrayFromShellItem(
          item.Get(), IID_PPV_ARGS(items.GetAddressOf()))))
    return false;
  ComPtr<IDataObject> data;
  if (FAILED(items->BindToHandler(nullptr, BHID_DataObject,
                                  IID_PPV_ARGS(data.GetAddressOf()))))
    return false;

  RECT bounds{};
  const auto *peer = browser.getPeer();
  if (!peer ||
      !GetWindowRect(static_cast<HWND>(peer->getNativeHandle()), &bounds))
    return false;
  POINT original{};
  if (!GetCursorPos(&original))
    return false;
  const juce::ScopeGuard restoreCursor{
      [&] { SetCursorPos(original.x, original.y); }};
  const POINT position{bounds.left + (bounds.right - bounds.left) / 2,
                       bounds.top + (bounds.bottom - bounds.top) / 2};
  if (!SetCursorPos(position.x, position.y))
    return false;
  auto source = Microsoft::WRL::Make<FileDragSource>(position);
  if (!source)
    return false;
  DWORD effect{};
  const auto result =
      DoDragDrop(data.Get(), source.Get(), DROPEFFECT_COPY, &effect);
  source->stop();
  if (result != DRAGDROP_S_DROP || (effect & DROPEFFECT_COPY) == 0) {
    std::cerr << "WebView2 did not accept the native OLE file drop\n";
    return false;
  }
  return true;
}
