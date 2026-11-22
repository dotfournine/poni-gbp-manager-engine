#include "core/mapped_file.hpp"

#include "core/crypto.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <system_error>

namespace poni::core {

namespace {

[[noreturn]] void fail(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

}  // namespace

std::shared_ptr<const MappedFile> MappedFile::map(int fd) {
    struct stat info {};
    if (::fstat(fd, &info) != 0) fail("fstat of a file to map");
    const auto size = static_cast<std::size_t>(info.st_size);
    // An empty file has nothing to map (mmap refuses a length of 0).
    if (size == 0) return std::shared_ptr<const MappedFile>(new MappedFile(nullptr, 0));
    void* data = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) fail("mmap of a file");
    return std::shared_ptr<const MappedFile>(new MappedFile(data, size));
}

MappedFile::~MappedFile() {
    if (data_) ::munmap(data_, size_);
}

int nameless_file() {
    const auto folder = std::filesystem::temp_directory_path();
    // Random, so that a name another process made up cannot be taken, and O_EXCL makes sure of it.
    const auto path = folder / ("poni-upload-" + random_token(16));
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) fail("open of a file in the temporary folder");
    // From here the file has no name: it lives as long as its descriptor and its mappings.
    if (::unlink(path.c_str()) != 0) {
        const int error = errno;
        ::close(fd);
        throw std::system_error(error, std::generic_category(), "unlink of a new file");
    }
    return fd;
}

}  // namespace poni::core
