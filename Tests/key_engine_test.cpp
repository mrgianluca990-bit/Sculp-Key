#include "../Source/KeyEngine.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
std::atomic<bool> trackAllocations { false };
std::atomic<std::size_t> allocationCount { 0 };

void countAllocation() noexcept {
    if (trackAllocations.load(std::memory_order_relaxed))
        allocationCount.fetch_add(1, std::memory_order_relaxed);
}

// Keep the malloc-backed hooks out of GCC's inliner to avoid a false
// mismatched-new-delete warning in this test harness.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
void freeTestAllocation(void* memory) noexcept { std::free(memory); }
}

void* operator new(std::size_t bytes) {
    countAllocation();
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory) noexcept { freeTestAllocation(memory); }
void operator delete(void* memory, std::size_t) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, std::size_t) noexcept { freeTestAllocation(memory); }

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    countAllocation();
    void* memory = nullptr;
    const auto boundary = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    if (posix_memalign(&memory, boundary, bytes == 0 ? 1 : bytes) == 0) return memory;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return ::operator new(bytes, alignment);
}
void operator delete(void* memory, std::align_val_t) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { freeTestAllocation(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept { freeTestAllocation(memory); }

void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes); } catch (...) { return nullptr; }
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { freeTestAllocation(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { freeTestAllocation(memory); }
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new(bytes, alignment); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return ::operator new[](bytes, alignment); } catch (...) { return nullptr; }
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    freeTestAllocation(memory);
}
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
    freeTestAllocation(memory);
}

namespace {
using sculptkey::KeyEngine;
using sculptkey::Sample;
constexpr double sampleRate = 48000.0;
constexpr double pi = 3.14159265358979323846;
constexpr float organMorph = 2.0f / 3.0f;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

double frequency(int note) { return 440.0 * std::exp2((static_cast<double>(note) - 69.0) / 12.0); }

// Deliberately artificial stereo bells. These fixtures are test data only,
// not an alternative to the real piano bank shipped with the instrument.
Sample fixture(int root, int layer, double seconds = 0.7, double sourceRate = 44100.0) {
    Sample sample;
    sample.sampleRate = sourceRate;
    sample.rootNote = root;
    sample.velocityLayer = layer;
    sample.roundRobin = 0;
    const auto frames = static_cast<std::size_t>(seconds * sourceRate);
    sample.left.resize(frames);
    sample.right.resize(frames);
    const double level = 0.10 + 0.09 * static_cast<double>(layer);
    const double omega = 2.0 * pi * frequency(root);
    for (std::size_t i = 0; i < frames; ++i) {
        const double time = static_cast<double>(i) / sourceRate;
        const double envelope = (1.0 - std::exp(-time / 0.001)) * std::exp(-time / 2.5);
        sample.left[i] = static_cast<float>(level * envelope
            * (std::sin(omega * time) + 0.10 * std::sin(2.71 * omega * time)));
        sample.right[i] = static_cast<float>(level * envelope
            * (0.62 * std::sin(omega * time + 0.31) + 0.06 * std::sin(2.39 * omega * time)));
    }
    return sample;
}

std::vector<Sample> bank(double seconds = 0.7) {
    std::vector<Sample> result;
    for (int root : { 60, 64, 67 })
        for (int layer = 0; layer < 3; ++layer)
            result.push_back(fixture(root, layer, seconds));
    return result;
}

void setup(KeyEngine& engine, float morph = 0.0f, double rate = sampleRate, double duration = 0.7) {
    engine.setSamples(bank(duration));
    engine.setMorph(morph);
    engine.setOutputGain(1.0f);
    engine.prepare(rate);
    engine.reset();
    require(engine.hasSamples(), "Valid stereo bank was rejected");
}

struct Audio {
    std::vector<float> left, right;
    explicit Audio(std::size_t count) : left(count, 7.0f), right(count, -7.0f) {}
};

void checkFinite(const Audio& audio) {
    require(audio.left.size() == audio.right.size(), "Invalid test audio buffer");
    for (std::size_t i = 0; i < audio.left.size(); ++i)
        require(std::isfinite(audio.left[i]) && std::isfinite(audio.right[i]),
            "Audio contains NaN or infinity");
}

Audio render(KeyEngine& engine, int frames) {
    Audio audio(static_cast<std::size_t>(frames));
    engine.render(audio.left.data(), audio.right.data(), frames);
    checkFinite(audio);
    require(std::isfinite(engine.peak()), "Peak meter is not finite");
    return audio;
}

void advance(KeyEngine& engine, int frames) {
    std::array<float, 512> left {}, right {};
    while (frames > 0) {
        const int count = std::min(frames, static_cast<int>(left.size()));
        engine.render(left.data(), right.data(), count);
        for (int i = 0; i < count; ++i)
            require(std::isfinite(left[static_cast<std::size_t>(i)])
                    && std::isfinite(right[static_cast<std::size_t>(i)]),
                "Advancing the engine produced non-finite audio");
        frames -= count;
    }
}

void waitForVoiceCount(KeyEngine& engine, int expected, double rate = sampleRate) {
    int remaining = static_cast<int>(3.0 * rate);
    while (engine.getActiveVoiceCount() != expected && remaining > 0) {
        const int count = std::min(remaining, 512);
        advance(engine, count);
        remaining -= count;
    }
    require(engine.getActiveVoiceCount() == expected,
        "Expected " + std::to_string(expected) + " voices after release, got "
            + std::to_string(engine.getActiveVoiceCount()));
}

double rms(const std::vector<float>& signal, std::size_t start = 0) {
    require(start < signal.size(), "Invalid RMS window");
    double energy = 0.0;
    for (std::size_t i = start; i < signal.size(); ++i)
        energy += static_cast<double>(signal[i]) * signal[i];
    return std::sqrt(energy / static_cast<double>(signal.size() - start));
}

double differenceRms(const std::vector<float>& a, const std::vector<float>& b) {
    require(a.size() == b.size() && !a.empty(), "Invalid difference buffers");
    double energy = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double delta = static_cast<double>(a[i]) - b[i];
        energy += delta * delta;
    }
    return std::sqrt(energy / static_cast<double>(a.size()));
}

