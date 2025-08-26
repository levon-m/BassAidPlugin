#include "PluginEditor.h"
#include <cmath>

// ================================================================
// Helpers / constants
namespace
{
    // Plugin background: #849483
    constexpr auto kPluginBg = 0xff849483;

    // Board colours
    constexpr auto kWoodEdge = 0xff8b5a2b; // brown edge
    constexpr auto kBoardFill = 0xffd2a679; // tan fill

    // Lines & accents
    constexpr auto kStringBlack = 0xff000000; // strings
    constexpr auto kFretSilver = 0xffa0a0a0; // darker silver
    constexpr auto kFretOutline = 0xff000000; // thin outline
    constexpr auto kNutBlack = 0xff000000; // nut
    constexpr auto kInlayFill = 0xffffffff; // inlay fill
    constexpr auto kInlayStroke = 0xff000000; // inlay stroke

    // Open-string circles
    constexpr auto kOpenFill = 0xfff5f5f5;
    constexpr auto kOpenStroke = 0xff000000;

    // Highlight (stronger blue than before)
    constexpr auto kHighlightBB = 0xff67b8ff;

    // Geometry
    constexpr int kNumStrings = 4;
    constexpr int kNumFrets = 12; // 1..12 on board (0 = open)

    // Note names
    static const char* kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    inline juce::String midiToNote (int midi)
    {
        const int n = juce::jmax (0, midi);
        const int idx = n % 12;
        const int oct = (n / 12) - 1;
        return juce::String (kNoteNames[idx]) + juce::String (oct);
    }

    // Map GUI rows to bass strings, bottom->top should be E A D G.
    // Our rows are indexed top=0 ... bottom=3, so:
    //   row 0 (top) -> G2, row 1 -> D2, row 2 -> A1, row 3 (bottom) -> E1
    inline int baseMidiForString (int stringIdx)
    {
        switch (stringIdx)
        {
            case 0:
                return 43; // G2 (top row)
            case 1:
                return 38; // D2
            case 2:
                return 33; // A1
            default:
                return 28; // E1 (bottom row)
        }
    }

    // Cubic ease-out 0..1
    inline float easeOutCubic01 (float t)
    {
        t = juce::jlimit (0.0f, 1.0f, t);
        return 1.0f - std::pow (1.0f - t, 3.0f);
    }
} // namespace

// ================================================================
// Fretboard (draws board, handles clicks & animations)
class FretboardComponent : public juce::Component,
                           private juce::Timer
{
public:
    FretboardComponent()
    {
        setOpaque (true);
        startTimerHz (60);
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    }

    std::function<void (const juce::String&)> onNotePlayed;

    // External trigger (e.g., from ML/UI)
    void triggerNote (int stringIdx, int fretIdx)
    {
        if (stringIdx < 0 || stringIdx >= kNumStrings)
            return;
        if (fretIdx < 0 || fretIdx > kNumFrets)
            return;

        if (openRadius <= 0.0f)
            rebuildStatic(); // ensure geometry exists

        Animation a;
        a.t0 = juce::Time::getMillisecondCounterHiRes() * 1e-3;
        a.duration = 1.80; // longer fade
        a.stringIdx = stringIdx;
        a.fretIdx = fretIdx;
        a.isOpen = (fretIdx == 0);
        a.noteName = midiToNote (baseMidiForString (stringIdx) + fretIdx);

        // center for circular highlight (same size as open-string circles)
        if (a.isOpen)
            a.center = openCircles[stringIdx].getCentre();
        else
        {
            const float cw = boardBounds.getWidth() / (float) kNumFrets;
            const float cx = boardBounds.getX() + (fretIdx - 0.5f) * cw;
            const float cy = rowRects[stringIdx].getCentreY();
            a.center = { cx, cy };
        }

        const juce::Rectangle<float> r (a.center.x - openRadius,
            a.center.y - openRadius,
            2.0f * openRadius,
            2.0f * openRadius);
        a.bounds = r.expanded (3.0f);

        if (onNotePlayed)
            onNotePlayed (a.noteName);

        active.add (a); // multiple highlights allowed
        repaint (a.bounds.toNearestInt()); // tight repaint
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (kPluginBg));

        if (staticLayer.isNull()
            || staticLayer.getWidth() != getWidth()
            || staticLayer.getHeight() != getHeight())
            rebuildStatic();

        g.drawImageAt (staticLayer, 0, 0);

        // Draw active highlights
        const double now = juce::Time::getMillisecondCounterHiRes() * 1e-3;
        for (int i = active.size(); --i >= 0;)
        {
            auto& a = active.getReference (i);
            const double t = (now - a.t0) / a.duration;
            if (t >= 1.0)
            {
                active.remove (i);
                continue;
            }

            const float alpha = (1.0f - easeOutCubic01 ((float) t));
            juce::Colour c = juce::Colour (kHighlightBB).withAlpha (alpha);
            g.setColour (c);

            const juce::Rectangle<float> r (a.center.x - openRadius,
                a.center.y - openRadius,
                2.0f * openRadius,
                2.0f * openRadius);
            g.fillEllipse (r);
        }
    }

    void resized() override
    {
        rebuildStatic();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = e.position;

        // Open strings first
        for (int s = 0; s < kNumStrings; ++s)
        {
            if (openCircles[s].contains (p))
            {
                triggerNote (s, 0);
                return;
            }
        }

        if (!boardBounds.contains (p))
            return;

        // Which string
        int stringIdx = -1;
        for (int s = 0; s < kNumStrings; ++s)
            if (rowRects[s].contains (p))
            {
                stringIdx = s;
                break;
            }
        if (stringIdx < 0)
            return;

        // Which fret (1..12)
        const float xRel = p.x - boardBounds.getX();
        const float cw = boardBounds.getWidth() / (float) kNumFrets;
        int fretIdx = (int) std::floor (xRel / cw) + 1;
        fretIdx = juce::jlimit (1, kNumFrets, fretIdx);

        triggerNote (stringIdx, fretIdx);
    }

