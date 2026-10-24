#pragma once

#include "brir/io/capture_stream.h"
#include "brir/renderer.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>
#include <memory>
#include <vector>

// Single-window app: pick a capture source and an output device, load a cinema
// set, and listen. The capture thread feeds a ClockBridge that the output device
// callback drains into the renderer (upmix -> cinema BRIRs -> limiter).
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
    void chooseCinema();
    void refreshCinemas();
    void loadCinema(const juce::File& file);
    void rebuildConvolver();
    void applyUpmixSettings();
    void applyHeadphoneEq();
    void applyBassManagement();
    void saveSettings();
    void logDiagnostics();
    void showError(const juce::String& message);

    juce::AudioDeviceManager deviceManager_;
    std::unique_ptr<juce::PropertiesFile> settings_;

    juce::Label headphoneLabel_, bassLabel_;
    juce::Label inputLabel_, outputLabel_, cinemaTitle_, cinemaLabel_, gainLabel_, centerLabel_, surroundLabel_,
        statusLabel_;
    juce::ComboBox inputBox_, cinemaBox_, headphoneBox_;
    juce::TextButton refreshButton_, startButton_, loadButton_;
    juce::ToggleButton bypassToggle_, upmixToggle_, subToggle_;
    juce::Slider gainSlider_, centerSlider_, surroundSlider_, bassSlider_, subSlider_;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> outputSelector_;
    std::unique_ptr<juce::FileChooser> fileChooser_;

    std::vector<brir::io::CaptureDevice> inputs_;
    brir::io::CaptureStream capture_;
    std::atomic<brir::ClockBridge*> bridge_{nullptr};
    std::atomic<int> callbacksInFlight_{0};

    // Owned by the message thread; only replaced while the device is stopped.
    std::unique_ptr<brir::Renderer> renderer_;
    double deviceRate_ = 0.0;
    std::vector<float> left_, right_;

    // Loaded cinema set: one BRIR pair per upmix feed, at cinemaRate_.
    brir::IrMatrix cinema_;
    double cinemaRate_ = 0.0;
    bool cinemaHasLfe_ = false;
    juce::String cinemaName_;
    juce::Array<juce::File> cinemaFiles_; // entries of cinemaBox_

    // Once-a-second diagnostics log (%APPDATA%/BrirCinema/diagnostics.csv),
    // restarted whenever capture starts.
    std::unique_ptr<juce::FileOutputStream> diagnostics_;
    double diagnosticsStart_ = 0.0;
    int timerTicks_ = 0;
};
