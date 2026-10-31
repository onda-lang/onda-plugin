#pragma once

#include <cstdint>
#include <functional>
#include <juce_gui_basics/juce_gui_basics.h>
#include <memory>

namespace onda::plugin {

// A presentation-only native child. Input, focus, IME, and file drops continue
// to reach the JUCE parent. Destroy the renderer before destroying this object.
class NativeGpuSurface {
public:
  struct Handles {
    uint32_t platform;
    uintptr_t window;
    uintptr_t display{};
  };
  static std::unique_ptr<NativeGpuSurface> create(juce::ComponentPeer &,
                                                  std::function<void()> redraw,
                                                  juce::String &error);
  virtual ~NativeGpuSurface() = default;
  virtual Handles handles() const = 0;
  // Bounds are relative to the parent peer, in JUCE logical coordinates.
  // Returns the child's physical framebuffer size, including pixel rounding.
  virtual juce::Point<int> setBounds(juce::Rectangle<int>, float scale) = 0;
  virtual void setVisible(bool) = 0;
};

} // namespace onda::plugin
