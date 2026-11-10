#include "GpuView.h"

namespace onda::plugin {
namespace {
int utf16Offset(const juce::String &text, int index) {
  auto p = text.getCharPointer();
  int units = 0;
  while (index > 0 && !p.isEmpty()) {
    --index;
    units += p.getAndAdvance() > 0xffff ? 2 : 1;
  }
  return units;
}
int characterOffset(const juce::String &text, int units) {
  auto p = text.getCharPointer();
  int index = 0;
  while (units > 0 && !p.isEmpty()) {
    units -= p.getAndAdvance() > 0xffff ? 2 : 1;
    ++index;
  }
  return index;
}
juce::var editEvent(const char *type) {
  juce::var result{new juce::DynamicObject};
  result.getDynamicObject()->setProperty("type", type);
  return result;
}
} // namespace
juce::var GpuView::editorState() const {
  return ui_ ? juce::JSON::parse(juce::String::fromUTF8(text_edit_(ui_)))
             : juce::var{};
}
bool GpuView::isTextInputActive() const { return editorState().isObject(); }
int GpuView::getTotalNumChars() const {
  return editorState()["text"].toString().length();
}
int GpuView::getCaretPosition() const {
  const auto state = editorState();
  return characterOffset(state["text"].toString(),
                         static_cast<int>(state["cursor"]));
}
juce::Range<int> GpuView::getHighlightedRegion() const {
  const auto state = editorState();
  const auto text = state["text"].toString();
  const auto a = characterOffset(text, static_cast<int>(state["anchor"]));
  const auto b = characterOffset(text, static_cast<int>(state["cursor"]));
  return {std::min(a, b), std::max(a, b)};
}
void GpuView::editText(const juce::String &text, int anchor, int caret) {
  auto value = editEvent("editText");
  auto &object = *value.getDynamicObject();
  object.setProperty("text", text);
  object.setProperty("anchor", utf16Offset(text, anchor));
  object.setProperty("cursor", utf16Offset(text, caret));
  input(value);
}
void GpuView::setHighlightedRegion(const juce::Range<int> &range) {
  editText(editorState()["text"].toString(), range.getStart(), range.getEnd());
}
void GpuView::setTemporaryUnderlining(
    const juce::Array<juce::Range<int>> &ranges) {
  composing_ = !ranges.isEmpty();
  const auto text = editorState()["text"].toString();
  auto value = editEvent("textMark");
  value.getDynamicObject()->setProperty(
      "start",
      ranges.isEmpty() ? 0 : utf16Offset(text, ranges.getFirst().getStart()));
  value.getDynamicObject()->setProperty(
      "end",
      ranges.isEmpty() ? 0 : utf16Offset(text, ranges.getLast().getEnd()));
  input(value);
}
juce::String GpuView::getTextInRange(const juce::Range<int> &range) const {
  return editorState()["text"].toString().substring(range.getStart(),
                                                    range.getEnd());
}
void GpuView::insertTextAtCaret(const juce::String &insert) {
  const auto original = editorState()["text"].toString();
  const auto range = getHighlightedRegion();
  const auto text = original.substring(0, range.getStart()) + insert +
                    original.substring(range.getEnd());
  const auto caret = range.getStart() + insert.length();
  editText(text, caret, caret);
}
juce::Rectangle<int> GpuView::getCaretRectangleForCharIndex(int index) const {
  if (!ui_)
    return {};
  const auto offset =
      static_cast<size_t>(utf16Offset(editorState()["text"].toString(), index));
  float rect[4]{};
  if (!text_bounds_(ui_, offset, offset, rect))
    return {};
  return juce::Rectangle<float>{rect[0], rect[1], rect[2], rect[3]}
      .getSmallestIntegerContainer();
}
int GpuView::getCharIndexForPoint(juce::Point<int> point) const {
  if (!ui_)
    return 0;
  const auto offset =
      text_hit_(ui_, static_cast<float>(point.x), static_cast<float>(point.y));
  return offset == SIZE_MAX ? 0
                            : characterOffset(editorState()["text"].toString(),
                                              static_cast<int>(offset));
}
juce::RectangleList<int> GpuView::getTextBounds(juce::Range<int> range) const {
  return juce::RectangleList<int>{
      getCaretRectangleForCharIndex(range.getStart())
          .getUnion(getCaretRectangleForCharIndex(range.getEnd()))};
}
} // namespace onda::plugin
