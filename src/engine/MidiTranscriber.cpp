#include "MidiTranscriber.h"
#include "StemCache.h"

#include <juce_dsp/juce_dsp.h>

#include "BasicPitch.h"       // third_party/basicpitch, SYSTEM-included
#include "BasicPitchWeights.h" // juce_add_binary_data (SplittrModels)

#include <cmath>

namespace splittr
{

namespace
{
    // Basic Pitch is trained at 22.05 kHz; anything else has to be resampled
    // to it before the CQT front end sees it.
    constexpr double modelSampleRate = 22050.0;

    // A quarter note is 960 ticks — the resolution most DAWs write, and fine
    // enough that a 1/64 triplet still lands on an integer.
    constexpr int ticksPerQuarterNote = 960;

    // With no detected tempo the file still needs *a* tempo, and 120 BPM makes
    // one beat half a second, so the notes sit at their wall-clock positions.
    constexpr double fallbackBpm = 120.0;

    struct StemParams
    {
        float minFrequency = -1.0f;
        float maxFrequency = -1.0f;
    };

    // Only the two stems with a genuinely narrower range get one. Basic Pitch's
    // thresholds stay at Spotify's defaults everywhere (see Params); the band
    // is the one place where knowing which stem this is actually buys accuracy.
    //
    //   Bass    A0 .. F4   — kills the octave-up ghosts a bass line grows when
    //                        the model latches onto the second harmonic.
    //   Vocals  C2 .. E6   — a hair either side of the human range, so breath
    //                        and cymbal bleed stop becoming notes.
    //
    // Guitar, Piano and Other keep the full 27.5 Hz .. 4186 Hz range: they are
    // the stems where a wrong guess would silently delete real notes.
    StemParams paramsForStem (int stemKind)
    {
        switch (stemKind)
        {
            case stemBass:   return { 27.5f, 350.0f };
            case stemVocals: return { 65.0f, 1320.0f };
            default:         return {};
        }
    }

    basicpitch::Weights vendoredWeights()
    {
        basicpitch::Weights w;

        w.cqtKernels = BasicPitchWeights::cqt_kernels_bin;
        w.cqtKernelBytes = (size_t) BasicPitchWeights::cqt_kernels_binSize;

        w.cnnContourJson = BasicPitchWeights::cnn_contour_model_json;
        w.cnnContourJsonBytes = (size_t) BasicPitchWeights::cnn_contour_model_jsonSize;

        w.cnnNoteJson = BasicPitchWeights::cnn_note_model_json;
        w.cnnNoteJsonBytes = (size_t) BasicPitchWeights::cnn_note_model_jsonSize;

        w.cnnOnsetInputJson = BasicPitchWeights::cnn_onset_1_model_json;
        w.cnnOnsetInputJsonBytes = (size_t) BasicPitchWeights::cnn_onset_1_model_jsonSize;

        w.cnnOnsetOutputJson = BasicPitchWeights::cnn_onset_2_model_json;
        w.cnnOnsetOutputJsonBytes = (size_t) BasicPitchWeights::cnn_onset_2_model_jsonSize;

        return w;
    }

