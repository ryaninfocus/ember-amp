#pragma once

#include "PluginProcessor.h"

class EmberAmpEditor;
class StreamOutput;

//==============================================================================
class EmberLookAndFeel : public juce::LookAndFeel_V4
{
public:
    EmberLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&,
                               bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    juce::Label* createSliderTextBox (juce::Slider&) override;
};

//==============================================================================
/** Knob value box: double-click to edit. It opens with the whole value selected, so you can
    just type the new number over it. */
class ValueLabel : public juce::Label
{
public:
    void editorShown (juce::TextEditor* ed) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
};

//==============================================================================
/** Rotary knob + title + value, attached to a parameter.
    style: "ring" (top strip), "chrome" / "black" (amp panels), "pedal". */
class Knob : public juce::Component
{
public:
    Knob (juce::AudioProcessorValueTreeState&, const juce::String& paramId, const juce::String& title,
          juce::Colour accent, const juce::String& style, juce::Colour textColour, float titleSize = 12.0f);
    void resized() override;
    juce::Slider& getSlider() { return slider; }
    juce::Label& getLabel()   { return label; }

private:
    juce::Slider slider;
    juce::Label label;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
};

//==============================================================================
class TunerDisplay : public juce::Component
{
public:
    void setFrequency (float hz);
    void paint (juce::Graphics&) override;

private:
    float freq = -1.0f, smoothedCents = 0.0f;
    int holdFrames = 0;
};

//==============================================================================
/** Thin vertical level meter. */
class LevelMeter : public juce::Component
{
public:
    void push (float peak);
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f;
};

//==============================================================================
/** One amp's page: a rendered amp head with its knobs, and the top of its speaker cab. */
class AmpPage : public juce::Component
{
public:
    AmpPage (EmberAmpEditor&, EmberAmpProcessor&, int amp);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    void chooseIR();
    int getAmp() const { return amp; }

    struct Theme { juce::Colour chassisTop, chassisBottom, grille, grilleLine, panelTop, panelBottom, text; bool lightPanel; };
    static Theme themeFor (int amp);

private:
    EmberAmpEditor& owner;
    EmberAmpProcessor& proc;
    const int amp;
    const juce::Colour accent;
    const Theme theme;

