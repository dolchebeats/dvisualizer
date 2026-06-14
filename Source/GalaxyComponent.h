#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/**
 * GalaxyComponent
 *
 * A reactive "spiral galaxy" visualizer:
 *
 *   1. Nebula dust layer  — large soft-edged radial-gradient blobs orbiting
 *                            a common centre with Keplerian-style differential
 *                            rotation (inner blobs orbit faster than outer
 *                            ones), which winds them into spiral arms over
 *                            time. Each blob is tied to one frequency band:
 *                            its hue follows that band's dominant frequency
 *                            content and its brightness/size follow that
 *                            band's amplitude. Painted with a very high
 *                            persistence (slow-fading trail) so the arms
 *                            smear into long glowing streaks, exactly like
 *                            long-exposure nebula photography.
 *
 *   2. Starfield layer    — a static field of tiny background stars plus a
 *                            smaller set of "active" stars whose brightness
 *                            tracks the FFT magnitude at an assigned bin.
 *                            All stars are tinted with the user's primary
 *                            colour and have NO trail — redrawn fresh every
 *                            frame so they stay crisp points of light.
 *
 * Threading model
 * ───────────────
 * timerCallback() and paint() both run on the message thread (60 Hz).
 * Audio data arrives via std::atomic<float> reads (band energies) and an
 * AbstractFifo read (FFT data) — identical pattern to AlchemyComponent and
 * SpectrumAnalyzerComponent, so no locking is required anywhere.
 *
 * Memory model
 * ────────────
 * Dust blobs and stars are fixed-size std::array members allocated at
 * construction. The FFT working buffer is a fixed std::array. The only
 * dynamically-sized resources are the two juce::Image canvases, which are
 * (re)created in resized() — never inside the audio thread.
 */
class GalaxyComponent : public juce::Component,
    private juce::Timer
{
public:
    explicit GalaxyComponent(DvisualizerAudioProcessor& processor);
    ~GalaxyComponent() noexcept override;

    /** Push new display colours from the message thread. */
    void setColours(juce::Colour background, juce::Colour primary);

    void paint(juce::Graphics&) override;
    void resized()                override;

private:
    //==========================================================================
    void timerCallback() override;

    /** Read latest band energies from the processor atomics. */
    void updateAudioData();

    /** Pull one FFT frame from the processor FIFO and compute magnitudes. */
    void updateFFT();

    //==========================================================================
    // ── Nebula dust blobs ──────────────────────────────────────────────────
    //
    // Each blob orbits the centre on an elliptical path. Angular velocity
    // decreases with orbital radius (Keplerian-ish: closer = faster), so the
    // blobs naturally wind into trailing spiral arms as the persistence trail
    // accumulates.
    struct DustBlob
    {
        float radius;        // orbital radius, in fractions of min(width,height)
        float angle;         // current orbital angle (radians)
        float angularSpeed;  // radians/frame — set once, scaled by 1/radius
        float ellipseRatio;  // y-radius / x-radius, gives the spiral an elliptical tilt
        float baseSize;      // base blob radius in fractions of min(width,height)
        int   bandIndex;     // which frequency band drives this blob's hue/intensity
        float hueOffset;     // per-blob hue offset added to the band-derived hue
    };

    static constexpr int numDustBlobs = 10;
    std::array<DustBlob, numDustBlobs> dustBlobs{};

    /** Initialise blob orbital parameters (called once at construction). */
    void initDustBlobs();

    /** Advance all blob orbital angles by one frame. */
    void updateDustBlobs();

    /** Paint all dust blobs into the persistence canvas. */
    void drawDustBlobs(juce::Graphics& g, juce::Point<float> centre, float minDim);

    // Persistence retention for the dust canvas — very close to 1.0 so
    // the spiral arms accumulate into long glowing streaks rather than
    // fading out within a few seconds.
    static constexpr float dustTrailAlpha = 0.045f;   // fill alpha each frame

    //==========================================================================
    // ── Starfield ──────────────────────────────────────────────────────────
    struct Star
    {
        juce::Point<float> pos;        // fixed position, fraction of (w,h) [0..1]
        float              baseSize;   // base radius in pixels
        float              twinklePhase; // per-star phase offset for idle shimmer
        int                fftBin;     // FFT magnitude index this star reacts to
        bool               reactive;   // true if tied to an FFT bin
    };

    static constexpr int numBackgroundStars = 220;  // static, idle shimmer only
    static constexpr int numReactiveStars = 80;   // tied to FFT bins
    static constexpr int numStars = numBackgroundStars + numReactiveStars;
    std::array<Star, numStars> stars{};

    /** Initialise star positions (called once at construction). */
    void initStars();

    /** Paint all stars fresh — no trail. */
    void drawStars(juce::Graphics& g);

    //==========================================================================
    // ── FFT ────────────────────────────────────────────────────────────────
    static constexpr int   fftDisplaySize = 256;   // magnitude bins after log mapping
    static constexpr float mindB = -90.0f;
    static constexpr float maxdB = 0.0f;

    std::array<float, DvisualizerAudioProcessor::fftSize * 2> fftData{};
    std::array<float, fftDisplaySize>                          fftMag{};

    juce::dsp::FFT                      forwardFFT;
    juce::dsp::WindowingFunction<float> window;

    //==========================================================================
    // ── Idle time / phases ────────────────────────────────────────────────
    float idleTime{ 0.0f };   // advances every frame, drives twinkle + slow drift

    //==========================================================================
    // ── Audio snapshot (message thread local copies) ──────────────────────
    float bands[DvisualizerAudioProcessor::numBands]{};

    //==========================================================================
    // ── Colour state ──────────────────────────────────────────────────────
    juce::Colour bgColour{ 0xff0d0d1a };
    juce::Colour primaryColour{ 0xff00d4ff };

    //==========================================================================
    // ── Off-screen canvases ───────────────────────────────────────────────
    // dustCanvas accumulates the slow-fading nebula trail.
    // Stars are drawn directly onto the screen graphics each frame (no trail),
    // composited on top of the blitted dustCanvas.
    juce::Image dustCanvas;

    //==========================================================================
    DvisualizerAudioProcessor& processor;

    // Simple LCG for deterministic randomness — no std::rand
    uint32_t rngState{ 0x9e3779b9u };
    float nextRand();   // returns [0..1)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GalaxyComponent)
};