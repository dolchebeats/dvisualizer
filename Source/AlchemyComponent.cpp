#include "AlchemyComponent.h"
#include "PluginProcessor.h"

//==============================================================================
AlchemyComponent::AlchemyComponent(DvisualizerAudioProcessor& p)
    : VisualizerBase(60, 0x12345678u),    // 60 Hz, distinct rng seed
    processor(p),
    // FFTProcessor: 256 bins, fast attack (0.15 keeps as gain not coefficient),
    // very slow release (0.95) so the radial ring "glows" with long decay.
    // Note: FFTProcessor takes smoothing coefficients directly (closer to 1 = slower).
    fftProc(p, radialSize, /*attack=*/ 0.85f, /*release=*/ 0.95f)
{
    radialMag.fill(0.0f);

    for (auto& part : particles)
        part.alive = false;
}

//==============================================================================
void AlchemyComponent::setColours(juce::Colour background, juce::Colour primary)
{
    // Base stores the colours and calls repaint().
    VisualizerBase::setColours(background, primary);

    // Also re-fill the persistent canvas immediately so there is no flash
    // of stale colour when switching palettes mid-session.
    if (canvas.isValid())
    {
        juce::Graphics gc(canvas);
        gc.fillAll(bgColour);
    }
}

//==============================================================================
void AlchemyComponent::resized()
{
    // (Re)create the off-screen canvas at the component's new size.
    // This is the only place where Image allocation happens — never inside
    // timerCallback or paint.
    canvas = juce::Image(juce::Image::ARGB,
        juce::jmax(1, getWidth()),
        juce::jmax(1, getHeight()),
        true);
    juce::Graphics gc(canvas);
    gc.fillAll(bgColour);
}

//==============================================================================
// ── Step 1: Pull audio data from the processor ───────────────────────────────

void AlchemyComponent::updateAudioData()
{
    for (int b = 0; b < DvisualizerAudioProcessor::numBands; ++b)
        bands[b] = processor.bandEnergy[b].load(std::memory_order_relaxed);

    // Consume the beat pulse atomically — swap to 0 so we react only once
    // per beat regardless of how many timer ticks pass before the next one.
    beat = processor.beatPulse.exchange(0.0f, std::memory_order_relaxed);
}

//==============================================================================
// ── Timer — runs at 60 Hz on the message thread ──────────────────────────────

void AlchemyComponent::timerCallback()
{
    // 1. Pull latest audio analysis
    updateAudioData();

    // 2. Run the shared FFT pipeline; if new data arrived, copy it into our
    //    locally-smoothed radialMag array with the Alchemy-specific smoothing.
    if (fftProc.pullAndProcess())
    {
        const auto& mag = fftProc.getMagnitudes();
        for (int i = 0; i < radialSize; ++i)
        {
            // Secondary smoothing on top of FFTProcessor's smoothing:
            // very slow release gives the radial ring its "glowing" look.
            radialMag[i] = 0.95f * radialMag[i] + 0.15f * mag[i];
        }
    }

    // 3. Spawn burst on beat
    if (beat > 0.5f)
    {
        const int count = static_cast<int> (8 + bands[1] * 40.0f);
        spawnBurst(count, beat);
    }

    // 4. Advance particles (curl-noise steering included)
    updateParticles();

    // ── Step 7: hue cycling ───────────────────────────────────────────────
    hueAngle += 0.003f + bands[2] * 0.015f;
    if (hueAngle > 1.0f) hueAngle -= 1.0f;

    // ── Step 7: camera pulse decays back toward 1.0 ───────────────────────
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
    const float cx = static_cast<float> (getWidth()) * 0.5f;
    const float cy = static_cast<float> (getHeight()) * 0.5f;

    int spawned = 0;
    for (auto& p : particles)
    {
        if (spawned >= count) break;
        if (p.alive)          continue;

        const float angle = nextRand() * juce::MathConstants<float>::twoPi;
        const float speed = 1.5f + nextRand() * 3.0f + energy * 4.0f;

        p.pos = { cx, cy };
        p.vel = { std::cos(angle) * speed, std::sin(angle) * speed };
        p.life = 0.7f + nextRand() * 0.3f;
        p.size = 0.15f + nextRand() * 1.5f + energy * 1.0f;
        p.hueOffset = nextRand();
        p.alive = true;
        ++spawned;
    }
}

