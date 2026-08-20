#include "BasicPitch.h"

#include "BasicPitchCNN.h"
#include "BasicPitchConstants.h"
#include "Features.h"
#include "Notes.h"

#include <algorithm>
#include <cmath>
#include <exception>

namespace basicpitch
{

struct Transcriber::Impl
{
    Features features;
    std::unique_ptr<BasicPitchCNN> cnn;
    Notes notes;
};

Transcriber::Transcriber() : impl (std::make_unique<Impl>()) {}
Transcriber::~Transcriber() = default;

bool Transcriber::prepare (const Weights& weights, std::string& error)
{
    if (! impl->features.loadKernels (weights.cqtKernels, weights.cqtKernelBytes))
    {
        error = "CQT kernel blob missing or malformed";
        return false;
    }

    if (weights.cnnContourJson == nullptr || weights.cnnNoteJson == nullptr
        || weights.cnnOnsetInputJson == nullptr || weights.cnnOnsetOutputJson == nullptr)
    {
        error = "CNN weights missing";
        return false;
    }

    try
    {
        impl->cnn = std::make_unique<BasicPitchCNN> (
            BasicPitchCNN::ModelJson { weights.cnnContourJson, weights.cnnContourJsonBytes },
            BasicPitchCNN::ModelJson { weights.cnnNoteJson, weights.cnnNoteJsonBytes },
            BasicPitchCNN::ModelJson { weights.cnnOnsetInputJson, weights.cnnOnsetInputJsonBytes },
            BasicPitchCNN::ModelJson { weights.cnnOnsetOutputJson, weights.cnnOnsetOutputJsonBytes });
    }
    catch (const std::exception& e)
    {
        error = std::string ("CNN weights unreadable: ") + e.what();
        impl->cnn.reset();
        return false;
    }

    return true;
}

bool Transcriber::transcribe (const float* audio, int numSamples, const Params& params,
                              const std::function<void (float)>& onProgress,
                              const std::function<bool()>& shouldContinue,
                              std::vector<Note>& out, std::string& error)
{
    out.clear();

    if (impl->cnn == nullptr)
    {
        error = "transcriber not prepared";
        return false;
    }

    if (audio == nullptr || numSamples <= 0)
    {
        error = "no audio";
        return false;
    }

    const auto alive = [&shouldContinue] { return shouldContinue == nullptr || shouldContinue(); };
    const auto report = [&onProgress] (float p) { if (onProgress != nullptr) onProgress (p); };

    // The nine-octave decimation needs about 1.5 s of signal; a shorter stem is
    // padded rather than refused, so a four-bar loop still transcribes.
    std::vector<float> padded;

    if (numSamples < Features::minSamples)
    {
        padded.assign ((size_t) Features::minSamples, 0.0f);
        std::copy (audio, audio + numSamples, padded.begin());
        audio = padded.data();
        numSamples = Features::minSamples;
    }

    report (0.0f);

    const auto numFrames = impl->features.compute (audio, numSamples);

    if (numFrames <= 0)
    {
        error = "CQT front end failed";
        return false;
    }

    if (! alive())
        return false;

    // The CQT is roughly a fifth of the work; the rest is the CNN.
    report (0.2f);

    const auto frames = (size_t) numFrames;

    if (frames <= (size_t) BasicPitchCNN::getNumFramesLookahead())
    {
        error = "clip too short to transcribe"; // Features::minSamples rules this out
        return false;
    }

    std::vector<std::vector<float>> contoursPG (frames, std::vector<float> (NUM_FREQ_IN, 0.0f));
    std::vector<std::vector<float>> notesPG (frames, std::vector<float> (NUM_FREQ_OUT, 0.0f));
    std::vector<std::vector<float>> onsetsPG (frames, std::vector<float> (NUM_FREQ_OUT, 0.0f));

    impl->cnn->reset();

    const auto lookahead = (size_t) BasicPitchCNN::getNumFramesLookahead();

    std::vector<float> frameInput ((size_t) (NUM_HARMONICS * NUM_FREQ_IN), 0.0f);
    const std::vector<float> silence ((size_t) (NUM_HARMONICS * NUM_FREQ_IN), 0.0f);

    // The CNN is causal with a fixed lookahead, so the stream is primed with
    // `lookahead` silent frames and then `lookahead` real ones whose outputs
    // belong to frames that do not exist yet; both go to the bin at index 0.
    for (size_t i = 0; i < lookahead; ++i)
        impl->cnn->frameInference (silence.data(), contoursPG[0], notesPG[0], onsetsPG[0]);

    for (size_t i = 0; i < lookahead && i < frames; ++i)
    {
        impl->features.harmonicStack ((int) i, frameInput.data());
        impl->cnn->frameInference (frameInput.data(), contoursPG[0], notesPG[0], onsetsPG[0]);
    }

    // Poll cancellation and progress about a hundred times over the clip rather
    // than every frame — a frame is microseconds.
    const auto chunk = std::max<size_t> (256, frames / 100);

    for (size_t i = lookahead; i < frames; ++i)
    {
        impl->features.harmonicStack ((int) i, frameInput.data());
        impl->cnn->frameInference (frameInput.data(), contoursPG[i - lookahead],
                                   notesPG[i - lookahead], onsetsPG[i - lookahead]);

        if (i % chunk == 0)
        {
            if (! alive())
                return false;

            report (0.2f + 0.75f * (float) i / (float) frames);
        }
    }

    // Flush the lookahead: silence in, the clip's last frames out.
    for (size_t i = frames; i < frames + lookahead; ++i)
        impl->cnn->frameInference (silence.data(), contoursPG[i - lookahead],
                                   notesPG[i - lookahead], onsetsPG[i - lookahead]);

    if (! alive())
        return false;

    report (0.95f);

    // The CQT has done its job; note creation allocates about as much again,
    // so hand the memory back before it does rather than after.
    impl->features.clear();

    // Notes::convert turns these straight into array indices without clamping,
    // so a band outside the model's own 27.5 Hz .. 4186 Hz range would walk off
    // the posteriorgram.
    const auto lowestHz = 440.0f * std::pow (2.0f, (float) (MIN_MIDI_NOTE - 69) / 12.0f);
    const auto highestHz = 440.0f * std::pow (2.0f, (float) (MAX_MIDI_NOTE - 69) / 12.0f);

    Notes::ConvertParams convert;
    convert.onsetThreshold = params.onsetThreshold;
    convert.frameThreshold = params.frameThreshold;
    convert.minNoteLength = (int) std::lround (params.minNoteLengthMs / 1000.0f
                                               / ((float) FFT_HOP / (float) AUDIO_SAMPLE_RATE));
    convert.minFrequency = params.minFrequency < 0.0f
                               ? -1.0f : std::max (params.minFrequency, lowestHz);
    convert.maxFrequency = params.maxFrequency < 0.0f
                               ? -1.0f : std::min (params.maxFrequency, highestHz);
    convert.inferOnsets = true;
    convert.melodiaTrick = true;
    convert.pitchBend = NoPitchBend; // a stem drops into a DAW as plain notes

    impl->notes.clear();

    const auto events = impl->notes.convert (notesPG, onsetsPG, contoursPG, convert, true);

    // The melodia pass sweeps the whole posteriorgram rather than the requested
    // band, so the band is enforced again on the way out.
    const auto lowestPitch = convert.minFrequency < 0.0f
                                 ? MIN_MIDI_NOTE : NoteUtils::hzToMidi (convert.minFrequency);
    const auto highestPitch = convert.maxFrequency < 0.0f
                                  ? MAX_MIDI_NOTE : NoteUtils::hzToMidi (convert.maxFrequency);

    out.reserve (events.size());

    for (const auto& event : events)
        if (event.pitch >= lowestPitch && event.pitch <= highestPitch)
            out.push_back ({ event.startTime, event.endTime, event.pitch, event.amplitude });

    // convert() walks the clip backwards, so the events come out newest-first
    // within a frame; sort by time for a MIDI writer that expects it.
    std::sort (out.begin(), out.end(), [] (const Note& a, const Note& b)
    {
        return a.startSeconds < b.startSeconds
               || (a.startSeconds == b.startSeconds && a.pitch < b.pitch);
    });

    impl->notes.clear();

    report (1.0f);

    return true;
}

} // namespace basicpitch
