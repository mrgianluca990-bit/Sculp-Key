#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace sculptkey {

struct Sample {
    std::vector<float> left, right;
    double sampleRate = 44100.0;
    int rootNote = 60;
    int velocityLayer = 0;
    int roundRobin = 0;
};

// Piano -> Rhodes -> Hammond -> Synth. All four states run for every logical
// voice, including inaudible layers and held notes whose piano sample has ended.
// setSamples/prepare are non-realtime; MIDI and render never allocate or lock.
class KeyEngine {
public:
    KeyEngine() { prepare(44100.0); }

    void setSamples(std::vector<Sample> samples) {
        allSoundOff();
        samples.erase(std::remove_if(samples.begin(), samples.end(), [](const Sample& s) {
            if (s.left.size() < 2 || (!s.right.empty() && s.right.size() != s.left.size())
                || !std::isfinite(s.sampleRate) || s.sampleRate < 1000.0 || s.sampleRate > 768000.0
                || s.rootNote < 0 || s.rootNote > 127 || s.velocityLayer < 0 || s.velocityLayer > 2)
                return true;
            bool nonzero = false;
            for (float x : s.left) {
                if (!std::isfinite(x)) return true;
                nonzero = nonzero || std::abs(x) > 1.0e-9f;
            }
            for (float x : s.right) {
                if (!std::isfinite(x)) return true;
                nonzero = nonzero || std::abs(x) > 1.0e-9f;
            }
            return !nonzero;
        }), samples.end());
        samples_ = std::move(samples);
        for (int note = 0; note < 128; ++note)
            for (int layer = 0; layer < 3; ++layer) buildChoices(note, layer);
        reset();
    }

    void prepare(double rate) {
        sampleRate_ = std::isfinite(rate) && rate >= 8000.0 && rate <= 768000.0 ? rate : 44100.0;
        for (std::size_t i = 0; i <= tableSize_; ++i) {
            const double fraction = static_cast<double>(i) / static_cast<double>(tableSize_);
            sine_[i] = static_cast<float>(std::sin(2.0 * pi_ * fraction));
            filterSteps_[i] = static_cast<float>(1.0 - std::exp(-2.0 * pi_ * 0.45 * fraction));
        }
        parameterStep_ = coefficient(0.015);
        bendStep_ = coefficient(0.004);
        normalAttackSamples_ = std::max(1, static_cast<int>(sampleRate_ * 0.0012));
        synthAttackSamples_ = std::max(1, static_cast<int>(sampleRate_ * 0.005));
        stealSamples_ = std::max(1, static_cast<int>(sampleRate_ * 0.005));
        pianoRelease_ = releaseCoefficient(0.22);
        epRelease_ = releaseCoefficient(0.28);
        organRelease_ = releaseCoefficient(0.075);
        synthRelease_ = releaseCoefficient(0.25);
        synthDecay_ = releaseCoefficient(0.35);
        hammerDecay_ = 1.0f - coefficient(0.006);
        clickDecay_ = 1.0f - coefficient(0.003);
        percussionDecay_ = 1.0f - coefficient(0.12);
        triangleLeak_ = static_cast<float>(std::exp(-2.0 * pi_ * 0.3 / sampleRate_));
        detuneUp_ = std::exp2(2.5 / 1200.0);
        detuneDown_ = 1.0 / detuneUp_;
        for (std::size_t i = 0; i < notes_.size(); ++i) {
            auto& n = notes_[i];
            n.hz = 440.0 * std::exp2((static_cast<double>(i) - 69.0) / 12.0);
            const double epTime = std::clamp(4.5 * std::pow(220.0 / n.hz, 0.28), 1.5, 8.0);
            const double bellTime = std::clamp(0.8 * std::pow(220.0 / n.hz, 0.20), 0.25, 1.5);
            n.epDecay = static_cast<float>(std::exp(-1.0 / (epTime * sampleRate_)));
            n.bellDecay = static_cast<float>(std::exp(-1.0 / (bellTime * sampleRate_)));
            n.barkDecay = static_cast<float>(std::exp(-1.0 / (0.32 * sampleRate_)));
        }
        reset();
    }

    void reset() {
        allSoundOff();
        for (auto& c : channels_) c = Channel{};
        for (auto& c : noteOnSequence_) c.fill(0);
        for (auto& c : noteOffSequence_) c.fill(0);
        rrCounters_.fill(0);
        lastRoundRobin_ = -1;
        order_ = frame_ = 0;
        tremoloPhase_ = rotaryPhase_ = 0.0;
        morphCurrent_ = morphTarget_;
        gainCurrent_ = gainTarget_;
    }

