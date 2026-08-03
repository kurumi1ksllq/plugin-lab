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
#include "analysis/GainReduction.h"
#include "scan/ScanEngine.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
                             private juce::AsyncUpdater,
                             private juce::ChangeListener,
                             private juce::Timer
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

        // 清除黑名单并全量重扫入口（计划步骤 4，风险 R4/R7）：一次挂起/崩溃被
        // 黑名单的插件可能已被修复——此按钮清空黑名单（含缓存持久化）后重扫。
        clearBlacklistButton.reset (new juce::TextButton ("Clear BL"));
        clearBlacklistButton->onClick = [this]
        {
            pluginManager->clearBlacklist();
            statusLabel->setText ("Blacklist cleared - rescanning", juce::dontSendNotification);
            scanPlugins();
        };
        addAndMakeVisible (clearBlacklistButton.get());

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

        // GR header (T4.4): GR_dB(t) timeline for dynamic/file sources.
        // Visible only while the GR measurement type is selected; shows the
        // partial timeline live during the sweep and the full timeline on
        // completion.
        grPlot.reset (new PlotWidget());
        grPlot->setAxisLabels ("Time (s)", "Gain Reduction (dB)");
        grPlot->setXAxisLog (false);
        grPlot->setAutoFitY (true);
        grPlot->setVisible (false);
        addAndMakeVisible (grPlot.get());

        // Measurement control row (right panel, above the plots).
        // Frequency response is wired end-to-end; harmonic/compression are
        // placeholders until phase 2 (they only report "not wired yet").
        measureFreqButton.reset (new juce::TextButton ("Freq Response"));
        measureFreqButton->onClick = [this]
        {
            currentMeasureType = juce::String (Protocol::MeasureType::freq);
            updateScanEstimate();
            startMeasurement (currentMeasureType);
        };
        addAndMakeVisible (measureFreqButton.get());

        measureHarmonicButton.reset (new juce::TextButton ("Harmonic"));
        measureHarmonicButton->onClick = [this]
        {
            currentMeasureType = juce::String (Protocol::MeasureType::harmonic);
            updateScanEstimate();
            startMeasurement (currentMeasureType);
        };
        addAndMakeVisible (measureHarmonicButton.get());

        measureCompressionButton.reset (new juce::TextButton ("Compression"));
        measureCompressionButton->onClick = [this]
        {
            currentMeasureType = juce::String (Protocol::MeasureType::compression);
            updateScanEstimate();
            startMeasurement (currentMeasureType);
        };
        addAndMakeVisible (measureCompressionButton.get());

        // GR timeline (T4.4): gain reduction over time for dynamic/file
        // sources — the GR header plot shows it live while measuring.
        measureGRButton.reset (new juce::TextButton ("GR"));
        measureGRButton->onClick = [this]
        {
            currentMeasureType = juce::String (Protocol::MeasureType::grTimeline);
            updateScanEstimate();
            updateGRPlotVisibility();
            startMeasurement (currentMeasureType);
        };
        addAndMakeVisible (measureGRButton.get());

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
            updateGRPlotVisibility();
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

        // Parameter-scan panel (right panel, row 3): sweep one plugin
        // parameter across the values listed in the editor — one measurement
        // round per value (T3.3).
        scanParamCombo.reset (new juce::ComboBox ("ScanParam"));
        scanParamCombo->setEnabled (false);  // filled when a plugin is loaded
        addAndMakeVisible (scanParamCombo.get());

        scanValuesBox.reset (new juce::TextEditor ("ScanValues"));
        scanValuesBox->setText ("0.0,0.25,0.5,0.75,1.0");
        scanValuesBox->onTextChange = [this] { updateScanEstimate(); };
        addAndMakeVisible (scanValuesBox.get());

        scanEstimateLabel.reset (new juce::Label ("ScanEstimate", "≈ -- s"));
        scanEstimateLabel->setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (scanEstimateLabel.get());

        scanRunButton.reset (new juce::TextButton ("Scan"));
        scanRunButton->onClick = [this] { startScan(); };
        addAndMakeVisible (scanRunButton.get());

        stopScanButton.reset (new juce::TextButton ("Stop"));
        stopScanButton->onClick = [this]
        {
            // The scan runs synchronously on the message thread; SweepRunner
            // yields the loop per block so this click is processed mid-scan.
            // Cancelling the session aborts the in-flight round and ScanEngine
            // stops at the next round boundary.
            if (measurementSession)
                measurementSession->cancel();
        };
        stopScanButton->setEnabled (false);
        addAndMakeVisible (stopScanButton.get());

        scanProgressLabel.reset (new juce::Label ("ScanProgress", ""));
        scanProgressLabel->setFont (juce::FontOptions (12.0f));
        addAndMakeVisible (scanProgressLabel.get());

        pluginManager = std::make_shared<PluginManager>();
        scanAlive = std::make_shared<std::atomic<bool>> (true);
        loadAlive = std::make_shared<std::atomic<bool>> (true);

        // 增量 UI 刷新（计划步骤 2）：KnownPluginList : ChangeBroadcaster，每个
        // 新插件 addType 发一次 change；内部 AsyncUpdater 自动投递消息线程并合并
        // （≤50ms 天然节流），回调里 updateContent 即可让插件逐条出现，无需手写
        // 转发。退订见析构（必须先于成员析构）。
        pluginManager->getKnownPlugins().addChangeListener (this);

        // --- IPC pipeline setup ---
        measurementSession = std::make_unique<MeasurementSession>();
        measurementSession->setSampleRate (48000.0);
        measurementSession->setBlockSize (512);

        // T4.4 live GR header: accumulate the dry/wet blocks streamed by
        // SweepRunner and refresh the partial GR timeline at most every
        // 50 ms while a GR measurement runs. Runs on the message thread
        // (both the GUI and IPC paths execute measurements there).
        measurementSession->setBlockCallback ([this] (float,
                                                       const juce::AudioBuffer<float>& dryBlock,
                                                       const juce::AudioBuffer<float>& wetBlock)
        {
            if (! grLiveActive)
                return;

            appendLiveBlock (liveDry, dryBlock);
            appendLiveBlock (liveWet, wetBlock);

            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastGRUpdateMs < 50)
                return;
            lastGRUpdateMs = now;

            updateLiveGR();
        });

        commandParser = std::make_unique<CommandParser>();
        commandParser->setPluginManager (pluginManager.get());
        commandParser->setSession (measurementSession.get());
        commandParser->setLoadPluginCallback ([this] (const juce::PluginDescription& d)
        {
            loadPluginByDescription (d);
        });
        commandParser->setStatusCallback ([this] (const juce::String& s)
        {
            // Scan round progress goes to the scan progress label; everything
            // else updates the main status bar.
            if (s.startsWith ("scan round"))
                scanProgressLabel->setText (s, juce::dontSendNotification);
            else
                statusLabel->setText (s, juce::dontSendNotification);
        });
        commandParser->setMeasurementCompleteCallback ([this] (const MeasurementResults& r)
        {
            measurementResult = r;
            hasMeasurement = true;
            triggerAsyncUpdate();
        });
        commandParser->setScanCompleteCallback ([this] (const ScanEngine::ScanResult& r)
        {
            lastScanResult = r;
            paramScanUpdatePending = true;
            triggerAsyncUpdate();
        });

        pipeServer = std::make_unique<PipeServer>();
        pipeServer->setCommandHandler ([this] (const juce::String& cmd)
        {
            return commandParser->handleCommand (cmd);
        });
        pipeServer->startup();
        setSize (1400, 850);

        updateScanEstimate();

        // Start initial scan on a background thread
        scanPlugins();
    }

    ~MainContentComponent() override
    {
        // 0. Unsubscribe from KnownPluginList changes FIRST — the scan thread's
        //    addType fires change messages; after this point no callback touches
        //    pluginListBox (must precede every member tear-down, plan step 2).
        if (pluginManager)
            pluginManager->getKnownPlugins().removeChangeListener (this);

        // 1. Cancel any in-progress measurement (returns within 1 block).
        if (measurementSession)
            measurementSession->cancel();

        // 2. Unload current plugin + close editor window.
        unloadCurrentPlugin();

        // 3. Shut down the pipe server (joins IPC thread).
        if (pipeServer)
            pipeServer->shutdown();

        // 4. Mark the message-thread completion lambdas dead, then abandon the
        //    dedicated scan/load threads WITHOUT joining (P0, plan step 0): a
        //    plugin DLL hung in DllMain/InitDll/GetPluginFactory can never be
        //    terminated in-process, and joining it blocks process exit forever.
        //    Each worker only touches state it owns (shared_ptr), and its final
        //    completion lambda runs on the message thread where the alive check
        //    is serialized with this destructor — so an abandoned worker's late
        //    return never touches destroyed members.
        *scanAlive = false;
        *loadAlive = false;

        // Join a worker only if it has already signalled completion; otherwise
        // it is (or may be) stuck inside a plugin DLL — abandon it via release()
        // (never join → no std::terminate). The OS thread keeps running until
        // the DLL returns; its callAsync is then discarded by the message queue.
        // The leaked std::thread object is bounded (≤3/session by the step-4
        // watchdog hang cap) and documented in STATUS.md.
        if (scanThread && scanThread->joinable())
        {
            if (scanOutcome && scanOutcome->ready.load())
                scanThread->join();
            else
                scanThread.release();
        }
        if (loadThread && loadThread->joinable())
        {
            if (loadOutcome && loadOutcome->ready.load())
                loadThread->join();
            else
                loadThread.release();
        }

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
            clearBlacklistButton->setBounds (header.removeFromRight (62).reduced (2));
            statusLabel->setBounds (header);
            pluginListBox->setBounds (leftPanel);
        }

        // Right panel: measurement controls on top (source/type row +
        // source-parameter row + scan row), then the magnitude plot (~65%)
        // and phase plot (~35%) below.
        auto plotArea = area;
        auto controls = plotArea.removeFromTop (122).reduced (0, 2);
        {
            const int gap = 6;

            // Row 1: source combo (right) + the four measure buttons.
            auto row1 = controls.removeFromTop (30);
            const int comboW = 110;
            sourceCombo->setBounds (row1.removeFromRight (comboW));
            row1.removeFromRight (gap);
            const int btnW = (row1.getWidth() - 3 * gap) / 4;
            measureFreqButton->setBounds (row1.removeFromLeft (btnW));
            row1.removeFromLeft (gap);
            measureHarmonicButton->setBounds (row1.removeFromLeft (btnW));
            row1.removeFromLeft (gap);
            measureCompressionButton->setBounds (row1.removeFromLeft (btnW));
            row1.removeFromLeft (gap);
            measureGRButton->setBounds (row1);

            // Row 2: source-specific parameters (file chooser / noise duration).
            controls.removeFromTop (4);
            auto row2 = controls.removeFromTop (26);
            chooseFileButton->setBounds (row2.removeFromLeft (120).reduced (0, 2));
            row2.removeFromLeft (gap);
            noiseDurationLabel->setBounds (row2.removeFromLeft (70).reduced (0, 2));
            noiseDurationBox->setBounds (row2.removeFromLeft (56).reduced (0, 2));

            // Row 3: parameter scan — param combo / values / estimate /
            // Scan + Stop buttons / live round progress.
            controls.removeFromTop (4);
            auto row3 = controls.removeFromTop (26);
            scanParamCombo->setBounds (row3.removeFromLeft (150).reduced (0, 2));
            row3.removeFromLeft (gap);
            scanValuesBox->setBounds (row3.removeFromLeft (170).reduced (0, 2));
            row3.removeFromLeft (gap);
            scanEstimateLabel->setBounds (row3.removeFromLeft (70).reduced (0, 2));
            row3.removeFromLeft (gap);
            scanRunButton->setBounds (row3.removeFromLeft (60).reduced (0, 2));
            row3.removeFromLeft (gap);
            stopScanButton->setBounds (row3.removeFromLeft (56).reduced (0, 2));
            row3.removeFromLeft (gap);
            scanProgressLabel->setBounds (row3);
        }

        // GR header strip (T4.4): takes the top of the plot area while the
        // GR timeline type is selected; the freq plots shrink below it.
        juce::Rectangle<int> grArea;
        if (grPlot->isVisible())
            grArea = plotArea.removeFromTop (110);

        auto phaseArea = plotArea.removeFromBottom (juce::roundToInt (plotArea.getHeight() * 0.35f));
        magPlot->setBounds (plotArea);
        phasePlot->setBounds (phaseArea);

        if (grPlot->isVisible())
            grPlot->setBounds (grArea.reduced (0, 2));
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
    /** KnownPluginList : ChangeBroadcaster callback (plan step 2 — incremental UI).
     *  Runs on the message thread (ChangeBroadcaster posts via its internal
     *  AsyncUpdater, auto-merged ≤50ms), so every newly discovered plugin refreshes
     *  the list immediately instead of waiting for the whole scan to finish. */
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        try
        {
            // 节流（未响应修复）：addType 每插件触发一次 change；Debug 下每次
            // updateContent+repaint 开销大，扫描期逐插件刷新会淹没消息线程
            // （用户操作排队 → Windows 判"未响应"）。经 AsyncUpdater 合并
            // （≤50ms 一次刷新），扫描期消息线程负担降一个数量级。
            pluginListUpdatePending = true;
            triggerAsyncUpdate();
        }
        catch (...)
        {
            CRASH_LOG_ERR ("Plugin list change", "exception caught");
        }
    }

    //==============================================================================
    /** Called on the message thread when a background job has finished and
     *  called triggerAsyncUpdate().  Processes whichever update is pending:
     *  scan completion or plugin-load completion.
     */
    void handleAsyncUpdate() override
    {
        // Incremental list refresh (throttled via AsyncUpdater merge — see
        // changeListenerCallback; processed before the scan-complete block so
        // the list is fresh when a plugin-load selects a row).
        if (pluginListUpdatePending)
        {
            pluginListUpdatePending = false;
            try
            {
                pluginListBox->updateContent();
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Plugin list update", "exception caught");
            }
        }

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

        // Process the parameter-scan result (before the measurement block —
        // the same ordering as the plugin-scan update above).
        if (paramScanUpdatePending.exchange (false))
        {
            try
            {
                scanInProgress = false;
                measurementInProgress = false;
                scanRunButton->setEnabled (true);
                stopScanButton->setEnabled (false);
                measureFreqButton->setEnabled (true);
                measureHarmonicButton->setEnabled (true);
                measureCompressionButton->setEnabled (true);
                measureGRButton->setEnabled (true);
                renderScanResult();
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Scan UI update", "exception caught");
                scanInProgress = false;
                measurementInProgress = false;
                scanRunButton->setEnabled (true);
                stopScanButton->setEnabled (false);
            }
        }

        // Render the measurement result on the right-panel plots.
        if (hasMeasurement)
        {
            hasMeasurement = false;

            // Measurement finished: re-arm the re-entry guard and re-enable
            // the measure buttons (empty-result path below also returns here).
            measurementInProgress = false;
            grLiveActive = false;
            measureFreqButton->setEnabled (true);
            measureHarmonicButton->setEnabled (true);
            measureCompressionButton->setEnabled (true);
            measureGRButton->setEnabled (true);

            // Non-signal sources are captured raw — show the capture summary
            // instead of analysis plots (analysis is phase 4). Exception: the
            // GR timeline analysis (dynamic/file) renders its header plot.
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

                    // Unreachable — the parser rejects gr_timeline for
                    // Source::signal (defensive, keeps the switch exhaustive).
                    case MeasurementSession::Type::grTimeline:
                        break;
                }
            }
            else if (measurementResult.type == MeasurementSession::Type::grTimeline)
            {
                renderGRTimeline (measurementResult.gr, buildGRStatusText (measurementResult.tau));
            }
            else
            {
                renderRawCapture();
            }
        }
    }

