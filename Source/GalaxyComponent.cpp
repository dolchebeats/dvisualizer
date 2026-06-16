#include "GalaxyComponent.h"
#include "PluginProcessor.h"

//==============================================================================
GalaxyComponent::GalaxyComponent(DvisualizerAudioProcessor& p)
    : VisualizerBase(60, 0x9e3779b9u),     // 60 Hz, distinct rng seed from Alchemy
    processor(p),
    // Fast attack (0.45 → complement 0.55 used for gain), slow release (0.90)
    // so reactive stars pop quickly and then fade gently.
    fftProc(p, fftDisplaySize, /*attack=*/ 0.45f, /*release=*/ 0.90f)
{
    initDustBlobs();
    initStars();
}

//==============================================================================
void GalaxyComponent::setColours(juce::Colour background, juce::Colour primary)
{
    VisualizerBase::setColours(background, primary);

    // Re-fill the dust canvas with the new background immediately so there
    // is no flash of the old colour bleeding through on palette switch.
    if (dustCanvas.isValid())
    {
        juce::Graphics gc(dustCanvas);
        gc.fillAll(bgColour);
    }
}

//==============================================================================
void GalaxyComponent::resized()
{
    dustCanvas = juce::Image(juce::Image::ARGB,
        juce::jmax(1, getWidth()),
        juce::jmax(1, getHeight()),
        true);
    juce::Graphics gc(dustCanvas);
    gc.fillAll(bgColour);
}

//==============================================================================
// ── Dust blob initialisation ──────────────────────────────────────────────

void GalaxyComponent::initDustBlobs()
{
    for (int i = 0; i < numDustBlobs; ++i)
    {
        auto& blob = dustBlobs[i];

        // Tighter radial spread (0.05..0.34) so blobs overlap heavily and
        // produce continuous smeared bands instead of separate circles.
        const float t = static_cast<float>(i) / static_cast<float>(numDustBlobs - 1);
        blob.radius = 0.05f + t * 0.29f;

        blob.angle = nextRand() * juce::MathConstants<float>::twoPi;

        // Keplerian differential rotation: angular speed ∝ 1 / radius.
        // High base speed (0.05) so inner blobs complete a revolution quickly,
        // winding the trail into tight arms fast.
        const float baseSpeed = 0.05f;
        blob.angularSpeed = baseSpeed / juce::jmax(0.05f, blob.radius);

        blob.ellipseRatio = 0.55f + 0.25f * std::sin(static_cast<float> (i) * 1.7f);
        blob.baseSize = 0.16f + t * 0.22f;

        // Cycle through bands so every band drives at least one blob
        blob.bandIndex = i % DvisualizerAudioProcessor::numBands;
        blob.hueOffset = std::fmod(t + static_cast<float> (blob.bandIndex) * 0.07f, 1.0f);
    }
}

//==============================================================================
// ── Star initialisation ───────────────────────────────────────────────────

void GalaxyComponent::initStars()
{
    // Background stars — scattered across the disc, orbit slowly, idle twinkle
    for (int i = 0; i < numBackgroundStars; ++i)
    {
        auto& star = stars[i];
        star.radius = 0.02f + nextRand() * 0.53f;
        star.angle = nextRand() * juce::MathConstants<float>::twoPi;
        star.angularSpeed = 0.005f / juce::jmax(0.05f, star.radius);
        star.ellipseRatio = 0.55f + 0.25f * std::sin(static_cast<float>(i) * 0.37f);
        star.baseSize = 0.25f + nextRand() * 0.55f;   // 0.25 .. 0.80 px
        star.twinklePhase = nextRand() * juce::MathConstants<float>::twoPi;
        star.fftBin = -1;
        star.reactive = false;
    }

    // Reactive stars — tied to FFT bins, placed slightly closer in (brighter core)
    for (int i = 0; i < numReactiveStars; ++i)
    {
        auto& star = stars[numBackgroundStars + i];
        star.radius = 0.02f + nextRand() * 0.45f;
        star.angle = nextRand() * juce::MathConstants<float>::twoPi;
        star.angularSpeed = 0.010f / juce::jmax(0.05f, star.radius);
        star.ellipseRatio = 0.55f + 0.25f * std::sin(static_cast<float>(i) * 0.41f);
        star.baseSize = 0.45f + nextRand() * 0.85f;   // 0.45 .. 1.3 px
        star.twinklePhase = nextRand() * juce::MathConstants<float>::twoPi;

        // Spread reactive stars evenly across the full FFT display range
        star.fftBin = static_cast<int> (
            (static_cast<float> (i) / static_cast<float> (numReactiveStars))
            * static_cast<float> (fftDisplaySize - 1));
        star.reactive = true;
    }
}

//==============================================================================
// ── Audio data ─────────────────────────────────────────────────────────────

void GalaxyComponent::updateAudioData()
{
    for (int b = 0; b < DvisualizerAudioProcessor::numBands; ++b)
        bands[b] = processor.bandEnergy[b].load(std::memory_order_relaxed);
}

//==============================================================================
// ── Timer ──────────────────────────────────────────────────────────────────

