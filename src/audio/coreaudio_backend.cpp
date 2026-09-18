#include "audio/coreaudio_backend.hpp"

#if AES67_SRT_WITH_COREAUDIO

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "audio/float_ring.hpp"
#include "audio/pcm.hpp"
#include "log.hpp"

namespace aes67_srt::audio {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

std::string device_name(AudioDeviceID device) {
  CFStringRef name = nullptr;
  UInt32 size = sizeof(name);
  AudioObjectPropertyAddress address{kAudioObjectPropertyName,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain};
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &name) !=
          noErr ||
      name == nullptr) {
    return {};
  }
  char buffer[256] = {0};
  const bool ok =
      CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8);
  CFRelease(name);
  return ok ? std::string(buffer) : std::string();
}

unsigned device_channels(AudioDeviceID device, bool input) {
  AudioObjectPropertyAddress address{
      kAudioDevicePropertyStreamConfiguration,
      input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
      kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) !=
          noErr ||
      size == 0) {
    return 0;
  }
  std::vector<uint8_t> storage(size);
  AudioBufferList* list = reinterpret_cast<AudioBufferList*>(storage.data());
  if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) !=
      noErr) {
    return 0;
  }
  unsigned channels = 0;
  for (UInt32 index = 0; index < list->mNumberBuffers; ++index) {
    channels += list->mBuffers[index].mNumberChannels;
  }
  return channels;
}

AudioDeviceID default_device(bool input) {
  AudioObjectPropertyAddress address{
      input ? kAudioHardwarePropertyDefaultInputDevice
            : kAudioHardwarePropertyDefaultOutputDevice,
      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
  AudioDeviceID device = kAudioObjectUnknown;
  UInt32 size = sizeof(device);
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr,
                                 &size, &device) != noErr) {
    return kAudioObjectUnknown;
  }
  return device;
}

/**
 * The device a configuration names, or the system default when it names none.
 *
 * Matching is by CoreAudio device name, which is what a person sees in Audio MIDI
 * Setup — so `audio.device` switches from `plughw:RAVENNA` on the appliance to
 * `BlackHole 64ch` on the Mac without a second configuration key.
 */
AudioDeviceID find_device(const std::string& name, bool input, std::string* error) {
  if (name.empty()) {
    const AudioDeviceID fallback = default_device(input);
    if (fallback == kAudioObjectUnknown) {
      fail(error, std::string("no default ") + (input ? "input" : "output") +
                      " CoreAudio device");
    }
    return fallback;
  }

  AudioObjectPropertyAddress list_address{kAudioHardwarePropertyDevices,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &list_address, 0,
                                     nullptr, &size) != noErr) {
    fail(error, "cannot list CoreAudio devices");
    return kAudioObjectUnknown;
  }
  std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &list_address, 0,
                                 nullptr, &size, devices.data()) != noErr) {
    fail(error, "cannot read the CoreAudio device list");
    return kAudioObjectUnknown;
  }

  std::string seen;
  for (AudioDeviceID device : devices) {
    const std::string candidate = device_name(device);
    if (!seen.empty()) {
      seen += ", ";
    }
    seen += candidate.empty() ? std::string("(unnamed)") : candidate;
    if (candidate == name) {
      return device;
    }
  }
  fail(error, "audio.device: no CoreAudio device named \"" + name +
                  "\" (found: " + seen + ")");
  return kAudioObjectUnknown;
}

/**
 * The client stream format for an AudioUnit on |channel_count| channels.
 *
 * **The count is the device's, not ours.** A HAL device exposes a fixed channel
 * count — BlackHole is 64 — and an AudioUnit whose client format disagrees does not
 * error: it interleaves our samples into the wrong stride, which arrives as
 * time-shifted, screechy audio rather than as a fault. So the units are opened at
 * the device's count and the callbacks map our channels into it.
 */
