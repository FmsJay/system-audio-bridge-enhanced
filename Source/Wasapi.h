#pragma once

// WASAPI capture/render engines. Deliberately free of JUCE and <windows.h> in the
// header so the two never fight over macros in the plugin translation unit.

#include <atomic>
#include <string>
#include <thread>
#include <vector>

// Single-producer / single-consumer stereo ring of planar float frames.
class StereoRing
{
public:
    void setSize (int frames);
    int available() const;
    int freeSpace() const;
    int write (const float* l, const float* r, int frames);
    int read (float* l, float* r, int frames);     // l/r may be null to discard
    void clear();

private:
    std::vector<float> L, R;
    int size = 0;
    std::atomic<int> readPos { 0 }, writePos { 0 };
};

// An active Windows render endpoint. Strings are UTF-8; id is the stable IMMDevice id.
struct AudioEndpoint
{
    std::string id, name;
    bool isDefault = false;
};

// Active render endpoints. Runs its own COM apartment, so it is safe from any thread.
std::vector<AudioEndpoint> listRenderEndpoints();

enum class CaptureSource
{
    excludeHostProcess,   // everything Windows plays except this process tree (no feedback)
    wholeDefaultDevice,   // classic loopback of the default output, like the original plugin
    selectedEndpoint      // whole-device loopback of one chosen endpoint, REAPER's own output included
};

class LoopbackCapture
{
public:
    ~LoopbackCapture() { stop(); }

    // endpointId/endpointName are only used with CaptureSource::selectedEndpoint.
    // A missing endpoint is reported, never replaced by another device.
    void start (int sampleRate, CaptureSource source, const std::string& endpointId = {},
                const std::string& endpointName = {});
    void stop();
    bool isRunning() const { return running.load(); }
    std::string status() const;

    StereoRing ring;

private:
    void threadMain (int sampleRate, CaptureSource source, std::string endpointId);
    void join();
    std::thread thread;
    std::atomic<bool> running { false }, quit { false };
    std::atomic<int> statusCode { 0 };
    std::string name;
};

class DeviceRender
{
public:
    ~DeviceRender() { stop(); }

    // An empty endpointId renders to the Windows default device.
    void start (int sampleRate, const std::string& endpointId = {}, const std::string& endpointName = {});
    void stop();
    bool isRunning() const { return running.load(); }
    std::string status() const;

    StereoRing ring;

private:
    void threadMain (int sampleRate, std::string endpointId);
    void join();
    std::thread thread;
    std::atomic<bool> running { false }, quit { false };
    std::atomic<int> statusCode { 0 };
    std::string name;
};
