#pragma once

#include "brir/io/capture_stream.h"
#include "brir/renderer.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>
#include <vector>

// Single-window app: pick a capture source and an output device, load a BRIR,
// and listen. The capture thread feeds a ClockBridge that the output device
// callback drains into the renderer.
class MainComponent : public juce::Component,
                      private juce::AudioIODeviceCallback,
                      private juce::ChangeListener,
                      private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

    void refreshInputs();
    void startCapture();
    void stopCapture();
    void chooseBrir();
    void loadBrir(const juce::File& file);
    void rebuildConvolver();
    void saveSettings();
    void showError(const juce::String& message);

    juce::AudioDeviceManager deviceManager_;
    std::unique_ptr<juce::PropertiesFile> settings_;

    juce::Label inputLabel_, outputLabel_, brirTitle_, brirLabel_, gainLabel_, statusLabel_;
    juce::ComboBox inputBox_;
    juce::TextButton refreshButton_, startButton_, loadButton_;
    juce::ToggleButton bypassToggle_;
    juce::Slider gainSlider_;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> outputSelector_;
    std::unique_ptr<juce::FileChooser> fileChooser_;

    std::vector<brir::io::CaptureDevice> inputs_;
    brir::io::CaptureStream capture_;
    std::atomic<brir::ClockBridge*> bridge_{nullptr};
    std::atomic<int> callbacksInFlight_{0};

    // Owned by the message thread; only swapped while the device is stopped.
    std::unique_ptr<brir::Renderer> renderer_;
    double deviceRate_ = 0.0;
    int deviceBlock_ = 0;
    std::vector<float> left_, right_;

    // Loaded BRIR as an IR matrix (feeds x ears) and its sample rate.
    brir::IrMatrix brir_;
    double brirRate_ = 0.0;
    juce::String brirName_;
};
