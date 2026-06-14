#include "GalaxyComponent.h"
#include "PluginProcessor.h"

//==============================================================================
GalaxyComponent::GalaxyComponent(DvisualizerAudioProcessor& p)
    : processor(p),
    forwardFFT(DvisualizerAudioProcessor::fftOrder),
    window(DvisualizerAudioProcessor::fftSize,
        juce::dsp::WindowingFunction<float>::hann)
{
    fftData.fill(0.0f);
    fftMag.fill(0.0f);

    initDustBlobs();
    initStars();

    startTimerHz(60);
}

GalaxyComponent::~GalaxyComponent() noexcept
{
    stopTimer();
}

void GalaxyComponent::setColours(juce::Colour background, juce::Colour primary)
{
    bgColour = background;
    primaryColour = primary;

    // Re-fill the dust canvas with the new background immediately so there
    // is no flash of the old colour bleeding through on palette switch.
    if (dustCanvas.isValid())
    {
        juce::Graphics gc(dustCanvas);
        gc.fillAll(bgColour);
    }
    repaint();
}

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
// LCG random number generator — message-thread only, no std::rand
float GalaxyComponent::nextRand()
{
    rngState = rngState * 1664525u + 1013904223u;
    return static_cast<float>(rngState >> 8) / static_cast<float>(1 << 24);
}

//==============================================================================
// ── Dust blob initialisation ──────────────────────────────────────────────
void GalaxyComponent::initDustBlobs()
{
    for (int i = 0; i < numDustBlobs; ++i)
    {
        auto& blob = dustBlobs[i];

        // Spread orbital radii from close-in to far-out (fractions of minDim)
        const float t = static_cast<float>(i) / static_cast<float>(numDustBlobs - 1);
        blob.radius = 0.08f + t * 0.42f;   // 0.08 .. 0.50

        // Random starting angle so blobs don't all line up
        blob.angle = nextRand() * juce::MathConstants<float>::twoPi;

        // Keplerian-style differential rotation: angular speed scales
        // inversely with radius, so inner blobs sweep around much faster
        // than outer ones — this is what winds the trail into spiral arms.
        // Base speed tuned so the innermost blob completes roughly one
        // revolution every ~8 seconds at 60 Hz, outermost much slower.
        const float baseSpeed = 0.0035f;
        blob.angularSpeed = baseSpeed / juce::jmax(0.08f, blob.radius);

        // Alternate ellipse tilt slightly per-blob for a less mechanical look
        blob.ellipseRatio = 0.55f + 0.25f * std::sin(static_cast<float>(i) * 1.7f);

        // Base visual size grows slightly with radius (outer dust = broader)
        blob.baseSize = 0.10f + t * 0.18f;

        // Cycle through frequency bands so every band drives at least
        // one or two blobs
        blob.bandIndex = i % DvisualizerAudioProcessor::numBands;

        // Spread hues evenly around the wheel, offset by band index too
        blob.hueOffset = std::fmod(t + static_cast<float>(blob.bandIndex) * 0.07f, 1.0f);
    }
}