private:
    struct Animation
    {
        double t0 { 0.0 };
        double duration { 1.80 };
        int stringIdx { 0 };
        int fretIdx { 0 };
        bool isOpen { false };
        juce::String noteName;
        juce::Point<float> center;
        juce::Rectangle<float> bounds;
    };

    juce::Array<Animation> active;

    // Cached static layer
    juce::Image staticLayer;

    // Geometry
    juce::Rectangle<float> boardBounds; // frets 1..12
    juce::Rectangle<float> rowRects[kNumStrings]; // per-string rows
    juce::Rectangle<float> openCircles[kNumStrings]; // open-string ellipses
    float openRadius { 0.0f }; // shared with highlight circles

    // Reusable paths
    juce::Path edgePath;
    juce::Path fretLines;
    juce::Path stringLines;
    juce::Path inlaysPath;
    juce::Path nutPath;

    juce::Rectangle<float> cellRect (int stringIdx, int fretIdx) const
    {
        const float cw = boardBounds.getWidth() / (float) kNumFrets;
        auto r = rowRects[stringIdx];
        r.setX (boardBounds.getX() + (fretIdx - 1) * cw);
        r.setWidth (cw);
        return r;
    }

    void rebuildStatic()
    {
        if (getWidth() <= 2 || getHeight() <= 2)
            return;

        staticLayer = juce::Image (juce::Image::RGB, getWidth(), getHeight(), true);
        juce::Graphics g (staticLayer);

        g.fillAll (juce::Colour (kPluginBg));

        // Layout
        const float margin = 16.0f;
        const float leftOpenPad = 64.0f; // space for open circles
        const float edgeThickness = 10.0f;

        const auto outer = getLocalBounds().toFloat().reduced (margin);
        const auto boardOuter = outer.withTrimmedLeft (leftOpenPad);
        boardBounds = boardOuter.reduced (edgeThickness);

        // Wood edge
        edgePath.clear();
        edgePath.addRoundedRectangle (boardOuter, 8.0f);
        g.setColour (juce::Colour (kWoodEdge));
        g.fillPath (edgePath);

        // Board fill
        g.setColour (juce::Colour (kBoardFill));
        g.fillRoundedRectangle (boardBounds, 6.0f);

        // Rows
        const float rowH = boardBounds.getHeight() / (float) kNumStrings;
        for (int s = 0; s < kNumStrings; ++s)
        {
            auto r = boardBounds.withY (boardBounds.getY() + s * rowH)
                         .withHeight (rowH);
            rowRects[s] = r.reduced (0.0f, juce::jmin (3.0f, r.getHeight() * 0.12f));
        }

        // Strings: a tad thicker (1.5 px), from nut to board end
        stringLines.clear();
        for (int s = 0; s < kNumStrings; ++s)
        {
            const float y = rowRects[s].getCentreY() + 0.5f;
            stringLines.startNewSubPath (std::round (boardBounds.getX()) + 0.5f, y);
            stringLines.lineTo (std::round (boardBounds.getRight()) + 0.5f, y);
        }
        g.setColour (juce::Colour (kStringBlack));
        g.strokePath (stringLines, juce::PathStrokeType (1.5f));

        // Frets: black outline then darker silver on top
        fretLines.clear();
        const float cw = boardBounds.getWidth() / (float) kNumFrets;
        for (int f = 1; f <= kNumFrets; ++f)
        {
            const float x = std::round (boardBounds.getX() + f * cw) + 0.5f;
            fretLines.startNewSubPath (x, boardBounds.getY());
            fretLines.lineTo (x, boardBounds.getBottom());
        }
        g.setColour (juce::Colour (kFretOutline));
        g.strokePath (fretLines, juce::PathStrokeType (2.6f));
        g.setColour (juce::Colour (kFretSilver));
        g.strokePath (fretLines, juce::PathStrokeType (2.0f));

        // Nut
        nutPath.clear();
        {
            const float x = std::round (boardBounds.getX()) + 0.5f;
            nutPath.startNewSubPath (x, boardBounds.getY());
            nutPath.lineTo (x, boardBounds.getBottom());
        }
        g.setColour (juce::Colour (kNutBlack));
        g.strokePath (nutPath, juce::PathStrokeType (2.5f));

        // Inlays: 3,5,7,9 single; 12 double (bigger + moved further out from strings)
        inlaysPath.clear();
        const int singleInlays[] = { 3, 5, 7, 9 };
        const float inlayR = juce::jlimit (8.0f, 20.0f, boardBounds.getHeight() * 0.052f);
        for (int idx : singleInlays)
        {
            const float cx = boardBounds.getX() + (idx - 0.5f) * cw;
            const float cy = boardBounds.getCentreY();
            inlaysPath.addEllipse (cx - inlayR, cy - inlayR, 2 * inlayR, 2 * inlayR);
        }
        {
            const float cx = boardBounds.getX() + (12 - 0.5f) * cw;
            // Move outward a bit more to avoid string overlap
            const float y1 = boardBounds.getY() + boardBounds.getHeight() * 0.28f;
            const float y2 = boardBounds.getY() + boardBounds.getHeight() * 0.72f;
            inlaysPath.addEllipse (cx - inlayR, y1 - inlayR, 2 * inlayR, 2 * inlayR);
            inlaysPath.addEllipse (cx - inlayR, y2 - inlayR, 2 * inlayR, 2 * inlayR);
        }
        g.setColour (juce::Colour (kInlayFill));
        g.fillPath (inlaysPath);
        g.setColour (juce::Colour (kInlayStroke));
        g.strokePath (inlaysPath, juce::PathStrokeType (1.0f));

        // Open-string circles (same size logic), moved slightly LEFT of the nut
        openRadius = juce::jlimit (12.0f, 22.0f, boardBounds.getHeight() * 0.065f);

        // Slightly bigger gap than before to avoid any nut overlap
        const float openGap = 12.0f; // px

        for (int s = 0; s < kNumStrings; ++s)
        {
            const float cy = rowRects[s].getCentreY();
            const float nutX = std::round (boardBounds.getX()) + 0.5f;
            const float cx = nutX - (openRadius + openGap);
            openCircles[s] = { cx - openRadius, cy - openRadius, 2 * openRadius, 2 * openRadius };

            g.setColour (juce::Colour (kOpenFill));
            g.fillEllipse (openCircles[s]);
            g.setColour (juce::Colour (kOpenStroke));
            g.drawEllipse (openCircles[s], 1.0f);
        }
    }

    void timerCallback() override
    {
        if (active.isEmpty())
            return;

        juce::Rectangle<int> dirty;
        for (const auto& a : active)
        {
            const auto b = a.bounds.toNearestInt();
            dirty = dirty.isEmpty() ? b : dirty.getUnion (b);
        }
        repaint (dirty.expanded (2));
    }
}; // class FretboardComponent

