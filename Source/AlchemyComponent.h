#pragma once
#include "VisualizerBase.h"
#include "FFTProcessor.h"

//==============================================================================
/**
 * AlchemyComponent  (Steps 2–7)
 *
 * A reactive particle + shape visualizer driven by real-time audio analysis.
 * Derives from VisualizerBase for shared colour state, timer lifecycle, and
 * the LCG random number generator.  Delegates all FFT work to FFTProcessor.
 *
 * Visual layers (painted back-to-front each frame into an off-screen canvas):
 *   1. Persistence trail   — semi-transparent bg fill leaves motion blur
 *   2. Radial spectrum     — FFT magnitudes plotted in polar coordinates (Step 4)
 *   3. Lissajous rings     — parametric orbit rings modulated by band energy (Step 5)
 *   4. Particle system     — beat-spawned particles steered by curl noise (Steps 2,6)
 *   5. Camera pulse        — whole scene scales slightly on each beat (Step 7)
 *
 * Threading model
 * ───────────────
 * timerCallback() and paint() both run on the message thread (60 Hz).
 * Audio data arrives via std::atomic<float> reads (band energies, beat pulse)
 * and an AbstractFifo read inside FFTProcessor — no locking required.
 *
 * Memory model
 * ────────────
 * Particles and the FFT working buffer are fixed-size std::array members —
 * no heap allocation after construction.  The off-screen canvas is (re)created
 * in resized() and never inside the audio or timer callbacks.
 */
class AlchemyComponent final : public VisualizerBase
{
public:
    explicit AlchemyComponent(DvisualizerAudioProcessor& processor);
    ~AlchemyComponent() noexcept override = default;

    /** Push new display colours — also re-fills the persistent canvas. */
    void setColours(juce::Colour background, juce::Colour primary) override;

    void paint(juce::Graphics&) override;
    void resized()                override;

private:
    //==========================================================================
    void timerCallback() override;

    // ── Step 1 helpers ───────────────────────────────────────────────────
    void updateAudioData();

    // ── Step 2: Particle system ──────────────────────────────────────────
    struct Particle
    {
        juce::Point<float> pos;          // current position (pixels)
        juce::Point<float> vel;          // velocity (pixels / frame)
        float              life{};  // countdown 1.0 → 0.0
        float              size{};  // radius in pixels
        float              hueOffset{};  // per-particle hue shift [0..1]
        bool               alive{ false };
    };

    static constexpr int maxParticles = 300;
    std::array<Particle, maxParticles> particles{};

    void spawnBurst(int count, float energy);
    void updateParticles();
    void drawParticles(juce::Graphics& g);

    // ── Step 3: Persistence trail ────────────────────────────────────────
    static constexpr float trailAlpha = 0.18f;

    // ── Step 4: Radial spectrum ──────────────────────────────────────────
    // Number of angular bins — only 256 needed here (fewer than scopeSize).
    static constexpr int radialSize = 256;

    // Local smoothed copy of the FFT magnitudes mapped to radial angles.
    // We keep a separate smoothed array with a very slow release (0.95/0.15)
    // because the radial spectrum intentionally has a different feel from
    // the line spectrum in SpectrumAnalyzerComponent.
    std::array<float, radialSize> radialMag{};

    void drawRadialSpectrum(juce::Graphics& g,
        juce::Point<float> centre,
        float innerR, float outerR);

    // ── Step 5: Lissajous rings ──────────────────────────────────────────
    float ringPhase{ 0.0f };

    void drawLissajousRings(juce::Graphics& g,
        juce::Point<float> centre,
        float radius);

    // ── Step 6: Curl noise field ─────────────────────────────────────────
    float noiseTime{ 0.0f };

    static juce::Point<float> curlNoise(float x, float y, float t);
    static float              smoothNoise(float x, float y);

    // ── Step 7: Colour cycling + camera pulse ────────────────────────────
    float hueAngle{ 0.0f };
    float cameraPulse{ 1.0f };

    // ── Audio snapshot (message thread local copies) ──────────────────────
    float bands[DvisualizerAudioProcessor::numBands]{};
    float beat{ 0.0f };

    // ── FFT ───────────────────────────────────────────────────────────────
    // Attack is fast (0.15), release is very slow (0.95) — the radial
    // spectrum intentionally "glows" longer than the line analyzer.
    FFTProcessor fftProc;

    // ── Off-screen canvas ─────────────────────────────────────────────────
    juce::Image canvas;

    //==========================================================================
    DvisualizerAudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlchemyComponent)
};