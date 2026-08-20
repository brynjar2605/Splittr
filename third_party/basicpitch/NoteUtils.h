//
// Splittr replacement for NeuralNote's Lib/Utils/NoteUtils.h.
//
// The upstream header pulls in JuceHeader.h for the scale/root-note string
// tables its editor needs. Notes.cpp — the only file here that includes it —
// uses nothing but hzToMidi, so this keeps the two conversions and drops the
// JUCE dependency, letting Notes.h/Notes.cpp stay byte-identical to upstream.
//

#ifndef NN_NOTEUTILS_H
#define NN_NOTEUTILS_H

#include <cmath>

namespace NoteUtils
{

/** Closest MIDI note number to a frequency in Hz. */
static inline int hzToMidi(float hz)
{
    return (int) std::round(12.0f * std::log2(hz / 440.0f) + 69.0f);
}

/** Frequency in Hz of a (possibly fractional) MIDI note number. */
static inline float midiToHz(float inMidiNote)
{
    return 440.0f * std::pow(2.0f, (inMidiNote - 69.0f) / 12.0f);
}

} // namespace NoteUtils

#endif // NN_NOTEUTILS_H
