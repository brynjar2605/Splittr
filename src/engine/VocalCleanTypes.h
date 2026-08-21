#pragma once

#include <juce_core/juce_core.h>

namespace splittr
{

// Which restoration model finishes the clean-vocal chain (issue #29). The two
// stages before it are toggles, but restoration is a choice: Apollo and
// AudioSR answer the same question two different ways (single-pass band-split
// vs. latent diffusion), and the experiment exists to A/B them.
enum class VocalRestoreMode
{
    none,
    apollo,
    audiosr
};

// One clean-vocal request: what the user toggled in the CLEAN panel, and the
// value the cache is keyed on. A default-constructed value means "do nothing";
// running it would be a copy, so isNoop() is the guard everything checks first.
struct VocalCleanParams
{
    bool dereverb = false;     // MelBand RoFormer de-reverb (anvuew)
    bool dereverbEcho = false; // MelBand RoFormer de-reverb+echo v2 (Sucial)
    VocalRestoreMode restore = VocalRestoreMode::none;

    bool isNoop() const
    {
        return ! dereverb && ! dereverbEcho && restore == VocalRestoreMode::none;
    }

    bool operator== (const VocalCleanParams& o) const
    {
        return dereverb == o.dereverb && dereverbEcho == o.dereverbEcho
            && restore == o.restore;
    }
    bool operator!= (const VocalCleanParams& o) const { return ! (*this == o); }

    // Compact one-attribute form for meta.xml, in the StemFx descriptor
    // tradition: noop serialises to the empty string, anything unparseable
    // reads back as noop. Tokens separated by ';' — "dr", "de", "re:apollo" /
    // "re:audiosr". Unknown tokens are ignored so a descriptor from a future
    // build degrades to the part this one knows.
    juce::String toDescriptor() const
    {
        juce::StringArray t;
        if (dereverb)
            t.add ("dr");
        if (dereverbEcho)
            t.add ("de");
        if (restore == VocalRestoreMode::apollo)
            t.add ("re:apollo");
        else if (restore == VocalRestoreMode::audiosr)
            t.add ("re:audiosr");
        return t.joinIntoString (";");
    }

    static VocalCleanParams fromDescriptor (const juce::String& s)
    {
        VocalCleanParams p;
        for (auto& tok : juce::StringArray::fromTokens (s, ";", {}))
        {
            auto t = tok.trim();
            if (t == "dr")
                p.dereverb = true;
            else if (t == "de")
                p.dereverbEcho = true;
            else if (t == "re:apollo")
                p.restore = VocalRestoreMode::apollo;
            else if (t == "re:audiosr")
                p.restore = VocalRestoreMode::audiosr;
        }
        return p;
    }

    // The cache filename tag: "Vocals_<tag>.wav" under <entry>/clean/. The
    // descriptor itself, filename-safe, rather than a hash of it — three
    // booleans' worth of states never collide, and a human reading the cache
    // dir can see what each render was.
    juce::String cacheTag() const
    {
        return toDescriptor().replaceCharacter (';', '-').replaceCharacter (':', '_');
    }
};

} // namespace splittr