AudioStreamBasicDescription float_format(const AudioFormat& format,
                                         unsigned channel_count) {
  AudioStreamBasicDescription asbd{};
  asbd.mSampleRate = static_cast<Float64>(format.sample_rate);
  asbd.mFormatID = kAudioFormatLinearPCM;
  // Interleaved float32: `kAudioFormatFlagsNativeFloatPacked` without the
  // non-interleaved flag, so the callback is a memcpy when the counts agree.
  asbd.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
  asbd.mChannelsPerFrame = channel_count;
  asbd.mBitsPerChannel = 32;
  asbd.mFramesPerPacket = 1;
  asbd.mBytesPerFrame = channel_count * sizeof(float);
  asbd.mBytesPerPacket = asbd.mBytesPerFrame;
  return asbd;
}

}  // namespace

struct CoreAudioBackend::Impl {
  AudioConfig config;
  AudioFormat format;
  bool opened = false;
  std::string detail;

  AudioDeviceID output_device = kAudioObjectUnknown;
  AudioDeviceID input_device = kAudioObjectUnknown;
  AudioUnit output_unit = nullptr;
  AudioUnit input_unit = nullptr;

  /**
   * The realtime boundary (ADR 0005). The render callback touches a ring and
   * nothing else — no lock, no allocation, no log.
   */
  FloatRing to_device;    // this process -> the device (output callback drains)
  FloatRing from_device;  // the device -> this process (input callback fills)

  /** Non-realtime scratch, one per direction: each is used by one engine thread. */
  std::vector<float> read_scratch;
  std::vector<float> write_scratch;
  std::vector<uint8_t> input_bytes;
  size_t max_input_frames = 0;

  /** The device's own channel counts, which the AudioUnits are opened at. */
  unsigned output_device_channels = 0;
  unsigned input_device_channels = 0;
  /** Realtime scratch for mapping our channels into the device's, and back. */
  std::vector<float> output_map_scratch;
  std::vector<float> input_map_scratch;

  std::atomic<unsigned> overruns{0};
  std::atomic<unsigned> underruns{0};

  static OSStatus output_callback(void* ref, AudioUnitRenderActionFlags*,
                                  const AudioTimeStamp*, UInt32, UInt32 frames,
                                  AudioBufferList* io) {
    Impl* self = static_cast<Impl*>(ref);
    if (io == nullptr || io->mNumberBuffers == 0) {
      return noErr;
    }
    float* out = static_cast<float*>(io->mBuffers[0].mData);
    const unsigned channels = self->format.channels;
    const unsigned device_channels = self->output_device_channels;
    if (device_channels == channels) {
      const size_t got = self->to_device.read(out, frames);
      if (got < frames) {
        std::memset(out + got * channels, 0,
                    (frames - got) * channels * sizeof(float));
        self->underruns.fetch_add(1);
      }
      return noErr;
    }
    // The device carries more channels than we do (or fewer): read our frames into
    // scratch and scatter them into the device's stride, leaving the rest silent.
    // Getting this wrong does not fail — it plays time-shifted screech.
    std::fill(self->output_map_scratch.begin(), self->output_map_scratch.end(),
              0.0f);
    const size_t got =
        self->to_device.read(self->output_map_scratch.data(), frames);
    if (got < frames) {
      self->underruns.fetch_add(1);
    }
    for (UInt32 frame = 0; frame < frames; ++frame) {
      float* destination = out + static_cast<size_t>(frame) * device_channels;
      const float* source =
          self->output_map_scratch.data() + static_cast<size_t>(frame) * channels;
      for (unsigned channel = 0; channel < device_channels; ++channel) {
        destination[channel] = channel < channels ? source[channel] : 0.0f;
      }
    }
    return noErr;
  }

