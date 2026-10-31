#pragma once
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>
#include <onda_ui.h>
#include <vector>

namespace onda::plugin {

class NativeGpuSurface;

// JUCE owns input and the message thread. A dedicated native child belongs
// exclusively to Rust's renderer; neither starts another event loop.
class GpuView final : public juce::Component,
                      public juce::TextInputTarget,
                      private juce::ComponentMovementWatcher,
                      private juce::AsyncUpdater {
public:
  using Command = std::function<void(const juce::var &)>;
  GpuView(Command command, std::function<void()> ready);
  ~GpuView() override;
  void tick();
  void suspend();
  bool meterVisible() const { return ui_ && meter_visible_(ui_); }
  void meter(int channels, const float *peaks, std::size_t count);
  bool scopeVisible() const { return ui_ && scope_visible_(ui_); }
  void message(const juce::var &message);
  void scope(int channels, const std::vector<float> &samples);
  const juce::String &error() const { return error_; }
  void mouseDown(const juce::MouseEvent &) override;
  void mouseMove(const juce::MouseEvent &) override;
  void mouseExit(const juce::MouseEvent &) override;
  void mouseDrag(const juce::MouseEvent &) override;
  void mouseUp(const juce::MouseEvent &) override;
  void mouseWheelMove(const juce::MouseEvent &,
                      const juce::MouseWheelDetails &) override;
  bool keyPressed(const juce::KeyPress &) override;
  void focusLost(FocusChangeType) override;
  bool isTextInputActive() const override;
  juce::Range<int> getHighlightedRegion() const override;
  void setHighlightedRegion(const juce::Range<int> &) override;
  void setTemporaryUnderlining(const juce::Array<juce::Range<int>> &) override;
  juce::String getTextInRange(const juce::Range<int> &) const override;
  void insertTextAtCaret(const juce::String &) override;
  int getCaretPosition() const override;
  juce::Rectangle<int> getCaretRectangleForCharIndex(int) const override;
  int getTotalNumChars() const override;
  int getCharIndexForPoint(juce::Point<int>) const override;
  juce::RectangleList<int> getTextBounds(juce::Range<int>) const override;

private:
  friend struct GpuViewTestAccess;
  bool initialize();
  void draw();
  void updateSurface();
  void componentMovedOrResized(bool, bool) override;
  void componentPeerChanged() override;
  void componentVisibilityChanged() override;
  void handleAsyncUpdate() override { tick(); }
  bool input(juce::var event);
  void drain();
  void release();
  juce::var editorState() const;
  void editText(const juce::String &, int anchor, int caret);
  juce::DynamicLibrary library_;
  std::unique_ptr<NativeGpuSurface> surface_;
  juce::uint32 peerId_{};
  juce::Point<int> framebufferSize_;
  OndaUi *ui_{};
  decltype(&onda_ui_create) create_{};
  decltype(&onda_ui_destroy) destroy_{};
  decltype(&onda_ui_resize) resize_{};
  decltype(&onda_ui_message) message_{};
  decltype(&onda_ui_input) input_{};
  decltype(&onda_ui_cursor) cursor_{};
  decltype(&onda_ui_render) render_{};
  decltype(&onda_ui_meter) meter_{};
  decltype(&onda_ui_meter_visible) meter_visible_{};
  decltype(&onda_ui_scope) scope_{};
  decltype(&onda_ui_scope_visible) scope_visible_{};
  decltype(&onda_ui_commands) commands_{};
  decltype(&onda_ui_error) error_fn_{};
  decltype(&onda_ui_text_edit) text_edit_{};
  decltype(&onda_ui_text_bounds) text_bounds_{};
  decltype(&onda_ui_text_hit) text_hit_{};
  Command command_;
  std::function<void()> ready_;
  juce::String error_;
  float scale_{1.0f};
  bool exposed_{true};
  bool composing_{};
  bool drawing_{};
};
} // namespace onda::plugin
