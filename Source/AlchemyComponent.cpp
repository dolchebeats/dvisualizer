#include "AlchemyComponent.h"
#include "PluginProcessor.h"

//==============================================================================
AlchemyComponent::AlchemyComponent(DvisualizerAudioProcessor& p)
    : processor(p),
    forwardFFT(DvisualizerAudioProcessor::fftOrder),
    window(DvisualizerAudioProcessor::fftSize,
        juce::dsp::WindowingFunction<float>::hann)
{
    fftData.fill(0.0f);
    radialMag.fill(0.0f);

    for (auto& part : particles)
        part.alive = false;

    startTimerHz(60);
}

AlchemyComponent::~AlchemyComponent() noexcept
{
    stopTimer();
}

void AlchemyComponent::setColours(juce::Colour background, juce::Colour primary)
{
    bgColour = background;
    primaryColour = primary;
    // Re-fill the canvas with the new background immediately so there is no
    // flash of stale colour when switching palettes.
    if (canvas.isValid())
    {
        juce::Graphics gc(canvas);
        gc.fillAll(bgColour);
    }
    repaint();
}

void AlchemyComponent::resized()
{
    // Recreate the off-screen canvas at the new size
    canvas = juce::Image(juce::Image::ARGB,
        juce::jmax(1, getWidth()),
        juce::jmax(1, getHeight()),
        true);
    juce::Graphics gc(canvas);
    gc.fillAll(bgColour);
}

//==============================================================================
// LCG random number generator — message-thread only, no std::rand
float AlchemyComponent::nextRand()
{
    rngState = rngState * 1664525u + 1013904223u;
    return static_cast<float>(rngState >> 8) / static_cast<float>(1 << 24);
}

//==============================================================================
// ── Step 1: pull audio data from the processor ───────────────────────────────
void AlchemyComponent::updateAudioData()
{
    for (int b = 0; b < DvisualizerAudioProcessor::numBands; ++b)
        bands[b] = processor.bandEnergy[b].load(std::memory_order_relaxed);

    // Consume the beat pulse — swap to 0 so we only react once per beat
    beat = processor.beatPulse.exchange(0.0f, std::memory_order_relaxed);
}

//==============================================================================
// ── Step 1 cont.: FFT read from FIFO ─────────────────────────────────────────
void AlchemyComponent::updateFFT()
{
    if (processor.fftFifo.getNumReady() < DvisualizerAudioProcessor::fftSize)
        return;

    int start1, size1, start2, size2;
    processor.fftFifo.prepareToRead(
        DvisualizerAudioProcessor::fftSize, start1, size1, start2, size2);

    if (size1 > 0)
        juce::FloatVectorOperations::copy(fftData.data(),
            processor.fftFifoBuffer.data() + start1, size1);
    if (size2 > 0)
        juce::FloatVectorOperations::copy(fftData.data() + size1,
            processor.fftFifoBuffer.data() + start2, size2);

    processor.fftFifo.finishedRead(size1 + size2);

    // Zero the imaginary upper half
    std::fill(fftData.begin() + DvisualizerAudioProcessor::fftSize,
        fftData.end(), 0.0f);

    // Window + FFT
    window.multiplyWithWindowingTable(fftData.data(),
        DvisualizerAudioProcessor::fftSize);
    forwardFFT.performFrequencyOnlyForwardTransform(fftData.data());

    // Map FFT bins → radialMag using log-frequency mapping
    const float sr = static_cast<float>(processor.currentSampleRate);
    const float freqPerBin = sr / static_cast<float>(DvisualizerAudioProcessor::fftSize);
    const int   nyquist = DvisualizerAudioProcessor::fftSize / 2;
    const float minFreq = 20.0f;
    const float maxFreq = juce::jmax(minFreq + 1.0f, sr * 0.5f);

    for (int i = 0; i < scopeSize; ++i)
    {
        const float p = static_cast<float>(i) / static_cast<float>(scopeSize - 1);
        const float freq = minFreq * std::pow(maxFreq / minFreq, p);
        const int   bin = juce::jlimit(1, nyquist - 1,
            static_cast<int>(freq / freqPerBin));

        const float dBVal = juce::Decibels::gainToDecibels(fftData[bin])
            - juce::Decibels::gainToDecibels(
                static_cast<float>(DvisualizerAudioProcessor::fftSize));

        // Smooth the radial magnitudes with a slow release
        const float newVal = juce::jmap(juce::jlimit(mindB, maxdB, dBVal),
            mindB, maxdB, 0.0f, 1.0f);
        radialMag[i] = 0.95f * radialMag[i] + 0.15f * newVal;
    }
}

