//
// Splittr's driver for the vendored Basic Pitch core, standing in for
// NeuralNote's Lib/Model/BasicPitch.{h,cpp}.
//
// Differences from upstream, all of them about running offline over a whole
// stem rather than live in an editor:
//
//   * the header is dependency-free (pimpl), so exactly one Splittr
//     translation unit — src/engine/MidiTranscriber.cpp — sees RTNeural,
//     Eigen or nlohmann::json, the same compile firewall DemucsSeparator puts
//     around demucs.cpp;
//   * the harmonic stacking is gathered per frame instead of materialised
//     (see Features.h), so a ten-minute stem does not need a gigabyte;
//   * transcription reports progress and can be cancelled mid-clip.
//
// The model itself is unchanged: Spotify's Basic Pitch weights (Apache-2.0),
// NeuralNote's RTNeural port of the CNN, and NeuralNote's note-creation code.
//

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace basicpitch
{

/** The five resource blobs, in whatever form the host has them. */
struct Weights
{
    const void* cqtKernels = nullptr;
    size_t cqtKernelBytes = 0;

    const char* cnnContourJson = nullptr;
    size_t cnnContourJsonBytes = 0;

    const char* cnnNoteJson = nullptr;
    size_t cnnNoteJsonBytes = 0;

    const char* cnnOnsetInputJson = nullptr;
    size_t cnnOnsetInputJsonBytes = 0;

    const char* cnnOnsetOutputJson = nullptr;
    size_t cnnOnsetOutputJsonBytes = 0;
};

struct Params
{
    // Basic Pitch's own two thresholds. `onsetThreshold` gates the onset
    // peak-picking (higher = fewer, more confident note starts);
    // `frameThreshold` decides how long a note is held (higher = shorter
    // notes). Spotify's defaults are 0.5 / 0.3.
    float onsetThreshold = 0.5f;
    float frameThreshold = 0.3f;

    // Basic Pitch's default is 127.70 ms — eleven 256/22050 s frames.
    float minNoteLengthMs = 127.7f;

    // Optional band limit, in Hz. Negative leaves the full 27.5 Hz .. 4186 Hz
    // range the model covers. Narrowing it is how a bass stem stops sprouting
    // octave-up ghosts.
    float minFrequency = -1.0f;
    float maxFrequency = -1.0f;
};

struct Note
{
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    int pitch = 0;         // MIDI note number, 21..108
    double amplitude = 0.0; // mean note-posteriorgram energy, 0..1
};

class Transcriber
{
public:
    Transcriber();
    ~Transcriber();

    /** Parses the weights. False (with a reason) when a blob is unusable. */
    bool prepare (const Weights&, std::string& error);

    /**
     * @param audio mono, 22050 Hz. Clips under ~1.5 s are zero-padded here.
     * @param onProgress 0..1, called perhaps a hundred times over a clip.
     * @param shouldContinue polled alongside it; false abandons the run.
     * @return false when cancelled or when the input could not be processed.
     */
    bool transcribe (const float* audio, int numSamples, const Params&,
                     const std::function<void (float)>& onProgress,
                     const std::function<bool()>& shouldContinue,
                     std::vector<Note>& out, std::string& error);

    /** Sample rate the model works at; callers must resample to it. */
    static constexpr double sampleRate = 22050.0;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    Transcriber (const Transcriber&) = delete;
    Transcriber& operator= (const Transcriber&) = delete;
};

} // namespace basicpitch