    bool hasSamples() const { return !samples_.empty(); }
    void setMorph(float value) {
        if (std::isfinite(value)) morphTarget_ = std::clamp(value, 0.0f, 1.0f);
    }
    void setOutputGain(float value) {
        if (std::isfinite(value)) gainTarget_ = std::clamp(value, 0.0f, 4.0f);
    }

    void noteOn(int note, float velocity, int channel = 1) {
        if (!valid(note, channel) || !std::isfinite(velocity)) return;
        if (velocity <= 0.0f) { noteOff(note, channel); return; }
        if (!hasSamples()) return;
        velocity = std::min(velocity, 1.0f);
        const int ch = channel - 1;
        const int variation = static_cast<int>(rrCounters_[static_cast<std::size_t>(note)]++ % 4);
        lastRoundRobin_ = variation;
        Voice* slot = nullptr;
        for (auto& voice : voices_) if (!voice.active) { slot = &voice; break; }
        bool stealing = false;
        if (slot == nullptr) {
            slot = &voices_[0];
            for (auto& voice : voices_) if (stealBefore(voice, *slot)) slot = &voice;
            saveTail(*slot);
            stealing = true;
        }
        *slot = Voice{};
        auto& v = *slot;
        v.active = v.keyDown = true;
        v.channel = ch;
        v.note = note;
        v.order = ++order_;
        v.keySequence = ++noteOnSequence_[static_cast<std::size_t>(ch)][static_cast<std::size_t>(note)];
        v.velocity = velocity;
        v.velocityGain = std::pow(velocity, 0.80f);
        v.organVelocity = 0.25f + 0.75f * velocity;
        v.variation = variation;
        v.attackRemaining = stealing ? stealSamples_ : normalAttackSamples_;
        v.synthAttackRemaining = synthAttackSamples_;
        v.frequency = notes_[static_cast<std::size_t>(note)].hz;
        v.epDecay = notes_[static_cast<std::size_t>(note)].epDecay;
        v.bellDecay = notes_[static_cast<std::size_t>(note)].bellDecay;
        v.barkDecay = notes_[static_cast<std::size_t>(note)].barkDecay;
        static constexpr std::array<float, 4> rrGain{1.0f, 0.992f, 1.006f, 0.997f};
        static constexpr std::array<float, 4> rrTone{1.0f, 0.965f, 1.025f, 0.985f};
        v.rrGain = rrGain[static_cast<std::size_t>(variation)];
        v.pianoToneStep = filterStep(12500.0f * rrTone[static_cast<std::size_t>(variation)]);
        v.epToneStep = filterStep(static_cast<float>(1800.0 + 4.0 * v.frequency)
            + 4000.0f * velocity * velocity);
        v.jazzToneStep = filterStep(static_cast<float>(1600.0 + 3.5 * v.frequency)
            + 2200.0f * velocity * velocity);
        v.jazzBark = smoothstep((velocity - 0.68f) / 0.32f);
        v.jazzAttackRemaining = std::max(1, static_cast<int>(sampleRate_
            * (0.0050 - 0.0025 * static_cast<double>(velocity))));
        v.epPhase = 0.004 * static_cast<double>(variation);
        v.bellPhase = 0.037 * static_cast<double>(variation);
        v.organPhase = wrap(static_cast<double>(frame_) * v.frequency * 0.5 / sampleRate_);
        v.sawA = 0.07 + 0.19 * static_cast<double>(variation);
        v.sawB = wrap(v.sawA + 0.17);
        v.subPhase = 0.0;
        v.triangle = -1.0f;
        v.noise = 0x9e3779b9u ^ (static_cast<std::uint32_t>(note + 1) * 0x85ebca6bu)
            ^ (static_cast<std::uint32_t>(variation + 1) * 0xc2b2ae35u)
            ^ static_cast<std::uint32_t>(channel * 97);

        int lower = 0, upper = 0;
        float upperWeight = 0.0f;
        const float midiVelocity = velocity * 127.0f;
        if (midiVelocity < 42.0f) lower = upper = 0;
        else if (midiVelocity < 58.0f) {
            lower = 0; upper = 1; upperWeight = smoothstep((midiVelocity - 42.0f) / 16.0f);
        } else if (midiVelocity < 77.0f) lower = upper = 1;
        else if (midiVelocity < 93.0f) {
            lower = 1; upper = 2; upperWeight = smoothstep((midiVelocity - 77.0f) / 16.0f);
        } else lower = upper = 2;
        initialisePlayhead(v.piano[0], note, lower, variation, 1.0f - upperWeight);
        if (upper != lower && upperWeight > 0.0f)
            initialisePlayhead(v.piano[1], note, upper, variation, upperWeight);
    }