double stereoRms(const Audio& audio) {
    const double left = rms(audio.left), right = rms(audio.right);
    return std::sqrt(0.5 * (left * left + right * right));
}

double maximumDifference(const Audio& a, const Audio& b) {
    require(a.left.size() == b.left.size(), "Invalid comparison buffers");
    double result = 0.0;
    for (std::size_t i = 0; i < a.left.size(); ++i) {
        result = std::max(result, std::abs(static_cast<double>(a.left[i]) - b.left[i]));
        result = std::max(result, std::abs(static_cast<double>(a.right[i]) - b.right[i]));
    }
    return result;
}

double spectralAmplitude(const std::vector<float>& signal, double hz,
                         double rate = sampleRate, std::size_t start = 0) {
    require(start + 2 < signal.size(), "Invalid spectrum window");
    const auto count = signal.size() - start;
    double real = 0.0, imaginary = 0.0, weight = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        const double window = 0.5 - 0.5 * std::cos(2.0 * pi * static_cast<double>(i)
            / static_cast<double>(count - 1));
        const double phase = 2.0 * pi * hz * static_cast<double>(i) / rate;
        real += signal[start + i] * window * std::cos(phase);
        imaginary -= signal[start + i] * window * std::sin(phase);
        weight += window;
    }
    return 2.0 * std::hypot(real, imaginary) / weight;
}

void requireSilent(const Audio& audio, const char* message) {
    require(std::all_of(audio.left.begin(), audio.left.end(), [](float value) { return value == 0.0f; })
            && std::all_of(audio.right.begin(), audio.right.end(), [](float value) { return value == 0.0f; }),
        message);
}