//==============================================================================
// ── Timer — runs at 60 Hz on the message thread ──────────────────────────────
void AlchemyComponent::timerCallback()
{
    // 1. Pull latest audio analysis
    updateAudioData();
    updateFFT();

    // 2. Spawn burst on beat
    if (beat > 0.5f)
    {
        // Number of new particles scales with bass energy
        const int count = static_cast<int>(8 + bands[1] * 40.0f);
        spawnBurst(count, beat);
    }

    // 3. Advance particles (includes curl noise steering)
    updateParticles();

    // ── Step 7: hue cycling ───────────────────────────────────────────────
    // Hue advances faster when the mid-band is active
    hueAngle += 0.003f + bands[2] * 0.015f;
    if (hueAngle > 1.0f) hueAngle -= 1.0f;

    // ── Step 7: camera pulse decays back to 1.0 ───────────────────────────
    const float targetPulse = 1.0f + beat * 0.06f + bands[1] * 0.03f;
    cameraPulse = 0.88f * cameraPulse + 0.12f * targetPulse;

    // ── Step 6: advance noise time ────────────────────────────────────────
    noiseTime += 0.012f + bands[2] * 0.018f;

    // ── Step 5: advance ring phase ────────────────────────────────────────
    ringPhase += 0.008f + bands[3] * 0.012f;

    repaint();
}

//==============================================================================
// ── Step 2: Particle system ───────────────────────────────────────────────────

void AlchemyComponent::spawnBurst(int count, float energy)
{
    const float cx = getWidth() * 0.5f;
    const float cy = getHeight() * 0.5f;

    int spawned = 0;
    for (auto& p : particles)
    {
        if (spawned >= count) break;
        if (p.alive) continue;

        // Random direction around full circle
        const float angle = nextRand() * juce::MathConstants<float>::twoPi;
        // Speed scaled by bass energy + some randomness
        const float speed = 1.5f + nextRand() * 3.0f + energy * 4.0f;

        p.pos = { cx, cy };
        p.vel = { std::cos(angle) * speed, std::sin(angle) * speed };
        p.life = 0.7f + nextRand() * 0.3f;   // 0.7..1.0
        p.size = 0.15f + nextRand() * 1.5f + energy * 1.0f;
        p.hueOffset = nextRand();
        p.alive = true;
        ++spawned;
    }
}

void AlchemyComponent::updateParticles()
{
    for (auto& p : particles)
    {
        if (!p.alive) continue;

        // ── Step 6: curl noise steering ────────────────────────────────
        // Normalise position to roughly [-1,1] space for noise lookup
        const float nx = p.pos.x / static_cast<float>(juce::jmax(1, getWidth()));
        const float ny = p.pos.y / static_cast<float>(juce::jmax(1, getHeight()));

        const auto curl = curlNoise(nx * 3.0f, ny * 3.0f, noiseTime);
        // Presence band modulates the strength of the noise pull
        const float noiseStrength = 0.08f + bands[4] * 0.25f;
        p.vel.x += curl.x * noiseStrength;
        p.vel.y += curl.y * noiseStrength;

        // Gentle drag so particles don't fly off forever
        p.vel.x *= 0.96f;
        p.vel.y *= 0.96f;

        p.pos.x += p.vel.x;
        p.pos.y += p.vel.y;

        // Life decay — faster decay when there is lots of high-frequency energy
        p.life -= 0.008f + bands[5] * 0.006f;
        if (p.life <= 0.0f)
            p.alive = false;
    }
}

