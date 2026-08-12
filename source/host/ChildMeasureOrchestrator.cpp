#include "ChildMeasureOrchestrator.h"

#include "ChildProcessCoordinator.h"

#include <utility>

namespace
{
    namespace Field = ChildMeasureContract::Field;

    constexpr int kPopPollMs = 200;            // per-pop slice inside waitForLine
    constexpr int kHandshakeTimeoutMs = 5000;  // start / restore responses
    constexpr int kLoadTimeoutMs = 40000;      // load response (child budget: 30 s + slack)
    constexpr int kMaxChildCrashes = 3;        // design Q4 / D3b: refuse after this many
}  // namespace

//==============================================================================

ChildMeasureOrchestrator::ChildMeasureOrchestrator (PluginHostChildCoordinator* coordinator_,
                                                    juce::String pluginPath_,
                                                    int resultTimeoutMs_)
    : coordinator (coordinator_),
      pluginPath (std::move (pluginPath_)),
      resultTimeoutMs (resultTimeoutMs_)
{
    jassert (coordinator != nullptr);
}

//==============================================================================

ChildMeasureContract::ChildMeasureOutcome ChildMeasureOrchestrator::run (
    const ChildMeasureContract::ChildMeasureRequest& request)
{
    ChildMeasureContract::ChildMeasureOutcome outcome;

    // Issue #3: a fresh run starts with a clean cancel state — a stop that
    // landed between runs (nothing in flight) must not cancel this run.
    coordinator->resetCancel();

    // a. ADR-D-7: the child implements frequency_response and harmonic (T1).
    //    Everything else (gr_timeline — deferred to a separate issue — and
    //    unknown types) fails WITHOUT touching the child — and never falls
    //    back to loading the (blacklisted) plugin in this process.
    if (request.type != "frequency_response" && request.type != "harmonic")
    {
        outcome.ok = false;
        outcome.error = "child measurement not implemented for type '" + request.type + "'";
        return outcome;
    }

    // b. Crash-loop gate (design Q4 / D3b): refuse once the child has
    //    crashed ≥ kMaxChildCrashes times since the LAST SUCCESSFUL run.
    //    crashBaseline is the member-level cross-run baseline — crashCount()
    //    as of the last success — so consecutive crashes accumulate ACROSS
    //    runs (failed/crashed runs never move it) and a successful run
    //    resets it (连续语义: 只有连续崩溃累积，成功即清零).
    const int baseline = crashBaseline;
    if (coordinator->crashCount() - baseline >= kMaxChildCrashes)
    {
        outcome.ok = false;
        outcome.error = "child process crashed (restarting)";
        return outcome;
    }

    // b2. A cancel that arrived before the sequence starts bails immediately.
    if (coordinator->isCancelRequested())
        return cancelledOutcome();

    // c. Recovery sequence (D3): a fresh child per run. restart() refuses a
    //    still-running child, so stop() first when a previous run left one
    //    alive (a deliberate stop is not a crash event).
    if (coordinator->isRunning())
        coordinator->stop();
    if (! coordinator->restart())
        return failedOutcome ("child measurement failed", baseline);
    if (coordinator->isCancelRequested())
        return cancelledOutcome();

    // start handshake
    if (! coordinator->sendLine (R"({"cmd":"start"})"))
        return failedOutcome ("child measurement failed", baseline);

    {
        juce::String line;
        if (! waitForLine ("\"pid\"", kHandshakeTimeoutMs, baseline, line))
        {
            if (coordinator->isCancelRequested())
                return cancelledOutcome();
            return failedOutcome ("child measurement failed", baseline);
        }
        if (! line.contains ("\"ok\":true"))
            return childOrGenericError (line, baseline);
    }
    if (coordinator->isCancelRequested())
        return cancelledOutcome();

    // load
    {
        juce::DynamicObject loadRequest;
        loadRequest.setProperty (Field::cmd, "load");
        loadRequest.setProperty ("path", pluginPath);
        const auto loadVar = juce::var (new juce::DynamicObject (loadRequest));
        if (! coordinator->sendLine (juce::JSON::toString (loadVar, true)))
            return failedOutcome ("child measurement failed", baseline);

        juce::String line;
        if (! waitForLine ("\"name\"", kLoadTimeoutMs, baseline, line))
        {
            if (coordinator->isCancelRequested())
                return cancelledOutcome();
            return failedOutcome ("child measurement failed", baseline);
        }
        if (! line.contains ("\"ok\":true"))
            return childOrGenericError (line, baseline);
    }
    if (coordinator->isCancelRequested())
        return cancelledOutcome();

    // restore_params: splice the cached snapshot (D3 recovery) only when one
    // exists — an empty cache means nothing was snapshotted, nothing to
    // restore.
    {
        const auto snapshot = coordinator->lastSnapshot();
        if (snapshot.isNotEmpty())
        {
            const juce::String restoreRequest = R"({"cmd":"restore_params","params":)" + snapshot + "}";
            if (! coordinator->sendLine (restoreRequest))
                return failedOutcome ("child measurement failed", baseline);

            juce::String line;
            if (! waitForLine ("\"ok\":", kHandshakeTimeoutMs, baseline, line))
            {
                if (coordinator->isCancelRequested())
                    return cancelledOutcome();
                return failedOutcome ("child measurement failed", baseline);
            }
            if (! line.contains ("\"ok\":true"))
                return childOrGenericError (line, baseline);
        }
    }
    if (coordinator->isCancelRequested())
        return cancelledOutcome();

    // measure: reuse the frozen Field constants so host and child vocabulary
    // cannot drift (ChildMeasureContract.h).
    {
        juce::DynamicObject measureRequest;
        measureRequest.setProperty (Field::cmd, Field::measure);
        measureRequest.setProperty (Field::type, request.type);
        measureRequest.setProperty (Field::excitation, request.excitation);
        measureRequest.setProperty (Field::sampleRate, request.sampleRate);
        measureRequest.setProperty (Field::blockSize, request.blockSize);
        measureRequest.setProperty (Field::exportPath, request.exportPath);
        measureRequest.setProperty (Field::wavPath, request.wavPath);
        const auto measureVar = juce::var (new juce::DynamicObject (measureRequest));
        if (! coordinator->sendLine (juce::JSON::toString (measureVar, true)))
            return failedOutcome ("child measurement failed", baseline);

        // Result collection: progress lines pass through, a mid-measure
        // crash bails early (waitForLine), a silent child times out, and a
        // user cancel (issue #3) stops the child deliberately.
        juce::String line;
        if (! waitForLine ("\"samples\"", resultTimeoutMs, baseline, line))
        {
            if (coordinator->isCancelRequested())
                return cancelledOutcome();
            return failedOutcome ("child measurement failed", baseline);
        }
        if (! line.contains ("\"ok\":true"))
            return childOrGenericError (line, baseline);

        // d. Map the child's report onto ChildMeasureResult (ADR-D-6: the
        //    child holds the only plugin instance — the metadata comes from
        //    the result line, not from a host-side plugin).
        const auto doc = juce::JSON::parse (line);
        if (! doc.isObject())
            return failedOutcome ("child measurement failed", baseline);

        outcome.ok = true;
        outcome.result.samples = static_cast<juce::int64> (doc[Field::samples]);
        outcome.result.rate = static_cast<double> (doc[Field::rate]);
        outcome.result.exportPath = doc[Field::exportPath].toString();
        outcome.result.wavPath = doc[Field::wavPath].toString();
        outcome.result.name = doc[Field::name].toString();
        outcome.result.classId = doc[Field::classId].toString();
        outcome.result.channels = static_cast<int> (doc[Field::channels]);
        outcome.result.latencySamples = static_cast<int> (doc[Field::latency]);

        // e. Success resets the crash-loop baseline (design Q4: 连续语义 —
        //    only crashes since the last successful run accumulate; a
        //    success clears the streak, so the next gate check counts from
        //    here). Failed/crashed runs never reach this line.
        crashBaseline = coordinator->crashCount();
        return outcome;
    }
}