void AlchemyComponent::updateParticles()
{
    const float invW = 1.0f / static_cast<float> (juce::jmax(1, getWidth()));
    const float invH = 1.0f / static_cast<float> (juce::jmax(1, getHeight()));

    for (auto& p : particles)
    {
        if (!p.alive) continue;

        // ── Step 6: curl noise steering ────────────────────────────────
        const float nx = p.pos.x * invW;
        const float ny = p.pos.y * invH;

        const auto curl = curlNoise(nx * 3.0f, ny * 3.0f, noiseTime);

        const float noiseStrength = 0.08f + bands[4] * 0.25f;
        p.vel.x += curl.x * noiseStrength;
        p.vel.y += curl.y * noiseStrength;

        // Gentle drag
        p.vel.x *= 0.96f;
        p.vel.y *= 0.96f;

        p.pos.x += p.vel.x;
        p.pos.y += p.vel.y;

        // Life decay — faster when lots of high-frequency energy is present
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

        const float alpha = juce::jlimit(0.0f, 1.0f, p.life * 2.0f);
        const float radius = p.size * p.life;

        // ── Step 7: colour cycling ──────────────────────────────────────
        const float hue = std::fmod(hueAngle + p.hueOffset, 1.0f);
        g.setColour(juce::Colour::fromHSV(hue, 0.75f, 1.0f, alpha));
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

    for (int i = 0; i < radialSize; ++i)
    {
        // Full circle, starting from the top (offset by -π/2)
        const float angle = (static_cast<float>(i) / static_cast<float>(radialSize))
            * twoPi - juce::MathConstants<float>::halfPi;

        const float r = innerR + radialMag[i] * range;

        const float x = centre.x + std::cos(angle) * r;
        const float y = centre.y + std::sin(angle) * r;

        if (first)
        {
            outerRing.startNewSubPath(x, y);
            innerRing.startNewSubPath(centre.x + std::cos(angle) * innerR,
                centre.y + std::sin(angle) * innerR);
            first = false;
        }
        else
        {
            outerRing.lineTo(x, y);
            innerRing.lineTo(centre.x + std::cos(angle) * innerR,
                centre.y + std::sin(angle) * innerR);
        }
    }
    outerRing.closeSubPath();
    innerRing.closeSubPath();

    const float hue = std::fmod(hueAngle + 0.1f, 1.0f);

    g.setColour(juce::Colour::fromHSV(hue, 0.6f, 1.0f, 0.5f));
    g.strokePath(outerRing,
        juce::PathStrokeType(1.2f, juce::PathStrokeType::curved));

    // Faint filled area between the inner baseline circle and the spectrum line
    juce::Path filled = outerRing;
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

    struct RingParams { float freqA; float freqB; int bandIdx; float alpha; };
    static constexpr RingParams rings[] =
    {
        { 2.0f, 3.0f, 1, 0.55f },   // bass   ring — 2:3 Lissajous
        { 3.0f, 4.0f, 2, 0.40f },   // mid    ring — 3:4
        { 5.0f, 7.0f, 4, 0.25f },   // treble ring — 5:7
    };

    for (const auto& ring : rings)
    {
        const float r = radius * (0.45f + bands[ring.bandIdx] * 0.55f);
        const float phase = ringPhase * ring.freqA;

        juce::Path path;
        for (int i = 0; i <= steps; ++i)
        {
            const float t = static_cast<float> (i) / static_cast<float> (steps) * twoPi;
            const float x = centre.x + r * std::sin(ring.freqA * t + phase);
            const float y = centre.y + r * std::sin(ring.freqB * t);
            if (i == 0) path.startNewSubPath(x, y);
            else        path.lineTo(x, y);
        }
        path.closeSubPath();

        const float hue = std::fmod(hueAngle + static_cast<float> (ring.bandIdx) * 0.18f, 1.0f);
        g.setColour(juce::Colour::fromHSV(hue, 0.7f, 1.0f, ring.alpha));
        g.strokePath(path, juce::PathStrokeType(1.0f, juce::PathStrokeType::curved));
    }
}

//==============================================================================
// ── Step 6: Curl noise ───────────────────────────────────────────────────────
// Uses sine/cosine-based pseudo-noise to produce a smooth 2-D vector field.
// Curl of a scalar potential F is ( ∂F/∂y, -∂F/∂x ), which by construction
// is divergence-free — particles swirl without accumulating or dispersing.

float AlchemyComponent::smoothNoise(float x, float y)
{
    return std::sin(x * 1.7f + std::cos(y * 1.3f))
        * std::cos(y * 1.5f + std::sin(x * 0.9f));
}

juce::Point<float> AlchemyComponent::curlNoise(float x, float y, float t)
{
    constexpr float eps = 0.01f;

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

    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());
    const float cx = w * 0.5f;
    const float cy = h * 0.5f;

    // ── Step 3: Persistence trail ────────────────────────────────────────
    // Paint the background semi-transparently over the canvas so previous
    // frames decay — this produces the motion-blur / trail effect.
    {
        juce::Graphics gc(canvas);

        // Fade previous frame toward the background
        gc.setColour(bgColour.withAlpha(trailAlpha));
        gc.fillRect(0, 0, canvas.getWidth(), canvas.getHeight());

        // ── Step 7: camera pulse ─────────────────────────────────────────
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