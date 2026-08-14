#pragma once

#include "OutputStream.h"

#include <cstdio>
#include <cstdlib>

//------------------------------------------------------------------------------------------------------------------------------------------
// Represents a generic output stream
//------------------------------------------------------------------------------------------------------------------------------------------
class FileOutputStream final : public OutputStream {
public:
    inline FileOutputStream(const char* const filePath, const bool bAppend) THROWS
        : mpFile(std::fopen(filePath, (bAppend) ?  "wab" : "wb"))
    {
        if (!mpFile)
            std::abort();  // Xbox: abort on file open failure (no exceptions)
    }

    inline FileOutputStream(FileOutputStream&& other) noexcept
        : mpFile(other.mpFile)
    {
        other.mpFile = nullptr;
    }

    virtual ~FileOutputStream() noexcept override {
        if (mpFile) {
            std::fclose(mpFile);
            mpFile = nullptr;
        }
    }

    virtual void writeBytes(const void* const pSrcBytes, const size_t numBytes) THROWS override {
        if (numBytes > 0) {
            if (std::fwrite(pSrcBytes, numBytes, 1, mpFile) != 1)
                std::abort();  // Xbox: abort on write failure (no exceptions)
        }
    }

    virtual void fillBytes(const size_t numBytes, const std::byte byteValue) THROWS override {
        for (size_t i = 0; i < numBytes; ++i) {
            if (std::fwrite(&byteValue, 1, 1, mpFile) != 1)
                std::abort();  // Xbox: abort on write failure (no exceptions)
        }
    }

    virtual size_t tell() THROWS override {
        const long offset = std::ftell(mpFile);

        if (offset < 0)
            std::abort();  // Xbox: abort on tell failure (no exceptions)

        return (size_t) offset;
    }

    virtual void flush() THROWS override {
        if (std::fflush(mpFile) != 0)
            std::abort();  // Xbox: abort on flush failure (no exceptions)
    }

private:
    inline FileOutputStream(const FileOutputStream& other) = delete;
    inline FileOutputStream& operator = (const FileOutputStream& other) = delete;
    inline FileOutputStream& operator = (FileOutputStream&& other) = delete;

    FILE* mpFile;
};
