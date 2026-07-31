#include <JuceHeader.h>
#include "host/PluginManager.h"
#include "ui/PluginEditorWindow.h"
#include "utils/CrashLog.h"
#include <atomic>
#include <mutex>

//==============================================================================
// Windows crash handler: writes minidump + diagnostics, then terminates.
#ifdef JUCE_WINDOWS
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG CALLBACK crashFilter (EXCEPTION_POINTERS* ep)
{
    auto code = ep->ExceptionRecord->ExceptionCode;
    auto addr = ep->ExceptionRecord->ExceptionAddress;

    CrashLog::write (CrashLog::Error, "UNHANDLED CRASH",
        "code=0x" + juce::String::toHexString ((int) code)
        + " addr=" + juce::String::toHexString ((uint64_t) addr));

    // Write a minidump for post-mortem analysis
    auto dumpPath = juce::File::getSpecialLocation (
        juce::File::SpecialLocationType::tempDirectory)
        .getChildFile ("pluginlab_crash.dmp");

    HANDLE hFile = CreateFileA (dumpPath.getFullPathName().toRawUTF8(),
                                GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION mei {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = TRUE;

        MiniDumpWriteDump (GetCurrentProcess(), GetCurrentProcessId(),
                           hFile, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle (hFile);
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

struct CrashFilterInstaller
{
    CrashFilterInstaller() { SetUnhandledExceptionFilter (crashFilter); }
} crashFilterInstaller;
#endif

//==============================================================================
class MainContentComponent : public juce::Component,
                             private juce::ListBoxModel
{
public:
    MainContentComponent()
    {
        CRASH_LOG_INFO ("App start", "Plugin Lab");

        pluginListBox.reset (new juce::ListBox ("Plugins", this));
        addAndMakeVisible (pluginListBox.get());

        statusLabel.reset (new juce::Label ("Status", "Ready"));
        statusLabel->setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (statusLabel.get());

        scanButton.reset (new juce::TextButton ("Scan VST3"));
        scanButton->onClick = [this] { scanPlugins(); };
        addAndMakeVisible (scanButton.get());

        pluginManager.reset (new PluginManager());
        setSize (1400, 850);

        // Start initial scan on a background thread
        scanPlugins();
    }

    ~MainContentComponent() override
    {
        unloadCurrentPlugin();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6);
        auto leftPanel = area.removeFromLeft (300);
        {
            auto header = leftPanel.removeFromTop (28);
            scanButton->setBounds (header.removeFromRight (75).reduced (2));
            statusLabel->setBounds (header);
            pluginListBox->setBounds (leftPanel);
        }
    }

    //==============================================================================
    int getNumRows() override
    {
        try
        {
            std::lock_guard<std::mutex> lock (listLock);
            return pluginManager ? pluginManager->getKnownPlugins().getNumTypes() : 0;
        }
        catch (...) { return 0; }
    }

    void paintListBoxItem (int row, juce::Graphics& g,
                           int width, int height, bool selected) override
    {
        juce::String name;
        try
        {
            std::lock_guard<std::mutex> lock (listLock);
            auto& list = pluginManager->getKnownPlugins();
            auto types = list.getTypes();
            if (juce::isPositiveAndBelow (row, types.size()))
                name = types.getReference (row).name;
        }
        catch (...) {}

        g.fillAll (selected ? juce::Colours::darkblue : juce::Colours::transparentBlack);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (name, 4, 0, width - 8, height, juce::Justification::centredLeft);
    }

    void selectedRowsChanged (int row) override
    {
        try
        {
            if (row >= 0) loadPlugin (row);
        }
        catch (const std::exception& e)
        {
            CRASH_LOG_ERR ("Row selected", juce::String ("exception: ") + e.what());
        }
        catch (...)
        {
            CRASH_LOG_ERR ("Row selected", "unknown exception caught");
        }
    }

private:
    //==============================================================================
    std::atomic<bool> scanDone { false };
    std::atomic<bool> scanRunning { false };
    std::atomic<bool> loadingRunning { false };
    std::mutex listLock;

    juce::PluginDescription* getPluginDescription (int index)
    {
        std::lock_guard<std::mutex> lock (listLock);
        auto& list = pluginManager->getKnownPlugins();
        auto types = list.getTypes();
        return juce::isPositiveAndBelow (index, types.size()) ? &types.getReference (index) : nullptr;
    }

    //==============================================================================
    void scanPlugins()
    {
        if (scanDone || scanRunning.exchange (true))
            return;

        statusLabel->setText ("Scanning VST3 plugins...", juce::dontSendNotification);
        scanButton->setEnabled (false);

        // Scan on background thread - UI stays responsive
        std::thread ([this]
        {
            try
            {
                CRASH_LOG_INFO ("Scan start", {});

                pluginManager->scanSystemDirectories();

                int count = 0;
                {
                    std::lock_guard<std::mutex> lock (listLock);
                    count = pluginManager->getKnownPlugins().getNumTypes();
                }

                CRASH_LOG_INFO ("Scan done", juce::String (count) + " plugins");
                scanDone = true;
                scanRunning = false;

                // Update UI on message thread
                juce::MessageManager::callAsync ([this, count]
                {
                    try
                    {
                        statusLabel->setText (juce::String (count) + " plugins found", juce::dontSendNotification);
                        pluginListBox->updateContent();
                        pluginListBox->repaint();
                        scanButton->setEnabled (true);
                    }
                    catch (...)
                    {
                        CRASH_LOG_ERR ("Scan UI update", "exception caught");
                    }
                });
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Scan thread", "exception caught");
                scanRunning = false;
                juce::MessageManager::callAsync ([this] { scanButton->setEnabled (true); });
            }
        }).detach();
    }

    //==============================================================================
    void loadPluginByDescription (const juce::PluginDescription& desc)
    {
        if (loadingRunning.exchange (true))
            return;

        juce::String safeName = desc.name;
        CRASH_LOG_INFO ("Loading", safeName);
        statusLabel->setText ("Loading: " + safeName + "...", juce::dontSendNotification);

        constexpr double sr = 48000.0;
        constexpr int bs = 512;

        // Background thread: only load the plugin instance.
        // Editor creation MUST happen on the message thread (JUCE hard assert).
        std::thread ([this, desc, safeName, sr, bs]
        {
            try
            {
                auto instance = pluginManager->loadPlugin (desc, sr, bs);

                juce::MessageManager::callAsync ([this, instance = std::move (instance), safeName]() mutable
                {
                    try
                    {
                        openEditorWindowFor (std::move (instance), safeName);
                    }
                    catch (...)
                    {
                        CRASH_LOG_ERR ("Load UI update", "exception caught");
                        loadingRunning = false;
                    }
                });
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Load thread", "exception caught for " + safeName);
                juce::MessageManager::callAsync ([this, safeName]
                {
                    statusLabel->setText ("Failed: " + safeName, juce::dontSendNotification);
                    loadingRunning = false;
                });
            }
        }).detach();
    }

    //==============================================================================
    void loadPlugin (int index)
    {
        juce::PluginDescription descCopy;
        auto* desc = getPluginDescription (index);   // getPluginDescription 内部已加锁,此处不要再锁(否则递归锁死)
        if (!desc) return;
        descCopy = *desc;
        loadPluginByDescription (descCopy);
    }

    void unloadCurrentPlugin()
    {
        // ~PluginEditorWindow → clearContentComponent → delete editor →
        // VST3 wrapper destroys view + calls editorBeingDeleted → instance
        // destructor runs (all on message thread).
        if (editorWindow)
            editorWindow.reset();
    }

    //==============================================================================
    void openEditorWindowFor (std::unique_ptr<juce::AudioPluginInstance> instance,
                              const juce::String& name)
    {
        unloadCurrentPlugin();

        if (!instance)
        {
            CRASH_LOG_WARN ("Load failed", name);
            statusLabel->setText ("Failed: " + name, juce::dontSendNotification);
            loadingRunning = false;
            return;
        }

        auto* editor = PluginManager::createEditorSafe (instance.get());

        if (editor)
        {
            editorWindow = std::make_unique<PluginEditorWindow> (
                std::move (instance), editor, name,
                [this] { onPluginWindowClosed(); });

            CRASH_LOG_INFO ("Editor ok", name + " "
                + juce::String (editorWindow->getWidth()) + "x"
                + juce::String (editorWindow->getHeight()));
            statusLabel->setText ("Loaded: " + name, juce::dontSendNotification);
        }
        else
        {
            CRASH_LOG_WARN ("No editor", name);
            statusLabel->setText ("Loaded (no editor): " + name, juce::dontSendNotification);
        }

        loadingRunning = false;
    }

    void onPluginWindowClosed()
    {
        unloadCurrentPlugin();
        statusLabel->setText ("Ready", juce::dontSendNotification);
        CRASH_LOG_INFO ("Editor window closed", {});
    }

    //==============================================================================
    std::unique_ptr<PluginManager> pluginManager;

    std::unique_ptr<juce::ListBox> pluginListBox;
    std::unique_ptr<juce::Label> statusLabel;
    std::unique_ptr<juce::TextButton> scanButton;

    std::unique_ptr<PluginEditorWindow> editorWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainContentComponent)
};

//==============================================================================
class PluginLabWindow : public juce::DocumentWindow
{
public:
    PluginLabWindow (const juce::String& name)
        : DocumentWindow (name,
                          juce::Desktop::getInstance().getDefaultLookAndFeel()
                              .findColour (juce::ResizableWindow::backgroundColourId),
                          DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainContentComponent(), true);
        setResizable (true, true);
        centreWithSize (1400, 850);
        setVisible (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

//==============================================================================
class PluginLabApplication : public juce::JUCEApplication
{
public:
    PluginLabApplication() {}

    const juce::String getApplicationName() override       { return "Plugin Lab"; }
    const juce::String getApplicationVersion() override    { return "1.0.0"; }

    void initialise (const juce::String&) override
    {
        mainWindow = std::make_unique<PluginLabWindow> (getApplicationName());
    }

    void shutdown() override
    {
        mainWindow = nullptr;
    }

private:
    std::unique_ptr<PluginLabWindow> mainWindow;
};

START_JUCE_APPLICATION (PluginLabApplication)