    void noteOff(int note, int channel = 1) {
        if (!valid(note, channel)) return;
        const auto ch = static_cast<std::size_t>(channel - 1);
        const auto key = static_cast<std::size_t>(note);
        auto& off = noteOffSequence_[ch][key];
        if (off >= noteOnSequence_[ch][key]) return;
        const auto sequence = ++off;
        // Sequence numbers preserve FIFO even if an older instance was stolen.
        for (auto& v : voices_) {
            if (v.active && v.channel == channel - 1 && v.note == note && v.keySequence == sequence) {
                v.keyDown = false;
                if (channels_[ch].sustain) v.pedalHeld = true;
                else beginRelease(v);
                break;
            }
        }
    }

    void pitchBend(float semitones, int channel = 1) {
        if (channel < 1 || channel > 16 || !std::isfinite(semitones)) return;
        channels_[static_cast<std::size_t>(channel - 1)].bendTarget = static_cast<float>(
            std::exp2(static_cast<double>(std::clamp(semitones, -48.0f, 48.0f)) / 12.0));
    }

    void sustainPedal(bool down, int channel = 1) {
        if (channel < 1 || channel > 16) return;
        channels_[static_cast<std::size_t>(channel - 1)].sustain = down;
        if (!down) for (auto& v : voices_)
            if (v.active && v.channel == channel - 1 && !v.keyDown && v.pedalHeld) beginRelease(v);
    }

    void allNotesOff(int channel = 0) {
        if (channel < 0 || channel > 16) return;
        for (auto& v : voices_) if (v.active && matches(v.channel, channel)) {
            v.keyDown = false;
            if (channels_[static_cast<std::size_t>(v.channel)].sustain) v.pedalHeld = true;
            else beginRelease(v);
        }
        consumePendingNotes(channel);
    }

    void allSoundOff(int channel = 0) {
        if (channel < 0 || channel > 16) return;
        for (auto& v : voices_) if (matches(v.channel, channel)) v = Voice{};
        for (auto& v : tails_) if (matches(v.channel, channel)) v = Voice{};
        consumePendingNotes(channel);
        peak_ = 0.0f;
    }

    void resetControllers(int channel = 1) {
        if (channel < 0 || channel > 16) return;
        for (int ch = 1; ch <= 16; ++ch) if (channel == 0 || channel == ch) {
            sustainPedal(false, ch);
            channels_[static_cast<std::size_t>(ch - 1)].bendTarget = 1.0f;
        }
    }

    void render(float* left, float* right, int count) {
        peak_ = 0.0f;
        for (int i = 0; i < count; ++i) {
            smooth(morphCurrent_, morphTarget_, parameterStep_);
            smooth(gainCurrent_, gainTarget_, parameterStep_);
            for (auto& ch : channels_) smooth(ch.bendCurrent, ch.bendTarget, bendStep_);
            const auto weights = morphWeights();
            Bus bus;
            for (auto& voice : voices_) if (voice.active) renderVoice(voice, bus);
            for (auto& voice : tails_) if (voice.active) renderVoice(voice, bus);
            const float tremolo = sine(tremoloPhase_);
            const float rotor = sine(rotaryPhase_);
            const float rotorQuadrature = sine(rotaryPhase_ + 0.25);
            // Subtle fixed stereo motion, with no delayed signal left after panic.
            const float epL = bus.ep * (1.0f + 0.065f * tremolo);
            const float epR = bus.ep * (1.0f - 0.065f * tremolo);
            const float organL = bus.organ * (0.96f + 0.055f * rotor) + bus.organUpper * 0.025f * rotorQuadrature;
            const float organR = bus.organ * (0.96f - 0.055f * rotor) - bus.organUpper * 0.025f * rotorQuadrature;
            float l = (weights[0] * pianoGain_ * bus.pianoL + weights[1] * rhodesGain_ * epL
                + weights[2] * organGain_ * organL + weights[3] * synthGain_ * bus.synthL) * gainCurrent_;
            float r = (weights[0] * pianoGain_ * bus.pianoR + weights[1] * rhodesGain_ * epR
                + weights[2] * organGain_ * organR + weights[3] * synthGain_ * bus.synthR) * gainCurrent_;
            l = safetyKnee(l); r = safetyKnee(r);
            if (left != nullptr) left[i] = l;
            if (right != nullptr) right[i] = r;
            peak_ = std::max(peak_, std::max(std::abs(l), std::abs(r)));
            tremoloPhase_ = wrap(tremoloPhase_ + 4.7 / sampleRate_);
            rotaryPhase_ = wrap(rotaryPhase_ + 4.2 / sampleRate_);
            ++frame_;
        }
    }

