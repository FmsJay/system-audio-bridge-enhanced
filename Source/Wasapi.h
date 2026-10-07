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

enum class CaptureSource
{
    excludeHostProcess,   // everything Windows plays except this process tree (no feedback)
    wholeDefaultDevice    // classic loopback of the default output, like the original plugin
};

class LoopbackCapture
{
public:
    ~LoopbackCapture() { stop(); }

    void start (int sampleRate, CaptureSource source);
    void stop();
    bool isRunning() const { return running.load(); }
    std::string status() const;

    StereoRing ring;

private:
    void threadMain (int sampleRate, CaptureSource source);
    std::thread thread;
    std::atomic<bool> running { false }, quit { false };
    std::atomic<int> statusCode { 0 };
};

class DeviceRender
{
public:
    ~DeviceRender() { stop(); }

    void start (int sampleRate);
    void stop();
    bool isRunning() const { return running.load(); }

    StereoRing ring;

private:
    void threadMain (int sampleRate);
    std::thread thread;
    std::atomic<bool> running { false }, quit { false };
};
