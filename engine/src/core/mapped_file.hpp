// A file on disk seen as bytes, without reading it into memory: an uploaded
// photo or video (D-101). The server writes a large request body into a
// file that has no name any more, maps it here, and the bytes go from the
// mapping to the checks and to the bucket. The pages are the file's and the
// system may drop them and read them again, so a video of 75 MB costs the
// engine no memory of its own; the file is gone when the last mapping is.
#pragma once

#include <cstddef>
#include <memory>
#include <string_view>

namespace poni::core {

class MappedFile {
public:
    // The whole of an open file, read-only. The descriptor may be closed
    // afterwards; the mapping keeps the file. Throws std::system_error.
    static std::shared_ptr<const MappedFile> map(int fd);

    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    std::string_view bytes() const { return {static_cast<const char*>(data_), size_}; }
    std::size_t size() const { return size_; }

private:
    MappedFile(void* data, std::size_t size) : data_(data), size_(size) {}
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

// A new file in the system's temporary folder (TMPDIR, else /tmp), open for
// reading and writing, under a random name that is removed before this
// returns: nothing can open it by name, and the system frees it when its
// last descriptor and mapping are gone, also after a crash. So there is
// nothing to clean up. The caller closes the descriptor. Throws
// std::system_error.
int nameless_file();

}  // namespace poni::core
