#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <optional>
#include <string>

namespace onda::plugin {

class Processor;
struct MidiActivitySnapshot;
struct WorkerStatus;

[[nodiscard]] std::optional<juce::WebBrowserComponent::Resource>
runViewResource(const juce::String &request);

[[nodiscard]] juce::var makeRunViewState(Processor &processor,
                                         const WorkerStatus &status,
                                         bool canExportProject,
                                         const std::string &actionError);

[[nodiscard]] juce::var makeRunViewScope(Processor &processor);

[[nodiscard]] juce::var
makeRunViewMidiActivity(const MidiActivitySnapshot &activity);

} // namespace onda::plugin
