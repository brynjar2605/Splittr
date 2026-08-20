#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <functional>

namespace splittr
{

// What a finished transcription turned out to hold — enough for the CLI to
// sanity-check a run and for the UI to say something useful if it wants to.
struct MidiTranscription
{
    int noteCount = 0;
    int lowestPitch = 0, highestPitch = 0; // MIDI note numbers; 0 when empty
    double firstNoteSeconds = 0.0, lastNoteSeconds = 0.0;
    double bpm = 0.0;      // the tempo actually written into the file
    bool tempoDetected = false; // false = the 120 BPM wall-clock fallback
};

// Audio-to-MIDI for one stem: Spotify's Basic Pitch, in-process.
//
// Wraps third_party/basicpitch. This is the ONLY translation unit that touches
// RTNeural / Eigen / nlohmann headers — keep it that way (compile firewall,
// same arrangement as DemucsSeparator around demucs.cpp).
class MidiTranscriber
{
public:
    enum class Status { ok, cancelled, failed };

    // Basic Pitch transcribes pitched material. Drums are a different problem
    // (onset detection plus kick/snare/hat classification), and a full mix —
    // our summed instrumental — transcribes to mush, so neither gets the
    // affordance (issue #19).
    static bool supports (int stemKind);

    // Reads `stemWav`, writes `outMidi`. `bpm` > 0 puts a tempo event in the
    // file and places the notes on that grid; 0 falls back to 120 BPM with the
    // notes at their wall-clock times.
    //
    // onProgress: [0,1]. shouldContinue: polled about a hundred times over a
    // clip; false abandons the run and leaves no file behind.
    static Status transcribe (const juce::File& stemWav, const juce::File& outMidi,
                              double bpm, int stemKind,
                              const std::function<void (float)>& onProgress,
                              const std::function<bool()>& shouldContinue,
                              MidiTranscription& out, juce::String& error);

    // The transcription that belongs beside a rendered stem WAV: same
    // directory, same basename, .mid. Both the engine and EXPORT… go through
    // here so they cannot disagree about where one lives.
    static juce::File midiFileFor (const juce::File& stemWav);
};

} // namespace splittr