void testEmptyMalformedAndInvalidMidi() {
    KeyEngine engine;
    engine.prepare(sampleRate);
    require(!engine.hasSamples(), "Empty engine reports a sample bank");
    for (float morph : { 0.0f, 1.0f / 3.0f, organMorph, 1.0f }) {
        engine.setMorph(morph);
        engine.noteOn(60, 0.8f);
        requireSilent(render(engine, 512), "An empty bank produced placeholder audio");
    }

    auto malformed = bank(0.03);
    malformed[0].left.clear();
    malformed[0].right.clear();
    malformed[1].sampleRate = std::numeric_limits<double>::quiet_NaN();
    malformed[2].sampleRate = 0.0;
    malformed[3].rootNote = -1;
    malformed[4].rootNote = 128;
    malformed[5].left[5] = std::numeric_limits<float>::quiet_NaN();
    malformed[6].right[6] = std::numeric_limits<float>::infinity();
    malformed[7].right.resize(3);
    malformed[8].left.resize(1);
    malformed[8].right.resize(1);
    engine.setSamples(std::move(malformed));
    engine.setMorph(0.3f);
    engine.noteOn(64, 1.0f);
    render(engine, 4096); // Invalid entries may be rejected or sanitised, never read unsafely.
    engine.allSoundOff();
    requireSilent(render(engine, 32), "Malformed-bank panic left audio");

    setup(engine);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    engine.noteOn(-1, 0.8f);
    engine.noteOn(128, 0.8f);
    engine.noteOn(60, 0.8f, 0);
    engine.noteOn(60, 0.8f, 17);
    engine.noteOn(60, nan);
    engine.noteOn(60, infinity);
    engine.noteOff(-1);
    engine.noteOff(128);
    engine.setMorph(nan);
    engine.setOutputGain(nan);
    engine.pitchBend(nan);
    require(engine.getActiveVoiceCount() == 0, "Invalid MIDI created a voice");
    requireSilent(render(engine, 128), "Invalid controls produced audio without a note");
    engine.noteOn(60, 0.7f);
    require(rms(render(engine, 4096).left) > 1.0e-5, "Valid note failed after invalid input");

    for (double rate : { 0.0, -48000.0, std::numeric_limits<double>::quiet_NaN() }) {
        engine.prepare(rate);
        engine.noteOn(60, 0.7f);
        render(engine, 128);
        engine.allSoundOff();
    }
}

void testStereoChordsAndVelocity() {
    KeyEngine engine;
    setup(engine);
    engine.noteOn(60, 0.8f);
    const auto stereo = render(engine, 8192);
    require(rms(stereo.left, 1000) > 1.0e-4 && rms(stereo.right, 1000) > 1.0e-4,
        "A stereo piano channel is silent");
    require(differenceRms(stereo.left, stereo.right) > 0.01 * rms(stereo.left),
        "The stereo sample was collapsed to dual mono");

    engine.reset();
    for (int note : { 60, 64, 67 }) engine.noteOn(note, 0.65f);
    require(engine.getActiveVoiceCount() == 3, "A chord did not create three independent voices");
    const auto chord = render(engine, 16384);
    for (int note : { 60, 64, 67 })
        require(spectralAmplitude(chord.left, frequency(note), sampleRate, 1500) > 1.0e-4,
            "A chord note is missing from the audio");
    engine.noteOff(64);
    waitForVoiceCount(engine, 2);
    engine.allNotesOff();
    waitForVoiceCount(engine, 0);

    double previousLevel = 0.0;
    for (float velocity : { 0.15f, 0.5f, 0.95f }) {
        engine.reset();
        engine.noteOn(60, velocity);
        const double level = rms(render(engine, 8192).left, 1500);
        require(level > previousLevel * 1.1, "Piano velocity does not increase the played dynamics");
        previousLevel = level;
    }
}

