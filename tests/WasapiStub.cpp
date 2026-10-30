// Stand-in for Wasapi.cpp in tests: records what the processor asks the engines to do,
// and treats only the ids in presentIds as plugged in.
#include "Wasapi.h"
#include <algorithm>
#include <set>
std::vector<std::string> engineLog;
std::set<std::string> presentIds { "{dev-A}", "{dev-B}" };

void StereoRing::setSize (int frames) { size = frames + 1; L.assign ((size_t) size, 0); R.assign ((size_t) size, 0); clear(); }
void StereoRing::clear() { readPos = 0; writePos = 0; }
int StereoRing::available() const { return size ? (writePos - readPos + size) % size : 0; }
int StereoRing::freeSpace() const { return size ? size - 1 - available() : 0; }
int StereoRing::write (const float*, const float*, int f) { return std::min (f, freeSpace()); }
int StereoRing::read (float*, float*, int f) { return std::min (f, available()); }

std::vector<AudioEndpoint> listRenderEndpoints() { return { { "{dev-A}", "Speakers A", true }, { "{dev-B}", "Headphones B", false } }; }

void LoopbackCapture::start (int rate, CaptureSource src, const std::string& id, const std::string& n)
{
    name = n;
    engineLog.push_back ("capture " + std::to_string (rate) + " mode=" + std::to_string ((int) src) + " id=" + id);
    const bool ok = src != CaptureSource::selectedEndpoint || presentIds.count (id) > 0;
    running = ok;
    statusCode = ok ? 4 : -2;
}
void LoopbackCapture::stop() { engineLog.push_back ("capture stop"); running = false; statusCode = 0; }
void LoopbackCapture::join() {}
void LoopbackCapture::threadMain (int, CaptureSource, std::string) {}
std::string LoopbackCapture::status() const { return "capture code " + std::to_string (statusCode.load()) + " " + name; }

void DeviceRender::start (int rate, const std::string& id, const std::string& n)
{
    name = n;
    engineLog.push_back ("render " + std::to_string (rate) + " id=" + id);
    const bool ok = id.empty() || presentIds.count (id) > 0;
    running = ok;
    statusCode = ok ? 2 : -2;
}
void DeviceRender::stop() { engineLog.push_back ("render stop"); running = false; statusCode = 0; }
void DeviceRender::join() {}
void DeviceRender::threadMain (int, std::string) {}
std::string DeviceRender::status() const { return "render code " + std::to_string (statusCode.load()) + " " + name; }
