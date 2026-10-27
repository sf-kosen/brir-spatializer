#include "MainComponent.h"

#include "HeadphoneProfiles.h"

#include <cmath>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>

namespace {

juce::String jp(const char* utf8)
{
    return juce::String::fromUTF8(utf8);
}

constexpr int kMaxRenderBlock = 1024;

// Loads a cinema set: cinema.json naming one 2-channel (left ear, right ear) WAV
// per upmix feed. Levels are calibrated by the generator and used as they are.
bool readCinema(const juce::File& json, brir::IrMatrix& irs, double& rate, juce::String& name, juce::String& error)
{
    juce::var meta = juce::JSON::parse(json);
    if (!meta.isObject() || meta["format"].toString() != "brir-cinema/1") {
        error = jp("映画館セット（cinema.json）として読み込めませんでした。");
        return false;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    irs.assign(brir::Renderer::kFeeds, std::vector<std::vector<float>>(2));
    rate = 0.0;
    int found = 0;

    if (juce::Array<juce::var>* feeds = meta["feeds"].getArray()) {
        for (const juce::var& feed : *feeds) {
            juce::String feedName = feed["name"].toString();
            int index = -1;
            for (int f = 0; f < brir::Renderer::kFeeds; ++f)
                if (feedName == brir::Renderer::kFeedNames[f]) index = f;
            if (index < 0) continue; // feeds this app doesn't drive (yet)

            juce::File wav = json.getSiblingFile(feed["file"].toString());
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(wav));
            if (!reader || reader->numChannels != 2 || reader->lengthInSamples <= 0 ||
                reader->lengthInSamples > 10 * reader->sampleRate) {
                error = wav.getFileName() + jp(" を読み込めませんでした（2ch・10秒以内のWAVが必要です）。");
                return false;
            }
            if (rate > 0.0 && std::abs(rate - reader->sampleRate) > 0.5) {
                error = jp("映画館セット内のサンプルレートが揃っていません。");
                return false;
            }
            rate = reader->sampleRate;

            int length = static_cast<int>(reader->lengthInSamples);
            juce::AudioBuffer<float> buffer(2, length);
            reader->read(&buffer, 0, length, 0, true, true);
            for (int ear = 0; ear < 2; ++ear)
                irs[static_cast<std::size_t>(index)][static_cast<std::size_t>(ear)].assign(
                    buffer.getReadPointer(ear), buffer.getReadPointer(ear) + length);
            ++found;
        }
    }
    if (found == 0) {
        error = jp("映画館セットにL/C/R/Ls/Rs/LFEのいずれも含まれていません。");
        return false;
    }
    name = meta["name"].toString();
    return true;
}

// The folder of generated cinema sets (Generated/cinema next to the build tree).
juce::File findCinemaRoot()
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 8 && dir.exists(); ++i, dir = dir.getParentDirectory()) {
        juce::File candidate = dir.getChildFile("Generated/cinema");
        if (candidate.isDirectory()) return candidate;
    }
    return {};
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
    cinemaTitle_.setText(jp("映画館"), juce::dontSendNotification);
    cinemaLabel_.setText(jp("未読込"), juce::dontSendNotification);
    gainLabel_.setText(jp("音量"), juce::dontSendNotification);
    centerLabel_.setText(jp("センター"), juce::dontSendNotification);
    surroundLabel_.setText(jp("サラウンド"), juce::dontSendNotification);
    statusLabel_.setJustificationType(juce::Justification::topLeft);
    statusLabel_.setFont(juce::FontOptions(14.0f));

    refreshButton_.setButtonText(jp("更新"));
    startButton_.setButtonText(jp("開始"));
    loadButton_.setButtonText(jp("ほかを読み込む…"));
    cinemaBox_.onChange = [this] {
        int index = cinemaBox_.getSelectedItemIndex();
        if (index >= 0 && index < cinemaFiles_.size()) loadCinema(cinemaFiles_[index]);
    };
    bypassToggle_.setButtonText(jp("バイパス（素の音と比較）"));
    upmixToggle_.setButtonText(jp("アップミックス"));
    levelerToggle_.setButtonText(jp("自動ヘッドルーム"));
    headphoneLabel_.setText(jp("イヤホン補正"), juce::dontSendNotification);
    headphoneBox_.addItem(jp("なし"), 1);
    headphoneBox_.addItem("final E3000", 2);
    headphoneBox_.setSelectedId(settings_->getIntValue("headphone", 1), juce::dontSendNotification);
    headphoneBox_.onChange = [this] {
        settings_->setValue("headphone", headphoneBox_.getSelectedId());
        applyHeadphoneEq();
    };
    bassLabel_.setText(jp("低音"), juce::dontSendNotification);
    bassSlider_.setRange(0.0, 15.0, 0.5);
    bassSlider_.setTextValueSuffix(" dB");
    // Low shelf on top of the neutral (diffuse-field) correction; around +8 dB
    // is close to what most listeners prefer on in-ears.
    bassSlider_.setValue(settings_->getDoubleValue("bassDb", 8.0), juce::dontSendNotification);
    bassSlider_.onValueChange = [this] {
        settings_->setValue("bassDb", bassSlider_.getValue());
        applyHeadphoneEq();
    };
    upmixToggle_.setToggleState(settings_->getBoolValue("upmix", true), juce::dontSendNotification);
    // Lowers loud masters as a whole so the limiter only catches stray peaks.
    levelerToggle_.setToggleState(settings_->getBoolValue("leveler", true), juce::dontSendNotification);
    subToggle_.setButtonText(jp("サブウーファー"));
    subToggle_.setToggleState(settings_->getBoolValue("subwoofer", true), juce::dontSendNotification);
    subSlider_.setRange(-6.0, 12.0, 0.5);
    subSlider_.setTextValueSuffix(" dB");
    // Level relative to the mains; a little hot, as many cinemas run their subs.
    subSlider_.setValue(settings_->getDoubleValue("subDb", 4.0), juce::dontSendNotification);
    auto subChanged = [this] {
        settings_->setValue("subwoofer", subToggle_.getToggleState());
        settings_->setValue("subDb", subSlider_.getValue());
        applyBassManagement();
    };
    subToggle_.onClick = subChanged;
    subSlider_.onValueChange = subChanged;

    gainSlider_.setRange(-24.0, 12.0, 0.5);
    gainSlider_.setTextValueSuffix(" dB");
    // Default -3 dB: film mixes peak near 0 dBFS and the room adds some peak
    // level, so this keeps the limiter mostly idle.
    gainSlider_.setValue(settings_->getDoubleValue("gainDb", -3.0), juce::dontSendNotification);
    for (juce::Slider* slider : {&centerSlider_, &surroundSlider_}) {
        slider->setRange(0.0, 100.0, 1.0);
        slider->setTextValueSuffix(" %");
    }
    centerSlider_.setValue(settings_->getDoubleValue("centerAmount", 100.0), juce::dontSendNotification);
    // Kept low: in a cinema most of the sound comes from the screen.
    surroundSlider_.setValue(settings_->getDoubleValue("ambienceToSurround", 30.0), juce::dontSendNotification);

    for (auto* c : std::initializer_list<juce::Component*>{
             &inputLabel_, &outputLabel_, &cinemaTitle_, &cinemaLabel_, &gainLabel_, &centerLabel_, &surroundLabel_,
             &statusLabel_, &inputBox_, &cinemaBox_, &refreshButton_, &startButton_, &loadButton_, &bypassToggle_, &upmixToggle_, &levelerToggle_,
             &gainSlider_, &centerSlider_, &surroundSlider_, &headphoneLabel_, &headphoneBox_, &bassLabel_, &bassSlider_, &subToggle_, &subSlider_})
        addAndMakeVisible(c);

    refreshButton_.onClick = [this] { refreshInputs(); };
    startButton_.onClick = [this] {
        if (bridge_.load()) stopCapture();
        else startCapture();
    };
    loadButton_.onClick = [this] { chooseCinema(); };
    bypassToggle_.onClick = [this] {
        if (renderer_) renderer_->setBypass(bypassToggle_.getToggleState());
    };
    levelerToggle_.onClick = [this] {
        if (renderer_) renderer_->setLevelerEnabled(levelerToggle_.getToggleState());
        settings_->setValue("leveler", levelerToggle_.getToggleState());
    };
    gainSlider_.onValueChange = [this] {
        if (renderer_) renderer_->setOutputGainDb(static_cast<float>(gainSlider_.getValue()));
        settings_->setValue("gainDb", gainSlider_.getValue());
    };
    auto upmixChanged = [this] {
        applyUpmixSettings();
        settings_->setValue("upmix", upmixToggle_.getToggleState());
        settings_->setValue("centerAmount", centerSlider_.getValue());
        settings_->setValue("ambienceToSurround", surroundSlider_.getValue());
    };
    upmixToggle_.onClick = upmixChanged;
    centerSlider_.onValueChange = upmixChanged;
    surroundSlider_.onValueChange = upmixChanged;

    std::unique_ptr<juce::XmlElement> deviceState = settings_->getXmlValue("outputDevice");
    deviceManager_.initialise(0, 2, deviceState.get(), true);
    deviceManager_.addAudioCallback(this);
    deviceManager_.addChangeListener(this);

    outputSelector_ = std::make_unique<juce::AudioDeviceSelectorComponent>(deviceManager_, 0, 0, 2, 2, false, false,
                                                                            false, false);
    addAndMakeVisible(*outputSelector_);

    refreshInputs();
    refreshCinemas();
    juce::File cinema(settings_->getValue("cinemaPath"));
    if (!cinema.existsAsFile() && !cinemaFiles_.isEmpty()) cinema = cinemaFiles_[0];
    if (cinema.existsAsFile()) loadCinema(cinema);

    setSize(760, 776);
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

    cinemaTitle_.setBounds(row(22));
    auto cinemaRow = row(28);
    loadButton_.setBounds(cinemaRow.removeFromRight(140));
    cinemaRow.removeFromRight(8);
    cinemaBox_.setBounds(cinemaRow.removeFromLeft(cinemaRow.getWidth() / 2));
    cinemaRow.removeFromLeft(8);
    cinemaLabel_.setBounds(cinemaRow);

    auto upmixRow = row(28);
    upmixToggle_.setBounds(upmixRow.removeFromLeft(140));
    auto centerArea = upmixRow.removeFromLeft(upmixRow.getWidth() / 2);
    centerLabel_.setBounds(centerArea.removeFromLeft(70));
    centerSlider_.setBounds(centerArea);
    surroundLabel_.setBounds(upmixRow.removeFromLeft(80));
    surroundSlider_.setBounds(upmixRow);

    auto subRow = row(28);
    subToggle_.setBounds(subRow.removeFromLeft(140));
    subSlider_.setBounds(subRow);

    auto headphoneRow = row(28);
    headphoneLabel_.setBounds(headphoneRow.removeFromLeft(100));
    headphoneBox_.setBounds(headphoneRow.removeFromLeft(220));
    headphoneRow.removeFromLeft(16);
    bassLabel_.setBounds(headphoneRow.removeFromLeft(50));
    bassSlider_.setBounds(headphoneRow);

    auto controls = row(28);
    bypassToggle_.setBounds(controls.removeFromLeft(240));
    levelerToggle_.setBounds(controls.removeFromLeft(160));
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

    juce::File log = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("BrirCinema/diagnostics.csv");
    log.deleteFile();
    diagnostics_ = std::make_unique<juce::FileOutputStream>(log);
    if (diagnostics_->openedOk()) {
        *diagnostics_ << "seconds,running,fill_ms,target_ms,buffered_ms,gap_ms,margin_ms,drift_ppm,underruns,overflows,"
                         "resyncs,capture_discontinuities,silence_fills,max_delivery_gap_ms,late_blocks,leveler_db,limiter_db,cpu,device_rate,"
                         "device_block,input,output\n";
        *diagnostics_ << "0,,,,,,,,,,,,,,,,,," << juce::String(output->getCurrentSampleRate(), 0) << ","
                      << output->getCurrentBufferSizeSamples() << ",\"" << inputName << "\",\"" << output->getName()
                      << "\"\n";
        diagnostics_->flush();
    } else {
        diagnostics_.reset();
    }
    diagnosticsStart_ = juce::Time::getMillisecondCounterHiRes() / 1000.0;
    startButton_.setButtonText(jp("停止"));
}