private:
    //==============================================================================
    // Background-thread completion channels (plan step 0). Each dedicated worker
    // writes ONLY into these shared_ptr-owned structs (never into members of
    // MainContentComponent directly), so an abandoned thread's late completion
    // is safe: the message-thread notify lambda checks the alive flag (serialized
    // with the destructor) before copying results into this->*.

    struct ScanOutcome
    {
        std::atomic<bool> done { false };   // true = scan completed, false = crash
        std::atomic<bool> ready { false };  // worker finished; join() is then safe
    };

    struct LoadOutcome
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::String name;
        std::atomic<bool> ready { false };
    };

    //==============================================================================
    std::atomic<bool> scanDone { false };
    std::atomic<bool> scanRunning { false };
    std::atomic<bool> loadingRunning { false };
    std::atomic<int> scanGeneration { 0 };   // stale-completion guard for scans
    std::atomic<int> loadGeneration { 0 };   // stale-completion guard for loads
    std::mutex listLock;

    // IPC between background threads (write) and handleAsyncUpdate (read).
    // Only one job per category runs at a time (scanRunning / loadingRunning
    // gates), so a single slot per category is safe.
    std::atomic<bool> scanUpdatePending { false };
    std::atomic<bool> loadUpdatePending { false };
    bool pluginListUpdatePending = false;   // 消息线程专用（增量列表刷新，AsyncUpdater 合并节流）
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
    /** 扫描看门狗（计划步骤 4）：消息线程 Timer 轮询 PluginManager 的扫描进度。
     *  若 progress 在 kScanHangTimeoutMs 内无任何变化 → 判定某个插件 DLL 挂起
     *  （scanNextFile 永不返回，无法进程内终止）→ 黑名单当前文件（立即持久化，
     *  重启不重挂）+ abandon 卡死线程 + 复位 scanRunning（UI 可操作）+ 挂起计数。
     *  挂起达到 kMaxScanHangs 后停止自动重扫（每次挂起泄漏一线程+锁一 DLL）。
     *  扫描正常完成时 notify 会 stopTimer()。 */
    void timerCallback() override
    {
        if (! pluginManager->isScanRunning())
        {
            stopTimer();
            return;
        }

        const auto now = juce::Time::getMillisecondCounter();
        if (now - pluginManager->getLastScanProgressTimeMs() > PluginManager::kScanHangTimeoutMs)
        {
            const bool capReached = pluginManager->handleScanHang();
            stopTimer();

            // M3（verifier）：abandon 后补 endScan()——卡死的扫描线程永不执行
            // endScan，否则 PluginManager.scanRunning 恒 true，getScanStatus 快照
            // 失真（running=true 但 UI 已显示 interrupted）。
            pluginManager->endScan();

            // abandon 卡死的扫描线程：绝不 join（与析构同语义）。
            if (scanThread && scanThread->joinable())
                scanThread.release();

            scanRunning = false;
            scanDone = false;
            scanButton->setEnabled (true);
            statusLabel->setText (capReached
                ? "Scan stopped: too many hung plugins (see crash log)"
                : "Scan interrupted: hung plugin blacklisted",
                juce::dontSendNotification);
            CRASH_LOG_WARN ("Watchdog",
                capReached ? "hang cap reached - further scans disabled"
                           : "scan thread abandoned after timeout");
        }
    }

    //==============================================================================
    void scanPlugins()
    {
        // scanDone 不再拦截重扫（计划步骤 2）：重扫走增量（cacheIsCurrent 跳过
        // 已最新插件，秒级完成），且步骤 4 的"清除黑名单并重扫"入口依赖可重复扫描。
        if (scanRunning.exchange (true))
            return;

        statusLabel->setText ("Scanning VST3 plugins...", juce::dontSendNotification);
        scanButton->setEnabled (false);

        // Reap the previous scan thread (gated: it already completed — a hung
        // scan would still hold scanRunning=true and the gate would have
        // returned above), then spawn a fresh dedicated one-shot thread.
        if (scanThread && scanThread->joinable())
            scanThread->join();

        const int generation = ++scanGeneration;
        scanOutcome = std::make_shared<ScanOutcome>();
        auto pm = pluginManager;
        auto alive = scanAlive;
        auto out = scanOutcome;

        // Completion is delivered via the message thread where the alive check
        // is serialized with the destructor — a late callback from an abandoned
        // thread never touches destroyed members. The generation check discards
        // stale callbacks if a newer scan has been started.
        auto notify = [alive, out, this, generation]
        {
            if (! alive->load() || scanGeneration.load() != generation)
                return;

            stopTimer();   // 看门狗：扫描正常完成，停止轮询

            scanDone = out->done.load();
            scanRunning = false;
            {
                std::lock_guard<std::mutex> lock (listLock);
                scannedCount = pluginManager->getKnownPlugins().getNumTypes();
            }
            scanUpdatePending = true;
            triggerAsyncUpdate();
        };

        scanThread = std::make_unique<std::thread> ([pm, out, notify]() mutable
        {
#ifdef JUCE_WINDOWS
            // 后台扫描线程降优先级（步骤 6 启动卡顿修复）：插件 DLL 初始化可能
            // CPU 密集（UA 系列等，实测单插件峰值多核满载），普通优先级会与用户
            // 前台操作抢占 CPU 导致整机卡顿。降为 BELOW_NORMAL 后扫描让位于前台。
            ::SetThreadPriority (::GetCurrentThread (), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            bool ok = true;
            try
            {
                CRASH_LOG_INFO ("Scan start", {});
                pm->scanSystemDirectories();
                CRASH_LOG_INFO ("Scan done", {});
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Scan thread", "exception caught");
                ok = false;
            }

            out->done = ok;
            out->ready = true;
            juce::MessageManager::callAsync (notify);
        });

        // 看门狗（计划步骤 4）：扫描期间每 500ms 轮询进度，无进展超时则按挂起处理。
        startTimer (500);
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

        // Reap the previous load thread (gated by loadingRunning — it already
        // completed), then spawn a fresh dedicated one-shot thread.
        if (loadThread && loadThread->joinable())
            loadThread->join();

        const int generation = ++loadGeneration;
        loadOutcome = std::make_shared<LoadOutcome>();
        auto pm = pluginManager;
        auto alive = loadAlive;
        auto out = loadOutcome;

        auto notify = [alive, out, this, generation]
        {
            if (! alive->load() || loadGeneration.load() != generation)
                return;

            pendingInstance = std::move (out->instance);
            pendingName = out->name;
            loadUpdatePending = true;
            triggerAsyncUpdate();
        };

        loadThread = std::make_unique<std::thread> ([pm, desc, safeName, sr, bs, out, notify]() mutable
        {
#ifdef JUCE_WINDOWS
            // 同扫描线程：插件构造可能 CPU 密集/耗时，降优先级避免抢占用户前台。
            ::SetThreadPriority (::GetCurrentThread (), THREAD_PRIORITY_BELOW_NORMAL);
#endif
            try
            {
                auto instance = pm->loadPlugin (desc, sr, bs);
                out->instance = std::move (instance);
                out->name = safeName;
            }
            catch (...)
            {
                CRASH_LOG_ERR ("Load thread", "exception caught for " + safeName);
                out->instance.reset();      // nullptr → openEditorWindowFor shows failure
                out->name = safeName;
            }

            out->ready = true;
            juce::MessageManager::callAsync (notify);
        });
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
        measureGRButton->setEnabled (false);
        statusLabel->setText ("Measuring...", juce::dontSendNotification);

        // T4.4 live GR header: accumulate the streamed dry/wet blocks and
        // show the partial timeline while a GR measurement runs.
        grLiveActive = (type == juce::String (Protocol::MeasureType::grTimeline));
        if (grLiveActive)
        {
            liveDry = juce::AudioBuffer<float>();
            liveWet = juce::AudioBuffer<float>();
            lastGRUpdateMs = 0;
            grPlot->clear();
            grPlot->repaint();
        }

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
        // NOTE: JSON::parse returns a temporary var — the DynamicObject must
        // be read through a persistent local, otherwise obj dangles after the
        // full expression and getProperty() is a use-after-free (0xc0000005).
        const auto responseVar = juce::JSON::parse (response);
        if (auto* obj = responseVar.getDynamicObject())
        {
            if (! (bool) obj->getProperty (Protocol::Status::ok))
            {
                measurementInProgress = false;
                grLiveActive = false;
                measureFreqButton->setEnabled (true);
                measureHarmonicButton->setEnabled (true);
                measureCompressionButton->setEnabled (true);
                measureGRButton->setEnabled (true);
                statusLabel->setText ("Measure failed: "
                    + obj->getProperty (Protocol::Status::error).toString(),
                    juce::dontSendNotification);
            }
        }
    }

    //==============================================================================
    // Parameter-scan helpers (T3.3).

    /** Fill the scan parameter combo from the loaded plugin (name shown,
     *  stable ID stored in scanParamIds). */
    void fillScanParamCombo (juce::AudioPluginInstance* inst)
    {
        scanParamCombo->clear();
        scanParamIds.clear();

        if (inst == nullptr)
        {
            scanParamCombo->setEnabled (false);
            return;
        }

        auto& params = inst->getParameters();
        for (int i = 0; i < params.size(); ++i)
        {
            auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*> (params[i]);
            scanParamIds.push_back (hosted != nullptr
                                        ? hosted->getParameterID()
                                        : juce::String (i));
            scanParamCombo->addItem (params[i]->getName (128), i + 1);
        }

        if (scanParamCombo->getNumItems() > 0)
        {
            scanParamCombo->setSelectedId (1);
            scanParamCombo->setEnabled (true);
        }
    }

    /** Start a parameter scan from the GUI. Runs synchronously on the
     *  message thread (mirroring startMeasurement); the result arrives via
     *  scanCompleteCallback → handleAsyncUpdate. */
    void startScan()
    {
        if (! pluginLoaded || scanParamCombo->getSelectedId() <= 0)
        {
            statusLabel->setText ("Load a plugin first", juce::dontSendNotification);
            return;
        }

        // Re-entry guard (shared with measurements — both run synchronously
        // on the message thread and must never overlap).
        if (measurementInProgress || scanInProgress)
            return;

        const auto sourceStr = selectedSourceString();

        // Source-specific prerequisites (same as the measure flow).
        if (sourceStr == juce::String (Protocol::Source::file)
            && ! selectedFile.existsAsFile())
        {
            statusLabel->setText ("Choose an audio file first", juce::dontSendNotification);
            return;
        }

        // Parse the comma-separated normalized values.
        std::vector<float> values;
        if (! parseScanValues (scanValuesBox->getText(), values))
        {
            statusLabel->setText ("Scan failed: invalid values (0..1)",
                                  juce::dontSendNotification);
            return;
        }

        scanInProgress = true;
        measurementInProgress = true;
        scanRunButton->setEnabled (false);
        stopScanButton->setEnabled (true);
        measureFreqButton->setEnabled (false);
        measureHarmonicButton->setEnabled (false);
        measureCompressionButton->setEnabled (false);
        measureGRButton->setEnabled (false);
        scanProgressLabel->setText ("", juce::dontSendNotification);
        statusLabel->setText ("Scanning...", juce::dontSendNotification);
        updateScanEstimate();

        const auto paramId = scanParamIds[static_cast<size_t> (scanParamCombo->getSelectedId() - 1)];

        // Build the scan command with the same source fields as measure.
        // NOTE: paramId comes from the plugin and must be JSON-escaped —
        // String::quoted() only wraps in quotes and breaks on embedded
        // quotes/backslashes, so use JSON::toString (same as the file path).
        juce::String json = R"({"cmd":"scan","type":")" + currentMeasureType
                          + R"(","param_id":)" + juce::JSON::toString (paramId)
                          + R"(,"values":[)";
        for (size_t i = 0; i < values.size(); ++i)
        {
            if (i > 0) json += ",";
            json += juce::String (values[i], 4);
        }
        json += R"(],"source":")" + sourceStr + R"(")";
        if (sourceStr == juce::String (Protocol::Source::file))
            json += R"(,"path":)" + juce::JSON::toString (selectedFile.getFullPathName());
        else if (sourceStr == juce::String (Protocol::Source::noise))
        {
            const double duration = noiseDurationBox->getText().getDoubleValue();
            if (duration > 0.0)
                json += R"(,"duration":)" + juce::String (duration);
        }
        json += "}";

        lastScanType = currentMeasureType;

        // Synchronous on the message thread (no IPC involved from the GUI).
        auto response = commandParser->handleCommand (json);

        // Surface errors. Success is reported by handleAsyncUpdate once the
        // scan result has been rendered; on failure the completion callback
        // never fires, so re-arm the guards and buttons here.
        // (Same dangling-temporary guard as startMeasurement — read the JSON
        //  through a persistent local, never a temporary var.)
        const auto responseVar = juce::JSON::parse (response);
        if (auto* obj = responseVar.getDynamicObject())
        {
            if (! (bool) obj->getProperty (Protocol::Status::ok))
            {
                measurementInProgress = false;
                scanInProgress = false;
                scanRunButton->setEnabled (true);
                stopScanButton->setEnabled (false);
                measureFreqButton->setEnabled (true);
                measureHarmonicButton->setEnabled (true);
                measureCompressionButton->setEnabled (true);
                measureGRButton->setEnabled (true);
                statusLabel->setText ("Scan failed: "
                    + obj->getProperty (Protocol::Status::error).toString(),
                    juce::dontSendNotification);
            }
        }
    }

    /** Parse a comma-separated list of normalized values (0..1). Returns
     *  false when no values are present or any value is out of range. */
    bool parseScanValues (const juce::String& text, std::vector<float>& out) const
    {
        out.clear();

        auto tokens = juce::StringArray::fromTokens (text, ",", "");
        for (const auto& token : tokens)
        {
            const auto trimmed = token.trim();
            if (trimmed.isEmpty())
                continue;

            const double v = trimmed.getDoubleValue();
            if (v < 0.0 || v > 1.0)
                return false;

            out.push_back (static_cast<float> (v));
        }

        return ! out.empty();
    }

    /** Recompute the "≈ N s" scan-duration estimate (values × per-round). */
    void updateScanEstimate()
    {
        std::vector<float> values;
        const int numValues = parseScanValues (scanValuesBox->getText(), values)
                                  ? static_cast<int> (values.size()) : 0;
        const double totalSeconds = static_cast<double> (numValues)
                                    * estimateSecondsPerRound();
        scanEstimateLabel->setText ("≈ " + juce::String (juce::roundToInt (totalSeconds))
                                    + " s", juce::dontSendNotification);
    }

    /** Rough seconds per scan round for the current source/type. */
    double estimateSecondsPerRound() const
    {
        switch (selectedSource())
        {
            case MeasurementSession::Source::noise:
                return juce::jmax (0.5, noiseDurationBox->getText().getDoubleValue());
            case MeasurementSession::Source::dynamic:
                return 2.0;
            case MeasurementSession::Source::file:
                return 5.0;  // input-file duration unknown — assume typical
            case MeasurementSession::Source::signal:
                break;
        }

        if (currentMeasureType == juce::String (Protocol::MeasureType::harmonic))
            return 3.0;
        if (currentMeasureType == juce::String (Protocol::MeasureType::compression))
            return 2.0;
        return 5.0;  // frequency-response sine sweep
    }

    /** Render the completed scan: for a frequency-response scan, one curve
     *  per family entry on each plot, coloured with the HSL palette. Other
     *  scan types are reported in the status bar (curve rendering is
     *  freq-first; harmonic/compression curves are phase 4+). */
    void renderScanResult()
    {
        const bool isFreq = (lastScanType == juce::String (Protocol::MeasureType::freq));

        if (! isFreq || lastScanResult.family.empty())
        {
            magPlot->clear();
            magPlot->repaint();
            phasePlot->clear();
            phasePlot->repaint();
            statusLabel->setText ("Scan complete: "
                + juce::String (static_cast<int> (lastScanResult.family.size()))
                + " runs" + (isFreq ? "" : " (curves only for frequency response)"),
                juce::dontSendNotification);
            return;
        }

        magPlot->setAxisLabels ("Frequency (Hz)", "Magnitude (dB)");
        magPlot->setXAxisLog (true);
        phasePlot->setAxisLabels ("Frequency (Hz)", "Phase (degrees)");
        phasePlot->setXAxisLog (true);
        magPlot->clear();
        phasePlot->clear();

        const int numCurves = static_cast<int> (lastScanResult.family.size());
        auto palette = PlotWidget::getPalette (numCurves);

        int drawn = 0;
        for (int i = 0; i < numCurves; ++i)
        {
            const auto& entry = lastScanResult.family[static_cast<size_t> (i)];
            if (entry.freq.raw.empty())
                continue;

            const juce::String curveName = entry.paramValueText.isNotEmpty()
                                               ? entry.paramValueText
                                               : juce::String (entry.paramValue, 3);

            PlotWidget::Series magSeries;
            magSeries.name = curveName;
            magSeries.colour = palette[static_cast<size_t> (i)];
            magSeries.lineWidth = 2.0f;
            magSeries.x.reserve (entry.freq.raw.size());
            magSeries.y.reserve (entry.freq.raw.size());
            for (const auto& p : entry.freq.raw)
            {
                magSeries.x.push_back (static_cast<float> (p.frequency));
                magSeries.y.push_back (static_cast<float> (p.magnitudeDB));
            }
            magPlot->addSeries (std::move (magSeries));

            PlotWidget::Series phaseSeries;
            phaseSeries.name = curveName;
            phaseSeries.colour = palette[static_cast<size_t> (i)];
            phaseSeries.lineWidth = 2.0f;
            phaseSeries.x.reserve (entry.freq.raw.size());
            phaseSeries.y.reserve (entry.freq.raw.size());
            for (const auto& p : entry.freq.raw)
            {
                phaseSeries.x.push_back (static_cast<float> (p.frequency));
                phaseSeries.y.push_back (static_cast<float> (p.phaseDeg));
            }
            phasePlot->addSeries (std::move (phaseSeries));

            ++drawn;
        }

        magPlot->repaint();
        phasePlot->repaint();

        statusLabel->setText ("Scan complete: " + juce::String (drawn)
            + " curves, " + juce::String (numCurves) + " runs",
            juce::dontSendNotification);
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
     *  plots never mix stale results from a different input source. Also
     *  clears any scan result. */
    void clearMeasurementDisplay()
    {
        hasMeasurement = false;
        measurementResult = MeasurementResults{};
        lastScanResult = ScanEngine::ScanResult{};
        scanProgressLabel->setText ("", juce::dontSendNotification);
        magPlot->clear();
        magPlot->repaint();
        phasePlot->clear();
        phasePlot->repaint();
        grPlot->clear();
        grPlot->repaint();
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
    // GR timeline (T4.4) — live header plot + completion rendering.

    /** Append one block to a live accumulation buffer (grows to fit). */
    static void appendLiveBlock (juce::AudioBuffer<float>& dst,
                                 const juce::AudioBuffer<float>& src)
    {
        if (src.getNumSamples() <= 0)
            return;

        const int oldSamples = dst.getNumSamples();
        const int newSamples = oldSamples + src.getNumSamples();
        const int numChans = juce::jmax (dst.getNumChannels(), src.getNumChannels());

        dst.setSize (numChans, newSamples, true, false, true);
        for (int ch = 0; ch < src.getNumChannels(); ++ch)
            dst.copyFrom (ch, oldSamples, src, ch, 0, src.getNumSamples());
    }

    /** Recompute the partial GR timeline from the accumulated dry/wet blocks
     *  and refresh the header plot (message thread, throttled to 50 ms). */
    void updateLiveGR()
    {
        if (liveDry.getNumSamples() == 0 || liveWet.getNumSamples() == 0)
            return;

        const double sr = measurementSession->getSampleRate();
        const int latency = (livePlugin != nullptr) ? livePlugin->getLatencySamples() : 0;
        renderGRTimeline (GainReduction::analyze (liveDry, liveWet, sr, latency), {});
    }

    /** Render a GR timeline (live partial or final) on the header plot. The
     *  status label is only touched when statusMsg is non-empty. */
    void renderGRTimeline (const GainReduction::Result& gr,
                           const juce::String& statusMsg) const
    {
        grPlot->setAxisLabels ("Time (s)", "Gain Reduction (dB)");
        grPlot->setXAxisLog (false);
        grPlot->setAutoFitY (true);
        grPlot->clear();

        if (! gr.timeline.empty())
        {
            PlotWidget::Series series;
            series.name = "GR";
            series.colour = juce::Colours::cyan;
            series.lineWidth = 2.0f;
            series.x.reserve (gr.timeline.size());
            series.y.reserve (gr.timeline.size());
            for (const auto& p : gr.timeline)
            {
                series.x.push_back (static_cast<float> (p.timeSec));
                series.y.push_back (static_cast<float> (p.grDB));
            }
            grPlot->addSeries (std::move (series));
        }

        grPlot->repaint();

        if (statusMsg.isNotEmpty())
            statusLabel->setText (statusMsg, juce::dontSendNotification);
    }

    /** Status text for a completed GR measurement (tau summary when valid). */
    static juce::String buildGRStatusText (const TimeConstants::Result& tau)
    {
        juce::String msg = "GR timeline complete";
        if (tau.valid)
            msg += ": attack " + juce::String (tau.tauAttackSec * 1000.0, 2) + " ms, release "
                   + juce::String (tau.tauReleaseSec * 1000.0, 2) + " ms";
        else
            msg += ", tau not estimable (no controlled edges)";
        return msg;
    }

    /** Show the GR header plot while the GR timeline type is selected
     *  (regardless of source — the file source also produces GR timelines). */
    void updateGRPlotVisibility()
    {
        const bool showGR = (currentMeasureType
                             == juce::String (Protocol::MeasureType::grTimeline));
        grPlot->setVisible (showGR);
        if (showGR)
        {
            resized();
            grPlot->repaint();
        }
    }

    //==============================================================================
    void openEditorWindowFor (std::unique_ptr<juce::AudioPluginInstance> instance,
                               const juce::String& name)
    {
        unloadCurrentPlugin();
        pluginLoaded = false;
        livePlugin = nullptr;

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
            livePlugin = rawInstance;
            pluginLoaded = true;

            // Populate the scan parameter combo from the loaded plugin.
            fillScanParamCombo (rawInstance);

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
        livePlugin = nullptr;
        commandParser->setPluginInstance (nullptr);
        measurementSession->setPluginInstance (nullptr);

        // The scan parameter combo needs a plugin — empty + disable it.
        scanParamCombo->clear();
        scanParamCombo->setEnabled (false);
        scanParamIds.clear();
        scanProgressLabel->setText ("", juce::dontSendNotification);
        statusLabel->setText ("Ready", juce::dontSendNotification);
        CRASH_LOG_INFO ("Editor window closed", {});
    }

    //==============================================================================
    // Member destruction order = REVERSE declaration order:
    //   the UI members below are destroyed first, then pluginManager (declared
    //   above the UI block), then the IPC members (declared first, destroyed
    //   last), then the atomics / mutex.
    // The explicit cleanup in ~MainContentComponent (cancel→shutdown→abandon
    // threads) runs BEFORE natural destruction, so IPC members are guaranteed
    // alive during shutdown. Abandoned scan/load workers hold their own
    // shared_ptr to PluginManager, so it stays alive until they finish and no
    // worker ever touches a destroyed member (its completion lambda is
    // alive-guarded on the message thread).

    // --- IPC pipeline (declared before pool/mgr for reverse-destruction order) ---
    std::unique_ptr<MeasurementSession> measurementSession;
    std::unique_ptr<CommandParser> commandParser;
    std::unique_ptr<PipeServer> pipeServer;

    // Measurement result held for UI rendering (T6/T8: all three types)
    MeasurementResults measurementResult;
    bool hasMeasurement = false;

    // --- Core ---
    // Shared: an abandoned scan/load worker keeps the manager alive until the
    // hung DLL call returns (plan step 0; see the destructor comments).
    std::shared_ptr<PluginManager> pluginManager;

    // Dedicated one-shot worker threads (plan step 0) — the ThreadPool no longer
    // carries scan/load jobs. The destructor abandons a hung worker via
    // release() instead of joining (bounded by the step-4 watchdog hang cap);
    // the alive flags are checked on the message thread, serialized with the
    // destructor, so abandoned workers can never touch destroyed members.
    std::unique_ptr<std::thread> scanThread;
    std::unique_ptr<std::thread> loadThread;
    std::shared_ptr<std::atomic<bool>> scanAlive;
    std::shared_ptr<std::atomic<bool>> loadAlive;
    std::shared_ptr<ScanOutcome> scanOutcome;
    std::shared_ptr<LoadOutcome> loadOutcome;

    std::unique_ptr<juce::ListBox> pluginListBox;
    std::unique_ptr<juce::Label> statusLabel;
    std::unique_ptr<juce::TextButton> scanButton;
    std::unique_ptr<juce::TextButton> clearBlacklistButton;   // 计划步骤 4：清除黑名单重扫

    // Measurement control row (right panel, above the plots)
    std::unique_ptr<juce::TextButton> measureFreqButton;
    std::unique_ptr<juce::TextButton> measureHarmonicButton;
    std::unique_ptr<juce::TextButton> measureCompressionButton;
    std::unique_ptr<juce::TextButton> measureGRButton;

    // Input source selector + source-specific controls
    std::unique_ptr<juce::ComboBox> sourceCombo;
    std::unique_ptr<juce::TextButton> chooseFileButton;
    std::unique_ptr<juce::Label> noiseDurationLabel;
    std::unique_ptr<juce::TextEditor> noiseDurationBox;
    std::unique_ptr<juce::FileChooser> fileChooser;  // kept alive during async launch
    juce::File selectedFile;
    bool pluginLoaded = false;

    // Parameter-scan panel (right panel, row 3)
    std::unique_ptr<juce::ComboBox> scanParamCombo;
    std::unique_ptr<juce::TextEditor> scanValuesBox;
    std::unique_ptr<juce::Label> scanEstimateLabel;
    std::unique_ptr<juce::TextButton> scanRunButton;
    std::unique_ptr<juce::TextButton> stopScanButton;
    std::unique_ptr<juce::Label> scanProgressLabel;

    /** Stable parameter IDs parallel to scanParamCombo items (item i+1 → ids[i]). */
    std::vector<juce::String> scanParamIds;

    /** Measurement type used by the scan — follows the last clicked measure
     *  button (default: frequency response). */
    juce::String currentMeasureType = juce::String (Protocol::MeasureType::freq);

    // Scan result held for UI rendering
    ScanEngine::ScanResult lastScanResult;
    juce::String lastScanType;

    // Re-entry guard for the synchronous GUI scan (mirrors measurementInProgress).
    bool scanInProgress = false;
    std::atomic<bool> paramScanUpdatePending { false };

    // Re-entry guard: a measurement runs synchronously on the message thread
    // (~5 s), so a second click must be ignored while one is in progress.
    bool measurementInProgress = false;

    // Right-panel frequency-response plots
    std::unique_ptr<PlotWidget> magPlot;
    std::unique_ptr<PlotWidget> phasePlot;

    // GR header (T4.4): GR_dB(t) timeline, live while measuring.
    std::unique_ptr<PlotWidget> grPlot;
    bool grLiveActive = false;
    juce::AudioBuffer<float> liveDry;          // accumulated dry blocks
    juce::AudioBuffer<float> liveWet;          // accumulated wet blocks
    uint32_t lastGRUpdateMs = 0;               // throttling (50 ms)
    juce::AudioPluginInstance* livePlugin = nullptr;  // latency source

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
