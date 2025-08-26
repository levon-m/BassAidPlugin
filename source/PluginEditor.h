#pragma once

#include "BinaryData.h"
#include "PluginProcessor.h"

#if JUCE_MODULE_AVAILABLE_melatonin_inspector
    #include "melatonin_inspector/melatonin_inspector.h"
#endif

#include <memory>

class FretboardComponent;

class PluginEditor : public juce::AudioProcessorEditor
{
public:
    explicit PluginEditor (PluginProcessor&);
    ~PluginEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    PluginProcessor& processorRef;

    std::unique_ptr<FretboardComponent> fretboard;
    juce::Label lastNoteLabel;

#if JUCE_MODULE_AVAILABLE_melatonin_inspector
    std::unique_ptr<melatonin::Inspector> inspector;
    juce::TextButton inspectButton { "Inspect the UI" };
#endif

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginEditor)
};
