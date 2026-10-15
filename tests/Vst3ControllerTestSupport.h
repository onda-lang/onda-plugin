#pragma once

#include <juce_audio_processors_headless/juce_audio_processors_headless.h>

#define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
#include <juce_audio_processors_headless/format_types/juce_VST3Headers.h>

#include <memory>

namespace vst3_test {
using namespace Steinberg;

template <typename Interface> class HostInterface : public Interface {
public:
  tresult PLUGIN_API queryInterface(const TUID, void **object) override {
    *object = nullptr;
    return kNoInterface;
  }
  uint32 PLUGIN_API addRef() override { return 1; }
  uint32 PLUGIN_API release() override { return 1; }
};

struct ReleaseInterface {
  template <typename Interface> void operator()(Interface *value) const {
    value->release();
  }
};

// Observe the exported controller while JUCE's original controller remains
// connected, so its real host feedback path is exercised as well.
class ObservedController final : public HostInterface<Vst::IComponentHandler> {
public:
  ~ObservedController() {
    if (controller_ != nullptr) {
      controller_->setComponentHandler(nullptr);
      if (connected_)
        controllerConnection_->disconnect(componentConnection_.get());
      if (initialized_)
        controller_->terminate();
    }
  }

  bool connect(Vst::IComponent &component, const juce::File &module) {
    if (!library_.open(module.getFullPathName()))
      return false;
    using GetFactory = IPluginFactory *(PLUGIN_API *)();
    const auto getFactory =
        reinterpret_cast<GetFactory>(library_.getFunction("GetPluginFactory"));
    if (getFactory == nullptr)
      return false;
    factory_.reset(getFactory());
    TUID classId{};
    Vst::IEditController *controller{};
    if (factory_ == nullptr ||
        component.getControllerClassId(classId) != kResultOk ||
        factory_->createInstance(classId, Vst::IEditController::iid,
                                 reinterpret_cast<void **>(&controller)) !=
            kResultOk)
      return false;
    controller_.reset(controller);
    initialized_ = controller_->initialize(nullptr) == kResultOk;
    if (!initialized_)
      return false;
    Vst::IConnectionPoint *componentConnection{}, *controllerConnection{};
    if (component.queryInterface(
            Vst::IConnectionPoint::iid,
            reinterpret_cast<void **>(&componentConnection)) != kResultOk)
      return false;
    componentConnection_.reset(componentConnection);
    if (controller_->queryInterface(
            Vst::IConnectionPoint::iid,
            reinterpret_cast<void **>(&controllerConnection)) != kResultOk)
      return false;
    controllerConnection_.reset(controllerConnection);
    connected_ =
        controllerConnection_->connect(componentConnection_.get()) == kResultOk;
    return connected_ && controller_->setComponentHandler(this) == kResultOk;
  }

  std::unique_ptr<IPlugView, ReleaseInterface> createView() const {
    return std::unique_ptr<IPlugView, ReleaseInterface>{
        controller_->createView(Vst::ViewType::kEditor)};
  }

  double value(const char *id) const {
    return controller_->getParamNormalized(
        juce::VST3ClientExtensions::convertJuceParameterId(id));
  }
  double plain(const char *id, const double value) const {
    return controller_->normalizedParamToPlain(
        juce::VST3ClientExtensions::convertJuceParameterId(id), value);
  }
  double normalized(const char *id, const double value) const {
    return controller_->plainParamToNormalized(
        juce::VST3ClientExtensions::convertJuceParameterId(id), value);
  }
  int32 stepCount(const char *id) const {
    const auto parameterId =
        juce::VST3ClientExtensions::convertJuceParameterId(id);
    for (int32 index = 0; index < controller_->getParameterCount(); ++index) {
      Vst::ParameterInfo info{};
      if (controller_->getParameterInfo(index, info) == kResultOk &&
          info.id == parameterId)
        return info.stepCount;
    }
    return -1;
  }
  tresult PLUGIN_API beginEdit(Vst::ParamID) override {
    ++gestures;
    return kResultOk;
  }
  tresult PLUGIN_API performEdit(Vst::ParamID, Vst::ParamValue) override {
    ++edits;
    return kResultOk;
  }
  tresult PLUGIN_API endEdit(Vst::ParamID) override {
    ++gestures;
    return kResultOk;
  }
  tresult PLUGIN_API restartComponent(const int32 flags) override {
    if ((flags & Vst::kParamValuesChanged) != 0)
      ++valueRefreshes;
    return kResultOk;
  }

  int edits{}, gestures{}, valueRefreshes{};

private:
  juce::DynamicLibrary library_;
  std::unique_ptr<IPluginFactory, ReleaseInterface> factory_;
  std::unique_ptr<Vst::IConnectionPoint, ReleaseInterface> componentConnection_;
  std::unique_ptr<Vst::IEditController, ReleaseInterface> controller_;
  std::unique_ptr<Vst::IConnectionPoint, ReleaseInterface>
      controllerConnection_;
  bool initialized_{}, connected_{};
};

} // namespace vst3_test