void GalaxyComponent::timerCallback()
{
    updateAudioData();
    fftProc.pullAndProcess();   // always pull; reactive stars read fftProc directly

    updateDustBlobs();
    updateStars();

    idleTime += 1.0f / 60.0f;

    // Global hue drift — full colour-wheel cycle roughly every 50 seconds.
    hueDrift += 0.00033f + bands[2] * 0.0006f;
    if (hueDrift > 1.0f)
        hueDrift -= 1.0f;

    repaint();
}

//==============================================================================
// ── Dust blob motion ───────────────────────────────────────────────────────

void GalaxyComponent::updateDustBlobs()
{
    for (auto& blob : dustBlobs)
    {
        // Energy boost from the blob's own band makes that arm "spin up"
        // slightly when louder content is present.
        const float energyBoost = 1.0f + bands[blob.bandIndex] * 0.6f;
        blob.angle += blob.angularSpeed * energyBoost;

        if (blob.angle > juce::MathConstants<float>::twoPi)
            blob.angle -= juce::MathConstants<float>::twoPi;
    }
}

//==============================================================================
// ── Star motion ────────────────────────────────────────────────────────────

void GalaxyComponent::updateStars()
{
    for (auto& star : stars)
    {
        star.angle += star.angularSpeed;

        if (star.angle > juce::MathConstants<float>::twoPi)
            star.angle -= juce::MathConstants<float>::twoPi;
    }
}

//==============================================================================
// ── Dust blob rendering ────────────────────────────────────────────────────

void GalaxyComponent::drawDustBlobs(juce::Graphics& g,
    juce::Point<float> centre,
    float minDim)
{
    for (const auto& blob : dustBlobs)
    {
        const float energy = bands[blob.bandIndex];

        const float orbitR = blob.radius * minDim;
        const float x = centre.x + std::cos(blob.angle) * orbitR;
        const float y = centre.y + std::sin(blob.angle) * orbitR * blob.ellipseRatio;

        const float size = (blob.baseSize * minDim) * (0.7f + energy * 0.9f);
        const float hue = std::fmod(blob.hueOffset + hueDrift + energy * 0.12f, 1.0f);
        const float alpha = juce::jlimit(0.06f, 0.75f, 0.08f + energy * 0.7f);

        const juce::Colour core = juce::Colour::fromHSV(hue, 0.85f, 1.0f, alpha);
        const juce::Colour edge = juce::Colour::fromHSV(hue, 0.85f, 1.0f, 0.0f);

        // Radial gradient: bright core fading to transparent at the edge
        juce::ColourGradient gradient(core, x, y, edge, x + size, y, true);
        gradient.addColour(0.5, core.withAlpha(alpha * 0.5f));

        g.setGradientFill(gradient);
        g.fillEllipse(x - size, y - size, size * 2.0f, size * 2.0f);
    }
}

//==============================================================================
// ── Star rendering ─────────────────────────────────────────────────────────

void GalaxyComponent::drawStars(juce::Graphics& g,
    juce::Point<float> centre,
    float minDim)
{
    const auto& mag = fftProc.getMagnitudes();

    for (const auto& star : stars)
    {
        const float orbitR = star.radius * minDim;
        const float x = centre.x + std::cos(star.angle) * orbitR;
        const float y = centre.y + std::sin(star.angle) * orbitR * star.ellipseRatio;

        float brightness;
        float size = star.baseSize;

        if (star.reactive)
        {
            // Brightness tracks the FFT magnitude at the star's assigned bin.
            const float m = mag[juce::jlimit(0, fftDisplaySize - 1, star.fftBin)];
            brightness = juce::jlimit(0.15f, 1.0f, 0.15f + m * 1.3f);
            size *= (0.6f + m * 1.8f);
        }
        else
        {
            // Background stars: gentle idle shimmer — no audio reactivity.
            const float twinkle = 0.5f + 0.5f * std::sin(idleTime * 1.5f + star.twinklePhase);
            brightness = juce::jlimit(0.2f, 0.9f, 0.3f + twinkle * 0.6f);
        }

        g.setColour(primaryColour.withAlpha(brightness));
        g.fillEllipse(x - size, y - size, size * 2.0f, size * 2.0f);
    }
}

//==============================================================================
// ── paint() ──────────────────────────────────────────────────────────────────

void GalaxyComponent::paint(juce::Graphics& g)
{
    if (!dustCanvas.isValid())
        return;

    const float w = static_cast<float> (getWidth());
    const float h = static_cast<float> (getHeight());
    const float minDim = juce::jmin(w, h);
    const juce::Point<float> centre{ w * 0.5f, h * 0.5f };

    // ── Nebula dust layer (heavy persistence) ─────────────────────────────
    {
        juce::Graphics gc(dustCanvas);

        // Very slight fade per frame → slow-fading spiral arm trails.
        gc.setColour(bgColour.withAlpha(dustTrailAlpha));
        gc.fillRect(0, 0, dustCanvas.getWidth(), dustCanvas.getHeight());

        drawDustBlobs(gc, centre, minDim);
    }

    // Blit accumulated dust canvas
    g.drawImage(dustCanvas, 0, 0, getWidth(), getHeight(),
        0, 0, dustCanvas.getWidth(), dustCanvas.getHeight());

    // ── Starfield (no trail — drawn fresh every frame on top) ──────────────
    drawStars(g, centre, minDim);
}