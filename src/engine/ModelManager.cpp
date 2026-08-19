#include "ModelManager.h"

namespace splittr
{

const std::vector<juce::String>& ModelManager::filenamesFor (Quality q)
{
    static const std::vector<juce::String> fast { "ggml-model-htdemucs-4s-f16.bin" };

    static const std::vector<juce::String> best {
        "ggml-model-htdemucs_ft_drums-4s-f16.bin",
        "ggml-model-htdemucs_ft_bass-4s-f16.bin",
        "ggml-model-htdemucs_ft_other-4s-f16.bin",
        "ggml-model-htdemucs_ft_vocals-4s-f16.bin",
    };

    return q == Quality::best ? best : fast;
}

juce::String ModelManager::urlFor (const juce::String& filename)
{
    return juce::String (baseUrl) + filename;
}

juce::File ModelManager::appDataDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("Splittr");
}

juce::File ModelManager::modelsDir()
{
    return appDataDir().getChildFile ("models");
}

juce::File ModelManager::settingsFile()
{
    return appDataDir().getChildFile ("Settings.xml");
}

std::unique_ptr<juce::XmlElement> ModelManager::readSettings()
{
    if (auto xml = juce::parseXML (settingsFile()))
        return xml;

    return std::make_unique<juce::XmlElement> ("SplittrSettings");
}

void ModelManager::writeSetting (const juce::String& attribute, const juce::String& value)
{
    auto xml = readSettings();
    xml->setAttribute (attribute, value);

    settingsFile().getParentDirectory().createDirectory();
    xml->writeTo (settingsFile());
}

Quality ModelManager::quality() const
{
    return readSettings()->getStringAttribute ("quality") == "best" ? Quality::best
                                                                    : Quality::fast;
}

void ModelManager::setQuality (Quality q)
{
    writeSetting ("quality", q == Quality::best ? "best" : "fast");
}

void ModelManager::setUserModelDir (const juce::File& dir)
{
    if (dir.isDirectory())
        writeSetting ("modelDir", dir.getFullPathName());
}

juce::File ModelManager::userModelDir() const
{
    return juce::File (readSettings()->getStringAttribute ("modelDir"));
}

std::vector<juce::File> ModelManager::filesFor (Quality q) const
{
    const auto userDir = userModelDir();

    std::vector<juce::File> files;

    for (const auto& name : filenamesFor (q))
    {
        if (userDir.isDirectory() && userDir.getChildFile (name).existsAsFile())
            files.push_back (userDir.getChildFile (name));
        else
            files.push_back (modelsDir().getChildFile (name));
    }

    return files;
}

std::vector<juce::File> ModelManager::missingFor (Quality q) const
{
    std::vector<juce::File> missing;

    for (const auto& file : filesFor (q))
        if (! file.existsAsFile())
            missing.push_back (file);

    return missing;
}

} // namespace splittr