//==============================================================================
// ── Star initialisation ───────────────────────────────────────────────────
void GalaxyComponent::initStars()
{
    // Background stars — scattered uniformly, idle twinkle only
    for (int i = 0; i < numBackgroundStars; ++i)
    {
        auto& star = stars[i];
        star.pos = { nextRand(), nextRand() };
        star.baseSize = 0.4f + nextRand() * 0.9f;   // 0.4 .. 1.3 px
        star.twinklePhase = nextRand() * juce::MathConstants<float>::twoPi;
        star.fftBin = -1;
        star.reactive = false;
    }

    // Reactive stars — slightly larger, each tied to one FFT display bin
    for (int i = 0; i < numReactiveStars; ++i)
    {
        auto& star = stars[numBackgroundStars + i];
        star.pos = { nextRand(), nextRand() };
        star.baseSize = 0.8f + nextRand() * 1.4f;   // 0.8 .. 2.2 px
        star.twinklePhase = nextRand() * juce::MathConstants<float>::twoPi;

        // Spread reactive stars across the full FFT display range
        star.fftBin = static_cast<int>(
            (static_cast<float>(i) / static_cast<float>(numReactiveStars))
            * static_cast<float>(fftDisplaySize - 1));
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

void GalaxyComponent::updateFFT()
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

    std::fill(fftData.begin() + DvisualizerAudioProcessor::fftSize,
        fftData.end(), 0.0f);

    window.multiplyWithWindowingTable(fftData.data(),
        DvisualizerAudioProcessor::fftSize);
    forwardFFT.performFrequencyOnlyForwardTransform(fftData.data());

    const float sr = static_cast<float>(processor.currentSampleRate);
    const float freqPerBin = sr / static_cast<float>(DvisualizerAudioProcessor::fftSize);
    const int   nyquist = DvisualizerAudioProcessor::fftSize / 2;
    const float minFreq = 20.0f;
    const float maxFreq = juce::jmax(minFreq + 1.0f, sr * 0.5f);

    for (int i = 0; i < fftDisplaySize; ++i)
    {
        const float p = static_cast<float>(i) / static_cast<float>(fftDisplaySize - 1);
        const float freq = minFreq * std::pow(maxFreq / minFreq, p);
        const int   bin = juce::jlimit(1, nyquist - 1,
            static_cast<int>(freq / freqPerBin));

        const float dBVal = juce::Decibels::gainToDecibels(fftData[bin])
            - juce::Decibels::gainToDecibels(
                static_cast<float>(DvisualizerAudioProcessor::fftSize));

        const float newVal = juce::jmap(juce::jlimit(mindB, maxdB, dBVal),
            mindB, maxdB, 0.0f, 1.0f);

        // Fast attack, slow release — stars pop quickly then fade gently
        const float coeff = (newVal > fftMag[i]) ? 0.55f : 0.90f;
        fftMag[i] = coeff * fftMag[i] + (1.0f - coeff) * newVal;
    }
}

//==============================================================================
// ── Timer ──────────────────────────────────────────────────────────────────
void GalaxyComponent::timerCallback()
{
    updateAudioData();
    updateFFT();
    updateDustBlobs();

    idleTime += 1.0f / 60.0f;

    repaint();
}

//==============================================================================
// ── Dust blob motion ───────────────────────────────────────────────────────
void GalaxyComponent::updateDustBlobs()
{
    for (auto& blob : dustBlobs)
    {
        // Orbital speed gets a gentle boost from the blob's own band energy —
        // louder content makes that arm of the galaxy "spin up" slightly,
        // reinforcing the sense that motion is driven by the music.
        const float energyBoost = 1.0f + bands[blob.bandIndex] * 0.6f;
        blob.angle += blob.angularSpeed * energyBoost;

        if (blob.angle > juce::MathConstants<float>::twoPi)
            blob.angle -= juce::MathConstants<float>::twoPi;
    }
}

//==============================================================================
// ── Dust blob rendering ────────────────────────────────────────────────────
void GalaxyComponent::drawDustBlobs(juce::Graphics& g, juce::Point<float> centre, float minDim)
{
    for (const auto& blob : dustBlobs)
    {
        const float energy = bands[blob.bandIndex];   // 0..1

        // Position on the elliptical orbit
        const float orbitR = blob.radius * minDim;
        const float x = centre.x + std::cos(blob.angle) * orbitR;
        const float y = centre.y + std::sin(blob.angle) * orbitR * blob.ellipseRatio;

        // Size pulses with amplitude — louder content = bigger, brighter cloud
        const float size = (blob.baseSize * minDim) * (0.7f + energy * 0.9f);

        // Hue derives from the blob's assigned frequency band plus its
        // static offset, so different frequency content produces visibly
        // different dust colours across the galaxy.
        const float hue = std::fmod(blob.hueOffset + energy * 0.12f, 1.0f);

        // Intensity (alpha) scales with amplitude — quiet bands fade toward
        // invisible, loud bands glow brightly.
        const float alpha = juce::jlimit(0.04f, 0.55f, 0.06f + energy * 0.5f);

        const juce::Colour core = juce::Colour::fromHSV(hue, 0.85f, 1.0f, alpha);
        const juce::Colour edge = juce::Colour::fromHSV(hue, 0.85f, 1.0f, 0.0f);

        juce::ColourGradient gradient(core, x, y, edge, x + size, y, true /* radial */);
        // Add a mid-stop so the falloff feels like a soft gas cloud rather
        // than a hard circle
        gradient.addColour(0.5, core.withAlpha(alpha * 0.5f));

        g.setGradientFill(gradient);
        g.fillEllipse(x - size, y - size, size * 2.0f, size * 2.0f);
    }
}

//==============================================================================
// ── Star rendering ─────────────────────────────────────────────────────────
void GalaxyComponent::drawStars(juce::Graphics& g)
{
    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());

    for (const auto& star : stars)
    {
        const float x = star.pos.x * w;
        const float y = star.pos.y * h;

        float brightness;
        float size = star.baseSize;

        if (star.reactive)
        {
            // Reactive stars: brightness tracks the FFT magnitude at their
            // assigned bin. They sit on a low idle floor so the field never
            // looks completely dead during quiet passages.
            const float mag = fftMag[juce::jlimit(0, fftDisplaySize - 1, star.fftBin)];
            brightness = juce::jlimit(0.15f, 1.0f, 0.15f + mag * 1.3f);
            size *= (0.6f + mag * 1.8f);
        }
        else
        {
            // Background stars: gentle idle shimmer using a sine wave —
            // no audio reactivity, just ambient sparkle.
            const float twinkle = 0.5f + 0.5f * std::sin(idleTime * 1.5f + star.twinklePhase);
            brightness = juce::jlimit(0.2f, 0.9f, 0.3f + twinkle * 0.6f);
        }

        // All stars tinted with the user's chosen primary colour
        g.setColour(primaryColour.withAlpha(brightness));
        g.fillEllipse(x - size, y - size, size * 2.0f, size * 2.0f);
    }
}

//==============================================================================
// ── paint() — composites dust trail + fresh starfield ────────────────────────
void GalaxyComponent::paint(juce::Graphics& g)
{
    if (!dustCanvas.isValid())
        return;

    const float w = static_cast<float>(getWidth());
    const float h = static_cast<float>(getHeight());
    const float minDim = juce::jmin(w, h);
    const juce::Point<float> centre{ w * 0.5f, h * 0.5f };

    // ── Nebula dust layer (heavy persistence) ─────────────────────────────
    // Fade the previous frame very slightly toward the background colour,
    // then paint this frame's dust blobs on top. The low fill alpha is what
    // produces the long, slow-fading spiral-arm trails.
    {
        juce::Graphics gc(dustCanvas);

        gc.setColour(bgColour.withAlpha(dustTrailAlpha));
        gc.fillRect(0, 0, dustCanvas.getWidth(), dustCanvas.getHeight());

        drawDustBlobs(gc, centre, minDim);
    }

    // Blit the accumulated dust canvas to the screen
    g.drawImage(dustCanvas, 0, 0, getWidth(), getHeight(),
        0, 0, dustCanvas.getWidth(), dustCanvas.getHeight());

    // ── Starfield (no trail — drawn fresh on top every frame) ──────────────
    drawStars(g);
}