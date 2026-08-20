#pragma once

#include <juce_core/juce_core.h>
#include <array>
#include <vector>
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

// A library row: everything the cache knows about one previous split, read
// from its meta.xml alone — the source file may have moved or been deleted
// since, and the entry has to stay browsable and re-openable either way.
struct CacheEntry
{
    juce::File dir;
    juce::String sourcePath;  // as recorded at split time; may no longer exist
    juce::String sourceName;  // filename portion of sourcePath
    double duration = 0.0;    // seconds
    double bpm = 0.0;         // 0 = undetected
    juce::String key;         // "F# MIN" etc., empty = undetected
    juce::String quality;     // raw meta attribute ("fast" / "best" / …)
    juce::Time modified;      // directory mtime — the date column
    juce::int64 bytes = 0;    // everything in the directory
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

    // True when the file lives inside the cache — a stem dragged out of the
    // plugin, say. Re-separating one of our own stems is never meaningful, so
    // the editor refuses those drops.
    static bool isInsideCache (const juce::File& file);

    // Every readable entry under cacheRoot(), newest first — the library list.
    // Cheap: one directory scan plus a small XML parse per entry.
    static std::vector<CacheEntry> enumerate();

    // (5 float32 stems ~ 370 MB for a 4-minute song, so allow a few songs.)
    static constexpr juce::int64 defaultMaxBytes = (juce::int64) 4 * 1024 * 1024 * 1024;

    // Delete oldest entries until the cache is under maxBytes.
    static void prune (juce::int64 maxBytes = defaultMaxBytes);
};

} // namespace splittr