void testSustainPerChannel() {
    KeyEngine engine;
    setup(engine, organMorph);
    engine.sustainPedal(true, 1);
    engine.noteOn(60, 0.7f, 1);
    engine.noteOn(64, 0.7f, 2);
    advance(engine, 1024);
    engine.noteOff(60, 1);
    engine.noteOff(64, 2);
    waitForVoiceCount(engine, 1);
    require(rms(render(engine, 4096).left) > 1.0e-5, "Sustained channel lost its sound");
    engine.sustainPedal(false, 2);
    require(engine.getActiveVoiceCount() == 1, "The other channel's pedal released a sustained voice");
    engine.sustainPedal(false, 1);
    waitForVoiceCount(engine, 0);
}

void testRepeatedNoteFifo() {
    KeyEngine engine;
    setup(engine, organMorph);
    engine.noteOn(60, 0.08f);
    advance(engine, 1024);
    engine.noteOn(60, 0.95f);
    advance(engine, 1024);
    require(engine.getActiveVoiceCount() == 2, "Repeated Note On replaced the older voice");
    engine.noteOff(60);
    waitForVoiceCount(engine, 1);
    advance(engine, 48000);
    const double remaining = stereoRms(render(engine, 4096));

    KeyEngine quietReference, loudReference;
    setup(quietReference, organMorph);
    setup(loudReference, organMorph);
    quietReference.noteOn(60, 0.08f);
    loudReference.noteOn(60, 0.95f);
    advance(quietReference, 48000);
    advance(loudReference, 48000);
    const double quiet = stereoRms(render(quietReference, 4096));
    const double loud = stereoRms(render(loudReference, 4096));
    require(loud > quiet && quiet > 1.0e-8, "FIFO reference voices are not distinguishable");
    require(remaining > std::sqrt(quiet * loud),
        "Repeated Note Off did not release the oldest instance (FIFO)");
    engine.noteOff(60);
    waitForVoiceCount(engine, 0);
    engine.noteOff(60); // An unmatched extra Note Off must remain harmless.
    requireSilent(render(engine, 64), "An extra Note Off resurrected a repeated voice");
}

void testAllNotesOffVersusAllSoundOff() {
    KeyEngine engine;
    setup(engine, organMorph);
    engine.sustainPedal(true, 1);
    engine.noteOn(60, 0.75f, 1);
    engine.noteOn(67, 0.7f, 2);
    advance(engine, 2048);
    engine.allNotesOff(1);
    advance(engine, 48000);
    require(engine.getActiveVoiceCount() == 2, "CC123 ignored sustain or affected another channel");

    engine.allSoundOff(1);
    require(engine.getActiveVoiceCount() == 1, "CC120 was not immediate and channel-specific");
    require(rms(render(engine, 2048).left) > 1.0e-5, "CC120 silenced the other channel");
    engine.allSoundOff();
    require(engine.getActiveVoiceCount() == 0, "Global CC120 left active voices");
    requireSilent(render(engine, 512), "CC120 must silence the very next sample");
    require(engine.peak() == 0.0f, "Meter remained active after CC120");

    engine.sustainPedal(false, 1);
    engine.noteOn(60, 0.7f, 1);
    advance(engine, 1024);
    engine.allNotesOff();
    waitForVoiceCount(engine, 0);
}

void testResetControllers() {
    KeyEngine engine;
    setup(engine, organMorph);
    for (int channel : { 1, 2 }) {
        engine.sustainPedal(true, channel);
        engine.pitchBend(2.0f, channel);
        engine.noteOn(channel == 1 ? 60 : 67, 0.75f, channel);
    }
    advance(engine, 2048);
    engine.noteOff(60, 1);
    engine.noteOff(67, 2);
    engine.resetControllers(1);
    waitForVoiceCount(engine, 1);
    const auto remaining = render(engine, 16384);
    require(spectralAmplitude(remaining.left, frequency(69)) > 1.0e-5,
        "Reset Controllers reset the bend on another channel");
    engine.resetControllers(2);
    waitForVoiceCount(engine, 0);

    setup(engine);
    engine.pitchBend(2.0f, 1);
    engine.noteOn(60, 0.7f, 1);
    advance(engine, 1024);
    engine.resetControllers(1);
    const auto unbent = render(engine, 16384);
    require(spectralAmplitude(unbent.left, frequency(60), sampleRate, 2000)
            > 5.0 * spectralAmplitude(unbent.left, frequency(62), sampleRate, 2000),
        "Reset Controllers did not restore the active note's original pitch");
}

