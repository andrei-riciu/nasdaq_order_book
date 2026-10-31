#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
namespace itch {

class MappedFile {
public:
  explicit MappedFile(const char *path); // throws std::system_error on failure
  ~MappedFile();

  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;

  std::span<const std::uint8_t> bytes() const { return {data_, size_}; }

private:
  const std::uint8_t *data_ = nullptr;
  std::size_t size_ = 0;
};

} // namespace itch