void AlchemyComponent::drawParticles(juce::Graphics& g)
{
    for (const auto& p : particles)
    {
        if (!p.alive) continue;

        // Fade alpha with life; shrink size toward end
        const float alpha = juce::jlimit(0.0f, 1.0f, p.life * 2.0f);
        const float radius = p.size * p.life;

        // ── Step 7: colour cycling ──────────────────────────────────────
        // Each particle has a hue offset from the master hue angle
        const float hue = std::fmod(hueAngle + p.hueOffset, 1.0f);
        const juce::Colour col =
            juce::Colour::fromHSV(hue, 0.75f, 1.0f, alpha);

        g.setColour(col);
        g.fillEllipse(p.pos.x - radius, p.pos.y - radius,
            radius * 2.0f, radius * 2.0f);
    }
}

//==============================================================================
// ── Step 4: Radial spectrum ───────────────────────────────────────────────────
void AlchemyComponent::drawRadialSpectrum(juce::Graphics& g,
    juce::Point<float> centre,
    float innerR, float outerR)
{
    const float range = outerR - innerR;
    const float twoPi = juce::MathConstants<float>::twoPi;

    juce::Path outerRing, innerRing;
    bool first = true;

    for (int i = 0; i < scopeSize; ++i)
    {
        // Angle: full circle, starting from the top (offset by -π/2)
        const float angle = (static_cast<float>(i) / static_cast<float>(scopeSize))
            * twoPi - juce::MathConstants<float>::halfPi;

        // Radius = inner + magnitude * available range
        const float r = innerR + radialMag[i] * range;

        const float x = centre.x + std::cos(angle) * r;
        const float y = centre.y + std::sin(angle) * r;

        if (first)
        {
            outerRing.startNewSubPath(x, y);
            // Inner baseline ring (just the inner circle anchor)
            innerRing.startNewSubPath(
                centre.x + std::cos(angle) * innerR,
                centre.y + std::sin(angle) * innerR);
            first = false;
        }
        else
        {
            outerRing.lineTo(x, y);
            innerRing.lineTo(
                centre.x + std::cos(angle) * innerR,
                centre.y + std::sin(angle) * innerR);
        }
    }
    outerRing.closeSubPath();
    innerRing.closeSubPath();

    // Draw with the primary colour at reduced alpha so layers blend nicely
    const float hue = std::fmod(hueAngle + 0.1f, 1.0f);
    g.setColour(juce::Colour::fromHSV(hue, 0.6f, 1.0f, 0.5f));
    g.strokePath(outerRing,
        juce::PathStrokeType(1.2f, juce::PathStrokeType::curved));

    // Faint filled area between inner circle and spectrum line
    juce::Path filled = outerRing;
    // Reverse-append the inner ring to create a filled annular region
    filled.addPath(innerRing);
    g.setColour(juce::Colour::fromHSV(hue, 0.5f, 0.8f, 0.12f));
    g.fillPath(filled);
}

//==============================================================================
// ── Step 5: Lissajous rings ───────────────────────────────────────────────────
void AlchemyComponent::drawLissajousRings(juce::Graphics& g,
    juce::Point<float> centre,
    float radius)
{
    const float twoPi = juce::MathConstants<float>::twoPi;
    const int   steps = 512;

    // Draw three rings with different frequency ratios and band modulation
    struct RingParams { float freqA; float freqB; int bandIdx; float alpha; };
    const RingParams rings[] =
    {
        { 2.0f, 3.0f, 1, 0.55f },   // bass   ring — 2:3 Lissajous
        { 3.0f, 4.0f, 2, 0.40f },   // mid    ring — 3:4
        { 5.0f, 7.0f, 4, 0.25f },   // treble ring — 5:7 (more complex)
    };

    for (const auto& ring : rings)
    {
        // Radius modulated by the corresponding band energy
        const float r = radius * (0.45f + bands[ring.bandIdx] * 0.55f);
        // Phase offset advances at a speed tied to ring phase
        const float phase = ringPhase * ring.freqA;

        juce::Path path;
        for (int i = 0; i <= steps; ++i)
        {
            const float t = static_cast<float>(i) / static_cast<float>(steps) * twoPi;
            const float x = centre.x + r * std::sin(ring.freqA * t + phase);
            const float y = centre.y + r * std::sin(ring.freqB * t);
            if (i == 0) path.startNewSubPath(x, y);
            else        path.lineTo(x, y);
        }
        path.closeSubPath();

        const float hue = std::fmod(hueAngle + static_cast<float>(ring.bandIdx) * 0.18f, 1.0f);
        g.setColour(juce::Colour::fromHSV(hue, 0.7f, 1.0f, ring.alpha));
        g.strokePath(path, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved));
    }
}