    // Mono-sum a stem and land it at 22.05 kHz. The anti-alias filter runs
    // before the interpolator, which is what makes this a resample rather than
    // a decimation with the top octave folded back over the vocals.
    //
    // Streamed in blocks rather than read whole: a ten-minute stereo stem is
    // 212 MB of float32 on disk, and materialising that plus its mono sum next
    // to the model's own working set doubled the peak for nothing. Both the
    // filters and the interpolator carry state across blocks, so the result is
    // the same samples either way.
    bool readAsModelRate (const juce::File& wav, std::vector<float>& out,
                          juce::String& error)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wav));

        if (reader == nullptr)
        {
            error = "COULD NOT READ STEM";
            return false;
        }

        const auto numSamples = (int) juce::jmin (reader->lengthInSamples,
                                                  (juce::int64) std::numeric_limits<int>::max());

        if (numSamples <= 0 || reader->sampleRate <= 0.0)
        {
            error = "STEM IS EMPTY";
            return false;
        }

        const auto ratio = reader->sampleRate / modelSampleRate;
        const auto resampling = std::abs (ratio - 1.0) > 1.0e-9;

        const auto outLength = (int) std::floor ((double) numSamples / ratio);

        if (outLength <= 0)
        {
            error = "STEM IS TOO SHORT";
            return false;
        }

        out.assign ((size_t) outLength, 0.0f);

        // Anti-alias before the interpolator sees anything. 0.98 of the target
        // Nyquist leaves the Butterworth room to be down where it matters.
        std::vector<juce::dsp::IIR::Filter<float>> lowpass;

        if (resampling)
        {
            const auto cutoff = (float) juce::jmin (modelSampleRate * 0.5,
                                                    reader->sampleRate * 0.5) * 0.98f;

            for (const auto& coefficients :
                 juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (
                     cutoff, reader->sampleRate, 4))
            {
                lowpass.emplace_back();
                lowpass.back().coefficients = coefficients;
                lowpass.back().reset();
            }
        }

        constexpr int blockSize = 1 << 16;

        juce::AudioBuffer<float> block ((int) reader->numChannels, blockSize);
        std::vector<float> pending;  // filtered source-rate samples not yet consumed
        pending.reserve ((size_t) blockSize * 2);

        juce::LagrangeInterpolator interpolator;
        auto written = 0;

        for (auto position = 0; position < numSamples && written < outLength;)
        {
            const auto count = juce::jmin (blockSize, numSamples - position);

            if (! reader->read (&block, 0, count, position, true, true))
            {
                error = "COULD NOT READ STEM";
                return false;
            }

            position += count;

            const auto channels = block.getNumChannels();
            const auto gain = 1.0f / (float) juce::jmax (1, channels);
            const auto base = pending.size();

            pending.resize (base + (size_t) count, 0.0f);

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* src = block.getReadPointer (ch);

                for (int i = 0; i < count; ++i)
                    pending[base + (size_t) i] += src[i] * gain;
            }

            for (auto& filter : lowpass)
                for (auto i = base; i < pending.size(); ++i)
                    pending[i] = filter.processSample (pending[i]);

            if (! resampling)
            {
                const auto take = juce::jmin ((int) pending.size(), outLength - written);
                std::copy (pending.begin(), pending.begin() + take, out.begin() + written);
                written += take;
                pending.clear();
                continue;
            }

            // Hand the interpolator as many output samples as the pending
            // input can cover, then keep whatever it did not consume.
            const auto produce = juce::jmin (outLength - written,
                                             (int) std::floor ((double) pending.size() / ratio));

            if (produce <= 0)
                continue;

            const auto used = interpolator.process (ratio, pending.data(),
                                                    out.data() + written, produce);

            written += produce;
            pending.erase (pending.begin(),
                           pending.begin() + juce::jmin ((int) pending.size(), used));
        }

        out.resize ((size_t) written);

        return written > 0;
    }

    // Basic Pitch's note amplitude is the mean note-posteriorgram energy over
    // the note, 0..1 — the same quantity its own exporter turns into velocity.
    int velocityFor (double amplitude)
    {
        return juce::jlimit (1, 127, (int) std::lround (amplitude * 127.0));
    }
} // namespace

bool MidiTranscriber::supports (int stemKind)
{
    return stemKind == stemVocals || stemKind == stemBass || stemKind == stemOther
           || stemKind == stemGuitar || stemKind == stemPiano;
}

juce::File MidiTranscriber::midiFileFor (const juce::File& stemWav)
{
    return stemWav.getSiblingFile (stemWav.getFileNameWithoutExtension() + ".mid");
}

