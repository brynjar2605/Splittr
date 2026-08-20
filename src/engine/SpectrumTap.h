#pragma once

#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace splittr
{

// The live spectrum behind the TUNE curve (issue #25). One instance is one
// measurement point: the audio thread pushes the samples that pass through it,
// and finished frames come out the other side for the panel to draw. The panel
// runs two -- one before the tuned stem's chain and one after -- so the display
// can show both what the EQ is aimed at and what it did.
//
// Threading. push() belongs to the audio thread and read() to the message
// thread, and there is nothing in between that either of them can block on: a
// frame is handed over by a single atomic exchange on each side (see the
// three-slot buffer below), and everything the audio thread touches is
// allocated once in prepare(). The producer never waits for a reader that is
// late; a reader that is late simply misses frames, which for a display is the
// right answer anyway.
//
// The frame layout is deliberately not the FFT's own: see numPoints.
class SpectrumTap
{
public:
    //==========================================================================
    // 4096 points is 10.8 Hz per bin and a 93 ms window at 44.1 kHz -- fine
    // enough to separate two bass notes, short enough that the display still
    // reads like a meter rather than an average. The hop is half the window, so
    // frames arrive at rate/2048: ~21.5 per second at 44.1 kHz (46.4 ms apart),
    // ~23.4 at 48 kHz -- under the editor's 30 Hz repaint, so at ordinary rates
    // the panel sees every frame. At 96 kHz they come faster than the repaint
    // and the hand-off drops the ones in between, which is what it is for.
    static constexpr int fftOrder = 12;
    static constexpr int fftSize = 1 << fftOrder; // 4096
    static constexpr int hopSize = fftSize / 2;   // 2048, 50% overlap

    // Frames are published on a fixed logarithmic layout rather than as raw
    // bins, for two reasons. It is sample-rate independent -- the same 512
    // points at 44.1 or 96 kHz -- so the panel's pixel mapping, and the
    // synthetic frames it is built against, never have to care what the host is
    // running at. And it is the resolution the display actually has: the panel
    // is drawn on a log frequency axis about 512 pixels wide at 1x scale, so
    // one point is one column, where raw bins would over-resolve the top octave
    // a hundredfold and under-resolve the bottom.
    //
    //     point p sits at 20 Hz * 1000^(p / 511), p in [0, 512)
    //                   = pointFrequency (p)
    //
    // i.e. 20 Hz .. 20 kHz, ~51 points per octave. Each point carries the
    // strongest bin inside its own slice of that axis (or, where the slice is
    // narrower than a bin -- everything below about 1 kHz at 44.1 kHz -- the
    // stronger of the two bins bracketing it), read off the parabola through
    // that bin and its neighbours so the level and the position of a tone
    // survive the reduction. Strongest, not average: averaging would make a
    // tone read low whenever it fell between bins, and this display exists to
    // be aimed at. See pointDb in the .cpp.
    //
    // The FFT's own resolution still applies underneath: 10.8 Hz at 44.1 kHz,
    // so the bottom two octaves are a smooth hump around a tone rather than a
    // line, however finely the axis is sampled there.
    static constexpr int numPoints = 512;
    static constexpr float lowestHz = 20.0f;
    static constexpr float highestHz = 20000.0f;

    // dBFS, with 0 dB meaning a full-scale sine: the readout is calibrated
    // against the window's own sum (see prepare) and peaks are read off the
    // parabola through the peak bin and its neighbours (see pointDb), so a
    // -20 dBFS tone reads -20 within about a third of a dB wherever between
    // bins it happens to land. Digital silence reads exactly floorDb.
    static constexpr float floorDb = -120.0f;

    // One published measurement. Trivially copyable by design -- read() hands
    // frames over by value, and the frame carries the rate it was measured at
    // so the consumer's ballistics do not have to be told separately.
    struct Frame
    {
        std::array<float, numPoints> db {};

        double sampleRate = 0.0;
        double framesPerSecond = 0.0; // sampleRate / hopSize

        // Counts published frames, so a consumer can tell a repeat from a gap.
        // Gaps are normal: the hand-off keeps only the freshest frame.
        juce::uint32 sequence = 0;
    };

    //==========================================================================
    SpectrumTap() = default;

    // Message thread, and the only place anything allocates. Idempotent: safe
    // to call again when the host changes rate, which is the only time it is
    // called while the object already holds state -- prepareToPlay and the
    // audio thread never overlap.
    void prepare (double sampleRate);

    // Drop the half-window of history that a seek, or a move to a different
    // stem, has made meaningless. Audio-thread safe: it clears fixed storage
    // and nothing else, and it deliberately leaves the last published frame and
    // the sequence counter where they are -- the display holds its picture
    // instead of flashing, and a consumer never sees the sequence go backwards.
    // The next frame is a full window away.
    void reset() noexcept;

    // Audio thread. Mono-sums the pair (a hard-panned full-scale sine therefore
    // reads -6 dB, a centred one 0 dB) into the window, and publishes a frame
    // on every hop boundary that falls inside the call. `right` may be null for
    // a mono source. No locks, no allocation, no logarithms per sample.
    void push (const float* left, const float* right, int numSamples) noexcept;

    // Message thread. Takes the freshest published frame, if there is one that
    // has not been taken yet, and returns false when there is not -- which is
    // the normal state on repaints that fall between frames.
    bool read (Frame& dest) noexcept;

    bool isPrepared() const noexcept { return prepared; }
    double framesPerSecond() const noexcept { return rate / (double) hopSize; }

    //==========================================================================
    // The layout, in closed form, for whoever is drawing it. Both are pure
    // functions of the constants above -- no instance, no sample rate -- so the
    // panel can lay out its axis and synthesise test frames without a tap.
    static float pointFrequency (int point) noexcept;
    static int nearestPoint (float hz) noexcept;

private:
    void buildPointMap();
    void publishFrame() noexcept;

    // The hand-off: three slots, one always private to each side, one shared.
    // The writer fills its own slot and swaps it into `shared`; the reader
    // swaps its own slot out for whatever is there. Both sides are a single
    // exchange -- wait-free, no retry loop, and no copy on the producer's side.
    // A double buffer would need the reader to re-check for tearing; a queue
    // would need the reader to keep up. Neither is worth it when the only frame
    // the display wants is the newest one.
    static constexpr unsigned slotMask = 0x3, freshFlag = 0x4;

    std::array<Frame, 3> slots;
    std::atomic<unsigned> shared { 0 };
    int writeSlot = 1;  // audio thread only
    int readSlot = 2;   // message thread only
    juce::uint32 nextSequence = 1;

    static_assert (std::atomic<unsigned>::is_always_lock_free,
                   "the hand-off has to be lock free to run on the audio thread");

    // Audio thread only, sized by prepare().
    std::vector<float> ring;    // fftSize, mono-summed, oldest sample at ringWrite
    std::vector<float> scratch; // fftSize * 2, windowed copy then FFT magnitudes
    int ringWrite = 0;
    int sinceHop = 0;
    int primed = 0; // samples since the last reset; frames wait for a full window

    // Built once, read by the audio thread.
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<float> window; // Hann
    float magScale = 1.0f;

    struct PointBins
    {
        int first, last;        // inclusive, the bins to search for the strongest
        float lowBin, highBin;  // the point's own slice of the axis, in fractional bins
    };

    std::vector<PointBins> pointBins;

    double rate = 44100.0;
    bool prepared = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumTap)
};

