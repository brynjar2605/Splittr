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
    bool readAsModelRate (const juce::File& wav, std::vector<float>& out,
                          double& sourceSeconds, juce::String& error)
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

        sourceSeconds = (double) numSamples / reader->sampleRate;

        juce::AudioBuffer<float> buffer ((int) reader->numChannels, numSamples);

        if (! reader->read (&buffer, 0, numSamples, 0, true, true))
        {
            error = "COULD NOT READ STEM";
            return false;
        }

        // LagrangeInterpolator reads a few samples past the last one it emits,
        // so the mono buffer carries a short zero tail.
        constexpr int guard = 8;

        std::vector<float> mono ((size_t) (numSamples + guard), 0.0f);

        {
            const auto channels = buffer.getNumChannels();
            const auto gain = 1.0f / (float) juce::jmax (1, channels);

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* src = buffer.getReadPointer (ch);

                for (int i = 0; i < numSamples; ++i)
                    mono[(size_t) i] += src[i] * gain;
            }
        }

        if (std::abs (reader->sampleRate - modelSampleRate) > 1.0e-6)
        {
            const auto cutoff = (float) juce::jmin (modelSampleRate * 0.5,
                                                    reader->sampleRate * 0.5) * 0.98f;

            for (const auto& coefficients :
                 juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod (
                     cutoff, reader->sampleRate, 4))
            {
                juce::dsp::IIR::Filter<float> filter;
                filter.coefficients = coefficients;
                filter.reset();

                for (int i = 0; i < numSamples; ++i)
                    mono[(size_t) i] = filter.processSample (mono[(size_t) i]);
            }

            const auto ratio = reader->sampleRate / modelSampleRate;
            const auto outLength = (int) std::floor ((double) numSamples / ratio);

            if (outLength <= 0)
            {
                error = "STEM IS TOO SHORT";
                return false;
            }

            out.assign ((size_t) outLength, 0.0f);

            juce::LagrangeInterpolator interpolator;
            interpolator.process (ratio, mono.data(), out.data(), outLength);
        }
        else
        {
            mono.resize ((size_t) numSamples);
            out = std::move (mono);
        }

        return true;
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
    auto sourceSeconds = 0.0;

    if (! readAsModelRate (stemWav, audio, sourceSeconds, error))
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

    noteTrack.updateMatchedPairs();
    noteTrack.sort();

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