void testPitchBendPerChannel() {
    KeyEngine reference, bent;
    setup(reference);
    setup(bent);
    for (auto* engine : { &reference, &bent }) {
        engine->noteOn(60, 0.7f, 1);
        engine->noteOn(67, 0.65f, 2);
    }
    bent.pitchBend(2.0f, 1);
    const auto original = render(reference, 16384);
    const auto changed = render(bent, 16384);
    const auto component = [](const Audio& audio, int note) {
        return spectralAmplitude(audio.left, frequency(note), sampleRate, 2000);
    };
    require(component(changed, 62) > 5.0 * component(changed, 60),
        "Pitch bend did not retune channel 1 by two semitones");
    const double otherRatio = component(changed, 67) / component(original, 67);
    require(otherRatio > 0.85 && otherRatio < 1.15,
        "Pitch bend altered the unbent channel");
}

void testHeldMorphAfterSampleEnd() {
    KeyEngine engine;
    setup(engine, 0.0f, sampleRate, 0.12);
    engine.noteOn(60, 0.8f);
    require(rms(render(engine, 4096).left) > 1.0e-5, "The short piano fixture did not sound");
    advance(engine, 24000);
    const auto expired = render(engine, 1024);
    require(rms(expired.left) < 1.0e-6 && rms(expired.right) < 1.0e-6,
        "The ended piano sample unexpectedly loops");
    require(engine.getActiveVoiceCount() == 1, "A held key was deleted when its piano sample ended");
    const int rr = engine.lastRoundRobin();
    engine.setMorph(organMorph);
    advance(engine, 4800);
    require(rms(render(engine, 4096).left) > 1.0e-4,
        "Morph to Hammond failed after the held piano sample ended");
    engine.setMorph(1.0f);
    advance(engine, 4800);
    require(rms(render(engine, 4096).left) > 1.0e-4,
        "Morph to synth failed on the same held key");
    require(engine.lastRoundRobin() == rr && engine.getActiveVoiceCount() == 1,
        "Changing morph retriggered or duplicated the held note");
    engine.noteOff(60);
    waitForVoiceCount(engine, 0);
}

void testVoiceStealingAndTails() {
    KeyEngine engine;
    setup(engine, organMorph);
    for (int voice = 0; voice < 48; ++voice)
        engine.noteOn(48 + voice % 24, 0.55f, 1 + voice / 24);
    advance(engine, 2048);
    require(engine.getActiveVoiceCount() == 48, "The engine did not provide 48 held voices");
    engine.noteOn(77, 0.7f, 3);
    require(engine.getActiveVoiceCount() > 48 && engine.getActiveVoiceCount() <= 56,
        "Voice stealing did not preserve a bounded release tail");
    for (int voice = 0; voice < 40; ++voice) {
        engine.noteOn(48 + voice % 24, 0.6f, 3 + voice % 3);
        render(engine, 16);
        require(engine.getActiveVoiceCount() <= 56, "Voice-stealing tails exceeded their bound");
    }
    advance(engine, 4800);
    require(engine.getActiveVoiceCount() == 48, "Stolen-voice tails did not finish");
    engine.allNotesOff();
    waitForVoiceCount(engine, 0);
    requireSilent(render(engine, 64), "Voice stealing left stale notes after release");
}

