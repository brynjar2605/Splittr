#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include "ModelManager.h" // Quality

namespace splittr
{

// Stem display/index order used everywhere in the app.
enum StemIndex { stemVocals = 0, stemDrums, stemBass, stemOther, stemInstrumental, numStems };

inline const char* stemName (int i)
{
    static const char* names[numStems] = { "Vocals", "Drums", "Bass", "Other", "Instrumental" };
    return names[i];
}

// One cache entry: the rendered stems plus a meta.xml with waveforms/peaks/stats.
struct CachedStems
{
    juce::File dir;
    std::array<juce::File, numStems> stems; // StemIndex order
    juce::File meta;

    bool complete() const
    {
        for (const auto& f : stems)
            if (! f.existsAsFile())
                return false;

        return meta.existsAsFile();
    }
};

// Rendered stems live in Application Support (never /tmp): Ableton references
// dragged files by path, so they must survive reboots and cache cleaners.
class StemCache
{
public:
    static juce::File cacheRoot(); // <appDataDir>/cache

    // Key is derived from absolute path + size + mtime + engine quality, so
    // an edited file (or a quality switch) re-separates while an untouched
    // one is an instant hit.
    static juce::String keyFor (const juce::File& source, Quality quality);
    static CachedStems entryFor (const juce::File& source, Quality quality);

    // Delete oldest entries until the cache is under maxBytes.
    // (5 float32 stems ~ 370 MB for a 4-minute song, so allow a few songs.)
    static void prune (juce::int64 maxBytes = (juce::int64) 4 * 1024 * 1024 * 1024);
};

} // namespace splittr