//==============================================================================
// ── Step 6: Curl noise ───────────────────────────────────────────────────────
// Uses a simple grid of sine/cosine-based pseudo-noise to produce a smooth
// 2-D vector field.  Curl of a scalar potential F is ( ∂F/∂y, -∂F/∂x ),
// which by construction is divergence-free — particles swirl without
// accumulating or dispersing.

float AlchemyComponent::smoothNoise(float x, float y)
{
    // Cheap smooth noise via overlapping sine harmonics — no lookup table needed
    return std::sin(x * 1.7f + std::cos(y * 1.3f))
        * std::cos(y * 1.5f + std::sin(x * 0.9f));
}

juce::Point<float> AlchemyComponent::curlNoise(float x, float y, float t)
{
    constexpr float eps = 0.01f;

    // Potential function F(x,y) evaluated at offset positions
    const float Fxpy = smoothNoise(x, y + eps + t * 0.3f);
    const float Fxmy = smoothNoise(x, y - eps + t * 0.3f);
    const float Fypx = smoothNoise(x + eps + t * 0.2f, y);
    const float Fymx = smoothNoise(x - eps + t * 0.2f, y);

    // Central-difference gradient → curl components
    const float dFdy = (Fxpy - Fxmy) / (2.0f * eps);
    const float dFdx = (Fypx - Fymx) / (2.0f * eps);

    return { dFdy, -dFdx };
}

//==============================================================================
// ── paint() — composites all layers ──────────────────────────────────────────
void AlchemyComponent::paint(juce::Graphics& g)
{
    if (!canvas.isValid())
        return;

    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;

    // ── Step 3: Persistence trail ────────────────────────────────────────
    // Paint the background semi-transparently over the canvas so previous
    // frames decay rather than being erased — this is the core of the
    // "motion blur / trail" effect.  We draw into the off-screen canvas,
    // then blit to the screen below.
    {
        juce::Graphics gc(canvas);

        // Fade previous frame
        gc.setColour(bgColour.withAlpha(trailAlpha));
        gc.fillRect(0, 0, canvas.getWidth(), canvas.getHeight());

        // ── Step 7: camera pulse ─────────────────────────────────────────
        // Apply a subtle scale transform centred on the canvas midpoint
        // so the whole scene breathes outward on each beat.
        juce::Graphics::ScopedSaveState saveState(gc);
        gc.addTransform(
            juce::AffineTransform::scale(cameraPulse, cameraPulse, cx, cy));

        // ── Step 4: Radial spectrum ───────────────────────────────────────
        const float minDim = juce::jmin(w, h);
        const float innerRad = minDim * 0.12f;
        const float outerRad = minDim * (1.0f + bands[1] * 1.0f);
        drawRadialSpectrum(gc, { cx, cy }, innerRad, outerRad);

        // ── Step 5: Lissajous rings ───────────────────────────────────────
        drawLissajousRings(gc, { cx, cy }, minDim * 0.38f);

        // ── Step 2: Particles ─────────────────────────────────────────────
        drawParticles(gc);
    }

    // Blit the accumulated canvas to screen
    g.drawImage(canvas, 0, 0, getWidth(), getHeight(),
        0, 0, canvas.getWidth(), canvas.getHeight());
}