    float peak() const { return peak_; }
    int lastRoundRobin() const { return lastRoundRobin_; }
    int getActiveVoiceCount() const {
        int result = 0;
        for (const auto& v : voices_) result += v.active ? 1 : 0;
        for (const auto& v : tails_) result += v.active ? 1 : 0;
        return result;
    }

private:
    static constexpr double pi_ = 3.1415926535897932384626433832795;
    static constexpr std::size_t tableSize_ = 2048;
    static constexpr float pianoGain_ = 0.55f;
    static constexpr float rhodesGain_ = 0.085f;
    static constexpr float organGain_ = 0.070f;
    static constexpr float synthGain_ = 0.240f;
    struct Channel { bool sustain = false; float bendCurrent = 1.0f, bendTarget = 1.0f; };
    struct NoteData { double hz = 440.0; float epDecay = 1.0f, bellDecay = 1.0f, barkDecay = 1.0f; };
    struct Choices { std::array<int, 16> indices{}; std::size_t count = 0; };
    struct Playhead { int sample = -1; double position = 0.0, increment = 1.0; float weight = 0.0f; };
    struct Voice {
        bool active = false, keyDown = false, pedalHeld = false, releasing = false;
        int channel = 0, note = 60, variation = 0, attackRemaining = 0, synthAttackRemaining = 0;
        int tailRemaining = 0;
        int jazzAttackRemaining = 0;
        std::uint64_t order = 0, keySequence = 0;
        std::uint32_t noise = 1;
        std::array<Playhead, 2> piano{};
        double frequency = 440.0, epPhase = 0.0, bellPhase = 0.0, organPhase = 0.0;
        double sawA = 0.0, sawB = 0.17, subPhase = 0.0;
        float velocity = 0.0f, velocityGain = 0.0f, organVelocity = 0.0f, rrGain = 1.0f;
        float attack = 0.0f, pianoGate = 1.0f, epGate = 1.0f, organGate = 1.0f, synthEnvelope = 0.0f;
        float epBody = 1.0f, epBell = 1.0f, epBark = 1.0f, hammer = 1.0f, click = 1.0f, percussion = 1.0f;
        float epDecay = 1.0f, bellDecay = 1.0f, barkDecay = 1.0f;
        float pianoToneStep = 1.0f, epToneStep = 1.0f;
        float pianoToneL = 0.0f, pianoToneR = 0.0f, epTone = 0.0f;
        float jazzToneStep = 1.0f, jazzTone = 0.0f, jazzAttack = 0.0f, jazzBark = 0.0f;
        float epPrevious = 0.0f, organPrevious = 0.0f;
        float filterL1 = 0.0f, filterL2 = 0.0f, filterR1 = 0.0f, filterR2 = 0.0f;
        float triangle = -1.0f, energy = 0.0f;
    };
    struct Bus { float pianoL = 0.0f, pianoR = 0.0f, ep = 0.0f, organ = 0.0f, organUpper = 0.0f, synthL = 0.0f, synthR = 0.0f; };

