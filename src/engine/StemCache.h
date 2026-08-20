#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include "ModelManager.h" // Quality

namespace splittr
{

// Every kind of stem the app can render. Which of them a run actually produces
// depends on the engine mode, so this is a vocabulary, not a count: use
// stemOrderFor (Quality) for the list a given mode renders, in display order.
enum StemIndex
{
    stemVocals = 0,
    stemDrums,
    stemBass,
    stemGuitar,
    stemPiano,
    stemOther,
    stemInstrumental,
    numStemKinds
};

// The 6-source mode renders every kind, so the vocabulary size doubles as the
// upper bound on stems in flight (used to size fixed audio-thread arrays).
constexpr int maxStems = numStemKinds; // 7

inline const char* stemName (int kind)
{
    static const char* names[numStemKinds] = {
        "Vocals", "Drums", "Bass", "Guitar", "Piano", "Other", "Instrumental"
    };

    return names[kind];
}

inline int stemKindNamed (const juce::String& name)
{
    for (int i = 0; i < numStemKinds; ++i)
        if (name.equalsIgnoreCase (stemName (i)))
            return i;

    return -1;
}

// demucs target order is drums, bass, other, vocals for htdemucs and
// drums, bass, other, vocals, guitar, piano for htdemucs_6s (verified against
// third_party/demucs.cpp/cli-apps/demucs.cpp and its README).
inline int stemKindForSource (int source)
{
    static const int kinds[6] = { stemDrums, stemBass, stemOther, stemVocals, stemGuitar, stemPiano };
    return juce::isPositiveAndBelow (source, 6) ? kinds[source] : -1;
}

// Inverse of the above; -1 for the summed instrumental, which is not a
// separated source.
inline int sourceForStemKind (int kind)
{
    for (int s = 0; s < 6; ++s)
        if (stemKindForSource (s) == kind)
            return s;

    return -1;
}

// Display order of the rendered stems for a given number of separated sources:
// vocals lead, the summed instrumental always comes last.
inline const std::vector<int>& stemOrderForSources (int numSources)
{
    static const std::vector<int> four {
        stemVocals, stemDrums, stemBass, stemOther, stemInstrumental
    };

    static const std::vector<int> six {
        stemVocals, stemDrums, stemBass, stemGuitar, stemPiano, stemOther, stemInstrumental
    };

    return numSources >= 6 ? six : four;
}

inline int sourceCountFor (Quality q) { return q == Quality::sixStem ? 6 : 4; }

inline const std::vector<int>& stemOrderFor (Quality q)
{
    return stemOrderForSources (sourceCountFor (q));
}

inline int stemCountFor (Quality q) { return (int) stemOrderFor (q).size(); }

// One cache entry: the rendered stems plus a meta.xml with waveforms/peaks/stats.
struct CachedStems
{
    juce::File dir;
    std::vector<juce::File> stems;  // display order
    std::vector<juce::String> names; // parallel to stems
    juce::File meta;

    bool complete() const
    {
        if (stems.empty())
            return false;

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
    // one is an instant hit. All three modes coexist.
    static juce::String keyFor (const juce::File& source, Quality quality);
    static CachedStems entryFor (const juce::File& source, Quality quality);

    // Re-point an entry at an explicit stem set — the one recorded in its
    // meta.xml — so a cached run can be reopened by the names it was written
    // with rather than by whatever mode is selected now.
    static CachedStems entryIn (const juce::File& dir, const juce::StringArray& stemNames);

    // True when the file lives inside the cache — a stem dragged out of the
    // plugin, say. Re-separating one of our own stems is never meaningful, so
    // the editor refuses those drops.
    static bool isInsideCache (const juce::File& file);

    // Delete oldest entries until the cache is under maxBytes.
    // (7 float32 stems ~ 520 MB for a 4-minute song, so allow a few songs.)
    static void prune (juce::int64 maxBytes = (juce::int64) 4 * 1024 * 1024 * 1024);
};

} // namespace splittr