void MainComponent::stopCapture()
{
    // Sequentially consistent on both sides: the callback announces itself, then
    // reads the pointer; we clear the pointer, then wait for announced callbacks.
    bridge_.store(nullptr);
    while (callbacksInFlight_.load() > 0) std::this_thread::yield();
    capture_.stop();
    diagnostics_.reset();
    startButton_.setButtonText(jp("開始"));
}

void MainComponent::chooseCinema()
{
    juce::File start(settings_->getValue("cinemaPath"));
    fileChooser_ = std::make_unique<juce::FileChooser>(jp("映画館セット（cinema.json）を選択"), start, "cinema.json");
    fileChooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this](const juce::FileChooser& chooser) {
                                  juce::File file = chooser.getResult();
                                  if (file.existsAsFile()) loadCinema(file);
                              });
}

void MainComponent::refreshCinemas()
{
    cinemaFiles_.clear();
    cinemaBox_.clear(juce::dontSendNotification);
    juce::File root = findCinemaRoot();
    if (!root.isDirectory()) return;
    juce::Array<juce::File> dirs = root.findChildFiles(juce::File::findDirectories, false);
    dirs.sort();
    for (const juce::File& dir : dirs) {
        juce::File json = dir.getChildFile("cinema.json");
        if (!json.existsAsFile()) continue;
        juce::String name = juce::JSON::parse(json)["name"].toString();
        cinemaFiles_.add(json);
        cinemaBox_.addItem(name.isNotEmpty() ? name : dir.getFileName(), cinemaFiles_.size());
    }
}

