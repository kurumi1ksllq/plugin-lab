#pragma once

#include <JuceHeader.h>

#include "ChildMeasureContract.h"

class PluginHostChildCoordinator;

/**
 * Host-side orchestrator for out-of-process plugin measurement (block D, D6 —
 * docs/plan-block-d-out-of-process.md D6 ticket + ADR-D-5/6/7 + design
 * Q3/Q4). Owns the coordinator + the D3 crash-recovery sequence.
 *
 * run() drives ONE measurement through the child with a
 * FRESH process per call — the recovery sequence
 * (docs/plan-block-d-out-of-process.md D3b, validated by T5):
 *
 *   restart() → {"cmd":"start"} → {"cmd":"load","path":...} →
 *   restore_params(cached snapshot, only when lastSnapshot() is non-empty) →
 *   {"cmd":"measure",...} → collect the result line (progress lines pass
 *   through silently) and map it onto ChildMeasureResult.
 *
 * ADR-D-6: the child reports the plugin metadata (name/class_id/
 * latency_samples/channels) the host has no instance to read for a
 * blacklisted plugin — run() only forwards what the request carries and maps
 * what the child reports back.
 *
 * Crash policy (design Q4 / D3b): "连续崩溃 ≥3 次" counts ACROSS runs —
 * this class owns the across-runs baseline itself. crashBaseline is
 * crashCount() as of the last SUCCESSFUL run (a member, never cleared by
 * the coordinator: its counter is cumulative). run() refuses BEFORE
 * touching the child when crashCount() - crashBaseline >= kMaxChildCrashes
 * — consecutive crashes accumulate across runs, and a successful run
 * resets the baseline, so crashes separated by a success never trip the
 * gate. A crash reported DURING the sequence (e.g. the child dies
 * mid-measure) surfaces the same error; a child that stays silent
 * (timeout) or answers {"ok":false,"error":...} surfaces the protocol
 * vocabulary ("child measurement failed" / the child's own error, e.g.
 * "no plugin loaded").
 *
 * ADR-D-7: frequency_response, harmonic (T1) and compression (T2) pass the
 * gate; any other type (gr_timeline — deferred to a separate issue — and
 * unknown types) is rejected WITHOUT touching the child — the blacklisted
 * plugin is never loaded in this process, and there is no fallback to
 * host-direct loading.
 *
 * THREADING: run() blocks synchronously on the calling thread (the
 * CommandParser-dispatched message thread). restart() refuses to join the
 * reader thread from inside the onCrash callback, but run() never runs on
 * that thread — it always calls restart() from a different (message) thread,
 * which the coordinator requires.
 */
class ChildMeasureOrchestrator
{
public:
    //==============================================================================
    /** @param coordinator      owned by the caller; the orchestrator drives
                               it but never destroys it.
        @param pluginPath      VST3 path forwarded verbatim to the child's
                               load command.
        @param resultTimeoutMs budget for the measure result line (default
                               60 s covers the child's 5 s sweep + analysis;
                               tests shorten it to exercise the timeout
                               path — same pattern as the coordinator's
                               heartbeatTimeoutMs). */
    ChildMeasureOrchestrator (PluginHostChildCoordinator* coordinator,
                              juce::String pluginPath,
                              int resultTimeoutMs = 60000);

    /** One frequency_response, harmonic or compression measurement through
        the child (see class docs for the sequence, the crash gate and the
        error vocabulary). */
    ChildMeasureContract::ChildMeasureOutcome run (const ChildMeasureContract::ChildMeasureRequest& request);

    /** Request cancellation of an in-flight run (issue #3): forwards to the
        coordinator's cancel flag, which the run's waitForLine polls. The
        run then stops the child deliberately and returns a "cancelled"
        outcome — never a crash. Thread-safe (atomic). */
    void cancel();

private:
    //==============================================================================
    /** Pop response lines until one matches `needle` or is an explicit
        {"ok":false,...} error — the caller inspects the line. Progress lines
        (liveness) and unrelated lines pass through. Returns false on timeout
        or when a crash was reported since `baseline` (the caller then fails
        with the crash vocabulary). */
    bool waitForLine (const juce::String& needle, int timeoutMs, int baseline,
                      juce::String& outLine) const;

    /** Failure outcome: "child process crashed (restarting)" when the child
        crashed since `baseline`, otherwise the given message. */
    ChildMeasureContract::ChildMeasureOutcome failedOutcome (juce::String message, int baseline) const;

    /** Failure outcome for a non-ok response line: surfaces the child's own
        error vocabulary when present ("no plugin loaded", ...), else the
        crash/generic vocabulary. */
    ChildMeasureContract::ChildMeasureOutcome childOrGenericError (const juce::String& responseLine,
                                                                   int baseline) const;

    /** Deliberate-cancel outcome (issue #3): stops the child (NOT a crash
        event — crashCount and the crash-loop gate stay untouched) and
        returns {"ok":false,"error":"cancelled"}. */
    ChildMeasureContract::ChildMeasureOutcome cancelledOutcome() const;

    PluginHostChildCoordinator* coordinator;
    juce::String pluginPath;
    int resultTimeoutMs;

    /** crashCount() as of the last SUCCESSFUL run — the crash-loop gate's
        baseline (design Q4 / D3b: 连续崩溃 across runs). Consecutive crashes
        accumulate against it (failed/crashed runs never update it); a
        successful run resets it to the then-current crashCount(), so only
        crashes AFTER the last success count toward the ≥ kMaxChildCrashes
        refusal. Initial 0 = the streak starts counting from construction. */
    int crashBaseline = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChildMeasureOrchestrator)
};