// ================================================================
// PluginEditor
PluginEditor::PluginEditor (PluginProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p)
{
    fretboard = std::make_unique<FretboardComponent>();
    addAndMakeVisible (*fretboard);

    lastNoteLabel.setText ("Click a fret or an open circle", juce::dontSendNotification);
    lastNoteLabel.setJustificationType (juce::Justification::centredLeft);
    lastNoteLabel.setFont (juce::Font (16.0f, juce::Font::bold)); // slightly bigger
    lastNoteLabel.setColour (juce::Label::textColourId, juce::Colours::black); // black text
    addAndMakeVisible (lastNoteLabel);

    fretboard->onNotePlayed = [this] (const juce::String& note) {
        lastNoteLabel.setText ("Note: " + note, juce::dontSendNotification);
    };

#if JUCE_MODULE_AVAILABLE_melatonin_inspector
    addAndMakeVisible (inspectButton);
    inspectButton.onClick = [this] {
        if (!inspector)
        {
            inspector = std::make_unique<melatonin::Inspector> (*this);
            inspector->onClose = [this]() { inspector.reset(); };
        }
        inspector->setVisible (true);
    };
#endif

    setResizable (true, true);
    setSize (980, 340);
}

PluginEditor::~PluginEditor() = default;

void PluginEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (kPluginBg));
}

void PluginEditor::resized()
{
    auto area = getLocalBounds().reduced (10);

    auto top = area.removeFromTop (28);
    lastNoteLabel.setBounds (top.removeFromLeft (area.proportionOfWidth (0.65f)));

#if JUCE_MODULE_AVAILABLE_melatonin_inspector
    inspectButton.setBounds (top.removeFromRight (120));
#endif

    fretboard->setBounds (area);
}