  static OSStatus input_callback(void* ref, AudioUnitRenderActionFlags* flags,
                                 const AudioTimeStamp* timestamp, UInt32 bus,
                                 UInt32 frames, AudioBufferList*) {
    Impl* self = static_cast<Impl*>(ref);
    if (frames > self->max_input_frames) {
      self->overruns.fetch_add(1);  // the scratch is bounded; drop rather than copy
      return noErr;
    }
    AudioBufferList* list =
        reinterpret_cast<AudioBufferList*>(self->input_bytes.data());
    const unsigned channels = self->format.channels;
    const unsigned device_channels = self->input_device_channels;
    list->mNumberBuffers = 1;
    list->mBuffers[0].mNumberChannels = device_channels;
    list->mBuffers[0].mDataByteSize = frames * device_channels * sizeof(float);
    list->mBuffers[0].mData = self->input_bytes.data() + sizeof(AudioBufferList);
    if (AudioUnitRender(self->input_unit, flags, timestamp, bus, frames, list) !=
        noErr) {
      self->overruns.fetch_add(1);
      return noErr;
    }
    const float* rendered = static_cast<const float*>(list->mBuffers[0].mData);
    if (device_channels == channels) {
      if (self->from_device.write(rendered, frames) < frames) {
        self->overruns.fetch_add(1);
      }
      return noErr;
    }
    // Take our channels out of the device's stride (the mirror of the output map).
    for (UInt32 frame = 0; frame < frames; ++frame) {
      const float* source = rendered + static_cast<size_t>(frame) * device_channels;
      float* destination =
          self->input_map_scratch.data() + static_cast<size_t>(frame) * channels;
      for (unsigned channel = 0; channel < channels; ++channel) {
        destination[channel] = channel < device_channels ? source[channel] : 0.0f;
      }
    }
    if (self->from_device.write(self->input_map_scratch.data(), frames) < frames) {
      self->overruns.fetch_add(1);
    }
    return noErr;
  }
};

CoreAudioBackend::CoreAudioBackend(const AudioConfig& config) : impl_(new Impl()) {
  impl_->config = config;
}

CoreAudioBackend::~CoreAudioBackend() {
  close();
}