void testRoundRobinAndDeterministicReset() {
    KeyEngine engine;
    setup(engine);
    std::vector<Audio> takes;
    takes.reserve(8);
    for (int hit = 0; hit < 8; ++hit) {
        engine.allSoundOff();
        engine.noteOn(60, 0.7f);
        require(engine.lastRoundRobin() == hit % 4, "The four modelled RR variations did not cycle");
        takes.push_back(render(engine, 4096));
    }
    bool audibleVariation = false;
    for (std::size_t i = 1; i < 4; ++i)
        audibleVariation = audibleVariation || differenceRms(takes[0].left, takes[i].left) > 1.0e-7;
    require(audibleVariation, "The RR index changes but the rendered attacks are identical");
    engine.reset();
    engine.noteOn(60, 0.7f);
    require(engine.lastRoundRobin() == 0, "Reset did not restart the RR cycle");
    const auto repeated = render(engine, 4096);
    require(maximumDifference(takes[0], repeated) < 1.0e-7,
        "Reset did not reproduce the same initial instrument state");
}

enum class EventKind { noteOn, noteOff, pedal, bend, morph, gain, notesOff, soundOff, controllers };
struct Event { int frame; EventKind kind; int note; int channel; float value; };
const std::array<Event, 22> events {{
    {0, EventKind::noteOn, 60, 1, 0.7f},
    {17, EventKind::noteOn, 64, 2, 0.5f},
    {133, EventKind::morph, 0, 0, 0.61f},
    {1027, EventKind::pedal, 0, 1, 1.0f},
    {1973, EventKind::noteOff, 60, 1, 0.0f},
    {2053, EventKind::noteOn, 67, 1, 0.8f},
    {4099, EventKind::bend, 0, 2, 1.3f},
    {6203, EventKind::morph, 0, 0, 1.0f},
    {7001, EventKind::gain, 0, 0, 0.57f},
    {8137, EventKind::noteOn, 67, 1, 0.3f},
    {9001, EventKind::noteOff, 67, 1, 0.0f},
    {10003, EventKind::notesOff, 0, 1, 0.0f},
    {11209, EventKind::pedal, 0, 1, 0.0f},
    {12803, EventKind::controllers, 0, 2, 0.0f},
    {13579, EventKind::noteOff, 64, 2, 0.0f},
    {16013, EventKind::soundOff, 0, 2, 0.0f},
    {18029, EventKind::morph, 0, 0, organMorph},
    {18029, EventKind::noteOn, 62, 16, 0.7f},
    {19511, EventKind::bend, 0, 16, -0.7f},
    {21001, EventKind::gain, 0, 0, 0.9f},
    {23003, EventKind::notesOff, 0, 0, 0.0f},
    {47001, EventKind::soundOff, 0, 0, 0.0f}
}};

void applyEvent(KeyEngine& engine, const Event& event) {
    switch (event.kind) {
        case EventKind::noteOn: engine.noteOn(event.note, event.value, event.channel); break;
        case EventKind::noteOff: engine.noteOff(event.note, event.channel); break;
        case EventKind::pedal: engine.sustainPedal(event.value != 0.0f, event.channel); break;
        case EventKind::bend: engine.pitchBend(event.value, event.channel); break;
        case EventKind::morph: engine.setMorph(event.value); break;
        case EventKind::gain: engine.setOutputGain(event.value); break;
        case EventKind::notesOff: engine.allNotesOff(event.channel); break;
        case EventKind::soundOff: engine.allSoundOff(event.channel); break;
        case EventKind::controllers: engine.resetControllers(event.channel); break;
    }
}

Audio renderSequence(KeyEngine& engine, const std::vector<int>& blockSizes) {
    constexpr int totalFrames = 48000;
    Audio audio(totalFrames);
    int frame = 0;
    std::size_t nextEvent = 0, nextBlock = 0;
    while (frame < totalFrames) {
        while (nextEvent < events.size() && events[nextEvent].frame == frame)
            applyEvent(engine, events[nextEvent++]);
        const int boundary = nextEvent < events.size() ? events[nextEvent].frame : totalFrames;
        const int count = std::min({ blockSizes[nextBlock++ % blockSizes.size()],
                                     boundary - frame, totalFrames - frame });
        require(count > 0, "Invalid event scheduling in the test harness");
        engine.render(audio.left.data() + frame, audio.right.data() + frame, count);
        frame += count;
    }
    checkFinite(audio);
    return audio;
}

