#pragma once

// Loads a cinema set (cinema.json + one 2-channel WAV per feed) for the tools.

#include "brir/convolver.h"
#include "brir/renderer.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>

namespace brir::tools {

inline bool loadCinema(const juce::File& json, double rate, IrMatrix& irs)
{
    juce::var meta = juce::JSON::parse(json);
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    irs.assign(Renderer::kFeeds, std::vector<std::vector<float>>(2));
    juce::Array<juce::var>* feeds = meta["feeds"].getArray();
    if (!feeds) return false;
    for (const juce::var& feed : *feeds) {
        for (int f = 0; f < Renderer::kFeeds; ++f) {
            if (feed["name"].toString() != Renderer::kFeedNames[f]) continue;
            std::unique_ptr<juce::AudioFormatReader> reader(
                formats.createReaderFor(json.getSiblingFile(feed["file"].toString())));
            if (!reader || reader->numChannels != 2) return false;
            int length = static_cast<int>(reader->lengthInSamples);
            juce::AudioBuffer<float> buffer(2, length);
            reader->read(&buffer, 0, length, 0, true, true);
            if (std::abs(reader->sampleRate - rate) > 0.5) {
                // Windowed-sinc resample; gain scales with the tap-rate ratio.
                double speed = reader->sampleRate / rate;
                int outLength = static_cast<int>(std::ceil(length / speed));
                for (int ear = 0; ear < 2; ++ear) {
                    std::vector<float> padded(buffer.getReadPointer(ear), buffer.getReadPointer(ear) + length);
                    padded.resize(padded.size() + 64, 0.0f);
                    std::vector<float> out(static_cast<std::size_t>(outLength));
                    juce::WindowedSincInterpolator interpolator;
                    interpolator.process(speed, padded.data(), out.data(), outLength);
                    for (float& v : out) v *= static_cast<float>(speed);
                    irs[static_cast<std::size_t>(f)][static_cast<std::size_t>(ear)] = std::move(out);
                }
            } else {
                for (int ear = 0; ear < 2; ++ear)
                    irs[static_cast<std::size_t>(f)][static_cast<std::size_t>(ear)].assign(
                        buffer.getReadPointer(ear), buffer.getReadPointer(ear) + length);
            }
        }
    }
    return true;
}

} // namespace brir::tools
