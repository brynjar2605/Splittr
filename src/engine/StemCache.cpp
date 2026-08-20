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

std::vector<CacheEntry> StemCache::enumerate()
{
    std::vector<CacheEntry> entries;

    for (const auto& dir : cacheRoot().findChildFiles (juce::File::findDirectories, false))
    {
        auto xml = juce::parseXML (dir.getChildFile ("meta.xml"));

        if (xml == nullptr)
            continue; // half-written, or not one of ours

        CacheEntry entry;
        entry.dir = dir;
        entry.sourcePath = xml->getStringAttribute ("sourcePath");

        // createFileWithoutCheckingPath: the source may be long gone, and a
        // missing file is not a reason to hide the stems we already have.
        entry.sourceName = entry.sourcePath.isNotEmpty()
                               ? juce::File::createFileWithoutCheckingPath (entry.sourcePath)
                                     .getFileName()
                               : dir.getFileName();

        entry.duration = xml->getDoubleAttribute ("duration");
        entry.bpm = xml->getDoubleAttribute ("bpm");
        entry.key = xml->getStringAttribute ("key");
        entry.quality = xml->getStringAttribute ("quality");
        entry.modified = dir.getLastModificationTime();

        // Everything in the directory: the stem count varies with the engine
        // mode, and stray sidecars (.asd) are still occupying the disk.
        for (const auto& file : dir.findChildFiles (juce::File::findFiles, false))
            entry.bytes += file.getSize();

        entries.push_back (std::move (entry));
    }

    std::sort (entries.begin(), entries.end(),
               [] (const CacheEntry& a, const CacheEntry& b) { return a.modified > b.modified; });

    return entries;
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
