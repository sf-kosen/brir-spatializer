#include "MainComponent.h"

#include <cmath>
#include <thread>

namespace {

juce::String jp(const char* utf8)
{
    return juce::String::fromUTF8(utf8);
}

constexpr int kMaxRenderBlock = 1024;

// Loads a legacy 4-channel BRIR (LL, LR, RL, RR) as a 2-feed x 2-ear matrix,
// scaled so uncorrelated unit-level L/R input comes out at unit level on average.
bool readBrir(const juce::File& file, brir::IrMatrix& irs, double& rate, juce::String& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (!reader) {
        error = jp("BRIRファイルを開けませんでした。");
        return false;
    }
    if (reader->numChannels != 4) {
        error = jp("BRIRは4ch（LL, LR, RL, RR）のWAVである必要があります。");
        return false;
    }
    if (reader->lengthInSamples <= 0 || reader->lengthInSamples > 10 * reader->sampleRate) {
        error = jp("BRIRの長さが不正です（最大10秒）。");
        return false;
    }

    int length = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> buffer(4, length);
    reader->read(&buffer, 0, length, 0, true, true);

    double energy = 0.0;
    for (int ch = 0; ch < 4; ++ch)
        for (int i = 0; i < length; ++i) energy += static_cast<double>(buffer.getSample(ch, i)) * buffer.getSample(ch, i);
    float scale = energy > 0.0 ? static_cast<float>(1.0 / std::sqrt(energy / 2.0)) : 1.0f;

    auto channel = [&](int ch) {
        std::vector<float> ir(static_cast<std::size_t>(length));
        for (int i = 0; i < length; ++i) ir[static_cast<std::size_t>(i)] = buffer.getSample(ch, i) * scale;
        return ir;
    };
    irs = {{channel(0), channel(1)}, {channel(2), channel(3)}};
    rate = reader->sampleRate;
    return true;
}

// Converts an IR to another sample rate with JUCE's windowed-sinc interpolator.
// Sample values are preserved, so the caller rescales the gain by the rate ratio.
std::vector<float> resampleIr(const std::vector<float>& ir, double fromRate, double toRate)
{
    double speed = fromRate / toRate;
    int outLength = static_cast<int>(std::ceil(static_cast<double>(ir.size()) / speed));
    std::vector<float> padded(ir);
    padded.resize(ir.size() + 64, 0.0f); // let the kernel run off the end
    std::vector<float> out(static_cast<std::size_t>(outLength));
    juce::WindowedSincInterpolator interpolator;
    interpolator.process(speed, padded.data(), out.data(), outLength);
    return out;
}

} // namespace

MainComponent::MainComponent()
{
    juce::PropertiesFile::Options options;
    options.applicationName = "BrirCinema";
    options.filenameSuffix = ".settings";
    options.folderName = "BrirCinema";
    options.osxLibrarySubFolder = "Application Support";
    settings_ = std::make_unique<juce::PropertiesFile>(options);

    juce::LookAndFeel::getDefaultLookAndFeel().setDefaultSansSerifTypefaceName("Yu Gothic UI");

    inputLabel_.setText(jp("入力（取り込み元）"), juce::dontSendNotification);
    outputLabel_.setText(jp("出力（ヘッドホン）"), juce::dontSendNotification);
    brirTitle_.setText("BRIR", juce::dontSendNotification);
    gainLabel_.setText(jp("音量"), juce::dontSendNotification);
    brirLabel_.setText(jp("未読込"), juce::dontSendNotification);
    statusLabel_.setJustificationType(juce::Justification::topLeft);
    statusLabel_.setFont(juce::FontOptions(14.0f));

    refreshButton_.setButtonText(jp("更新"));
    startButton_.setButtonText(jp("開始"));
    loadButton_.setButtonText(jp("BRIRを読み込む…"));
    bypassToggle_.setButtonText(jp("バイパス（素の音と比較）"));

    gainSlider_.setRange(-24.0, 12.0, 0.5);
    gainSlider_.setTextValueSuffix(" dB");
    gainSlider_.setValue(settings_->getDoubleValue("gainDb", 0.0), juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*>{&inputLabel_, &outputLabel_, &brirTitle_, &brirLabel_,
                                                          &gainLabel_, &statusLabel_, &inputBox_, &refreshButton_,
                                                          &startButton_, &loadButton_, &bypassToggle_, &gainSlider_})
        addAndMakeVisible(c);

    refreshButton_.onClick = [this] { refreshInputs(); };
    startButton_.onClick = [this] {
        if (bridge_.load()) stopCapture();
        else startCapture();
    };
    loadButton_.onClick = [this] { chooseBrir(); };
    bypassToggle_.onClick = [this] {
        if (renderer_) renderer_->setBypass(bypassToggle_.getToggleState());
    };
    gainSlider_.onValueChange = [this] {
        if (renderer_) renderer_->setOutputGainDb(static_cast<float>(gainSlider_.getValue()));
        settings_->setValue("gainDb", gainSlider_.getValue());
    };

    std::unique_ptr<juce::XmlElement> deviceState = settings_->getXmlValue("outputDevice");
    deviceManager_.initialise(0, 2, deviceState.get(), true);
    deviceManager_.addAudioCallback(this);
    deviceManager_.addChangeListener(this);

    outputSelector_ = std::make_unique<juce::AudioDeviceSelectorComponent>(deviceManager_, 0, 0, 2, 2, false, false,
                                                                            false, false);
    addAndMakeVisible(*outputSelector_);

    refreshInputs();
    juce::File lastBrir(settings_->getValue("brirPath"));
    if (lastBrir.existsAsFile()) loadBrir(lastBrir);

    setSize(760, 640);
    startTimerHz(10);
}

