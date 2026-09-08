#pragma once

#include "Processor.h"
#include "TestWait.h"

#include <stdexcept>

namespace onda::plugin {

// Exercise the same message-thread work as the timer without relying on OS
// scheduling. Timer delivery itself has a separate integration test.
struct ProcessorTestAccess {
  static void service(Processor &processor) {
    if (!juce::MessageManager::getInstance()->isThisTheMessageThread())
      throw std::logic_error("Processor updates require the message thread");
    processor.timerCallback();
  }
};

} // namespace onda::plugin

namespace test {

inline void dispatchMessages() {
  if (!juce::MessageManager::getInstance()->runDispatchLoopUntil(10))
    throw std::runtime_error("The test message loop was stopped");
}

template <typename Predicate>
bool waitForMessage(Predicate predicate,
                    std::chrono::milliseconds timeout = waitTimeout) {
  return waitUntil(predicate, dispatchMessages, timeout);
}

inline void service(onda::plugin::Processor &processor) {
  onda::plugin::ProcessorTestAccess::service(processor);
}

} // namespace test
