#pragma once

// A named shared-memory region, so two unrelated processes — the HAL plug-in's host
// process and the application — can map the same audio block (ADR 0007, spec 0002).
// `SharedAudio` describes what lies in the memory; this class is how the memory is
// obtained and how its name is tidied up.
//
// POSIX `shm_open`, which Linux and macOS both provide. A mapping survives
// `unlink`, so the name can be cleaned up while the region is still in use; only
// the name goes, not the memory.

#include <cstddef>
#include <string>

namespace aes67_srt::audio {

/**
 * The longest name POSIX shared memory accepts here. macOS caps it at 31
 * characters (`PSHMNAMLEN`) where Linux allows far more, so the product honours the
 * shorter limit and refuses a longer name by name rather than failing at
 * `shm_open` with a bare `ENAMETOOLONG`. The product's own name is well inside it.
 */
inline constexpr size_t kMaxSharedRegionName = 31;

class SharedRegion {
 public:
  SharedRegion() = default;
  ~SharedRegion();
  SharedRegion(const SharedRegion&) = delete;
  SharedRegion& operator=(const SharedRegion&) = delete;

  /**
   * Open the region `name`, creating it if it does not exist, and map `bytes` of
   * it.
   *
   * `*created` says whether **this** call made it, which is what tells the caller
   * to initialize the block: the creator calls `SharedAudio::create`, everyone else
   * `SharedAudio::attach`. `name` must begin with a slash, as `shm_open` requires.
   */
  bool open(const std::string& name, size_t bytes, bool* created,
            std::string* error);

  /** Unmap. Does not remove the name; that is `unlink`, and the creator's job. */
  void close();
  bool is_open() const { return data_ != nullptr; }

  void* data() const { return data_; }
  size_t bytes() const { return bytes_; }
  const std::string& name() const { return name_; }

  /**
   * Remove the name. The region stays alive while a mapping holds it, so this is
   * safe while either side is still using it — the mappings keep the memory.
   */
  static bool unlink(const std::string& name);

 private:
  std::string name_;
  int fd_ = -1;
  void* data_ = nullptr;
  size_t bytes_ = 0;
};

}  // namespace aes67_srt::audio