void testBlockInvariance() {
    KeyEngine a, b;
    setup(a, 0.2f);
    setup(b, 0.2f);
    const auto reference = renderSequence(a, { 257 });
    const auto fragmented = renderSequence(b, { 1, 3, 64, 511, 7, 1024 });
    require(maximumDifference(reference, fragmented) < 2.0e-6,
        "The same sample-timed events depend on the host block partition");

    a.setMorph(0.2f);
    a.setOutputGain(1.0f);
    a.reset();
    const auto repeated = renderSequence(a, { 113, 5, 1024 });
    require(maximumDifference(reference, repeated) < 2.0e-6,
        "Reset did not reproduce a complete multichannel performance");
}

void testSampleRatesAndPitch() {
    for (double rate : { 44100.0, 48000.0, 96000.0 }) {
        KeyEngine engine;
        setup(engine, 0.0f, rate);
        engine.noteOn(60, 0.75f);
        const auto audio = render(engine, static_cast<int>(rate * 0.3));
        const auto start = static_cast<std::size_t>(rate * 0.04);
        const double correct = spectralAmplitude(audio.left, frequency(60), rate, start);
        const double wrong = spectralAmplitude(audio.left, frequency(61), rate, start);
        require(correct > 10.0 * wrong && correct > 1.0e-4,
            "Sample-rate conversion changed the piano pitch");
        for (float morph : { 1.0f / 3.0f, organMorph, 1.0f }) {
            engine.setMorph(morph);
            advance(engine, static_cast<int>(rate * 0.08));
            require(rms(render(engine, static_cast<int>(rate * 0.04)).left) > 1.0e-5,
                "A morph anchor is silent at a supported sample rate");
        }
        engine.noteOff(60);
        waitForVoiceCount(engine, 0, rate);
    }
}

void testOutputGainAndMeter() {
    KeyEngine full, half;
    setup(full, organMorph);
    setup(half, organMorph);
    half.setOutputGain(0.5f);
    full.noteOn(60, 0.75f);
    half.noteOn(60, 0.75f);
    const auto fullAudio = render(full, 16384);
    const auto halfAudio = render(half, 16384);
    const double ratio = rms(halfAudio.left, 6000) / rms(fullAudio.left, 6000);
    require(ratio > 0.49 && ratio < 0.51, "Output gain is not a linear amplitude control");
    float actualPeak = 0.0f;
    for (std::size_t i = 0; i < halfAudio.left.size(); ++i)
        actualPeak = std::max({ actualPeak, std::abs(halfAudio.left[i]), std::abs(halfAudio.right[i]) });
    require(std::abs(half.peak() - actualPeak) < 1.0e-5f, "Peak meter does not describe the rendered block");
    half.setOutputGain(0.0f);
    const auto muted = render(half, 24000);
    require(rms(muted.left, 12000) < 1.0e-7 && rms(muted.right, 12000) < 1.0e-7,
        "Zero output gain did not silence a held organ voice");
    half.setOutputGain(1.0f);
    require(rms(render(half, 8192).left, 4000) > 1.0e-5, "Restoring gain lost the held note");
}

struct AllocationScope {
    AllocationScope() {
        allocationCount.store(0, std::memory_order_relaxed);
        trackAllocations.store(true, std::memory_order_relaxed);
    }
    ~AllocationScope() { trackAllocations.store(false, std::memory_order_relaxed); }
};

