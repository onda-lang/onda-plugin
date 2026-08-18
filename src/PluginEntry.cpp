#include "Processor.h"

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
#if defined(ONDA_PLUGIN_PRODUCT_INSTRUMENT)
  return new onda::plugin::Processor(onda::plugin::Product::instrument);
#elif defined(ONDA_PLUGIN_PRODUCT_EFFECT)
  return new onda::plugin::Processor(onda::plugin::Product::effect);
#else
#error "An Onda plugin product must be selected"
#endif
}
