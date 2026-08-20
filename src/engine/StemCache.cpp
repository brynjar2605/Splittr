#include "StemCache.h"
#include "ModelManager.h"

namespace splittr
{

namespace
{
    // Only the song title is untrusted, so only it goes through
    // createLegalFileName: that also strips '#', which is the whole difference
    // between F# and F, and the stem/BPM/key parts are ours to begin with.
    //
    // It drops ',' as well, which is what keeps the meta's comma-joined file
    // list unambiguous — do not swap it for a laxer sanitiser without giving
    // that list a different separator.
    juce::String legalSongName (const juce::String& name)
    {
        auto song = juce::File::createLegalFileName (name).trim();

        // macOS allows 255 bytes per path component and the suffix can add ~40,
        // so a long — or merely multi-byte — title needs trimming well before
        // createLegalFileName's 128-character cap bites.
        while (song.getNumBytesAsUTF8() > 160)
            song = song.dropLastCharacters (1);

        return song.trim();
    }
} // namespace

juce::String compactKey (const juce::String& key)
{
    const auto trimmed = key.trim();

    if (trimmed.isEmpty())
        return {};

    // "F# MIN" -> "F#m"; major is the unmarked case, so "Bb MAJ" -> "Bb".
    const auto tonic = trimmed.upToFirstOccurrenceOf (" ", false, false);

    return trimmed.endsWithIgnoreCase ("MIN") ? tonic + "m" : tonic;
}

juce::String stemFileName (const juce::String& songName, const juce::String& stemName,
                           double bpm, const juce::String& key)
{
    const auto song = legalSongName (songName);

    juce::String name;

    if (song.isNotEmpty())
        name << song << " - ";

    name << stemName;

    if (bpm > 0.0)
        name << " - " << juce::roundToInt (bpm) << "BPM";

    if (const auto compact = compactKey (key); compact.isNotEmpty())
        name << " - " << compact;

    return name + ".wav";
}

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

CachedStems StemCache::entryIn (const juce::File& dir, const juce::StringArray& stemNames,
                                const juce::StringArray& stemFiles)
{
    CachedStems entry;
    entry.dir = dir;

    for (int i = 0; i < stemNames.size(); ++i)
    {
        // <Name>.wav is the pre-#11 layout, and still the answer whenever an
        // entry does not record what its stems are called.
        const auto file = i < stemFiles.size() && stemFiles[i].isNotEmpty()
                              ? stemFiles[i]
                              : stemNames[i] + ".wav";

        entry.names.push_back (stemNames[i]);
        entry.stems.push_back (dir.getChildFile (file));
    }

    entry.meta = dir.getChildFile ("meta.xml");
    return entry;
}

juce::File StemCache::dirFor (const juce::File& source, Quality quality)
{
    return cacheRoot().getChildFile (keyFor (source, quality));
}

CachedStems StemCache::entryFor (const juce::File& source, Quality quality)
{
    juce::StringArray names;

    for (auto kind : stemOrderFor (quality))
        names.add (stemName (kind));

    return entryIn (dirFor (source, quality), names);
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