MidiTranscriber::Status MidiTranscriber::transcribe (
    const juce::File& stemWav, const juce::File& outMidi,
    double bpm, int stemKind,
    const std::function<void (float)>& onProgress,
    const std::function<bool()>& shouldContinue,
    MidiTranscription& out, juce::String& error)
{
    out = {};

    const auto alive = [&shouldContinue] { return shouldContinue == nullptr || shouldContinue(); };
    const auto report = [&onProgress] (float p) { if (onProgress != nullptr) onProgress (p); };

    report (0.0f);

    std::vector<float> audio;

    if (! readAsModelRate (stemWav, audio, error))
        return Status::failed;

    if (! alive())
        return Status::cancelled;

    report (0.05f);

    basicpitch::Transcriber transcriber;
    std::string reason;

    if (! transcriber.prepare (vendoredWeights(), reason))
    {
        error = "MIDI MODEL UNAVAILABLE";
        return Status::failed;
    }

    const auto stem = paramsForStem (stemKind);

    basicpitch::Params params;
    params.minFrequency = stem.minFrequency;
    params.maxFrequency = stem.maxFrequency;

    std::vector<basicpitch::Note> notes;

    const auto ok = transcriber.transcribe (
        audio.data(), (int) audio.size(), params,
        [&report] (float p) { report (0.05f + 0.90f * p); },
        [&alive] { return alive(); },
        notes, reason);

    if (! ok)
    {
        if (! alive())
            return Status::cancelled;

        error = "TRANSCRIPTION FAILED";
        return Status::failed;
    }

    // --- write the .mid ---------------------------------------------------
    out.tempoDetected = bpm > 0.0;
    out.bpm = out.tempoDetected ? bpm : fallbackBpm;

    // Seconds to ticks through the tempo we are about to declare: with a real
    // BPM the notes land on that grid, and with the 120 fallback a tick is a
    // fixed slice of wall-clock time.
    const auto ticksPerSecond = out.bpm / 60.0 * (double) ticksPerQuarterNote;

    juce::MidiMessageSequence tempoTrack;
    tempoTrack.addEvent (juce::MidiMessage::textMetaEvent (3, stemWav.getFileNameWithoutExtension()), 0.0);
    tempoTrack.addEvent (juce::MidiMessage::tempoMetaEvent (
                             (int) std::lround (60.0e6 / out.bpm)), 0.0);
    tempoTrack.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);

    juce::MidiMessageSequence noteTrack;
    noteTrack.addEvent (juce::MidiMessage::textMetaEvent (3, juce::String (stemName (stemKind))), 0.0);

    for (const auto& note : notes)
    {
        const auto start = note.startSeconds * ticksPerSecond;
        const auto end = juce::jmax (start + 1.0, note.endSeconds * ticksPerSecond);

        noteTrack.addEvent (juce::MidiMessage::noteOn (1, note.pitch, (juce::uint8) velocityFor (note.amplitude)), start);
        noteTrack.addEvent (juce::MidiMessage::noteOff (1, note.pitch), end);

        if (out.noteCount == 0)
        {
            out.lowestPitch = out.highestPitch = note.pitch;
            out.firstNoteSeconds = note.startSeconds;
        }
        else
        {
            out.lowestPitch = juce::jmin (out.lowestPitch, note.pitch);
            out.highestPitch = juce::jmax (out.highestPitch, note.pitch);
        }

        out.lastNoteSeconds = juce::jmax (out.lastNoteSeconds, note.endSeconds);
        ++out.noteCount;
    }

    noteTrack.sort();
    noteTrack.updateMatchedPairs(); // after the sort, so the pairs match what is written

    juce::MidiFile file;
    file.setTicksPerQuarterNote (ticksPerQuarterNote);
    file.addTrack (tempoTrack);
    file.addTrack (noteTrack);

    outMidi.deleteFile();

    juce::FileOutputStream stream (outMidi);

    if (! stream.openedOk() || ! file.writeTo (stream, 1))
    {
        outMidi.deleteFile();
        error = "COULD NOT WRITE MIDI";
        return Status::failed;
    }

    stream.flush();

    report (1.0f);

    return Status::ok;
}

} // namespace splittr
