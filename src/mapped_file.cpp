#include "itch/mapped_file.hpp"
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace itch {
namespace {
[[noreturn]] void fail(int err, const char *what, const char *path) {
  throw std::system_error(err, std::generic_category(),
                          std::string(what) + " " + path);
}
} // namespace

MappedFile::MappedFile(const char *path) {
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd == -1)
    fail(errno, "open", path);

  struct stat st{};
  if (::fstat(fd, &st) == -1) {
    const int err = errno;
    ::close(fd);
    fail(err, "fstat", path);
  }
  size_ = static_cast<std::size_t>(st.st_size);

  if (size_ == 0) {
    ::close(fd);
    return;
  }

  void *p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
  const int err = errno;
  ::close(fd);
  if (p == MAP_FAILED)
    fail(err, "mmap", path);

  ::madvise(p, size_, MADV_SEQUENTIAL);

  data_ = static_cast<const std::uint8_t *>(p);
}

MappedFile::~MappedFile() {
  if (data_)
    ::munmap(const_cast<std::uint8_t *>(data_), size_);
}
} // namespace itch
