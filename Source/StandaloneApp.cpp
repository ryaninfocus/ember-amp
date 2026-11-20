// Ember Amp's standalone app: JUCE's standard app, but in a real system window
// (macOS traffic-light buttons / the Windows title bar), with an app menu on the Mac.
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#if JucePlugin_Build_Standalone
#include <juce_audio_plugin_client/Standalone/juce_StandaloneFilterWindow.h>

namespace
{
using namespace juce;

/** The commands that used to sit behind JUCE's "Options" button. */
static PopupMenu makeAppMenu()
{
    PopupMenu m;
    m.addItem ("Audio Settings...", [] { if (auto* h = StandalonePluginHolder::getInstance()) h->showAudioSettingsDialog(); });
    m.addSeparator();
    m.addItem ("Save Current State...", [] { if (auto* h = StandalonePluginHolder::getInstance()) h->askUserToSaveState(); });
    m.addItem ("Load a Saved State...", [] { if (auto* h = StandalonePluginHolder::getInstance()) h->askUserToLoadState(); });
    return m;
}

class EmberWindow final : public StandaloneFilterWindow
{
public:
    EmberWindow (const String& title, Colour bg, std::unique_ptr<StandalonePluginHolder> holder)
        : StandaloneFilterWindow (title, bg, std::move (holder))
    {
        // JUCE's own "Options" button lives in its home-made title bar; the editor has SETTINGS instead
        for (auto* c : getChildren())
            if (auto* b = dynamic_cast<TextButton*> (c); b != nullptr && b->getButtonText() == "Options")
                b->setVisible (false);

        setUsingNativeTitleBar (true);
       #if JUCE_WINDOWS
        setTitleBarButtonsRequired (DocumentWindow::minimiseButton | DocumentWindow::closeButton, false);
       #endif
    }
};

#if JUCE_MAC
class MacMenu final : public MenuBarModel
{
public:
    StringArray getMenuBarNames() override                       { return { "Audio" }; }
    PopupMenu getMenuForIndex (int, const String&) override      { return makeAppMenu(); }
    void menuItemSelected (int, int) override                    {}
};
#endif

class EmberStandaloneApp final : public JUCEApplication
{
public:
    EmberStandaloneApp()
    {
        PropertiesFile::Options options;   // same settings file as before, so nothing is lost
        options.applicationName     = CharPointer_UTF8 (JucePlugin_Name);
        options.filenameSuffix      = ".settings";
        options.osxLibrarySubFolder = "Application Support";
        options.folderName          = "";
        appProperties.setStorageParameters (options);
    }

    const String getApplicationName() override              { return CharPointer_UTF8 (JucePlugin_Name); }
    const String getApplicationVersion() override           { return JucePlugin_VersionString; }
    bool moreThanOneInstanceAllowed() override              { return false; }
    void anotherInstanceStarted (const String&) override
    {
        if (mainWindow != nullptr) { mainWindow->setMinimised (false); mainWindow->toFront (true); }
    }

    void initialise (const String&) override
    {
       #if JUCE_MAC
        auto extra = makeAppMenu();   // "Audio Settings..." also in the Ember Amp menu, under About
        MenuBarModel::setMacMainMenu (&macMenu, &extra);
       #endif
        mainWindow = std::make_unique<EmberWindow> (getApplicationName(),
                                                    LookAndFeel::getDefaultLookAndFeel().findColour (ResizableWindow::backgroundColourId),
                                                    createPluginHolder());
        mainWindow->setVisible (true);
    }

    void shutdown() override
    {
       #if JUCE_MAC
        MenuBarModel::setMacMainMenu (nullptr);
       #endif
        mainWindow = nullptr;
        appProperties.saveIfNeeded();
    }

    // Cmd+Q, the red button, Alt+F4, the X, or the Dock's Quit
    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->getPluginHolder()->savePluginState();

        if (ModalComponentManager::getInstance()->cancelAllModalComponents())
            Timer::callAfterDelay (100, [] { if (auto* app = JUCEApplicationBase::getInstance()) app->systemRequestedQuit(); });
        else
            quit();
    }

private:
    std::unique_ptr<StandalonePluginHolder> createPluginHolder()
    {
        return std::make_unique<StandalonePluginHolder> (appProperties.getUserSettings(), false, String{}, nullptr,
                                                         Array<StandalonePluginHolder::PluginInOuts>{}, false);
    }

    ApplicationProperties appProperties;
    std::unique_ptr<EmberWindow> mainWindow;
   #if JUCE_MAC
    MacMenu macMenu;
   #endif
};
} // namespace

juce::JUCEApplicationBase* juce_CreateApplication();
juce::JUCEApplicationBase* juce_CreateApplication() { return new EmberStandaloneApp(); }
#endif
