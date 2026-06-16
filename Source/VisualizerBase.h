#pragma once
#include <JuceHeader.h>

//==============================================================================
/**
 * VisualizerBase
 *
 * Abstract base class shared by every visualizer component
 * (SpectrumAnalyzerComponent, AlchemyComponent, GalaxyComponent).
 *
 * Consolidates the three pieces of boilerplate that were previously
 * copy-pasted across all three files:
 *
 *   1. Colour state (bgColour / primaryColour) and setColours()
 *   2. Timer lifecycle (startTimerHz in ctor, stopTimer in dtor)
 *   3. A deterministic LCG random number generator (nextRand())
 *
 * Threading notes
 * ───────────────
 * All methods here — including nextRand() — are intended to be called
 * exclusively from the message thread (inside timerCallback or paint).
 * No locking is required because the message thread is single-threaded.
 *
 * Subclasses must:
 *   - Call VisualizerBase(rateHz) in their constructor to start the timer.
 *   - Implement timerCallback() (declared pure here via the private Timer).
 *   - Implement paint() and resized() as usual for juce::Component.
 */
class VisualizerBase : public juce::Component,
    private juce::Timer
{
public:
    //==========================================================================
    // Construction / destruction

    /**
     * @param timerHz  Repaint rate in Hz.  Pass 60 for all current visualizers.
     * @param rngSeed  Starting state for the per-instance LCG.
     *                 Different seeds give each visualizer its own random stream.
     */
    explicit VisualizerBase(int timerHz, uint32_t rngSeed = 0x12345678u)
        : rngState(rngSeed)
    {
        startTimerHz(timerHz);
    }

    ~VisualizerBase() noexcept override
    {
        stopTimer();
    }

    //==========================================================================
    // Colour management

    /**
     * Push a new background / primary colour pair from the message thread.
     * Derived classes may override to perform extra work (e.g. re-filling a
     * persistent off-screen canvas), but should call the base implementation
     * first so that bgColour and primaryColour are always kept up to date.
     */
    virtual void setColours(juce::Colour background, juce::Colour primary)
    {
        bgColour = background;
        primaryColour = primary;
        repaint();
    }

protected:
    //==========================================================================
    // Shared state — available to all subclasses

    juce::Colour bgColour{ 0xff0d0d1a };   // default: Dark Navy
    juce::Colour primaryColour{ 0xff00d4ff };   // default: Cyan

    //==========================================================================
    // LCG random number generator
    //
    // A simple 32-bit linear congruential generator — deterministic, cheap,
    // and safe to call from the message thread without any synchronisation.
    // Produces values in [0, 1).
    //
    // Constants are the classic Numerical Recipes LCG:
    //   X_{n+1} = 1664525 * X_n + 1013904223  (mod 2^32)

    /** Returns the next pseudo-random float in [0, 1). */
    float nextRand()
    {
        rngState = rngState * 1664525u + 1013904223u;
        // Drop the 8 low-order bits (least random), use the upper 24 bits.
        return static_cast<float> (rngState >> 8)
            / static_cast<float> (1 << 24);
    }

private:
    //==========================================================================
    // Timer — subclasses must implement this.
    // The override is private here so callers cannot call timerCallback()
    // directly; it is only reachable via the Timer machinery.
    void timerCallback() override = 0;

    uint32_t rngState;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VisualizerBase)
};