bool CoreAudioBackend::open(const AudioFormat& format, std::string* error) {
  if (format.sample_rate == 0 || format.channels == 0 ||
      format.period_frames == 0) {
    return fail(error, "coreaudio: a period of zero frames has no meaning");
  }
  impl_->format = format;

  impl_->output_device = find_device(impl_->config.device, false, error);
  if (impl_->output_device == kAudioObjectUnknown) {
    return false;
  }
  impl_->input_device = find_device(impl_->config.device, true, error);
  if (impl_->input_device == kAudioObjectUnknown) {
    // The message already names the device and what was found.
    return false;
  }

  const unsigned out_channels = device_channels(impl_->output_device, false);
  const unsigned in_channels = device_channels(impl_->input_device, true);
  if (out_channels < format.channels && in_channels < format.channels) {
    return fail(error, "coreaudio: device \"" + device_name(impl_->output_device) +
                           "\" carries " + std::to_string(out_channels) +
                           " output and " + std::to_string(in_channels) +
                           " input channels, and this link needs " +
                           std::to_string(format.channels));
  }

  impl_->output_device_channels = out_channels;
  impl_->input_device_channels = in_channels;

  // Rings: well above any device I/O buffer, so a bursty callback cannot starve.
  const size_t ring_frames = std::max<size_t>(format.period_frames * 64, 4096);
  if (!impl_->to_device.open(ring_frames, format.channels) ||
      !impl_->from_device.open(ring_frames, format.channels)) {
    return fail(error, "coreaudio: cannot size the rings");
  }
  impl_->read_scratch.assign(
      static_cast<size_t>(format.period_frames) * format.channels, 0.0f);
  impl_->write_scratch.assign(
      static_cast<size_t>(format.period_frames) * format.channels, 0.0f);
  impl_->max_input_frames = std::max<size_t>(format.period_frames * 8, 4096);
  const unsigned native_input =
      std::max(in_channels, static_cast<unsigned>(format.channels));
  impl_->input_bytes.assign(sizeof(AudioBufferList) + impl_->max_input_frames *
                                                          native_input *
                                                          sizeof(float),
                            0);
  impl_->output_map_scratch.assign(impl_->max_input_frames * format.channels, 0.0f);
  impl_->input_map_scratch.assign(impl_->max_input_frames * format.channels, 0.0f);

  AudioComponentDescription description{};
  description.componentType = kAudioUnitType_Output;
  description.componentSubType = kAudioUnitSubType_HALOutput;
  description.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent component = AudioComponentFindNext(nullptr, &description);
  if (component == nullptr) {
    return fail(error, "coreaudio: no HAL output AudioUnit on this system");
  }

  // Each unit is opened at its device's channel count (see float_format).
  const AudioStreamBasicDescription out_asbd = float_format(format, out_channels);
  const AudioStreamBasicDescription in_asbd = float_format(format, in_channels);
  const UInt32 on = 1;
  const UInt32 off = 0;

  // ---- the output side: this process plays into the device ----
  if (out_channels >= format.channels) {
    if (AudioComponentInstanceNew(component, &impl_->output_unit) != noErr) {
      return fail(error, "coreaudio: cannot create the output AudioUnit");
    }
    AudioUnitSetProperty(impl_->output_unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &on, sizeof(on));
    AudioUnitSetProperty(impl_->output_unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &off, sizeof(off));
    AudioUnitSetProperty(impl_->output_unit, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &impl_->output_device,
                         sizeof(impl_->output_device));
    const OSStatus set_format =
        AudioUnitSetProperty(impl_->output_unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &out_asbd, sizeof(out_asbd));
    if (set_format != noErr) {
      return fail(error, "coreaudio: the device refuses interleaved float32 at " +
                             std::to_string(format.sample_rate) + " Hz");
    }
    AURenderCallbackStruct callback{&Impl::output_callback, impl_.get()};
    AudioUnitSetProperty(impl_->output_unit, kAudioUnitProperty_SetRenderCallback,
                         kAudioUnitScope_Input, 0, &callback, sizeof(callback));
    if (AudioUnitInitialize(impl_->output_unit) != noErr) {
      return fail(error, "coreaudio: cannot initialize the output AudioUnit");
    }
  }

  // ---- the input side: the device gives this process audio ----
  if (in_channels >= format.channels) {
    if (AudioComponentInstanceNew(component, &impl_->input_unit) != noErr) {
      return fail(error, "coreaudio: cannot create the input AudioUnit");
    }
    AudioUnitSetProperty(impl_->input_unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Input, 1, &on, sizeof(on));
    AudioUnitSetProperty(impl_->input_unit, kAudioOutputUnitProperty_EnableIO,
                         kAudioUnitScope_Output, 0, &off, sizeof(off));
    AudioUnitSetProperty(impl_->input_unit, kAudioOutputUnitProperty_CurrentDevice,
                         kAudioUnitScope_Global, 0, &impl_->input_device,
                         sizeof(impl_->input_device));
    const OSStatus set_format =
        AudioUnitSetProperty(impl_->input_unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Output, 1, &in_asbd, sizeof(in_asbd));
    if (set_format != noErr) {
      return fail(error, "coreaudio: the device refuses interleaved float32 at " +
                             std::to_string(format.sample_rate) + " Hz");
    }
    AURenderCallbackStruct callback{&Impl::input_callback, impl_.get()};
    AudioUnitSetProperty(impl_->input_unit,
                         kAudioOutputUnitProperty_SetInputCallback,
                         kAudioUnitScope_Global, 0, &callback, sizeof(callback));
    if (AudioUnitInitialize(impl_->input_unit) != noErr) {
      return fail(error, "coreaudio: cannot initialize the input AudioUnit");
    }
  }

  if (impl_->output_unit != nullptr &&
      AudioOutputUnitStart(impl_->output_unit) != noErr) {
    return fail(error, "coreaudio: cannot start the output AudioUnit");
  }
  if (impl_->input_unit != nullptr &&
      AudioOutputUnitStart(impl_->input_unit) != noErr) {
    return fail(error, "coreaudio: cannot start the input AudioUnit");
  }

  impl_->detail = device_name(impl_->output_device) + " " +
                  std::to_string(format.channels) + "ch @" +
                  std::to_string(format.sample_rate) + "Hz, float32 rings (" +
                  std::to_string(out_channels) + " out / " +
                  std::to_string(in_channels) + " in available)";
  impl_->opened = true;
  log().write(LogLevel::info, "coreaudio: opened " + impl_->detail);
  return true;
}

