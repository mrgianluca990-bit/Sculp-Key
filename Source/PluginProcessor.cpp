#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "KeySampleData.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
const char* findBundledResource (const juce::String& fileName, int& size)
{
    // Resolve original filenames instead of guessing JUCE's C++ identifier rules.
    const auto basename = fileName.fromLastOccurrenceOf ("/", false, false);
    for (int i = 0; i < KeySamples::namedResourceListSize; ++i)
    {
        if (basename == KeySamples::originalFilenames[i])
            return KeySamples::getNamedResource (KeySamples::namedResourceList[i], size);
    }

    size = 0;
    return nullptr;
}
}

juce::AudioProcessorValueTreeState::ParameterLayout SculptKeyProcessor::createLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout parameters;
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "morph", 1 }, "Morph",
        NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("%")));
    parameters.add (std::make_unique<AudioParameterFloat> (
        ParameterID { "output", 1 }, "Output",
        NormalisableRange<float> (-60.0f, 6.0f, 0.1f), 0.0f,
        AudioParameterFloatAttributes().withLabel ("dB")));
    return parameters;
}

SculptKeyProcessor::SculptKeyProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "SCULPT_KEY_STATE", createLayout())
{
    morphParameter = apvts.getRawParameterValue ("morph");
    outputParameter = apvts.getRawParameterValue ("output");
    loadBundledSamples();
}

void SculptKeyProcessor::loadBundledSamples()
{
    // All decoding and allocation happens before playback. The engine's bank is
    // immutable afterwards; the audio callback never loads files or takes a lock.
    int manifestSize = 0;
    const auto* manifestData = findBundledResource ("manifest.json", manifestSize);
    if (manifestData == nullptr || manifestSize <= 0)
    {
        sampleMessage = "Libreria campioni mancante";
        return;
    }

    juce::var manifest;
    const auto parsed = juce::JSON::parse (
        juce::String::fromUTF8 (manifestData, manifestSize), manifest);
    const auto sampleFiles = manifest.getProperty ("sampleFiles", {});
    const auto* entries = sampleFiles.getArray();
    if (parsed.failed() || entries == nullptr || entries->isEmpty())
    {
        sampleMessage = "Manifest della libreria non valido";
        return;
    }

    juce::WavAudioFormat wav;
    std::vector<sculptkey::Sample> samples;
    samples.reserve (static_cast<std::size_t> (entries->size()));
    constexpr int modelledRoundRobinCount = 4;

    for (const auto& entry : *entries)
    {
        if (! entry.hasProperty ("file") || ! entry.hasProperty ("rootNote")
            || ! entry.hasProperty ("velocityLayer") || ! entry.hasProperty ("roundRobin"))
        {
            sampleMessage = "Mappatura campioni incompleta";
            return;
        }

        const auto filename = entry["file"].toString();
        int dataSize = 0;
        const auto* data = findBundledResource (filename, dataSize);
        if (data == nullptr || dataSize <= 0)
        {
            sampleMessage = "Campione mancante: " + filename;
            return;
        }

        std::unique_ptr<juce::AudioFormatReader> reader (
            wav.createReaderFor (new juce::MemoryInputStream (
                data, static_cast<std::size_t> (dataSize), false), true));
        if (reader == nullptr || reader->lengthInSamples <= 0
            || reader->lengthInSamples > 20000000 || reader->numChannels == 0
            || ! std::isfinite (reader->sampleRate) || reader->sampleRate <= 0.0)
        {
            sampleMessage = "Campione non valido: " + filename;
            return;
        }

        const int length = static_cast<int> (reader->lengthInSamples);
        juce::AudioBuffer<float> decoded (static_cast<int> (reader->numChannels), length);
        if (! reader->read (&decoded, 0, length, 0, true, true))
        {
            sampleMessage = "Lettura campione fallita: " + filename;
            return;
        }

        sculptkey::Sample sample;
        sample.rootNote = static_cast<int> (entry["rootNote"]);
        sample.velocityLayer = static_cast<int> (entry["velocityLayer"]);
        sample.roundRobin = static_cast<int> (entry["roundRobin"]);
        sample.sampleRate = reader->sampleRate;
        if (sample.rootNote < 0 || sample.rootNote > 127
            || sample.velocityLayer < 0 || sample.velocityLayer > 127
            || sample.roundRobin < 0 || sample.roundRobin > 127)
        {
            sampleMessage = "Valori di mappatura non validi: " + filename;
            return;
        }

        const auto* left = decoded.getReadPointer (0);
        const auto* right = decoded.getNumChannels() > 1 ? decoded.getReadPointer (1) : left;
        sample.left.assign (left, left + length);
        sample.right.assign (right, right + length);
        samples.push_back (std::move (sample));
    }

    engine.setSamples (std::move (samples));
    samplesReady = engine.hasSamples();
    roundRobinCount = modelledRoundRobinCount;
    sampleMessage = samplesReady ? juce::String() : juce::String ("Libreria campioni vuota");
}

