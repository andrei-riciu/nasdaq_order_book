#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <system_error>

#include "itch/mapped_file.hpp"

namespace fs = std::filesystem;

namespace {
fs::path write_temp(const char *name, std::string_view contents) {
  const auto path = fs::temp_directory_path() / name;
  std::ofstream(path, std::ios::binary) << contents;
  return path;
}
} // namespace

TEST(MappedFile, MapsWholeFile) {
  const auto path = write_temp("itch_mf_contents.bin", "ITCH");
  {
    itch::MappedFile f(path.c_str());
    ASSERT_EQ(f.bytes().size(), 4u);
    EXPECT_EQ(f.bytes()[0], std::uint8_t{'I'});
    EXPECT_EQ(f.bytes()[3], std::uint8_t{'H'});
  } // unmapped here, before the file is removed
  fs::remove(path);
}

TEST(MappedFile, EmptyFileGivesEmptySpan) {
  const auto path = write_temp("itch_mf_empty.bin", "");
  {
    itch::MappedFile f(path.c_str());
    EXPECT_TRUE(f.bytes().empty());
  }
  fs::remove(path);
}

TEST(MappedFile, MissingFileThrows) {
  EXPECT_THROW(itch::MappedFile("/nonexistent/itch.bin"), std::system_error);
}
