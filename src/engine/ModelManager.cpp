#include "ModelManager.h"

namespace splittr
{

juce::File ModelManager::appDataDir()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("Application Support")
        .getChildFile ("Splittr");
}

juce::File ModelManager::defaultModelFile()
{
    return appDataDir().getChildFile ("models").getChildFile ("ggml-model-htdemucs-4s-f16.bin");
}

juce::File ModelManager::settingsFile()
{
    return appDataDir().getChildFile ("Settings.xml");
}

juce::File ModelManager::userOverride() const
{
    if (auto xml = juce::parseXML (settingsFile()))
        return juce::File (xml->getStringAttribute ("modelPath"));

    return {};
}

void ModelManager::setUserOverride (const juce::File& file)
{
    juce::XmlElement xml ("SplittrSettings");
    xml.setAttribute ("modelPath", file.getFullPathName());

    settingsFile().getParentDirectory().createDirectory();
    xml.writeTo (settingsFile());
}

juce::File ModelManager::resolve() const
{
    if (auto override_ = userOverride(); override_.existsAsFile())
        return override_;

    return defaultModelFile();
}

} // namespace splittr
