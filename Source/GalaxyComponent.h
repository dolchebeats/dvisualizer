#pragma once
#include "VisualizerBase.h"
#include "FFTProcessor.h"

//==============================================================================
/**
 * GalaxyComponent
 *
 * A reactive "spiral galaxy" visualizer that derives from VisualizerBase
 * for shared colour state, timer lifecycle, and the LCG random number
 * generator.  Delegates all FFT work to FFTProcessor.
 *
 * Visual layers:
 *
 *   1. Nebula dust layer  — large soft-edged radial-gradient blobs orbiting
 *                            the centre with Keplerian differential rotation
 *                            (inner blobs orbit faster), which winds them into
 *                            spiral arms.  Each blob is tied to a frequency
 *                            band: hue follows dominant frequency content,
 *                            brightness/size follow amplitude.  Heavy
 *                            persistence trail smears the arms into long
 *                            glowing streaks.
 *
 *   2. Starfield layer    — static background stars (idle shimmer) plus
 *                            reactive stars whose brightness tracks FFT
 *                            magnitude at an assigned bin.  All stars are
 *                            tinted with the primary colour and drawn fresh
 *                            every frame (no trail) for crisp points of light.
 *
 * Threading model
 * ───────────────
 * timerCallback() and paint() both run on the message thread (60 Hz).
 * Audio data arrives via std::atomic<float> reads and AbstractFifo reads
 * inside FFTProcessor — no locking required.
 *
 * Memory model
 * ────────────
 * All dust blobs and stars are fixed-size std::array members.  The off-screen
 * canvas is (re)created in resized() only.
 */
class GalaxyComponent final : public VisualizerBase
{
public:
    explicit GalaxyComponent(DvisualizerAudioProcessor& processor);
    ~GalaxyComponent() noexcept override = default;

    /** Push new display colours — also re-fills the persistent dust canvas. */
    void setColours(juce::Colour background, juce::Colour primary) override;

    void paint(juce::Graphics&) override;
    void resized()                override;

private:
    //==========================================================================
    void timerCallback() override;

    void updateAudioData();

    //==========================================================================
    // ── Nebula dust blobs ──────────────────────────────────────────────────
    struct DustBlob
    {
        float radius;        // orbital radius, fraction of min(width,height)
        float angle;         // current orbital angle (radians)
        float angularSpeed;  // radians/frame — set by 1/radius (Keplerian)
        float ellipseRatio;  // y-radius / x-radius (elliptical tilt)
        float baseSize;      // base blob radius, fraction of min(width,height)
        int   bandIndex;     // which frequency band drives hue/intensity
        float hueOffset;     // per-blob static hue offset
    };

    static constexpr int numDustBlobs = 16;
    std::array<DustBlob, numDustBlobs> dustBlobs{};

    void initDustBlobs();
    void updateDustBlobs();
    void drawDustBlobs(juce::Graphics& g,
        juce::Point<float> centre,
        float minDim);

    // Persistence retention: close to 1.0 so spiral arms accumulate into
    // long glowing streaks.
    static constexpr float dustTrailAlpha = 0.030f;

    //==========================================================================
    // ── Starfield ──────────────────────────────────────────────────────────
    struct Star
    {
        float radius;        // orbital radius, fraction of min(width,height)
        float angle;         // current orbital angle (radians)
        float angularSpeed;  // radians/frame (same Keplerian law as dust)
        float ellipseRatio;  // matches dust blob tilt

        float baseSize;      // base radius in pixels
        float twinklePhase;  // per-star phase for idle shimmer
        int   fftBin;        // FFT magnitude bin index (-1 for background stars)
        bool  reactive;      // true if tied to an FFT bin
    };

    static constexpr int numBackgroundStars = 220;
    static constexpr int numReactiveStars = 80;
    static constexpr int numStars = numBackgroundStars + numReactiveStars;
    std::array<Star, numStars> stars{};

    void initStars();
    void updateStars();
    void drawStars(juce::Graphics& g,
        juce::Point<float> centre,
        float minDim);

    //==========================================================================
    // ── FFT ────────────────────────────────────────────────────────────────
    // 256 display bins, fast attack / slow release — reactive stars pop
    // quickly and then fade gently.
    static constexpr int fftDisplaySize = 256;
    FFTProcessor fftProc;

    //==========================================================================
    // ── Time / animation state ────────────────────────────────────────────
    float idleTime{ 0.0f };   // advances every frame; drives twinkle
    float hueDrift{ 0.0f };   // slowly cycles the global palette hue

    //==========================================================================
    // ── Audio snapshot (message thread local copies) ──────────────────────
    float bands[DvisualizerAudioProcessor::numBands]{};

    //==========================================================================
    // ── Off-screen canvas ─────────────────────────────────────────────────
    juce::Image dustCanvas;

    //==========================================================================
    DvisualizerAudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GalaxyComponent)
};