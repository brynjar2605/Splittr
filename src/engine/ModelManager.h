#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace splittr
{

// Quality vs speed:
//   fast = htdemucs 4-source, one model, ~realtime
//   best = htdemucs_ft ensemble, four fine-tuned models (one per source),
//          noticeably better separation at ~4x the inference cost
enum class Quality { fast, best };

// Resolves, remembers and describes the ggml weight files.
class ModelManager
{
public:
    // Pinned source of the pretrained weights (MIT, converted by the
    // demucs.cpp author). TODO before public release: mirror on the
    // project's own GitHub Releases and pin a revision-specific URL.
    static constexpr const char* baseUrl =
        "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/";

    // All htdemucs 4-source variants share one architecture => one size.
    static constexpr juce::int64 bytesPerModel = 83994361;

    // For Quality::best the order matches the htdemucs target order:
    // drums, bass, other, vocals (upstream demucs_ft mapping).
    static const std::vector<juce::String>& filenamesFor (Quality);
    static juce::String urlFor (const juce::String& filename);

    static juce::File appDataDir(); // ~/Library/Application Support/Splittr
    static juce::File modelsDir();  // <appDataDir>/models

    Quality quality() const;
    void setQuality (Quality);

    // Local paths for the given quality; a file from the user-located
    // directory wins over the managed models dir.
    std::vector<juce::File> filesFor (Quality) const;
    std::vector<juce::File> missingFor (Quality) const;

    // "LOCATE FILE MANUALLY…": the user picks any one model file; we remember
    // its directory and look for the original filenames there first.
    void setUserModelDir (const juce::File& dir);
    juce::File userModelDir() const;

    // Last EXPORT… destination.
    void setLastExportDir (const juce::File& dir);
    juce::File lastExportDir() const;

    // Generic Settings.xml access for UI preferences (theme, accent, …).
    static juce::String getUiSetting (const juce::String& attribute, const juce::String& fallback);
    static void setUiSetting (const juce::String& attribute, const juce::String& value);

private:
    static juce::File settingsFile();
    static std::unique_ptr<juce::XmlElement> readSettings();
    static void writeSetting (const juce::String& attribute, const juce::String& value);
};

} // namespace splittr
