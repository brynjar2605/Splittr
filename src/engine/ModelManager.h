#pragma once

#include <juce_core/juce_core.h>
#include <vector>

namespace splittr
{

// Quality vs speed:
//   fast    = htdemucs 4-source, one model, ~realtime
//   best    = htdemucs_ft ensemble, four fine-tuned models (one per source),
//             noticeably better separation at ~4x the inference cost
//   sixStem = htdemucs_6s, one model, adds guitar and piano to the four
//             standard sources (those two are the weaker targets upstream)
enum class Quality { fast, best, sixStem };

// Resolves, remembers and describes the ggml weight files.
class ModelManager
{
public:
    // Pinned source of the pretrained weights (MIT, converted by the
    // demucs.cpp author). TODO before public release: mirror on the
    // project's own GitHub Releases and pin a revision-specific URL.
    static constexpr const char* baseUrl =
        "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/";

    // Expected on-disk size, per weight file. All htdemucs 4-source variants
    // share one architecture and one size; the 6-source export is a different
    // (smaller) file, so the download size check has to be per file.
    static constexpr juce::int64 bytes4Source = 83994361;
    static constexpr juce::int64 bytes6Source = 54855129;

    static juce::int64 expectedBytes (const juce::String& filename);
    static juce::int64 expectedBytesFor (Quality); // whole set

    // For Quality::best the order matches the htdemucs target order:
    // drums, bass, other, vocals (upstream demucs_ft mapping).
    static const std::vector<juce::String>& filenamesFor (Quality);
    static juce::String urlFor (const juce::String& filename);

    static juce::File appDataDir(); // ~/Library/Application Support/Splittr
    static juce::File modelsDir();  // <appDataDir>/models

    Quality quality() const;
    void setQuality (Quality);

    // Settings.xml / meta.xml spelling of a mode ("fast", "best", "6s").
    static juce::String qualityToken (Quality);

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
