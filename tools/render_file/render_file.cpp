// Renders a stereo audio file through the same chain as the app (upmix ->
// cinema BRIRs -> leveler -> limiter) into a binaural WAV, for listening and analysis.
//
//   render_file <cinema.json> <input> <output.wav> [--no-upmix] [--center N] [--surround N] [--sub dB]
//               [--gain dB] [--no-leveler] [--feeds feeds.wav]
//
// --sub sets the subwoofer level when the set has an LFE feed (default +4 dB).
// --gain is the output gain (the app's gain slider; default 0 dB).
// --no-leveler turns the automatic headroom off, leaving only the limiter.
// --feeds also writes the five upmix feeds (L C R Ls Rs) for analysis.

#include "brir/renderer.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdio>
#include <cstring>

namespace {

bool loadCinema(const juce::File& json, double rate, brir::IrMatrix& irs)
{
    juce::var meta = juce::JSON::parse(json);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    irs.assign(brir::Renderer::kFeeds, std::vector<std::vector<float>>(2));
    juce::Array<juce::var>* feeds = meta["feeds"].getArray();
    if (!feeds) return false;
    for (const juce::var& feed : *feeds) {
        for (int f = 0; f < brir::Renderer::kFeeds; ++f) {
            if (feed["name"].toString() != brir::Renderer::kFeedNames[f]) continue;
            std::unique_ptr<juce::AudioFormatReader> reader(
                formats.createReaderFor(json.getSiblingFile(feed["file"].toString())));
            if (!reader || reader->numChannels != 2 || std::abs(reader->sampleRate - rate) > 0.5) return false;
            int length = static_cast<int>(reader->lengthInSamples);
            juce::AudioBuffer<float> buffer(2, length);
            reader->read(&buffer, 0, length, 0, true, true);
            for (int ear = 0; ear < 2; ++ear)
                irs[static_cast<std::size_t>(f)][static_cast<std::size_t>(ear)].assign(buffer.getReadPointer(ear),
                                                                                       buffer.getReadPointer(ear) + length);
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: render_file <cinema.json> <input> <output.wav> [--no-upmix] [--center N] [--surround N] [--sub dB] [--gain dB] [--no-leveler]\n");
        return 1;
    }
    juce::File cinema(juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[1])));
    juce::File input(juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[2])));
    juce::File output(juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[3])));
    bool upmix = true, leveler = true;
    float center = 1.0f, surround = 0.3f, subDb = 4.0f, gainDb = 0.0f;
    juce::File feedsFile;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-upmix") == 0) upmix = false;
        else if (std::strcmp(argv[i], "--no-leveler") == 0) leveler = false;
        else if (std::strcmp(argv[i], "--gain") == 0 && i + 1 < argc) gainDb = std::stof(argv[++i]);
        else if (std::strcmp(argv[i], "--center") == 0 && i + 1 < argc) center = std::stof(argv[++i]) / 100.0f;
        else if (std::strcmp(argv[i], "--surround") == 0 && i + 1 < argc) surround = std::stof(argv[++i]) / 100.0f;
        else if (std::strcmp(argv[i], "--sub") == 0 && i + 1 < argc) subDb = std::stof(argv[++i]);
        else if (std::strcmp(argv[i], "--feeds") == 0 && i + 1 < argc)
            feedsFile = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[++i]));
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(input));
    if (!reader) {
        std::fprintf(stderr, "Cannot read input.\n");
        return 1;
    }
    double rate = reader->sampleRate;
    int length = static_cast<int>(reader->lengthInSamples);
    juce::AudioBuffer<float> audio(2, length);
    reader->read(&audio, 0, length, 0, true, true);
    if (reader->numChannels == 1) audio.copyFrom(1, 0, audio, 0, 0, length);

    brir::IrMatrix irs;
    if (!loadCinema(cinema, rate, irs)) {
        std::fprintf(stderr, "Cannot load cinema set at %.0f Hz.\n", rate);
        return 1;
    }

    constexpr int block = 512;
    brir::Renderer renderer(rate, block);
    renderer.upmixer().setEnabled(upmix);
    renderer.upmixer().setCenterAmount(center);
    renderer.upmixer().setSurroundAmount(surround);
    renderer.setBassManagement(!irs[brir::Renderer::kLfe][0].empty(), subDb);
    renderer.setOutputGainDb(gainDb);
    renderer.setLevelerEnabled(leveler);
    brir::ConvolverConfig config;
    config.threaded = false; // offline: deterministic, no deadlines
    renderer.setConvolver(std::make_unique<brir::Convolver>(brir::Renderer::kFeeds, 2, irs, config));
    renderer.process(nullptr, nullptr, 0); // adopt the convolver so latencySamples() includes it

    // Pad with silence so the reverb tail is kept, then drop the latency.
    int latency = renderer.latencySamples();
    int tail = static_cast<int>(rate * 1.5);
    juce::AudioBuffer<float> out(2, length + tail + latency);
    out.clear();
    out.copyFrom(0, 0, audio, 0, 0, length);
    out.copyFrom(1, 0, audio, 1, 0, length);
    // Optional: the upmix feeds on their own (same settings, separate instance).
    juce::AudioBuffer<float> feeds;
    if (feedsFile != juce::File()) {
        brir::Upmixer upmixer(rate);
        upmixer.setEnabled(upmix);
        upmixer.setCenterAmount(center);
        upmixer.setSurroundAmount(surround);
        feeds.setSize(brir::Upmixer::kFeeds, out.getNumSamples());
        for (int pos = 0; pos < out.getNumSamples(); pos += block) {
            int n = std::min(block, out.getNumSamples() - pos);
            const float* in[2] = {out.getReadPointer(0, pos), out.getReadPointer(1, pos)};
            float* f[brir::Upmixer::kFeeds];
            for (int i = 0; i < brir::Upmixer::kFeeds; ++i) f[i] = feeds.getWritePointer(i, pos);
            upmixer.process(in, f, n);
        }
    }

    int limitedBlocks = 0, leveledBlocks = 0, blocks = 0;
    float deepest = 0.0f, deepestLevel = 0.0f;
    double levelSum = 0.0;
    for (int pos = 0; pos < out.getNumSamples(); pos += block) {
        int n = std::min(block, out.getNumSamples() - pos);
        float* io[2] = {out.getWritePointer(0, pos), out.getWritePointer(1, pos)};
        renderer.process(io, io, n);
        float gr = renderer.limiterGainDb();
        deepest = std::min(deepest, gr);
        limitedBlocks += gr < -0.1f ? 1 : 0;
        float lv = renderer.levelerGainDb();
        deepestLevel = std::min(deepestLevel, lv);
        leveledBlocks += lv < -0.1f ? 1 : 0;
        levelSum += lv;
        ++blocks;
    }
    std::printf("Leveler active in %.1f%% of blocks, mean %.1f dB, deepest %.1f dB\n", 100.0 * leveledBlocks / blocks,
                levelSum / blocks, deepestLevel);
    std::printf("Limiter active in %.1f%% of blocks, deepest %.1f dB\n", 100.0 * limitedBlocks / blocks, deepest);

    if (feeds.getNumChannels() > 0) {
        int upLatency = brir::Upmixer(rate).latencySamples();
        juce::AudioBuffer<float> trimmedFeeds(brir::Upmixer::kFeeds, length);
        for (int ch = 0; ch < brir::Upmixer::kFeeds; ++ch) trimmedFeeds.copyFrom(ch, 0, feeds, ch, upLatency, length);
        feedsFile.deleteFile();
        std::unique_ptr<juce::OutputStream> fs = feedsFile.createOutputStream();
        juce::WavAudioFormat fmt;
        std::unique_ptr<juce::AudioFormatWriter> w(fmt.createWriterFor(fs.get(), rate, brir::Upmixer::kFeeds, 32, {}, 0));
        if (w) {
            fs.release();
            w->writeFromAudioSampleBuffer(trimmedFeeds, 0, length);
        }
    }

    juce::AudioBuffer<float> trimmed(2, length + tail);
    for (int ch = 0; ch < 2; ++ch) trimmed.copyFrom(ch, 0, out, ch, latency, length + tail);

    output.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = output.createOutputStream();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer(
        wav.createWriterFor(stream.get(), rate, 2, 32, {}, 0));
    if (!writer) {
        std::fprintf(stderr, "Cannot write output.\n");
        return 1;
    }
    stream.release();
    writer->writeFromAudioSampleBuffer(trimmed, 0, trimmed.getNumSamples());
    std::printf("Rendered %.2f s at %.0f Hz (latency %d samples, limiter %.1f dB at end)\n",
                trimmed.getNumSamples() / rate, rate, latency, renderer.limiterGainDb());
    return 0;
}