//==============================================================================
// Display ballistics, deliberately on the consumer's side of the hand-off. The
// audio thread publishes what it measured and nothing else; how fast the
// picture moves is the panel's taste, and changing it must not cost the audio
// thread a thought. Own one per tap.
//
// Drive it from the panel's timer: one update() per repaint, with the frame
// read() just handed over or nullptr when it handed over none. Times are wall
// clock, derived from the repaint rate rather than the frame rate, so the
// picture behaves the same whether the host runs at 44.1 or 96 kHz.
class SpectrumSmoother
{
public:
    // Attack is always faster than release: a spectrum display that fell as
    // fast as it rose would be unreadable. Fast rises effectively instantly --
    // at 21.5 frames a second there is nothing shorter than a frame to resolve
    // -- and only the fall time really separates the three.
    enum class Speed
    {
        fast,   // 10 ms / 150 ms
        medium, // 30 ms / 450 ms
        slow    // 90 ms / 1400 ms
    };

    SpectrumSmoother();

    void setSpeed (Speed);
    Speed speed() const noexcept { return current; }

    // The rate update() is called at; 30 Hz to match the editor's timer.
    void setTickRate (double hz);

    // Back to silence, with no fall: for opening the panel, or moving the tap
    // to another stem, where sliding down from the last stem's picture would be
    // a lie.
    void reset();

    // One repaint's worth. Pass the frame read() produced, or nullptr when it
    // produced none -- the held target carries over, so a repaint that falls
    // between frames neither jumps nor decays.
    void update (const SpectrumTap::Frame* frame);

    // Both in dB on SpectrumTap's layout, ready to be tilted and drawn.
    const std::array<float, SpectrumTap::numPoints>& level() const noexcept { return smoothed; }
    const std::array<float, SpectrumTap::numPoints>& peak() const noexcept { return peaks; }

private:
    void updateCoefficients();

    std::array<float, SpectrumTap::numPoints> target {};
    std::array<float, SpectrumTap::numPoints> smoothed {};
    std::array<float, SpectrumTap::numPoints> peaks {};
    std::array<float, SpectrumTap::numPoints> holdLeft {}; // seconds of hold remaining

    Speed current = Speed::medium;
    double tickRate = 30.0;
    float attackCoefficient = 1.0f, releaseCoefficient = 1.0f;

    // Long enough to catch the eye on a passing transient, short enough that
    // the ghost still tracks a mix that is changing.
    static constexpr float peakHoldSeconds = 1.2f;
    static constexpr float peakFallDbPerSecond = 14.0f;
};

//==============================================================================
// The display tilt (issue #25). Music is pink-ish, so an honest readout of it
// slopes down and every judgement about balance has to be made against that
// slope. Lifting the picture by a few dB per octave puts pink material back on
// the horizontal, which is what makes a spectrum useful for aiming an EQ.
//
// It belongs here, in the paint, and not in the frames: the frames are the
// measurement, and the tilt is a view setting the user can change without the
// audio thread ever hearing about it. Pivoted at 1 kHz, which only fixes where
// the picture sits vertically.
inline float spectrumTiltDb (float hz, float dbPerOctave = 4.5f) noexcept
{
    return dbPerOctave * std::log2 (juce::jmax (hz, 1.0e-3f) / 1000.0f);
}

} // namespace splittr
