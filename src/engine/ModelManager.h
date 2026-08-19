#pragma once

#include <juce_core/juce_core.h>

namespace splittr
{

// Resolves and remembers where the htdemucs ggml weights live.
class ModelManager
{
public:
    // Pinned source of the pretrained weights (MIT, converted by the
    // demucs.cpp author). TODO before public release: mirror on the
    // project's own GitHub Releases and pin a revision-specific URL.
    static constexpr const char* downloadUrl =
        "https://huggingface.co/datasets/Retrobear/demucs.cpp/resolve/main/ggml-model-htdemucs-4s-f16.bin";
    static constexpr juce::int64 expectedBytes = 83994361;

    static juce::File appDataDir();       // ~/Library/Application Support/Splittr
    static juce::File defaultModelFile(); // <appDataDir>/models/ggml-model-htdemucs-4s-f16.bin

    // User override set via "LOCATE FILE MANUALLY…", persisted in Settings.xml.
    juce::File userOverride() const;
    void setUserOverride (const juce::File&);

    // The model file to use, or a non-existent File if none is available yet.
    juce::File resolve() const;

private:
    static juce::File settingsFile();
};

} // namespace splittr