void MainComponent::loadCinema(const juce::File& file)
{
    brir::IrMatrix irs;
    double rate = 0.0;
    juce::String name, error;
    if (!readCinema(file, irs, rate, name, error)) return showError(error);

    cinemaHasLfe_ = !irs[brir::Renderer::kLfe][0].empty();
    cinema_ = std::move(irs);
    cinemaRate_ = rate;
    cinemaName_ = name.isNotEmpty() ? name : file.getParentDirectory().getFileName();
    settings_->setValue("cinemaPath", file.getFullPathName());
    cinemaBox_.setSelectedItemIndex(cinemaFiles_.indexOf(file), juce::dontSendNotification);
    applyBassManagement();
    rebuildConvolver();
}

void MainComponent::applyBassManagement()
{
    // Older sets have no LFE feed; their mains keep the full range.
    subToggle_.setEnabled(cinemaHasLfe_);
    subSlider_.setEnabled(cinemaHasLfe_ && subToggle_.getToggleState());
    if (renderer_)
        renderer_->setBassManagement(cinemaHasLfe_ && subToggle_.getToggleState(), static_cast<float>(subSlider_.getValue()));
}

void MainComponent::applyHeadphoneEq()
{
    bool on = headphoneBox_.getSelectedId() == 2;
    bassSlider_.setEnabled(on);
    if (!renderer_) return;
    if (!on) {
        renderer_->headphoneEq().setProfile({}, 0.0f);
        return;
    }
    std::vector<brir::EqBand> bands(headphone_profiles::final_e3000.begin(), headphone_profiles::final_e3000.end());
    bands.push_back({brir::EqBand::LowShelf, 120.0f, static_cast<float>(bassSlider_.getValue()), 0.7f});
    // Loudness-matched rather than peak-safe: the correction cuts the bass
    // heavily, so a peak-based preamp made everything ~10 dB quieter. The
    // output limiter catches the rare overs in the boosted presence region.
    float preampDb = -std::max(0.0f, brir::HeadphoneEq::averageGainDb(bands, 48000.0));
    renderer_->headphoneEq().setProfile(bands, preampDb);
}

