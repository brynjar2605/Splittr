#include "StemCache.h"
#include "ModelManager.h"

namespace splittr
{

juce::File StemCache::cacheRoot()
{
    return ModelManager::appDataDir().getChildFile ("cache");
}

juce::String StemCache::keyFor (const juce::File& source, Quality quality)
{
    const auto identity = source.getFullPathName()
                          + "|" + juce::String (source.getSize())
                          + "|" + juce::String (source.getLastModificationTime().toMilliseconds())
                          + (quality == Quality::best ? "|ft" : "");

    return juce::String::toHexString (identity.hashCode64());
}

CachedStems StemCache::entryFor (const juce::File& source, Quality quality)
{
    CachedStems entry;
    entry.dir = cacheRoot().getChildFile (keyFor (source, quality));
    entry.vocals = entry.dir.getChildFile ("Vocals.wav");
    entry.instrumental = entry.dir.getChildFile ("Instrumental.wav");
    entry.meta = entry.dir.getChildFile ("meta.xml");
    return entry;
}

void StemCache::prune (juce::int64 maxBytes)
{
    auto entries = cacheRoot().findChildFiles (juce::File::findDirectories, false);

    juce::int64 total = 0;

    for (const auto& dir : entries)
        total += dir.getChildFile ("Vocals.wav").getSize()
                 + dir.getChildFile ("Instrumental.wav").getSize();

    if (total <= maxBytes)
        return;

    std::sort (entries.begin(), entries.end(),
               [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() < b.getLastModificationTime(); });

    for (const auto& dir : entries)
    {
        if (total <= maxBytes)
            break;

        total -= dir.getChildFile ("Vocals.wav").getSize()
                 + dir.getChildFile ("Instrumental.wav").getSize();

        dir.deleteRecursively();
    }
}

} // namespace splittr