    juce::OwnedArray<Knob> knobs;     // [Drive], Gain, Bass, Mid, Treble, Presence, Sustain, Volume
    juce::ToggleButton cabButton { "CAB" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabAttachment;
    juce::TextButton loadIRButton { "LOAD IR" }, clearIRButton { "CLEAR" };
    juce::Label irLabel;
    std::unique_ptr<juce::FileChooser> fileChooser;
    juce::Image logo;

    // cab + mic summary, and a link to the CAB page where they are edited
    juce::Label cabSummary;
    juce::TextButton editCabButton { "EDIT CAB" };

    juce::Rectangle<int> head, grille, panel, cabTop;
    bool shownActive = false, shownCab = true;
};

//==============================================================================
/** A stompbox: name, knobs and an on/off footswitch. */
class PedalUnit : public juce::Component
{
public:
    PedalUnit (juce::AudioProcessorValueTreeState&, const juce::String& name, juce::Colour body,
               const juce::String& onParam, std::vector<std::pair<juce::String, juce::String>> knobs);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    juce::String name;
    juce::Colour body;
    juce::OwnedArray<Knob> knobs;
    juce::TextButton footswitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> footAttachment;
    bool shownOn = false;
    juce::Rectangle<float> nameArea;
};

/** The pedalboard page: 5 pedals before the amp, 3 after. */
class PedalsPage : public juce::Component
{
public:
    explicit PedalsPage (EmberAmpProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    EmberAmpProcessor& proc;
    PedalUnit gatePedal, compPedal, odPedal, dsPedal, pitchPedal, chorusPedal, delayPedal, reverbPedal;
    juce::Rectangle<int> ampSlot;
    int shownAmp = -1;
};

//==============================================================================
/** The CAB page: the live amp's cabinet, microphone, and where the mic sits (drag it on the speaker). */
class CabPage : public juce::Component, private juce::Timer
{
public:
    CabPage (EmberAmpEditor&, EmberAmpProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    juce::Point<float> getMicPointForTest() const       { return micPoint(); }
    juce::Point<float> getSpeakerCentreForTest() const  { return speakerCentre(); }

private:
    void bindToAmp (int amp);
    void setParam (const char* id, float value);
    juce::RangedAudioParameter* param (const char* id) const;
    float value (const char* id) const;
    juce::Point<float> speakerCentre() const;           // the miked speaker
    juce::Point<float> speakerCentreAt (int index) const;
    float speakerRadius() const;
    int speakerCount() const;
    int mikedSpeaker() const;                           // 0-based
    void timerCallback() override;                      // glides the drawn mic toward its settings
    void placeMicAt (juce::Point<float>, bool mayChangeSpeaker);   // on the miked speaker (or the one clicked)
    juce::Point<float> micPoint() const;

    EmberAmpEditor& ed;
    EmberAmpProcessor& proc;
    int amp = -1;

    juce::Label title, cabTitle, micTitle, irTitle, hint;
    juce::OwnedArray<juce::TextButton> cabButtons, micButtons;
    std::unique_ptr<Knob> posKnob, distKnob;
    juce::ToggleButton cabOn { "CAB" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> cabOnAtt;
    juce::Label irLabel;
    juce::TextButton loadIRButton { "LOAD IR" }, clearIRButton { "CLEAR" };

    juce::Rectangle<int> cabView, side;
    float micAngle = 0.0f;              // where around the cone the mic was dragged (only for drawing)
    float animPos = -1.0f, animDist = -1.0f;   // what's drawn: eases toward the real values
    juce::Point<float> animCentre;
    float shownSpk = -1.0f;
    bool dragging = false;
    float shownCab = -1.0f, shownMic = -1.0f, shownPos = -1.0f, shownDist = -1.0f;
    bool shownIR = false, shownOn = true;
};

//==============================================================================
/** The EQ page: a 4-band semi-parametric post EQ (plus low / high cut) as a rack unit, with its curve above. */
class EqPage : public juce::Component
{
public:
    EqPage (EmberAmpProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh();

private:
    EmberAmpProcessor& proc;
    juce::OwnedArray<Knob> topKnobs;     // low cut, freq 1..4, high cut
    juce::OwnedArray<Knob> gainKnobs;    // gain 1..4
    juce::ToggleButton power { "EQ" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> powerAtt;
    juce::Rectangle<int> curveArea, unit, face;
    juce::Rectangle<float> plot;      // the graph inside curveArea
    int amp = -1, dragBand = -1;
    double topHz() const;
    float xForHz (double hz) const;
    double hzForX (float x) const;
    float yForDb (float db) const;
    float dbForY (float y) const;
    juce::Point<float> handleFor (int band) const;
public:
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
private:
    float lastSig = 0.0f;
    bool shownOn = false;
};

//==============================================================================
class EmberAmpEditor : public juce::AudioProcessorEditor,
                       public juce::FileDragAndDropTarget,
                       private juce::ChangeListener,
                       private juce::Timer
{
public:
    explicit EmberAmpEditor (EmberAmpProcessor&);
    ~EmberAmpEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true;  repaint(); }
    void fileDragExit (const juce::StringArray&) override            { dragHover = false; repaint(); }
    void filesDropped (const juce::StringArray& files, int, int) override;
    bool keyPressed (const juce::KeyPress&) override;

    void showPage (int page);            // 0 = pedals, 1..4 = amps, 5 = cab
    static constexpr int cabPageIndex = 1 + EmberAmpProcessor::numAmps;
    static constexpr int eqPageIndex  = 2 + EmberAmpProcessor::numAmps;
    bool isAmpPage() const { return currentPage >= 1 && currentPage <= EmberAmpProcessor::numAmps; }
    void presetsChanged();               // a preset was loaded from somewhere
    static juce::Colour ampAccent (int amp);

private:
    void timerCallback() override;
    void updateMuteButton();
    float detectPitch();

    EmberAmpProcessor& proc;
    EmberLookAndFeel lnf;

    // navigation
    juce::TextButton tabs[3 + EmberAmpProcessor::numAmps];   // pedals, the amps, cab, eq

    // top control strip
    LevelMeter inMeter, outMeter;
    Knob inputKnob, gateKnob, transposeKnob, outputKnob;
    juce::ToggleButton gateSwitch { "GATE" }, transposeSwitch { "TRANSPOSE" }, dynamicsSwitch { "DYNAMICS" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> gateSwitchAtt, transposeSwitchAtt, dynamicsAtt;
    juce::TextButton prevPreset, nextPreset;
    juce::TextButton presetDeleteIcon, presetImportIcon, presetSearchIcon;   // savePresetButton is the save icon
    juce::ToggleButton monoSwitch { "MODE" }, doublerSwitch { "DOUBLER" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> monoAtt, doublerAtt;
    std::unique_ptr<Knob> spreadKnob;
    std::unique_ptr<juce::FileChooser> importChooser;
    juce::Rectangle<int> modeHead, doublerHead, presetsHead, dynamicsHead;
    void deleteCurrentPreset();
    void importPreset();
    juce::TextButton savePresetButton { "SAVE AS..." };
    juce::ComboBox presetBox;
    juce::Array<juce::File> userPresetFiles;
    void rebuildPresetMenu();
    int currentPresetMenuId() const;
    void presetChosen();
    void stepPreset (int delta);
    void askToSavePreset();
    std::vector<juce::Rectangle<int>> dividers;

    // status bar
    TunerDisplay tuner;
    juce::TextButton muteButton;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> muteAttachment;
    bool lastMuteShown = false;

    // Standalone-only: audio buffer size, latency, input channel
    void refreshLatencyUI();
    void changeListenerCallback (juce::ChangeBroadcaster*) override; // audio device changed
    void saveStandaloneState (bool includeDevice);
    bool isStandalone = false, userChangedAudio = false;
    juce::ComboBox bufferBox, inputChannelBox;
    juce::Label latencyLabel;
    juce::String lastDeviceKey;

    // Standalone-only: "Stream" sends a second copy (plus an optional mic) to a virtual cable for Discord
    friend class StreamPanel;
    std::unique_ptr<StreamOutput> stream;
    juce::ToggleButton streamSwitch { "STREAM" };
    juce::TextButton streamMoreButton;
    juce::TextButton settingsButton { "SETTINGS" };   // audio device, save / load state
    bool streamWanted = false;
    juce::String streamError;
    juce::String streamStatusText() const;
    void startStream (bool showProblems);
    void stopStream();
    void showStreamPanel();
    void saveStreamSettings();

    // pages
    PedalsPage pedalsPage;
    std::unique_ptr<AmpPage> ampPages[EmberAmpProcessor::numAmps];
    std::unique_ptr<CabPage> cabPage;
    std::unique_ptr<EqPage> eqPage;
    friend class CabPage;
    int currentPage = 1, lastSelectedAmp = -1, lastProgramShown = -1;
    juce::String lastUserPresetShown;
    juce::Rectangle<int> stageArea, statusArea;
    juce::Image starfield;   // background, rendered once per size

    bool dragHover = false;
    juce::Image logo;
    std::vector<float> pitchBuffer, yin;
    int frameCounter = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EmberAmpEditor)
};
