// The AES67-SRT HAL plug-in (ADR 0007, spec 0002, issue #37). It presents one
// 64-in/64-out CoreAudio device and moves audio between the HAL's I/O callbacks and
// a named shared-memory block the application maps.
//
// The realtime callbacks do nothing but call DeviceBridge, which is where the
// silence-pad and overwrite-oldest rules live and where a test proves the path
// allocates nothing. The control thread does the only allocating work — opening the
// region and binding the bridge — before any I/O starts.
//
// The plug-in's own CMake lives in the repository root; this file is compiled into
// a
// `.driver` bundle only on macOS. It has no SRT, no daemon, no PTP and no engine:
// all of those are the application's, on the other side of the shared memory.

#include <aspl/Context.hpp>
#include <aspl/Device.hpp>
#include <aspl/Driver.hpp>
#include <aspl/IORequestHandler.hpp>
#include <aspl/Plugin.hpp>
#include <aspl/Stream.hpp>

#include <CoreAudio/AudioServerPlugIn.h>
#include <os/log.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>

#include "audio/device_bridge.hpp"
#include "audio/hal_shared.hpp"
#include "audio/shared_region.hpp"
#include "audio/shared_ring.hpp"

namespace {

using aes67_srt::audio::DeviceBridge;
using aes67_srt::audio::kHalCapacityFrames;
using aes67_srt::audio::kHalChannels;
using aes67_srt::audio::kHalRegionName;
using aes67_srt::audio::shared_bytes_for;
using aes67_srt::audio::SharedAudio;
using aes67_srt::audio::SharedRegion;

constexpr UInt32 kSampleRate = 48000;

// The control thread may log, and must: whether the shared region opened inside the
// plug-in's sandbox is the one thing a hand-test cannot otherwise see.
os_log_t LogHandle() {
  static os_log_t handle = os_log_create("dev.aes67-srt.driver", "hal");
  return handle;
}

AudioStreamBasicDescription FloatFormat() {
  AudioStreamBasicDescription format;
  std::memset(&format, 0, sizeof(format));
  format.mSampleRate = kSampleRate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagsNativeEndian |
                        kAudioFormatFlagIsPacked;
  format.mBitsPerChannel = 32;
  format.mChannelsPerFrame = kHalChannels;
  format.mBytesPerFrame = 4 * kHalChannels;
  format.mFramesPerPacket = 1;
  format.mBytesPerPacket = 4 * kHalChannels;
  return format;
}

class DriverHandler : public aspl::ControlRequestHandler,
                      public aspl::IORequestHandler {
 public:
  // --- control thread: the only place that may allocate -------------------

  OSStatus OnStartIO() override {
    // Open (or create) the named region and bind the bridge. Doing this here,
    // before the first I/O request, is what lets the callbacks stay allocation-
    // and syscall-free. A failure leaves the bridge unbound: the device stays
    // present and the callbacks answer with silence until the application maps
    // the region, rather than taking the audio server down.
    const size_t bytes = shared_bytes_for(kHalCapacityFrames, kHalChannels);
    bool created = false;
    std::string error;
    if (!region_.open(kHalRegionName, bytes, &created, &error)) {
      unbound_reason_ = error;
      os_log_error(LogHandle(),
                   "shared region unavailable, device stays silent: %{public}s",
                   error.c_str());
      return kAudioHardwareNoError;
    }
    const bool ready = created
                           ? audio_.create(region_.data(), bytes,
                                           kHalCapacityFrames, kHalChannels, &error)
                           : audio_.attach(region_.data(), bytes, &error);
    if (!ready) {
      unbound_reason_ = error;
      region_.close();
      return kAudioHardwareNoError;
    }
    bridge_.configure(kHalChannels);
    bridge_.bind(&audio_.to_host(), &audio_.from_host());
    unbound_reason_.clear();
    os_log_info(LogHandle(), "shared region %{public}s, %u channels bound",
                created ? "created" : "attached", kHalChannels);
    return kAudioHardwareNoError;
  }

  void OnStopIO() override {
    bridge_.unbind();
    region_.close();
  }

  // --- realtime I/O: copy, and nothing else -------------------------------

  void OnReadClientInput(const std::shared_ptr<aspl::Client>&,
                         const std::shared_ptr<aspl::Stream>&, Float64, Float64,
                         void* bytes, UInt32 bytesCount) override {
    const size_t frames = bytesCount / (sizeof(float) * kHalChannels);
    bridge_.read_input(static_cast<float*>(bytes), frames);
  }

  void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>&, Float64, Float64,
                          const void* bytes, UInt32 bytesCount) override {
    const size_t frames = bytesCount / (sizeof(float) * kHalChannels);
    bridge_.write_output(static_cast<const float*>(bytes), frames);
  }

 private:
  SharedRegion region_;
  SharedAudio audio_;
  DeviceBridge bridge_;
  std::string unbound_reason_;
};

std::shared_ptr<aspl::Driver> CreateDriver() {
  auto context = std::make_shared<aspl::Context>();

  aspl::DeviceParameters device_params;
  device_params.Name = "AES67-SRT";
  device_params.Manufacturer = "aes67-srt";
  device_params.SampleRate = kSampleRate;
  device_params.ChannelCount = kHalChannels;
  device_params.DeviceUID = "aes67-srt-device";
  // A bridge a DAW selects, not a sound device: it must not become the system's
  // default and route alerts into a silent link (spec 0002, decided 2026-09-19).
  device_params.CanBeDefault = false;
  device_params.CanBeDefaultForSystemSounds = false;
  // The send path wants the mix of every client, which is libASPL's default.
  device_params.EnableMixing = true;

  auto device = std::make_shared<aspl::Device>(context, device_params);

  aspl::StreamParameters input_params;  // device -> client: the DAW records this
  input_params.Direction = aspl::Direction::Input;
  input_params.StartingChannel = 1;
  input_params.Format = FloatFormat();
  device->AddStreamAsync(input_params);

  aspl::StreamParameters output_params;  // client -> device: the DAW plays this
  output_params.Direction = aspl::Direction::Output;
  output_params.StartingChannel = 1;
  output_params.Format = FloatFormat();
  device->AddStreamAsync(output_params);

  auto handler = std::make_shared<DriverHandler>();
  device->SetControlHandler(handler);
  device->SetIOHandler(handler);

  auto plugin = std::make_shared<aspl::Plugin>(context);
  plugin->AddDevice(device);

  return std::make_shared<aspl::Driver>(context, plugin);
}

}  // namespace

extern "C" void* AES67SRTEntryPoint(CFAllocatorRef allocator, CFUUIDRef typeUUID) {
  (void)allocator;
  if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
    return nullptr;
  }
  static std::shared_ptr<aspl::Driver> driver = CreateDriver();
  return driver->GetReference();
}