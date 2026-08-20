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
    const auto qualityToken = quality == Quality::best      ? "|ft"
                              : quality == Quality::sixStem ? "|6s"
                                                            : "";

    const auto identity = source.getFullPathName()
                          + "|" + juce::String (source.getSize())
                          + "|" + juce::String (source.getLastModificationTime().toMilliseconds())
                          + qualityToken;

    return juce::String::toHexString (identity.hashCode64());
}

CachedStems StemCache::entryIn (const juce::File& dir, const juce::StringArray& stemNames)
{
    CachedStems entry;
    entry.dir = dir;

    for (const auto& name : stemNames)
    {
        entry.names.push_back (name);
        entry.stems.push_back (dir.getChildFile (name + ".wav"));
    }

    entry.meta = dir.getChildFile ("meta.xml");
    return entry;
}

CachedStems StemCache::entryFor (const juce::File& source, Quality quality)
{
    juce::StringArray names;

    for (auto kind : stemOrderFor (quality))
        names.add (stemName (kind));

    return entryIn (cacheRoot().getChildFile (keyFor (source, quality)), names);
}

bool StemCache::isInsideCache (const juce::File& file)
{
    const auto root = cacheRoot();
    return file == root || file.isAChildOf (root);
}

void StemCache::prune (juce::int64 maxBytes)
{
    auto entries = cacheRoot().findChildFiles (juce::File::findDirectories, false);

    // Sum every rendered WAV rather than a fixed stem list: an entry's stem set
    // depends on the mode that wrote it.
    const auto entryBytes = [] (const juce::File& dir)
    {
        juce::int64 bytes = 0;

        for (const auto& wav : dir.findChildFiles (juce::File::findFiles, false, "*.wav"))
            bytes += wav.getSize();

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
