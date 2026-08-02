#include <JuceHeader.h>
#include "host/PluginManager.h"
#include "ui/PluginEditorWindow.h"
#include "ui/PlotWidget.h"
#include "utils/CrashLog.h"
#include "ipc/PipeServer.h"
#include "ipc/CommandParser.h"
#include "ipc/Protocol.h"
#include "capture/MeasurementSession.h"
#include "analysis/FreqResponse.h"
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

        // Right-panel frequency-response plots (magnitude on top, phase below).
        magPlot.reset (new PlotWidget());
        magPlot->setAxisLabels ("Frequency (Hz)", "Magnitude (dB)");
        magPlot->setXAxisLog (true);
        magPlot->setAutoFitY (true);
        addAndMakeVisible (magPlot.get());

        phasePlot.reset (new PlotWidget());
        phasePlot->setAxisLabels ("Frequency (Hz)", "Phase (degrees)");
        phasePlot->setXAxisLog (true);
        phasePlot->setAutoFitY (true);
        addAndMakeVisible (phasePlot.get());

        // Measurement control row (right panel, above the plots).
        // Frequency response is wired end-to-end; harmonic/compression are
        // placeholders until phase 2 (they only report "not wired yet").
        measureFreqButton.reset (new juce::TextButton ("Freq Response"));
        measureFreqButton->onClick = [this]
        {
            startMeasurement (juce::String (Protocol::MeasureType::freq));
        };
        addAndMakeVisible (measureFreqButton.get());

        measureHarmonicButton.reset (new juce::TextButton ("Harmonic"));
        measureHarmonicButton->onClick = [this]
        {
            startMeasurement (juce::String (Protocol::MeasureType::harmonic));
        };
        addAndMakeVisible (measureHarmonicButton.get());

        measureCompressionButton.reset (new juce::TextButton ("Compression"));
        measureCompressionButton->onClick = [this]
        {
            startMeasurement (juce::String (Protocol::MeasureType::compression));
        };
        addAndMakeVisible (measureCompressionButton.get());

        // Input source selector (signal | file | noise | dynamic). The source
        // decides which signal is generated; non-signal sources are captured
        // raw (no analysis — that is phase 4).
        sourceCombo.reset (new juce::ComboBox ("Source"));
        sourceCombo->addItem ("Signal", 1);
        sourceCombo->addItem ("File", 2);
        sourceCombo->addItem ("Noise", 3);
        sourceCombo->addItem ("Dynamic", 4);
        sourceCombo->setSelectedId (1);
        sourceCombo->onChange = [this]
        {
            updateSourceControls();
            clearMeasurementDisplay();
        };
        addAndMakeVisible (sourceCombo.get());

        // File source: input-file picker (only visible for the file source).
        chooseFileButton.reset (new juce::TextButton ("Choose WAV..."));
        chooseFileButton->onClick = [this] { chooseAudioFile(); };
        chooseFileButton->setVisible (false);
        addAndMakeVisible (chooseFileButton.get());

        // Noise source: duration entry (only visible for the noise source).
        noiseDurationLabel.reset (new juce::Label ("NoiseDuration", "Duration:"));
        noiseDurationLabel->setFont (juce::FontOptions (12.0f));
        noiseDurationLabel->setJustificationType (juce::Justification::centredRight);
        noiseDurationLabel->setVisible (false);
        addAndMakeVisible (noiseDurationLabel.get());

        noiseDurationBox.reset (new juce::TextEditor ("NoiseDurationBox"));
        noiseDurationBox->setText ("2.0");
        noiseDurationBox->setVisible (false);
        addAndMakeVisible (noiseDurationBox.get());

        pluginManager.reset (new PluginManager());
        threadPool = std::make_unique<juce::ThreadPool> (2);

        // --- IPC pipeline setup ---
        measurementSession = std::make_unique<MeasurementSession>();
        measurementSession->setSampleRate (48000.0);
        measurementSession->setBlockSize (512);

        commandParser = std::make_unique<CommandParser>();
        commandParser->setPluginManager (pluginManager.get());
        commandParser->setSession (measurementSession.get());
        commandParser->setLoadPluginCallback ([this] (const juce::PluginDescription& d)
        {
            loadPluginByDescription (d);
        });
        commandParser->setStatusCallback ([this] (const juce::String& s)
        {
            statusLabel->setText (s, juce::dontSendNotification);
        });
        commandParser->setMeasurementCompleteCallback ([this] (const MeasurementResults& r)
        {
            measurementResult = r;
            hasMeasurement = true;
            triggerAsyncUpdate();
        });

        pipeServer = std::make_unique<PipeServer>();
        pipeServer->setCommandHandler ([this] (const juce::String& cmd)
        {
            return commandParser->handleCommand (cmd);
        });
        pipeServer->startup();
        setSize (1400, 850);

        // Start initial scan on a background thread
        scanPlugins();
    }

    ~MainContentComponent() override
    {
        // 1. Cancel any in-progress measurement (returns within 1 block).
        if (measurementSession)
            measurementSession->cancel();

        // 2. Unload current plugin + close editor window.
        unloadCurrentPlugin();

        // 3. Shut down the pipe server (joins IPC thread).
        if (pipeServer)
            pipeServer->shutdown();

        // 4. Join all background jobs BEFORE member destructors run.
        //    ThreadPool destructor blocks until every running/queued job completes,
        //    guaranteeing no ThreadPoolJob accesses this->* after this point.
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

        // Right panel: measurement controls on top (source/type row +
        // source-parameter row), then the magnitude plot (~65%) and phase
        // plot (~35%) below.
        auto plotArea = area;
        auto controls = plotArea.removeFromTop (92).reduced (0, 2);
        {
            const int gap = 6;

            // Row 1: source combo (right) + the three measure buttons.
            auto row1 = controls.removeFromTop (30);
            const int comboW = 110;
            sourceCombo->setBounds (row1.removeFromRight (comboW));
            row1.removeFromRight (gap);
            const int btnW = (row1.getWidth() - 2 * gap) / 3;
            measureFreqButton->setBounds (row1.removeFromLeft (btnW));
            row1.removeFromLeft (gap);
            measureHarmonicButton->setBounds (row1.removeFromLeft (btnW));
            row1.removeFromLeft (gap);
            measureCompressionButton->setBounds (row1);

            // Row 2: source-specific parameters (file chooser / noise duration).
            controls.removeFromTop (4);
            auto row2 = controls.removeFromTop (26);
            chooseFileButton->setBounds (row2.removeFromLeft (120).reduced (0, 2));
            row2.removeFromLeft (gap);
            noiseDurationLabel->setBounds (row2.removeFromLeft (70).reduced (0, 2));
            noiseDurationBox->setBounds (row2.removeFromLeft (56).reduced (0, 2));
        }

        auto phaseArea = plotArea.removeFromBottom (juce::roundToInt (plotArea.getHeight() * 0.35f));
        magPlot->setBounds (plotArea);
        phasePlot->setBounds (phaseArea);
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

        // Render the measurement result on the right-panel plots.
        if (hasMeasurement)
        {
            hasMeasurement = false;

            // Measurement finished: re-arm the re-entry guard and re-enable
            // the measure buttons (empty-result path below also returns here).
            measurementInProgress = false;
            measureFreqButton->setEnabled (true);
            measureHarmonicButton->setEnabled (true);
            measureCompressionButton->setEnabled (true);

            // Non-signal sources are captured raw — show the capture summary
            // instead of analysis plots (analysis is phase 4).
            if (measurementResult.source == juce::String (Protocol::Source::signal))
            {
                switch (measurementResult.type)
                {
                    case MeasurementSession::Type::frequencyResponse:
                        renderFrequencyResponse();
                        break;
                    case MeasurementSession::Type::harmonicAnalysis:
                        renderHarmonicAnalysis();
                        break;
                    case MeasurementSession::Type::compressionCurve:
                        renderCompressionCurve();
                        break;
                }
            }
            else
            {
                renderRawCapture();
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
    // Measurement rendering — one renderer per measurement type. Each clears
    // both plots and sets the axis labels before drawing, so switching between
    // measurement types never mixes stale data or mismatched axes.

    /** Frequency response: magnitude (top) + phase (bottom), log-X. */
    void renderFrequencyResponse() const
    {
        magPlot->setAxisLabels ("Frequency (Hz)", "Magnitude (dB)");
        magPlot->setXAxisLog (true);
        phasePlot->setAxisLabels ("Frequency (Hz)", "Phase (degrees)");
        phasePlot->setXAxisLog (true);

        auto& raw = measurementResult.freq.raw;

        // Empty-result guard: leave the plots untouched.
        if (raw.empty())
        {
            statusLabel->setText ("Measurement complete (no data)",
                                  juce::dontSendNotification);
            return;
        }

        statusLabel->setText ("Measurement complete: "
            + juce::String (static_cast<int> (raw.size()))
            + " frequency points", juce::dontSendNotification);

        // Magnitude curve (cyan).
        PlotWidget::Series magSeries;
        magSeries.name = "Magnitude";
        magSeries.colour = juce::Colours::cyan;
        magSeries.lineWidth = 2.0f;
        magSeries.x.reserve (raw.size());
        magSeries.y.reserve (raw.size());
        for (const auto& p : raw)
        {
            magSeries.x.push_back (static_cast<float> (p.frequency));
            magSeries.y.push_back (static_cast<float> (p.magnitudeDB));
        }

        magPlot->clear();
        magPlot->addSeries (std::move (magSeries));
        magPlot->repaint();

        // Phase curve (yellow).
        PlotWidget::Series phaseSeries;
        phaseSeries.name = "Phase";
        phaseSeries.colour = juce::Colours::yellow;
        phaseSeries.lineWidth = 2.0f;
        phaseSeries.x.reserve (raw.size());
        phaseSeries.y.reserve (raw.size());
        for (const auto& p : raw)
        {
            phaseSeries.x.push_back (static_cast<float> (p.frequency));
            phaseSeries.y.push_back (static_cast<float> (p.phaseDeg));
        }

        phasePlot->clear();
        phasePlot->addSeries (std::move (phaseSeries));
        phasePlot->repaint();
    }

    /** Harmonic analysis: magnitudes of the first tone (order 1..N), THD in
     *  the status label. Linear-X, bottom plot stays empty. */
    void renderHarmonicAnalysis() const
    {
        magPlot->setAxisLabels ("Harmonic Order", "Magnitude (dB)");
        magPlot->setXAxisLog (false);
        phasePlot->setAxisLabels ("Harmonic Order", "Magnitude (dB)");
        phasePlot->setXAxisLog (false);
        phasePlot->clear();
        phasePlot->repaint();

        auto& tones = measurementResult.harmonic.tones;

        // Empty-result guard: leave the plots untouched.
        if (tones.empty())
        {
            statusLabel->setText ("Measurement complete (no data)",
                                  juce::dontSendNotification);
            return;
        }

        // Render the first tone's spectrum: fundamental as order 1, then each
        // measured harmonic.
        const auto& tone = tones[0];

        PlotWidget::Series harmSeries;
        harmSeries.name = "Harmonics";
        harmSeries.colour = juce::Colours::cyan;
        harmSeries.lineWidth = 2.0f;
        harmSeries.x.push_back (1.0f);
        harmSeries.y.push_back (static_cast<float> (tone.fundamentalDB));
        for (const auto& h : tone.harmonics)
        {
            harmSeries.x.push_back (static_cast<float> (h.order));
            harmSeries.y.push_back (static_cast<float> (h.magnitudeDB));
        }

        magPlot->clear();
        magPlot->addSeries (std::move (harmSeries));
        magPlot->repaint();

        statusLabel->setText ("THD: " + juce::String (tone.thdPercent, 2)
            + "% @ " + juce::String (tone.fundamentalFreq, 0)
            + " Hz", juce::dontSendNotification);
    }

    /** Compression curve: input vs output level; fitted parameters in the
     *  status label. Linear-X, bottom plot stays empty. */
    void renderCompressionCurve() const
    {
        magPlot->setAxisLabels ("Input (dB)", "Output (dB)");
        magPlot->setXAxisLog (false);
        phasePlot->setAxisLabels ("Input (dB)", "Gain Reduction (dB)");
        phasePlot->setXAxisLog (false);
        phasePlot->clear();
        phasePlot->repaint();

        auto& curve = measurementResult.compression.curve;

        // Empty-result guard: leave the plots untouched.
        if (curve.empty())
        {
            statusLabel->setText ("Measurement complete (no data)",
                                  juce::dontSendNotification);
            return;
        }

        PlotWidget::Series curveSeries;
        curveSeries.name = "Compression";
        curveSeries.colour = juce::Colours::cyan;
        curveSeries.lineWidth = 2.0f;
        curveSeries.x.reserve (curve.size());
        curveSeries.y.reserve (curve.size());
        for (const auto& p : curve)
        {
            curveSeries.x.push_back (static_cast<float> (p.inputDB));
            curveSeries.y.push_back (static_cast<float> (p.outputDB));
        }

        magPlot->clear();
        magPlot->addSeries (std::move (curveSeries));
        magPlot->repaint();

        const auto& f = measurementResult.compression.fitted;
        statusLabel->setText ("Ratio " + juce::String (f.ratio, 2) + ":1, threshold "
            + juce::String (f.thresholdDB, 1) + " dB, knee "
            + juce::String (f.kneeDB, 1) + " dB", juce::dontSendNotification);
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
    /** Start a measurement from the GUI.  Runs on the message thread.
     *
     *  All three measurement types are wired end-to-end: handleCommand
     *  executes synchronously here (the GUI freezes while measuring — a known
     *  Pro-Q 4 thread-affinity requirement), and the result is pushed to the
     *  plots via measurementCompleteCallback → handleAsyncUpdate.
     */
    void startMeasurement (const juce::String& type)
    {
        if (! pluginLoaded)
        {
            statusLabel->setText ("Load a plugin first", juce::dontSendNotification);
            return;
        }

        // Re-entry guard: the measurement runs synchronously on the message
        // thread.  With the message loop yielding during the sweep (SweepRunner)
        // the button stays clickable, so ignore a second click while running.
        if (measurementInProgress)
            return;

        const auto sourceStr = selectedSourceString();

        // Source-specific prerequisites.
        if (sourceStr == juce::String (Protocol::Source::file)
            && ! selectedFile.existsAsFile())
        {
            statusLabel->setText ("Choose an audio file first", juce::dontSendNotification);
            return;
        }

        measurementInProgress = true;
        measureFreqButton->setEnabled (false);
        measureHarmonicButton->setEnabled (false);
        measureCompressionButton->setEnabled (false);
        statusLabel->setText ("Measuring...", juce::dontSendNotification);

        // Build the measure command with the source field + source params.
        juce::String json = R"({"cmd":"measure","type":")" + type
                          + R"(","source":")" + sourceStr + R"(")";
        if (sourceStr == juce::String (Protocol::Source::file))
            json += R"(,"path":)" + juce::JSON::toString (selectedFile.getFullPathName());
        else if (sourceStr == juce::String (Protocol::Source::noise))
        {
            const double duration = noiseDurationBox->getText().getDoubleValue();
            if (duration > 0.0)
                json += R"(,"duration":)" + juce::String (duration);
        }
        json += "}";

        // Synchronous on the message thread (no IPC involved from the GUI).
        auto response = commandParser->handleCommand (json);

        // Surface errors (e.g. "measurement failed").  Success is reported by
        // handleAsyncUpdate once the result has been rendered to the plots.
        // On failure the completion callback never fires, so re-arm the
        // guard here (otherwise the measure buttons stay locked forever).
        if (auto* obj = juce::JSON::parse (response).getDynamicObject())
        {
            if (! (bool) obj->getProperty (Protocol::Status::ok))
            {
                measurementInProgress = false;
                measureFreqButton->setEnabled (true);
                measureHarmonicButton->setEnabled (true);
                measureCompressionButton->setEnabled (true);
                statusLabel->setText ("Measure failed: "
                    + obj->getProperty (Protocol::Status::error).toString(),
                    juce::dontSendNotification);
            }
        }
    }

    //==============================================================================
    // Input-source helpers.

    MeasurementSession::Source selectedSource() const
    {
        switch (sourceCombo->getSelectedId())
        {
            case 2: return MeasurementSession::Source::file;
            case 3: return MeasurementSession::Source::noise;
            case 4: return MeasurementSession::Source::dynamic;
            default: return MeasurementSession::Source::signal;
        }
    }

    juce::String selectedSourceString() const
    {
        switch (selectedSource())
        {
            case MeasurementSession::Source::file:    return juce::String (Protocol::Source::file);
            case MeasurementSession::Source::noise:   return juce::String (Protocol::Source::noise);
            case MeasurementSession::Source::dynamic: return juce::String (Protocol::Source::dynamic);
            default: return juce::String (Protocol::Source::signal);
        }
    }

    void updateSourceControls()
    {
        const bool isFile  = selectedSource() == MeasurementSession::Source::file;
        const bool isNoise = selectedSource() == MeasurementSession::Source::noise;
        chooseFileButton->setVisible (isFile);
        noiseDurationLabel->setVisible (isNoise);
        noiseDurationBox->setVisible (isNoise);
    }

    /** Drop the previous measurement result when the source changes, so the
     *  plots never mix stale results from a different input source. */
    void clearMeasurementDisplay()
    {
        hasMeasurement = false;
        measurementResult = MeasurementResults{};
        magPlot->clear();
        magPlot->repaint();
        phasePlot->clear();
        phasePlot->repaint();
    }

    void chooseAudioFile()
    {
        // Kept alive as a member: the native dialog runs async on the message
        // thread and the chooser must outlive this call.
        fileChooser = std::make_unique<juce::FileChooser> (
            "Select audio file",
            juce::File::getCurrentWorkingDirectory(),
            "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg", true);
        fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                  | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc)
            {
                auto f = fc.getResult();
                if (f.existsAsFile())
                {
                    selectedFile = f;
                    statusLabel->setText ("File: " + f.getFileName(),
                                          juce::dontSendNotification);
                }
            });
    }

    /** Raw capture (non-signal source): clear both plots and show the
     *  capture summary in the status bar (analysis is phase 4). */
    void renderRawCapture() const
    {
        magPlot->clear();
        magPlot->repaint();
        phasePlot->clear();
        phasePlot->repaint();

        statusLabel->setText ("Raw capture: " + juce::String (measurementResult.rawSamples)
            + " samples @ " + juce::String (measurementResult.rawSampleRate, 0) + " Hz",
            juce::dontSendNotification);
    }

    //==============================================================================
    void openEditorWindowFor (std::unique_ptr<juce::AudioPluginInstance> instance,
                               const juce::String& name)
    {
        unloadCurrentPlugin();
        pluginLoaded = false;

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
            // Place the editor window next to the main window (right of it,
            // or below when the screen is too narrow) so it never covers the
            // right panel where the measurement plots live.
            editorWindow = std::make_unique<PluginEditorWindow> (
                std::move (instance), editor, name,
                [this] { onPluginWindowClosed(); },
                getScreenBounds());

            // Wire the loaded plugin into the IPC pipeline
            auto* rawInstance = editorWindow->getPluginInstance();
            commandParser->setPluginInstance (rawInstance);
            measurementSession->setPluginInstance (rawInstance);
            pluginLoaded = true;

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
        pluginLoaded = false;
        commandParser->setPluginInstance (nullptr);
        measurementSession->setPluginInstance (nullptr);
        statusLabel->setText ("Ready", juce::dontSendNotification);
        CRASH_LOG_INFO ("Editor window closed", {});
    }

    //==============================================================================
    // Member destruction order = REVERSE declaration order:
    //   threadPool is destroyed first → joins all background jobs,
    //   then UI members, then pluginManager, then IPC members,
    //   then atomics / mutex.
    // The explicit cleanup in ~MainContentComponent (cancel→shutdown→nullThreadPool)
    // runs BEFORE natural destruction, so IPC members are guaranteed alive
    // during shutdown.

    // --- IPC pipeline (declared before pool/mgr for reverse-destruction order) ---
    std::unique_ptr<MeasurementSession> measurementSession;
    std::unique_ptr<CommandParser> commandParser;
    std::unique_ptr<PipeServer> pipeServer;

    // Measurement result held for UI rendering (T6/T8: all three types)
    MeasurementResults measurementResult;
    bool hasMeasurement = false;

    // --- Core ---
    std::unique_ptr<PluginManager> pluginManager;
    std::unique_ptr<juce::ThreadPool> threadPool;

    std::unique_ptr<juce::ListBox> pluginListBox;
    std::unique_ptr<juce::Label> statusLabel;
    std::unique_ptr<juce::TextButton> scanButton;

    // Measurement control row (right panel, above the plots)
    std::unique_ptr<juce::TextButton> measureFreqButton;
    std::unique_ptr<juce::TextButton> measureHarmonicButton;
    std::unique_ptr<juce::TextButton> measureCompressionButton;

    // Input source selector + source-specific controls
    std::unique_ptr<juce::ComboBox> sourceCombo;
    std::unique_ptr<juce::TextButton> chooseFileButton;
    std::unique_ptr<juce::Label> noiseDurationLabel;
    std::unique_ptr<juce::TextEditor> noiseDurationBox;
    std::unique_ptr<juce::FileChooser> fileChooser;  // kept alive during async launch
    juce::File selectedFile;
    bool pluginLoaded = false;

    // Re-entry guard: a measurement runs synchronously on the message thread
    // (~5 s), so a second click must be ignored while one is in progress.
    bool measurementInProgress = false;

    // Right-panel frequency-response plots
    std::unique_ptr<PlotWidget> magPlot;
    std::unique_ptr<PlotWidget> phasePlot;

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
