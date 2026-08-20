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

    for (int i = 0; i < numStems; ++i)
        entry.stems[(size_t) i] = entry.dir.getChildFile (juce::String (stemName (i)) + ".wav");

    entry.meta = entry.dir.getChildFile ("meta.xml");
    return entry;
}

bool StemCache::isInsideCache (const juce::File& file)
{
    const auto root = cacheRoot();
    return file == root || file.isAChildOf (root);
}

void StemCache::prune (juce::int64 maxBytes)
{
    auto entries = cacheRoot().findChildFiles (juce::File::findDirectories, false);

    const auto entryBytes = [] (const juce::File& dir)
    {
        juce::int64 bytes = 0;
        for (int i = 0; i < numStems; ++i)
            bytes += dir.getChildFile (juce::String (stemName (i)) + ".wav").getSize();
        return bytes;
    };

    juce::int64 total = 0;

    for (const auto& dir : entries)
        total += entryBytes (dir);

    if (total <= maxBytes)
        return;

    std::sort (entries.begin(), entries.end(),
               [] (const juce::File& a, const juce::File& b)
               { return a.getLastModificationTime() < b.getLastModificationTime(); });

    for (const auto& dir : entries)
    {
        if (total <= maxBytes)
            break;

        total -= entryBytes (dir);
        dir.deleteRecursively();
    }
}

} // namespace splittr