MainComponent::~MainComponent()
{
    stopTimer();
    stopCapture();
    deviceManager_.removeAudioCallback(this);
    deviceManager_.removeChangeListener(this);
    saveSettings();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(16);
    auto row = [&](int height) {
        auto r = area.removeFromTop(height);
        area.removeFromTop(8);
        return r;
    };

    inputLabel_.setBounds(row(22));
    auto inputRow = row(28);
    startButton_.setBounds(inputRow.removeFromRight(90));
    inputRow.removeFromRight(8);
    refreshButton_.setBounds(inputRow.removeFromRight(70));
    inputRow.removeFromRight(8);
    inputBox_.setBounds(inputRow);

    brirTitle_.setBounds(row(22));
    auto brirRow = row(28);
    loadButton_.setBounds(brirRow.removeFromRight(170));
    brirLabel_.setBounds(brirRow);

    auto controls = row(28);
    bypassToggle_.setBounds(controls.removeFromLeft(240));
    gainLabel_.setBounds(controls.removeFromLeft(50));
    gainSlider_.setBounds(controls);

    statusLabel_.setBounds(area.removeFromBottom(110));
    area.removeFromBottom(8);
    outputLabel_.setBounds(row(22));
    outputSelector_->setBounds(area);
}

void MainComponent::refreshInputs()
{
    juce::String previous = settings_->getValue("inputName");
    if (inputBox_.getSelectedItemIndex() >= 0 && static_cast<std::size_t>(inputBox_.getSelectedItemIndex()) < inputs_.size())
        previous = juce::String::fromUTF8(inputs_[static_cast<std::size_t>(inputBox_.getSelectedItemIndex())].name.c_str());

    inputBox_.clear(juce::dontSendNotification);
    try {
        inputs_ = brir::io::listCaptureDevices();
    } catch (const std::exception& e) {
        inputs_.clear();
        showError(e.what());
    }
    for (std::size_t i = 0; i < inputs_.size(); ++i) {
        juce::String name = juce::String::fromUTF8(inputs_[i].name.c_str());
        inputBox_.addItem((inputs_[i].loopback ? jp("[ループバック] ") : jp("[入力] ")) + name, static_cast<int>(i) + 1);
        if (name == previous) inputBox_.setSelectedItemIndex(static_cast<int>(i), juce::dontSendNotification);
    }
}