void MainComponent::applyUpmixSettings()
{
    bool enabled = upmixToggle_.getToggleState();
    centerSlider_.setEnabled(enabled);
    surroundSlider_.setEnabled(enabled);
    if (!renderer_) return;
    brir::Upmixer& upmixer = renderer_->upmixer();
    upmixer.setEnabled(enabled);
    upmixer.setCenterAmount(static_cast<float>(centerSlider_.getValue() / 100.0));
    upmixer.setSurroundAmount(static_cast<float>(surroundSlider_.getValue() / 100.0));
}

void MainComponent::rebuildConvolver()
{
    if (cinema_.empty()) return;
    if (!renderer_) {
        cinemaLabel_.setText(cinemaName_ + jp("（出力デバイス待ち）"), juce::dontSendNotification);
        return;
    }
    brir::IrMatrix irs = cinema_;
    juce::String note;
    if (std::abs(cinemaRate_ - deviceRate_) > 0.5) {
        // More taps per second means proportionally more gain; keep it unchanged.
        double scale = cinemaRate_ / deviceRate_;
        for (auto& row : irs)
            for (auto& ir : row) {
                if (ir.empty()) continue;
                ir = resampleIr(ir, cinemaRate_, deviceRate_);
                for (float& v : ir) v *= static_cast<float>(scale);
            }
        note = "  " + juce::String(cinemaRate_ / 1000.0, 1) + " -> " + juce::String(deviceRate_ / 1000.0, 1) + jp(" kHz に変換");
    }
    // Tail stages run on worker threads with deadlines; give them the same
    // scheduling class as the audio threads.
    brir::ConvolverConfig config;
    config.onWorkerStart = [] {
        DWORD task = 0;
        AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    };
    if (!renderer_->setConvolver(std::make_unique<brir::Convolver>(brir::Renderer::kFeeds, 2, irs, config)))
        return showError(jp("前の映画館セットの切り替え中です。少し待ってから再度読み込んでください。"));
    cinemaLabel_.setText(note.trim(), juce::dontSendNotification);
}

