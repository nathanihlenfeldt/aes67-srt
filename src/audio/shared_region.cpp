#include "audio/shared_region.hpp"

#if defined(__unix__) || defined(__APPLE__)

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

#endif

namespace aes67_srt::audio {

namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

}  // namespace

SharedRegion::~SharedRegion() {
  close();
}

#if defined(__unix__) || defined(__APPLE__)

bool SharedRegion::open(const std::string& name, size_t bytes, bool* created,
                        std::string* error) {
  if (created != nullptr) {
    *created = false;
  }
  if (is_open()) {
    return fail(error, "shared region: already open");
  }
  if (name.empty() || name[0] != '/') {
    return fail(error,
                "shared region name: must begin with '/' (got \"" + name + "\")");
  }
  if (bytes == 0) {
    return fail(error, "shared region bytes: must be positive");
  }

  // Try to create it exclusively first; if it already exists, open the existing
  // one. This is the only reliable way to know whether *this* process is the
  // creator.
  bool made_it = false;
  int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd >= 0) {
    made_it = true;
  } else if (errno == EEXIST) {
    fd = shm_open(name.c_str(), O_RDWR, 0600);
  }
  if (fd < 0) {
    return fail(error, "shared region open: " + std::string(std::strerror(errno)));
  }

  if (made_it && ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    const std::string reason = std::strerror(errno);
    ::close(fd);
    shm_unlink(name.c_str());
    return fail(error, "shared region size: " + reason);
  }

  void* mapping = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    const std::string reason = std::strerror(errno);
    ::close(fd);
    return fail(error, "shared region map: " + reason);
  }

  name_ = name;
  fd_ = fd;
  data_ = mapping;
  bytes_ = bytes;
  if (created != nullptr) {
    *created = made_it;
  }
  return true;
}

void SharedRegion::close() {
  if (data_ != nullptr) {
    munmap(data_, bytes_);
  }
  if (fd_ >= 0) {
    ::close(fd_);
  }
  data_ = nullptr;
  fd_ = -1;
  bytes_ = 0;
  name_.clear();
}

bool SharedRegion::unlink(const std::string& name) {
  if (name.empty() || name[0] != '/') {
    return false;
  }
  return shm_unlink(name.c_str()) == 0;
}

#else

// Not a target platform; the region refuses rather than pretending.
bool SharedRegion::open(const std::string&, size_t, bool* created,
                        std::string* error) {
  if (created != nullptr) {
    *created = false;
  }
  return fail(error, "shared region: not supported on this platform");
}

void SharedRegion::close() {}

bool SharedRegion::unlink(const std::string&) {
  return false;
}

#endif

}  // namespace aes67_srt::audio