bool SculptKeyProcessor::isBusesLayoutSupported (const BusesLayout& layout) const
{
    return layout.getMainInputChannelSet().isDisabled()
        && layout.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void SculptKeyProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused (samplesPerBlock);
    // Install restored targets before prepare() snaps its smoothing state.
    // The first note therefore starts at the saved timbre and output level.
    engine.setMorph (morphParameter->load (std::memory_order_relaxed) * 0.01f);
    engine.setOutputGain (juce::Decibels::decibelsToGain (
        outputParameter->load (std::memory_order_relaxed), -60.0f));
    engine.prepare (sampleRate);
    meterLevel.store (0.0f, std::memory_order_relaxed);
    roundRobinStep.store (-1, std::memory_order_relaxed);
}

void SculptKeyProcessor::releaseResources()
{
    engine.reset();
    meterLevel.store (0.0f, std::memory_order_relaxed);
    roundRobinStep.store (-1, std::memory_order_relaxed);
}

void SculptKeyProcessor::handleMidi (const juce::MidiMessage& message)
{
    if (message.isNoteOn())
        engine.noteOn (message.getNoteNumber(), message.getFloatVelocity(), message.getChannel());
    else if (message.isNoteOff())
        engine.noteOff (message.getNoteNumber(), message.getChannel());
    else if (message.isAllSoundOff())
        engine.allSoundOff (message.getChannel());
    else if (message.isAllNotesOff())
        engine.allNotesOff (message.getChannel());
    else if (message.isPitchWheel())
    {
        constexpr float rangeInSemitones = 2.0f;
        const int value = message.getPitchWheelValue() - 8192;
        engine.pitchBend (rangeInSemitones * static_cast<float> (value)
                          / (value >= 0 ? 8191.0f : 8192.0f), message.getChannel());
    }
    else if (message.isController() && message.getControllerNumber() == 64)
        engine.sustainPedal (message.getControllerValue() >= 64, message.getChannel());
    else if (message.isController() && message.getControllerNumber() == 121)
        engine.resetControllers (message.getChannel());
}

void SculptKeyProcessor::renderSegment (juce::AudioBuffer<float>& audio, int start, int length)
{
    if (length > 0)
        engine.render (audio.getWritePointer (0, start), audio.getWritePointer (1, start), length);
}

void SculptKeyProcessor::processBlock (juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    audio.clear();
    if (audio.getNumChannels() < 2 || ! samplesReady)
    {
        midi.clear();
        meterLevel.store (0.0f, std::memory_order_relaxed);
        return;
    }

    engine.setMorph (morphParameter->load (std::memory_order_relaxed) * 0.01f);
    engine.setOutputGain (juce::Decibels::decibelsToGain (
        outputParameter->load (std::memory_order_relaxed), -60.0f));

    int cursor = 0;
    const int blockLength = audio.getNumSamples();
    for (const auto metadata : midi)
    {
        const int position = juce::jlimit (cursor, blockLength, metadata.samplePosition);
        renderSegment (audio, cursor, position - cursor);
        // Channel messages fit in MidiMessage's inline storage. Ignore SysEx
        // before constructing a message so it cannot allocate on the audio thread.
        if (metadata.numBytes <= 3)
            handleMidi (metadata.getMessage());
        cursor = position;
    }
    renderSegment (audio, cursor, blockLength - cursor);
    midi.clear();

    const float peak = blockLength > 0 ? audio.getMagnitude (0, blockLength) : 0.0f;
    meterLevel.store (peak, std::memory_order_relaxed);
    roundRobinStep.store (engine.lastRoundRobin(), std::memory_order_relaxed);
}

juce::AudioProcessorEditor* SculptKeyProcessor::createEditor()
{
    return new SculptKeyEditor (*this);
}

void SculptKeyProcessor::getStateInformation (juce::MemoryBlock& data)
{
    auto state = apvts.copyState();
    state.setProperty ("schemaVersion", 1, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, data);
}

void SculptKeyProcessor::setStateInformation (const void* data, int size)
{
    if (const auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SculptKeyProcessor();
}