void CoreAudioBackend::close() {
  if (impl_ == nullptr) {
    return;
  }
  if (impl_->output_unit != nullptr) {
    AudioOutputUnitStop(impl_->output_unit);
    AudioUnitUninitialize(impl_->output_unit);
    AudioComponentInstanceDispose(impl_->output_unit);
    impl_->output_unit = nullptr;
  }
  if (impl_->input_unit != nullptr) {
    AudioOutputUnitStop(impl_->input_unit);
    AudioUnitUninitialize(impl_->input_unit);
    AudioComponentInstanceDispose(impl_->input_unit);
    impl_->input_unit = nullptr;
  }
  impl_->to_device.close();
  impl_->from_device.close();
  impl_->opened = false;
}

bool CoreAudioBackend::is_open() const {
  return impl_ != nullptr && impl_->opened;
}

bool CoreAudioBackend::read(uint8_t* destination, unsigned frames,
                            std::string* error) {
  if (!is_open()) {
    return fail(error, "coreaudio: not open");
  }
  if (destination == nullptr || frames == 0) {
    return fail(error, "coreaudio: no buffer to read into");
  }

  // Device-paced, the way the engine expects: wait for the callback to have
  // produced a whole period, then take it. Waiting happens here, on the engine's
  // thread, never in the callback.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (impl_->from_device.available_read() < frames &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }

  std::fill(impl_->read_scratch.begin(), impl_->read_scratch.end(), 0.0f);
  const size_t got = impl_->from_device.read(impl_->read_scratch.data(), frames);
  if (got < frames) {
    // The device gave less than a period: silence for the rest, counted, exactly
    // as the contract says. A silent input is not a failure.
    impl_->overruns.fetch_add(1);
  }
  if (impl_->format.sample_bytes == 2) {
    for (size_t sample = 0;
         sample < static_cast<size_t>(frames) * impl_->format.channels; ++sample) {
      float scaled = impl_->read_scratch[sample] * 32767.0f;
      if (scaled > 32767.0f)
        scaled = 32767.0f;
      if (scaled < -32768.0f)
        scaled = -32768.0f;
      const int16_t value = static_cast<int16_t>(scaled);
      destination[sample * 2] = static_cast<uint8_t>(value & 0xff);
      destination[sample * 2 + 1] = static_cast<uint8_t>((value >> 8) & 0xff);
    }
  } else {
    float_to_s24_3le(impl_->read_scratch.data(), frames, impl_->format.channels,
                     destination);
  }
  return true;
}

bool CoreAudioBackend::write(const uint8_t* source, unsigned frames,
                             std::string* error) {
  if (!is_open()) {
    return fail(error, "coreaudio: not open");
  }
  if (source == nullptr || frames == 0) {
    return fail(error, "coreaudio: no audio to write");
  }

  if (impl_->format.sample_bytes == 2) {
    for (size_t sample = 0;
         sample < static_cast<size_t>(frames) * impl_->format.channels; ++sample) {
      const int16_t value = static_cast<int16_t>(
          static_cast<uint16_t>(source[sample * 2]) |
          (static_cast<uint16_t>(source[sample * 2 + 1]) << 8));
      impl_->write_scratch[sample] = static_cast<float>(value) / 32768.0f;
    }
  } else {
    s24_3le_to_float(source, frames, impl_->format.channels,
                     impl_->write_scratch.data());
  }

  // Device-paced: wait for the output callback to have drained room.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (impl_->to_device.available_write() < frames &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  }
  if (impl_->to_device.write(impl_->write_scratch.data(), frames) < frames) {
    impl_->underruns.fetch_add(1);
  }
  return true;
}

std::string CoreAudioBackend::kind() const {
  return "coreaudio";
}

std::string CoreAudioBackend::detail() const {
  return impl_->detail.empty() ? std::string("CoreAudio device (not open)")
                               : impl_->detail;
}

const AudioFormat& CoreAudioBackend::format() const {
  return impl_->format;
}

unsigned CoreAudioBackend::overruns() const {
  return impl_->overruns.load();
}

unsigned CoreAudioBackend::underruns() const {
  return impl_->underruns.load();
}

}  // namespace aes67_srt::audio

#endif  // AES67_SRT_WITH_COREAUDIO
