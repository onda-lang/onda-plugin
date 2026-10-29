#pragma once

#include <juce_core/juce_core.h>

#include <string>

namespace onda::plugin {

class Processor;
struct MidiActivitySnapshot;
struct WorkerStatus;

[[nodiscard]] juce::var makeRunViewState(Processor &processor,
                                         const WorkerStatus &status,
                                         bool canExportProject,
                                         const std::string &actionError,
                                         bool resetEventArguments = false,
                                         bool includeRuntimeLog = true);

[[nodiscard]] juce::var makeRunViewParameterValues(Processor &processor,
                                                   const WorkerStatus &status);

[[nodiscard]] juce::var makeRunViewLog(Processor &processor);

[[nodiscard]] juce::var
makeRunViewMidiActivity(const MidiActivitySnapshot &activity);

} // namespace onda::plugin
