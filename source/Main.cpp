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
                             private juce::ListBoxModel,
                             private juce::AsyncUpdater
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
        threadPool = std::make_unique<juce::ThreadPool> (2);
        setSize (1400, 850);

        // Start initial scan on a background thread
        scanPlugins();
    }

    ~MainContentComponent() override
    {
        unloadCurrentPlugin();

        // CRITICAL: join all background tasks BEFORE any member destructors run.
        // ThreadPool destructor blocks until every running/queued job completes,
        // guaranteeing no ThreadPoolJob accesses this->* after this point.
        threadPool = nullptr;

        // Discard any handleAsyncUpdate that was triggered during job finalisation.
        // cancelPendingUpdate() blocks if the callback is currently in-flight
        // (cannot happen here — both destructor and handleAsyncUpdate run on the
        // message thread, so they are mutually exclusive).
        cancelPendingUpdate();
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

    //==============================================================================
    /** Called on the message thread when a background job has finished and
     *  called triggerAsyncUpdate().  Processes whichever update is pending:
     *  scan completion or plugin-load completion.
     */
    void handleAsyncUpdate() override
    {
        // Process scan result — always first so the list is refreshed before a
        // plugin-load tries to select the right row.
        if (scanUpdatePending.exchange (false))
        {
            try
            {
                if (scanDone)
                {
                    statusLabel->setText (
                        juce::String (scannedCount.load()) + " plugins found",
                        juce::dontSendNotification);
                }
                else
                {
                    statusLabel->setText ("Scan error", juce::dontSendNotification);
                }
                pluginListBox->updateContent();
                pluginListBox->repaint();
                scanButton->setEnabled (true);
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Scan UI update", "exception caught");
            }
        }

        // Process plugin-load result.
        if (loadUpdatePending.exchange (false))
        {
            try
            {
                auto inst = std::move (pendingInstance);
                juce::String name = std::move (pendingName);
                openEditorWindowFor (std::move (inst), name);
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Load UI update", "exception caught");
                loadingRunning = false;
            }
        }
    }

private:
    //==============================================================================
    // Background-thread job helpers — nested classes have access to all private
    // members of MainContentComponent (C++11 implicit friend).

    struct ScanJob final : public juce::ThreadPoolJob
    {
        MainContentComponent& owner;

        ScanJob (MainContentComponent& o)
            : ThreadPoolJob ("PluginScan"), owner (o) {}

        JobStatus runJob() override
        {
            try
            {
                CRASH_LOG_INFO ("Scan start", {});
                owner.pluginManager->scanSystemDirectories();

                int count = 0;
                {
                    std::lock_guard<std::mutex> lock (owner.listLock);
                    count = owner.pluginManager->getKnownPlugins().getNumTypes();
                }

                CRASH_LOG_INFO ("Scan done", juce::String (count) + " plugins");
                owner.scanDone = true;
                owner.scanRunning = false;
                owner.scannedCount = count;
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Scan thread", "exception caught");
                owner.scanRunning = false;
                // scanDone stays false → handleAsyncUpdate shows "Scan error"
            }

            owner.scanUpdatePending = true;
            owner.triggerAsyncUpdate();
            return jobHasFinished;
        }
    };

    struct LoadJob final : public juce::ThreadPoolJob
    {
        MainContentComponent& owner;
        juce::PluginDescription desc;
        juce::String name;
        double sampleRate;
        int blockSize;

        LoadJob (MainContentComponent& o,
                 const juce::PluginDescription& d,
                 juce::String n,
                 double sr,
                 int bs)
            : ThreadPoolJob ("PluginLoad"),
              owner (o), desc (d), name (std::move (n)),
              sampleRate (sr), blockSize (bs) {}

        JobStatus runJob() override
        {
            try
            {
                auto instance = owner.pluginManager->loadPlugin (desc, sampleRate, blockSize);
                owner.pendingInstance = std::move (instance);
                owner.pendingName = name;
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Load thread", "exception caught for " + name);
                owner.pendingInstance.reset();      // nullptr → openEditorWindowFor shows failure
                owner.pendingName = name;
            }

            owner.loadUpdatePending = true;
            owner.triggerAsyncUpdate();
            return jobHasFinished;
        }
    };

    //==============================================================================
    std::atomic<bool> scanDone { false };
    std::atomic<bool> scanRunning { false };
    std::atomic<bool> loadingRunning { false };
    std::mutex listLock;

    // IPC between background threads (write) and handleAsyncUpdate (read).
    // Only one job per category runs at a time (scanRunning / loadingRunning
    // gates), so a single slot per category is safe.
    std::atomic<bool> scanUpdatePending { false };
    std::atomic<bool> loadUpdatePending { false };
    std::atomic<int>  scannedCount { 0 };
    std::unique_ptr<juce::AudioPluginInstance> pendingInstance;
    juce::String pendingName;

    // Returns a COPY of the plugin description for the given row, guarded by
    // listLock. Returning a copy (not a pointer into the list) is essential:
    // the scan thread may re-enter the list after the lock is released, which
    // would leave a returned raw pointer dangling (use-after-free).
    bool getPluginDescription (int index, juce::PluginDescription& out)
    {
        std::lock_guard<std::mutex> lock (listLock);
        auto& list = pluginManager->getKnownPlugins();
        auto types = list.getTypes();
        if (! juce::isPositiveAndBelow (index, types.size()))
            return false;
        out = types.getReference (index);
        return true;
    }

    //==============================================================================
    void scanPlugins()
    {
        if (scanDone || scanRunning.exchange (true))
            return;

        statusLabel->setText ("Scanning VST3 plugins...", juce::dontSendNotification);
        scanButton->setEnabled (false);

        // Ownership of the job transfers to the pool; job is auto-deleted after runJob().
        threadPool->addJob (new ScanJob (*this), true);
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

        // Ownership of the job transfers to the pool; job is auto-deleted after runJob().
        threadPool->addJob (new LoadJob (*this, desc, safeName, sr, bs), true);
    }

    //==============================================================================
    void loadPlugin (int index)
    {
        // Copy the description under the lock so the scan thread can't invalidate
        // it between lookup and copy (use-after-free).
        juce::PluginDescription descCopy;
        if (! getPluginDescription (index, descCopy))
            return;
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

        // Fall back to the generic parameter editor when the plugin has no
        // native GUI (createEditor returned null). The plugin is still fully
        // usable: the generic editor shows all its parameters as sliders.
        if (editor == nullptr)
        {
            try
            {
                editor = new juce::GenericAudioProcessorEditor (*instance);
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Generic editor crash", name);
                editor = nullptr;
            }
        }

        if (editor)
        {
            editorWindow = std::make_unique<PluginEditorWindow> (
                std::move (instance), editor, name,
                [this] { onPluginWindowClosed(); });

            const bool isGeneric = dynamic_cast<juce::GenericAudioProcessorEditor*> (editor) != nullptr;
            CRASH_LOG_INFO (isGeneric ? "Editor ok (generic)" : "Editor ok", name + " "
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
    // Member destruction order = REVERSE declaration order:
    //   threadPool is destroyed first → joins all background jobs,
    //   then UI members, then pluginManager, then atomics / mutex.
    // The explicit `threadPool = nullptr` in ~MainContentComponent makes this
    // deterministic regardless of compiler-specific layout.
    std::unique_ptr<PluginManager> pluginManager;
    std::unique_ptr<juce::ThreadPool> threadPool;

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
