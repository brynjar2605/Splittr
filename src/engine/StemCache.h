#pragma once

#include <juce_core/juce_core.h>

namespace splittr
{

// One cache entry: the rendered stems plus a meta.xml with waveforms/peaks/stats.
struct CachedStems
{
    juce::File dir;
    juce::File vocals;
    juce::File instrumental;
    juce::File meta;

    bool complete() const
    {
        return vocals.existsAsFile() && instrumental.existsAsFile() && meta.existsAsFile();
    }
};

// Rendered stems live in Application Support (never /tmp): Ableton references
// dragged files by path, so they must survive reboots and cache cleaners.
class StemCache
{
public:
    static juce::File cacheRoot(); // <appDataDir>/cache

    // Key is derived from absolute path + size + mtime, so an edited file
    // re-separates while an untouched one is an instant hit.
    static juce::String keyFor (const juce::File& source);
    static CachedStems entryFor (const juce::File& source);

    // Delete oldest entries until the cache is under maxBytes.
    static void prune (juce::int64 maxBytes = (juce::int64) 2 * 1024 * 1024 * 1024);
};

} // namespace splittr