    static bool valid(int note, int channel) { return note >= 0 && note < 128 && channel >= 1 && channel <= 16; }
    static bool matches(int internalChannel, int channel) { return channel == 0 || internalChannel == channel - 1; }
    static double wrap(double x) { return x - std::floor(x); }
    static float smoothstep(float x) { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }
    static void smooth(float& value, float target, float step) {
        const float difference = target - value;
        value = std::abs(difference) < 1.0e-8f ? target : value + step * difference;
    }
    static void decay(float& value, float coefficientValue) {
        value *= coefficientValue;
        if (value < 1.0e-8f) value = 0.0f;
    }
    float coefficient(double seconds) const { return static_cast<float>(1.0 - std::exp(-1.0 / (seconds * sampleRate_))); }
    float releaseCoefficient(double seconds) const { return static_cast<float>(std::exp(-6.907755278982137 / (seconds * sampleRate_))); }
    float sine(double phase) const {
        const double position = wrap(phase) * static_cast<double>(tableSize_);
        const auto index = static_cast<std::size_t>(position);
        const float fraction = static_cast<float>(position - static_cast<double>(index));
        return sine_[index] + (sine_[index + 1] - sine_[index]) * fraction;
    }
    float filterStep(float hz) const {
        const float position = std::clamp(hz / static_cast<float>(sampleRate_ * 0.45), 0.0f, 1.0f)
            * static_cast<float>(tableSize_);
        const auto index = static_cast<std::size_t>(position);
        const auto next = std::min(index + 1, tableSize_);
        return filterSteps_[index] + (filterSteps_[next] - filterSteps_[index])
            * (position - static_cast<float>(index));
    }
    float partialGain(double hz) const {
        return smoothstep(static_cast<float>((sampleRate_ * 0.45 - hz) / (sampleRate_ * 0.05)));
    }
    std::array<float, 4> morphWeights() const {
        std::array<float, 4> weights{};
        const float position = std::clamp(morphCurrent_, 0.0f, 1.0f) * 3.0f;
        const int segment = std::min(2, static_cast<int>(position));
        const float t = smoothstep(position - static_cast<float>(segment));
        weights[static_cast<std::size_t>(segment)] = sine(0.25 + static_cast<double>(t) * 0.25);
        weights[static_cast<std::size_t>(segment + 1)] = sine(static_cast<double>(t) * 0.25);
        return weights;
    }
    static float safetyKnee(float x) {
        if (!std::isfinite(x)) return 0.0f;
        const float absolute = std::abs(x);
        if (absolute <= 0.90f) return x;
        const float excess = (absolute - 0.90f) * 10.0f;
        return std::copysign(0.90f + 0.10f * (excess / (1.0f + excess)), x);
    }
    static float antialiasedPickup(float x, float& previous) {
        const float y = (x + previous)
            / (std::sqrt(1.0f + x * x) + std::sqrt(1.0f + previous * previous));
        previous = x;
        return y;
    }
    static float blep(double phase, double step) {
        if (phase < step) { const double t = phase / step; return static_cast<float>(t + t - t * t - 1.0); }
        if (phase > 1.0 - step) { const double t = (phase - 1.0) / step; return static_cast<float>(t * t + t + t + 1.0); }
        return 0.0f;
    }
    static float saw(double phase, double step) { return static_cast<float>(2.0 * phase - 1.0) - blep(phase, step); }
    static float noise(Voice& v) {
        v.noise ^= v.noise << 13; v.noise ^= v.noise >> 17; v.noise ^= v.noise << 5;
        return static_cast<float>(v.noise >> 8) * (1.0f / 8388608.0f) - 1.0f;
    }
    void consumePendingNotes(int channel) {
        for (int ch = 0; ch < 16; ++ch) if (matches(ch, channel))
            noteOffSequence_[static_cast<std::size_t>(ch)] = noteOnSequence_[static_cast<std::size_t>(ch)];
    }
    static void beginRelease(Voice& v) { v.keyDown = false; v.pedalHeld = false; v.releasing = true; }
    static bool stealBefore(const Voice& a, const Voice& b) {
        const int aClass = a.releasing ? 0 : (a.keyDown ? 2 : 1);
        const int bClass = b.releasing ? 0 : (b.keyDown ? 2 : 1);
        if (aClass != bClass) return aClass < bClass;
        if (std::abs(a.energy - b.energy) > 1.0e-6f) return a.energy < b.energy;
        return a.order < b.order;
    }
    void saveTail(const Voice& source) {
        if (!source.active || source.attack <= 0.0f) return;
        auto* tail = &tails_[0];
        for (auto& candidate : tails_) {
            if (!candidate.active) { tail = &candidate; break; }
            if (candidate.energy * static_cast<float>(candidate.tailRemaining)
                < tail->energy * static_cast<float>(tail->tailRemaining)) tail = &candidate;
        }
        *tail = source;
        beginRelease(*tail);
        tail->tailRemaining = stealSamples_;
    }
    void buildChoices(int note, int layer) {
        auto& choices = choices_[static_cast<std::size_t>(note)][static_cast<std::size_t>(layer)];
        choices.count = 0;
        int bestRoot = -1, bestLayer = -1, bestScore = std::numeric_limits<int>::max();
        for (const auto& s : samples_) {
            const int score = std::abs(s.velocityLayer - layer) * 256 + std::abs(s.rootNote - note);
            if (score < bestScore || (score == bestScore && s.rootNote < bestRoot)) {
                bestRoot = s.rootNote; bestLayer = s.velocityLayer; bestScore = score;
            }
        }
        for (std::size_t i = 0; i < samples_.size() && choices.count < choices.indices.size(); ++i) {
            const auto& s = samples_[i];
            if (s.rootNote == bestRoot && s.velocityLayer == bestLayer) choices.indices[choices.count++] = static_cast<int>(i);
        }
        std::sort(choices.indices.begin(), choices.indices.begin() + static_cast<std::ptrdiff_t>(choices.count),
            [this](int a, int b) { return samples_[static_cast<std::size_t>(a)].roundRobin < samples_[static_cast<std::size_t>(b)].roundRobin; });
    }
    void initialisePlayhead(Playhead& p, int note, int layer, int variation, float weight) {
        const auto& choices = choices_[static_cast<std::size_t>(note)][static_cast<std::size_t>(layer)];
        if (choices.count == 0) return;
        p.sample = choices.indices[static_cast<std::size_t>(variation) % choices.count];
        const auto& sample = samples_[static_cast<std::size_t>(p.sample)];
        p.increment = sample.sampleRate / sampleRate_
            * notes_[static_cast<std::size_t>(note)].hz / notes_[static_cast<std::size_t>(sample.rootNote)].hz;
        p.weight = weight;
    }
    static float interpolateSample(const std::vector<float>& audio, std::size_t i, float t) {
        const float a = audio[i == 0 ? 0 : i - 1];
        const float b = audio[i], c = audio[i + 1], d = audio[std::min(i + 2, audio.size() - 1)];
        const float c1 = 0.5f * (c - a);
        const float c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * t + c2) * t + c1) * t + b;
    }
    void renderPiano(Voice& v, double bend, float& left, float& right) {
        left = right = 0.0f;
        for (auto& p : v.piano) {
            if (p.sample < 0) continue;
            const auto& sample = samples_[static_cast<std::size_t>(p.sample)];
            if (p.position >= static_cast<double>(sample.left.size() - 1)) continue;
            const auto index = static_cast<std::size_t>(p.position);
            const float fraction = static_cast<float>(p.position - static_cast<double>(index));
            const double increment = p.increment * bend;
            const double remaining = (static_cast<double>(sample.left.size() - 1) - p.position)
                / std::max(1.0e-12, increment);
            const float endFade = static_cast<float>(std::min(1.0, remaining / (sampleRate_ * 0.003)));
            left += interpolateSample(sample.left, index, fraction) * p.weight * endFade;
            const auto& rightData = sample.right.empty() ? sample.left : sample.right;
            right += interpolateSample(rightData, index, fraction) * p.weight * endFade;
            p.position += increment;
        }
        smooth(v.pianoToneL, left, v.pianoToneStep);
        smooth(v.pianoToneR, right, v.pianoToneStep);
        left = v.pianoToneL * v.pianoGate * v.velocityGain * v.rrGain;
        right = v.pianoToneR * v.pianoGate * v.velocityGain * v.rrGain;
    }

    void renderVoice(Voice& v, Bus& bus) {
        float tailGain = 1.0f;
        if (v.tailRemaining > 0) {
            tailGain = static_cast<float>(v.tailRemaining) / static_cast<float>(stealSamples_);
            if (--v.tailRemaining == 0) { v.active = false; return; }
        }
        if (v.attackRemaining > 0) {
            v.attack += (1.0f - v.attack) / static_cast<float>(v.attackRemaining);
            --v.attackRemaining;
        }
        if (v.releasing) {
            decay(v.pianoGate, pianoRelease_); decay(v.epGate, epRelease_);
            decay(v.organGate, organRelease_); decay(v.synthEnvelope, synthRelease_);
            if (std::max({v.pianoGate, v.epGate, v.organGate, v.synthEnvelope}) < 1.0e-6f) {
                v.active = false; return;
            }
        } else if (v.synthAttackRemaining > 0) {
            v.synthEnvelope += (1.0f - v.synthEnvelope) / static_cast<float>(v.synthAttackRemaining);
            --v.synthAttackRemaining;
        } else v.synthEnvelope = 0.5f + (v.synthEnvelope - 0.5f) * synthDecay_;

        const double bend = channels_[static_cast<std::size_t>(v.channel)].bendCurrent;
        const double hz = std::min(sampleRate_ * 0.42, v.frequency * bend);
        const double step = hz / sampleRate_;
        const float n = noise(v);
        float pL = 0.0f, pR = 0.0f;
        renderPiano(v, bend, pL, pR);

        // A modal tine/steel-bar model: decaying body, velocity-dependent pickup
        // harmonics and an inharmonic bell near 6.8*f0; no FM preset substitution.
        const float s1 = sine(v.epPhase);
        const float s2 = sine(v.epPhase * 2.0) * partialGain(hz * 2.0);
        const float s3 = sine(v.epPhase * 3.0) * partialGain(hz * 3.0);
        const float hard = v.velocity * v.velocity;
        const float epBody = (0.78f * s1 + (0.10f + 0.15f * hard) * s2
            + (0.035f + 0.13f * hard) * s3 * v.epBark) * v.epBody;
        const float pickupDrive = 1.0f + 1.5f * hard * partialGain(hz * 10.0);
        const float pickup = antialiasedPickup(epBody * pickupDrive, v.epPrevious) / (0.85f + 0.30f * hard);
        const float tine = sine(v.bellPhase) * partialGain(hz * 6.8)
            + 0.22f * sine(v.bellPhase * 2.0) * partialGain(hz * 13.6) * v.epBark;
        const float epRaw = 0.75f * epBody + 0.25f * pickup
            + (0.07f + 0.21f * hard) * tine * v.epBell + 0.014f * n * v.hammer;
        smooth(v.epTone, epRaw, v.epToneStep);
        const float ep = v.epTone * v.epGate * v.velocityGain * v.rrGain;

        // Jazz voicing: the body carries the note; the tine is a brief accent.
        // Pickup bark enters progressively only above a firm key strike. Reuse
        // the existing modal waves/pickup, without another nonlinear processor.
        const float jazzBody = (0.86f * s1 + (0.08f + 0.08f * hard) * s2
            + (0.018f + 0.075f * v.jazzBark) * s3 * v.epBark) * v.epBody;
        const float jazzRaw = 0.94f * jazzBody + (0.035f + 0.10f * v.jazzBark) * pickup
            + (0.018f + 0.045f * hard) * tine * v.epBell * v.epBell
            + 0.004f * n * v.hammer;
        smooth(v.jazzTone, jazzRaw, v.jazzToneStep);
        if (v.jazzAttackRemaining > 0) {
            v.jazzAttack += (1.0f - v.jazzAttack) / static_cast<float>(v.jazzAttackRemaining);
            --v.jazzAttackRemaining;
        }
        const float jazzEp = 0.96f * v.jazzTone * v.jazzAttack
            * v.epGate * v.velocityGain * v.rrGain;
        v.epPhase = wrap(v.epPhase + step);
        v.bellPhase = wrap(v.bellPhase + step * 6.8);
        decay(v.epBody, v.epDecay); decay(v.epBell, v.bellDecay); decay(v.epBark, v.barkDecay);
        decay(v.hammer, hammerDecay_);

        // Rock drawbars: fuller 16', 5 1/3', 4' and upper-mid components.
        // Both voicings share the same nine band-limited tonewheel partials.
        static constexpr std::array<int, 9> ratios{1, 3, 2, 4, 6, 8, 10, 12, 16};
        static constexpr std::array<float, 9> levels{0.08f, 0.07f, 0.80f, 0.22f, 0.12f, 0.10f, 0.045f, 0.030f, 0.025f};
        static constexpr std::array<float, 9> rockLevels{0.30f, 0.30f, 0.78f, 0.44f, 0.27f, 0.18f, 0.08f, 0.06f, 0.045f};
        float organ = 0.0f, organUpper = 0.0f;
        float rockOrgan = 0.0f, rockUpper = 0.0f;
        for (std::size_t h = 0; h < ratios.size(); ++h) {
            const double ratio = static_cast<double>(ratios[h]);
            const float wave = sine(v.organPhase * ratio);
            const float band = partialGain(hz * ratio * 0.5);
            const float partial = levels[h] * wave * band;
            const float rockPartial = rockLevels[h] * wave * band;
            organ += partial;
            if (h >= 3) organUpper += partial;
            rockOrgan += rockPartial;
            if (h >= 3) rockUpper += rockPartial;
        }
        const float percussionWave = sine(v.organPhase * 6.0);
        const float percussionBand = partialGain(hz * 3.0);
        organ += 0.14f * percussionWave * percussionBand * v.percussion;
        organ += 0.018f * n * v.click;
        const float drivenOrgan = antialiasedPickup(organ * 1.25f, v.organPrevious);
        rockOrgan += 0.23f * percussionWave * percussionBand * v.percussion;
        rockOrgan += 0.026f * n * v.click;
        // A small share of the existing antialiased pickup adds controlled grit;
        // stronger drawbars provide the rock character without a new fuzz stage.
        rockOrgan = 0.82f * (0.85f * rockOrgan + 0.15f * drivenOrgan)
            * v.organGate * v.organVelocity;
        rockUpper *= (0.82f * 0.85f) * v.organGate * v.organVelocity;
        organ = (0.78f * organ + 0.22f * drivenOrgan) * v.organGate * v.organVelocity;
        organUpper *= v.organGate * v.organVelocity;
        v.organPhase = wrap(v.organPhase + step * 0.5);
        decay(v.click, clickDecay_); decay(v.percussion, percussionDecay_);

        // Two independently running polyBLEP saws, five cents apart, with a
        // bandlimited-square integrator supplying the quieter octave triangle.
        const double upStep = std::min(0.45, step * detuneUp_);
        const double downStep = std::min(0.45, step * detuneDown_);
        const float a = saw(v.sawA, upStep), b = saw(v.sawB, downStep);
        const double subStep = step * 0.5;
        const float square = (v.subPhase < 0.5 ? 1.0f : -1.0f)
            + blep(v.subPhase, subStep) - blep(wrap(v.subPhase + 0.5), subStep);
        v.triangle = v.triangle * triangleLeak_ + static_cast<float>(4.0 * subStep) * square;
        const float synthL = 0.45f * a + 0.30f * b + 0.20f * v.triangle;
        const float synthR = 0.30f * a + 0.45f * b + 0.20f * v.triangle;
        const float cutoff = static_cast<float>(hz) * (2.2f + 8.0f * v.synthEnvelope + 5.0f * v.velocity) + 300.0f;
        const float filter = filterStep(cutoff);
        smooth(v.filterL1, synthL, filter); smooth(v.filterL2, v.filterL1, filter);
        smooth(v.filterR1, synthR, filter); smooth(v.filterR2, v.filterR1, filter);
        const float sL = v.filterL2 * v.synthEnvelope * v.velocityGain;
        const float sR = v.filterR2 * v.synthEnvelope * v.velocityGain;
        v.sawA = wrap(v.sawA + upStep); v.sawB = wrap(v.sawB + downStep);
        v.subPhase = wrap(v.subPhase + subStep);

        const float gain = v.attack * tailGain;
        bus.pianoL += pL * gain; bus.pianoR += pR * gain;
        bus.ep += jazzEp * gain;
        // Keep the original numerical leakage at the exact Synth endpoint:
        // sin(pi) in the unchanged morph table is tiny but not identically zero.
        const bool synthEndpoint = morphCurrent_ == 1.0f;
        bus.organ += (synthEndpoint ? organ : rockOrgan) * gain;
        bus.organUpper += (synthEndpoint ? organUpper : rockUpper) * gain;
        bus.synthL += sL * gain; bus.synthR += sR * gain;
        // Retain the preceding allocation reference so changing an inaudible
        // Rhodes/organ cannot choose different piano/synth voices for stealing.
        // Oscillators, envelopes and nonlinear pickup work are shared above.
        const float energy = std::max({std::abs(pL) * pianoGain_, std::abs(pR) * pianoGain_,
            std::abs(ep) * rhodesGain_, std::abs(organ) * organGain_,
            std::abs(sL) * synthGain_, std::abs(sR) * synthGain_}) * gain;
        v.energy += 0.01f * (energy - v.energy);
    }

    std::vector<Sample> samples_;
    std::array<std::array<Choices, 3>, 128> choices_{};
    std::array<Voice, 48> voices_{};
    std::array<Voice, 8> tails_{};
    std::array<Channel, 16> channels_{};
    std::array<NoteData, 128> notes_{};
    std::array<std::array<std::uint64_t, 128>, 16> noteOnSequence_{}, noteOffSequence_{};
    std::array<std::uint32_t, 128> rrCounters_{};
    std::array<float, tableSize_ + 1> sine_{}, filterSteps_{};
    double sampleRate_ = 44100.0, detuneUp_ = 1.0, detuneDown_ = 1.0;
    double tremoloPhase_ = 0.0, rotaryPhase_ = 0.0;
    std::uint64_t order_ = 0, frame_ = 0;
    float morphTarget_ = 0.0f, morphCurrent_ = 0.0f, gainTarget_ = 1.0f, gainCurrent_ = 1.0f;
    float parameterStep_ = 0.01f, bendStep_ = 0.01f;
    float pianoRelease_ = 0.99f, epRelease_ = 0.99f, organRelease_ = 0.99f, synthRelease_ = 0.99f;
    float synthDecay_ = 0.99f, hammerDecay_ = 0.99f, clickDecay_ = 0.99f, percussionDecay_ = 0.99f;
    float triangleLeak_ = 0.99999f, peak_ = 0.0f;
    int normalAttackSamples_ = 64, synthAttackSamples_ = 220, stealSamples_ = 220;
    int lastRoundRobin_ = -1;
};

} // namespace sculptkey