void MainComponent::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    double rate = device->getCurrentSampleRate();
    bool rateChanged = std::abs(rate - deviceRate_) > 0.5;
    deviceRate_ = rate;

    left_.assign(kMaxRenderBlock, 0.0f);
    right_.assign(kMaxRenderBlock, 0.0f);
    if (!renderer_ || rateChanged) {
        renderer_ = std::make_unique<brir::Renderer>(rate, kMaxRenderBlock);
        renderer_->setBypass(bypassToggle_.getToggleState());
        renderer_->setOutputGainDb(static_cast<float>(gainSlider_.getValue()));
        renderer_->setLevelerEnabled(levelerToggle_.getToggleState());
        applyUpmixSettings();
        applyHeadphoneEq();
        applyBassManagement();
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

void MainComponent::logDiagnostics()
{
    brir::ClockBridge* bridge = bridge_.load();
    if (!diagnostics_ || !bridge) return;
    brir::ClockBridgeStats s = bridge->stats();
    brir::io::CaptureStats c = capture_.stats();
    juce::AudioIODevice* output = deviceManager_.getCurrentAudioDevice();
    double t = juce::Time::getMillisecondCounterHiRes() / 1000.0 - diagnosticsStart_;
    *diagnostics_ << juce::String(t, 1) << "," << (s.running ? 1 : 0) << "," << juce::String(s.fillMs, 2) << ","
                  << juce::String(s.targetMs, 2) << "," << juce::String(s.bufferedMs, 2) << "," << juce::String(s.gapMs, 2)
                  << "," << juce::String(s.marginMs, 2) << "," << juce::String(s.driftPpm, 1) << ","
                  << juce::String(static_cast<juce::int64>(s.underruns)) << ","
                  << juce::String(static_cast<juce::int64>(s.overflows)) << ","
                  << juce::String(static_cast<juce::int64>(s.resyncs)) << ","
                  << juce::String(static_cast<juce::int64>(c.discontinuities)) << ","
                  << juce::String(static_cast<juce::int64>(c.silenceFills)) << ","
                  << juce::String(capture_.takeMaxDeliveryGapMs(), 1) << ","
                  << juce::String(static_cast<juce::int64>(renderer_ ? renderer_->lateBlocks() : 0)) << ","
                  << juce::String(renderer_ ? renderer_->levelerGainDb() : 0.0f, 1) << ","
                  << juce::String(renderer_ ? renderer_->limiterGainDb() : 0.0f, 1) << ","
                  << juce::String(deviceManager_.getCpuUsage() * 100.0, 1) << ","
                  << (output ? juce::String(output->getCurrentSampleRate(), 0) : juce::String()) << ","
                  << (output ? juce::String(output->getCurrentBufferSizeSamples()) : juce::String()) << ",,\n";
    diagnostics_->flush();
}

void MainComponent::timerCallback()
{
    if (renderer_) renderer_->releaseRetired();
    if (++timerTicks_ % 10 == 0) logDiagnostics();

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
             << jp("途切れ ") << juce::String(static_cast<juce::int64>(s.underruns)) << " / "
             << jp("再同期 ") << juce::String(static_cast<juce::int64>(s.resyncs + s.overflows)) << " / "
             << jp("畳み込み遅れ ") << juce::String(static_cast<juce::int64>(renderer_ ? renderer_->lateBlocks() : 0))
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
    if (renderer_ && renderer_->levelerGainDb() < -0.1f)
        text << jp("自動ヘッドルーム: ") << juce::String(renderer_->levelerGainDb(), 1) << " dB   ";
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