void testRealtimeAllocations() {
    KeyEngine engine;
    setup(engine, 0.8f);
    std::array<float, 256> left {}, right {};
    {
        AllocationScope scope;
        for (int step = 0; step < 160; ++step) {
            const int channel = 1 + step % 16;
            engine.noteOn(48 + step % 36, 0.75f, channel);
            if (step % 11 == 0) engine.sustainPedal(true, channel);
            if (step % 13 == 0) engine.sustainPedal(false, channel);
            if (step % 29 == 0) engine.allNotesOff(channel);
            if (step % 31 == 0) engine.resetControllers(channel);
            engine.setMorph(static_cast<float>(step % 101) / 100.0f);
            engine.setOutputGain(0.8f);
            engine.pitchBend(static_cast<float>(step % 5) - 2.0f, channel);
            engine.render(left.data(), right.data(), static_cast<int>(left.size()));
            if (step % 3 == 0) engine.noteOff(48 + step % 36, channel);
        }
        engine.allNotesOff();
        engine.allSoundOff();
        engine.reset();
        engine.render(left.data(), right.data(), static_cast<int>(left.size()));
    }
    require(allocationCount.load(std::memory_order_relaxed) == 0,
        "The MIDI/render path allocated C++ heap memory after setup");
    require(engine.getActiveVoiceCount() == 0, "Realtime panic/reset left active voices");
}

void reportCpu() {
    KeyEngine engine;
    setup(engine, 0.85f);
    for (int voice = 0; voice < 48; ++voice)
        engine.noteOn(48 + voice % 24, 0.6f, 1 + voice / 24);
    advance(engine, 512);
    std::array<float, 256> left {}, right {};
    constexpr int blocks = 188;
    volatile double checksum = 0.0;
    const auto wallStart = std::chrono::steady_clock::now();
    const auto cpuStart = std::clock();
    for (int block = 0; block < blocks; ++block) {
        engine.render(left.data(), right.data(), static_cast<int>(left.size()));
        checksum += left[0] + right.back();
    }
    const double cpu = static_cast<double>(std::clock() - cpuStart) / CLOCKS_PER_SEC;
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - wallStart).count();
    const double audioSeconds = static_cast<double>(blocks * static_cast<int>(left.size())) / sampleRate;
    require(std::isfinite(checksum), "CPU run produced non-finite audio");
    std::cout << std::fixed << std::setprecision(4)
              << "CPU observation: 48 held voices, morph 85%, 48 kHz, blocks 256; "
              << audioSeconds << " s audio / " << cpu << " s process CPU / " << wall
              << " s wall; CPU/audio " << 100.0 * cpu / audioSeconds << "%\n";
    std::cout << "CPU figures describe this machine and run; no fixed performance threshold is imposed.\n";
}
}

int main() {
    struct Test { const char* name; void (*run)(); };
    const std::array<Test, 14> tests {{
        { "empty/malformed bank and invalid input", testEmptyMalformedAndInvalidMidi },
        { "real stereo, chords and velocity", testStereoChordsAndVelocity },
        { "sustain per MIDI channel", testSustainPerChannel },
        { "repeated-note FIFO", testRepeatedNoteFifo },
        { "CC123 versus immediate CC120", testAllNotesOffVersusAllSoundOff },
        { "Reset Controllers per channel", testResetControllers },
        { "pitch bend per MIDI channel", testPitchBendPerChannel },
        { "held morph after sample end", testHeldMorphAfterSampleEnd },
        { "48 voices and bounded stealing tails", testVoiceStealingAndTails },
        { "four modelled RR variations and reset", testRoundRobinAndDeterministicReset },
        { "sample-timed event/block invariance", testBlockInvariance },
        { "44.1/48/96 kHz and sample pitch", testSampleRatesAndPitch },
        { "output gain and peak metering", testOutputGainAndMeter },
        { "no C++ heap allocation in MIDI/render", testRealtimeAllocations }
    }};
    int failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "PASS: " << test.name << '\n';
        } catch (const std::exception& error) {
            trackAllocations.store(false, std::memory_order_relaxed);
            ++failures;
            std::cerr << "FAIL: " << test.name << ": " << error.what() << '\n';
        }
    }
    if (failures == 0) {
        try { reportCpu(); }
        catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL: CPU observation: " << error.what() << '\n';
        }
    }
    std::cout << (failures == 0 ? "PASS" : "FAIL") << ": " << tests.size()
              << " core behaviour checks, " << failures << " failure(s).\n";
    return failures == 0 ? 0 : 1;
}
