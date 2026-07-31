#pragma once

#include <JuceHeader.h>
#include <functional>

/**
 * Windows Named Pipe server.
 * Listens for incoming JSON commands on \\.\pipe\PluginLab
 * and dispatches them via a callback.
 */
class PipeServer : private juce::Thread
{
public:
    PipeServer();
    ~PipeServer() override;

    //==============================================================================
    /** Start the pipe server on a background thread. */
    void startup();

    /** Stop the pipe server. */
    void shutdown();

    /** Set the command handler callback.
     *  Callback receives the JSON command string and must return a JSON response.
     */
    using CommandHandler = std::function<juce::String (const juce::String& command)>;
    void setCommandHandler (CommandHandler handler);

    /** Returns true if a client is connected. */
    bool isClientConnected() const { return clientConnected; }

private:
    void run() override;

    juce::CriticalSection lock;
    void* hPipe = nullptr;  // HANDLE to the named pipe
    volatile bool clientConnected = false;
    CommandHandler commandHandler;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeServer)
};