//==============================================================================

bool ChildMeasureOrchestrator::waitForLine (const juce::String& needle, int timeoutMs,
                                            int baseline, juce::String& outLine) const
{
    const auto deadline = juce::Time::getMillisecondCounter()
                        + static_cast<juce::uint32> (timeoutMs);

    while (juce::Time::getMillisecondCounter() < deadline)
    {
        // Issue #3: a user cancel is honoured at the poll granularity — the
        // caller then returns the deliberate-cancel outcome.
        if (coordinator->isCancelRequested())
            return false;

        const auto line = coordinator->popLine (kPopPollMs);
        if (! line.isEmpty())
        {
            outLine = line;

            // Progress lines are the protocol's liveness signal — pass them
            // through silently (never a result, never an error).
            if (line.contains ("\"progress\""))
                continue;

            // An explicit {"ok":false,...} ends the wait too — the caller
            // inspects the line and surfaces the child's error vocabulary.
            if (line.contains (needle) || line.contains ("\"ok\":false"))
                return true;

            continue;   // unrelated line — keep popping
        }

        // No line within this slice: a reported crash (e.g. the child died
        // mid-measure) bails out early instead of waiting out the timeout.
        if (coordinator->crashCount() > baseline)
            return false;
    }

    return false;   // timeout
}

//==============================================================================

void ChildMeasureOrchestrator::cancel()
{
    coordinator->requestCancel();
}

ChildMeasureContract::ChildMeasureOutcome ChildMeasureOrchestrator::cancelledOutcome() const
{
    // Issue #3: deliberate user cancel — stop the child (a stop is never a
    // crash event, so crashCount / the crash-loop gate / the blacklist stay
    // untouched) and report the cancel vocabulary.
    coordinator->stop();
    ChildMeasureContract::ChildMeasureOutcome outcome;
    outcome.ok = false;
    outcome.error = "cancelled";
    return outcome;
}

ChildMeasureContract::ChildMeasureOutcome ChildMeasureOrchestrator::failedOutcome (
    juce::String message, int baseline) const
{
    ChildMeasureContract::ChildMeasureOutcome outcome;
    outcome.ok = false;

    // A crash since the run's baseline outranks a generic failure (D3b:
    // 测量中的崩溃 → "child process crashed (restarting)").
    outcome.error = (coordinator->crashCount() > baseline)
                        ? juce::String ("child process crashed (restarting)")
                        : std::move (message);
    return outcome;
}

ChildMeasureContract::ChildMeasureOutcome ChildMeasureOrchestrator::childOrGenericError (
    const juce::String& responseLine, int baseline) const
{
    ChildMeasureContract::ChildMeasureOutcome outcome;
    outcome.ok = false;

    // An {"ok":false,"error":"..."} line surfaces the child's own vocabulary
    // (e.g. "no plugin loaded") — the error text IS the protocol alignment.
    const auto doc = juce::JSON::parse (responseLine);
    const auto childError = doc.isObject() ? doc["error"].toString() : juce::String();
    if (childError.isNotEmpty())
    {
        outcome.error = childError;
        return outcome;
    }

    return failedOutcome ("child measurement failed", baseline);
}