void MainComponent::startCapture()
{
    int index = inputBox_.getSelectedItemIndex();
    juce::AudioIODevice* output = deviceManager_.getCurrentAudioDevice();
    if (index < 0 || static_cast<std::size_t>(index) >= inputs_.size()) return showError(jp("入力を選択してください。"));
    if (!output) return showError(jp("出力デバイスを選択してください。"));

    const brir::io::CaptureDevice& device = inputs_[static_cast<std::size_t>(index)];
    juce::String inputName = juce::String::fromUTF8(device.name.c_str());
    if (device.loopback && inputName == output->getName())
        return showError(jp("同じデバイスのループバックをそのデバイスへ出力すると音がループします。別の出力を選んでください。"));

    brir::ClockBridgeConfig config;
    config.outputRate = output->getCurrentSampleRate();
    try {
        capture_.start(device, config);
    } catch (const std::exception& e) {
        return showError(jp("入力を開始できませんでした: ") + e.what());
    }
    bridge_.store(&capture_.bridge(), std::memory_order_release);
    settings_->setValue("inputName", inputName);
    startButton_.setButtonText(jp("停止"));
}

void MainComponent::stopCapture()
{
    // Sequentially consistent on both sides: the callback announces itself, then
    // reads the pointer; we clear the pointer, then wait for announced callbacks.
    bridge_.store(nullptr);
    while (callbacksInFlight_.load() > 0) std::this_thread::yield();
    capture_.stop();
    startButton_.setButtonText(jp("開始"));
}

void MainComponent::chooseBrir()
{
    juce::File start(settings_->getValue("brirPath"));
    fileChooser_ = std::make_unique<juce::FileChooser>(jp("BRIR（4ch WAV）を選択"), start, "*.wav");
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& chooser) {
                                  juce::File file = chooser.getResult();
                                  if (file.existsAsFile()) loadBrir(file);
                              });
}

void MainComponent::loadBrir(const juce::File& file)
{
    brir::IrMatrix irs;
    double rate = 0.0;
    juce::String error;
    if (!readBrir(file, irs, rate, error)) return showError(error);

    brir_ = std::move(irs);
    brirRate_ = rate;
    brirName_ = file.getFileName();
    settings_->setValue("brirPath", file.getFullPathName());
    rebuildConvolver();
}

void MainComponent::rebuildConvolver()
{
    if (brir_.empty()) return;
    if (!renderer_) {
        brirLabel_.setText(brirName_ + jp("（出力デバイス待ち）"), juce::dontSendNotification);
        return;
    }
    brir::IrMatrix irs = brir_;
    juce::String note;
    if (std::abs(brirRate_ - deviceRate_) > 0.5) {
        // More taps per second means proportionally more gain; keep it unchanged.
        double scale = brirRate_ / deviceRate_;
        for (auto& row : irs)
            for (auto& ir : row) {
                ir = resampleIr(ir, brirRate_, deviceRate_);
                for (float& v : ir) v *= static_cast<float>(scale);
            }
        note = "  " + juce::String(brirRate_ / 1000.0, 1) + " -> " + juce::String(deviceRate_ / 1000.0, 1) + jp(" kHz に変換");
    }
    if (!renderer_->setConvolver(std::make_unique<brir::Convolver>(brir::Renderer::kFeeds, 2, irs)))
        return showError(jp("前のBRIRの切り替え中です。少し待ってから再度読み込んでください。"));
    brirLabel_.setText(brirName_ + "  (" + juce::String(brir_[0][0].size() / brirRate_, 2) + " s)" + note,
                       juce::dontSendNotification);
}

void MainComponent::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    double rate = device->getCurrentSampleRate();
    int block = device->getCurrentBufferSizeSamples();
    bool rateChanged = std::abs(rate - deviceRate_) > 0.5;
    deviceRate_ = rate;
    deviceBlock_ = block;

    left_.assign(kMaxRenderBlock, 0.0f);
    right_.assign(kMaxRenderBlock, 0.0f);
    if (!renderer_ || rateChanged) {
        renderer_ = std::make_unique<brir::Renderer>(rate, kMaxRenderBlock);
        renderer_->setBypass(bypassToggle_.getToggleState());
        renderer_->setOutputGainDb(static_cast<float>(gainSlider_.getValue()));
    }

    juce::MessageManager::callAsync([safeThis = juce::Component::SafePointer<MainComponent>(this), rateChanged] {
        MainComponent* self = safeThis.getComponent();
        if (!self) return;
        self->rebuildConvolver();
        // The bridge converts to the output rate, so it must be rebuilt for a new rate.
        if (rateChanged && self->bridge_.load()) {
            self->stopCapture();
            self->startCapture();
        }
    });
}

