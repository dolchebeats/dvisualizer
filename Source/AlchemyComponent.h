#pragma once
#include <JuceHeader.h>
#include "PluginProcessor.h"

//==============================================================================
/**
 * AlchemyComponent  (Steps 2–7)
 *
 * A reactive particle + shape visualizer driven by real-time audio analysis.
 *
 * Visual layers (painted back-to-front each frame):
 *   1. Persistence trail   — semi-transparent bg fill leaves motion blur
 *   2. Radial spectrum     — FFT magnitudes plotted in polar coordinates (Step 4)
 *   3. Lissajous rings     — parametric orbit rings modulated by band energy (Step 5)
 *   4. Particle system     — beat-spawned particles steered by curl noise (Steps 2,6)
 *   5. Camera pulse        — whole scene scales slightly on each beat (Step 7)
 *
 * Threading model
 * ───────────────
 * timerCallback() and paint() both run on the message thread.
 * Audio data arrives via std::atomic<float> reads (band energies, beat pulse)
 * and an AbstractFifo read (FFT data) — no locking required.
 *
 * Memory model
 * ────────────
 * Particles are stored in a fixed-size std::array allocated at construction;
 * no heap allocation occurs after init.  The FFT working buffer is likewise
 * a fixed std::array member.
 */
class AlchemyComponent : public juce::Component,
    private juce::Timer
{
public:
    explicit AlchemyComponent(DvisualizerAudioProcessor& processor);
    ~AlchemyComponent() noexcept override;

    /** Push new display colours from the message thread. */
    void setColours(juce::Colour background, juce::Colour primary);

    void paint(juce::Graphics&) override;
    void resized()                override;

private:
    //==========================================================================
    void timerCallback() override;

    // ── Step 1 helpers ───────────────────────────────────────────────────
    /** Read latest band energies and beat pulse from the processor atomics. */
    void updateAudioData();

    /** Pull one FFT frame from the processor FIFO and compute magnitudes. */
    void updateFFT();

    // ── Step 2: Particle system ──────────────────────────────────────────
    struct Particle
    {
        juce::Point<float> pos;       // current position (pixels)
        juce::Point<float> vel;       // velocity (pixels/frame)
        float              life;      // countdown 1.0 → 0.0
        float              size;      // radius in pixels
        float              hueOffset; // per-particle hue shift [0..1]
        bool               alive{ false };
    };

    static constexpr int maxParticles = 300;
    std::array<Particle, maxParticles> particles{};

    /** Spawn a burst of particles at the scene centre on a beat. */
    void spawnBurst(int count, float energy);

    /** Advance all live particles by one frame. */
    void updateParticles();

    /** Draw all live particles into g. */
    void drawParticles(juce::Graphics& g);

    // ── Step 3: Persistence trail ────────────────────────────────────────
    // Instead of fillAll(bg), each frame we fill with bg.withAlpha(trailAlpha)
    // so previous frames fade slowly — handled directly in paint().
    static constexpr float trailAlpha = 0.18f;

    // ── Step 4: Radial spectrum ──────────────────────────────────────────
    static constexpr int   scopeSize = 256;     // angular resolution
    static constexpr float mindB = -90.0f;
    static constexpr float maxdB = 0.0f;

    // FFT working buffer (2× fftSize for in-place transform)
    std::array<float, DvisualizerAudioProcessor::fftSize * 2> fftData{};
    std::array<float, scopeSize>                               radialMag{};

    juce::dsp::FFT                      forwardFFT;
    juce::dsp::WindowingFunction<float> window;

    void drawRadialSpectrum(juce::Graphics& g,
        juce::Point<float> centre,
        float innerR, float outerR);

    // ── Step 5: Lissajous rings ──────────────────────────────────────────
    float ringPhase{ 0.0f };   // slowly advances each frame

    void drawLissajousRings(juce::Graphics& g,
        juce::Point<float> centre,
        float radius);

    // ── Step 6: Curl noise field ─────────────────────────────────────────
    float noiseTime{ 0.0f };   // pseudo-time offset for noise evolution

    /** 2-D curl noise vector at position (x,y,t).
     *  Returns a direction that particles should accelerate toward.
     *  Pure maths — no allocation, no side effects. */
    static juce::Point<float> curlNoise(float x, float y, float t);

    // Tiny pseudo-random hash used by curlNoise
    static float smoothNoise(float x, float y);

    // ── Step 7: Colour cycling + camera pulse ────────────────────────────
    float hueAngle{ 0.0f };   // master hue, advances with mid-band energy
    float cameraPulse{ 1.0f };   // scale applied to the whole scene each frame

    // ── Audio snapshot (message thread local copies) ──────────────────────
    float bands[DvisualizerAudioProcessor::numBands]{};
    float beat{ 0.0f };   // current beat pulse [0..1]

    // ── Colour state ──────────────────────────────────────────────────────
    juce::Colour bgColour{ 0xff0d0d1a };
    juce::Colour primaryColour{ 0xff00d4ff };

    // ── Off-screen image for persistence accumulation ─────────────────────
    // We paint into this Image every frame then blit it to the screen.
    // Sized to match the component in resized().
    juce::Image canvas;

    //==========================================================================
    DvisualizerAudioProcessor& processor;

    // Simple LCG for deterministic per-particle randomness — no std::rand
    // (which is not guaranteed re-entrant)
    uint32_t rngState{ 0x12345678u };
    float nextRand();   // returns [0..1)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AlchemyComponent)
};