void MainComponent::audioDeviceStopped() {}

void MainComponent::audioDeviceIOCallbackWithContext(const float* const*, int, float* const* outputs, int numOutputs,
                                                     int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    callbacksInFlight_.fetch_add(1);
    brir::ClockBridge* bridge = bridge_.load();

    for (int offset = 0; offset < numSamples; offset += kMaxRenderBlock) {
        int n = std::min(kMaxRenderBlock, numSamples - offset);
        float* stereo[2] = {left_.data(), right_.data()};
        if (bridge) bridge->read(stereo, n);
        else {
            std::fill_n(left_.data(), n, 0.0f);
            std::fill_n(right_.data(), n, 0.0f);
        }
        if (renderer_) renderer_->process(stereo, stereo, n);

        for (int ch = 0; ch < numOutputs; ++ch) {
            if (!outputs[ch]) continue;
            if (ch < 2) std::copy_n(stereo[ch], n, outputs[ch] + offset);
            else std::fill_n(outputs[ch] + offset, n, 0.0f);
        }
    }
    callbacksInFlight_.fetch_sub(1);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    saveSettings();
}

void MainComponent::timerCallback()
{
    if (renderer_) renderer_->releaseRetired();

    juce::String text;
    juce::AudioIODevice* output = deviceManager_.getCurrentAudioDevice();
    brir::ClockBridge* bridge = bridge_.load();
    double totalMs = 0.0;

    if (bridge) {
        brir::ClockBridgeStats s = bridge->stats();
        brir::io::CaptureStats c = capture_.stats();
        text << jp("入力: ") << juce::String(c.sampleRate / 1000.0, 1) << " kHz / " << c.deviceChannels << "ch   "
             << jp("バッファ ") << juce::String(s.fillMs, 1) << " / " << juce::String(s.targetMs, 1) << " ms   "
             << jp("クロック差 ") << (s.driftPpm >= 0 ? "+" : "") << juce::String(s.driftPpm, 0) << " ppm   "
             << jp("途切れ ") << juce::String(static_cast<juce::int64>(s.underruns))
             << (s.running ? "" : jp("（バッファ中）")) << "\n";
        totalMs += s.targetMs + 1000.0 * bridge->resamplerLatencyFrames() / std::max(1.0, c.sampleRate);
        juce::String error = capture_.lastError();
        if (error.isNotEmpty()) text << jp("入力エラー: ") << error << "\n";
    } else {
        text << jp("入力: 停止中") << "\n";
    }

    if (output && deviceRate_ > 0.0) {
        double blockMs = 1000.0 * output->getCurrentBufferSizeSamples() / deviceRate_;
        double deviceMs = 1000.0 * output->getOutputLatencyInSamples() / deviceRate_;
        double renderMs = renderer_ ? 1000.0 * renderer_->latencySamples() / deviceRate_ : 0.0;
        text << jp("出力: ") << output->getName() << "  " << juce::String(deviceRate_ / 1000.0, 1) << " kHz / "
             << output->getCurrentBufferSizeSamples() << jp(" サンプル   処理 ") << juce::String(renderMs, 1) << " ms   CPU "
             << juce::String(deviceManager_.getCpuUsage() * 100.0, 1) << "%\n";
        totalMs += blockMs + deviceMs + renderMs;
        if (bridge) text << jp("推定遅延（取り込み→出力）: 約 ") << juce::String(totalMs, 0) << " ms\n";
    }
    if (renderer_ && renderer_->limiterGainDb() < -0.1f)
        text << jp("リミッター作動中: ") << juce::String(renderer_->limiterGainDb(), 1) << " dB\n";

    statusLabel_.setText(text, juce::dontSendNotification);
}

void MainComponent::saveSettings()
{
    if (std::unique_ptr<juce::XmlElement> state = deviceManager_.createStateXml())
        settings_->setValue("outputDevice", state.get());
    settings_->saveIfNeeded();
}

void MainComponent::showError(const juce::String& message)
{
    juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "BRIR Cinema", message);
}
