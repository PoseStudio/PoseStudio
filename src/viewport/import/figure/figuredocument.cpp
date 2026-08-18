/**
 * @file figuredocument.cpp
 * @brief Implementation of FigureDocument + the gzip helpers. See figuredocument.h.
 */

#include "figuredocument.h"

#include <miniz.h>

#include <fstream>
#include <stdexcept>

namespace pose {

bool isGzip(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= 2 && bytes[0] == 0x1f && bytes[1] == 0x8b;
}

std::vector<uint8_t> gunzip(const uint8_t* data, std::size_t size) {
    // Minimal gzip (RFC 1952) container parse, then raw-DEFLATE inflate of the payload via miniz.
    // We use miniz's tinfl (raw deflate) rather than its zlib API because the zlib API expects a
    // zlib header, not a gzip one — so we strip the gzip framing ourselves.
    if (size < 18 || data[0] != 0x1f || data[1] != 0x8b) {
        throw std::runtime_error("gunzip: not a gzip stream");
    }
    if (data[2] != 8) { // CM: only DEFLATE is defined
        throw std::runtime_error("gunzip: unsupported compression method");
    }

    const uint8_t flg = data[3];
    std::size_t pos = 10; // fixed header: magic(2) CM(1) FLG(1) MTIME(4) XFL(1) OS(1)

    auto need = [&](std::size_t n) {
        if (pos + n > size) {
            throw std::runtime_error("gunzip: truncated gzip header");
        }
    };
    if (flg & 0x04) { // FEXTRA: 2-byte length + payload
        need(2);
        const std::size_t xlen = static_cast<std::size_t>(data[pos]) |
                                 (static_cast<std::size_t>(data[pos + 1]) << 8);
        pos += 2;
        need(xlen);
        pos += xlen;
    }
    if (flg & 0x08) { // FNAME: zero-terminated
        while (pos < size && data[pos] != 0) ++pos;
        need(1);
        ++pos;
    }
    if (flg & 0x10) { // FCOMMENT: zero-terminated
        while (pos < size && data[pos] != 0) ++pos;
        need(1);
        ++pos;
    }
    if (flg & 0x02) { // FHCRC: 2-byte header CRC
        need(2);
        pos += 2;
    }

    // The remaining bytes are the DEFLATE stream followed by an 8-byte trailer (CRC32 + ISIZE).
    if (pos + 8 > size) {
        throw std::runtime_error("gunzip: no room for gzip trailer");
    }
    const std::size_t deflateSize = size - pos - 8;

    std::size_t outLen = 0;
    void* out = tinfl_decompress_mem_to_heap(data + pos, deflateSize, &outLen, 0 /*raw deflate*/);
    if (!out) {
        throw std::runtime_error("gunzip: inflate failed");
    }
    std::vector<uint8_t> result(static_cast<const uint8_t*>(out),
                                static_cast<const uint8_t*>(out) + outLen);
    mz_free(out);
    return result;
}

FigureDocument FigureDocument::loadFromBytes(std::vector<uint8_t> bytes, const std::string& sourceName) {
    if (isGzip(bytes)) {
        bytes = gunzip(bytes.data(), bytes.size());
    }
    try {
        // The payload is UTF-8 JSON. Parse directly over the byte range.
        nlohmann::json root = nlohmann::json::parse(bytes.begin(), bytes.end());
        return FigureDocument(std::move(root));
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error("Failed to parse figure file '" + sourceName + "': " + e.what());
    }
}

FigureDocument FigureDocument::loadFromFile(const std::string& path) {
    // One sized read, not an istreambuf_iterator: the iterator pulls a character at a time through
    // virtual calls, which was measurable across the hundreds of files a figure import touches.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Cannot open figure file: " + path);
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Figure file is empty: " + path);
    }
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), size)) {
        throw std::runtime_error("Cannot read figure file: " + path);
    }
    return loadFromBytes(std::move(bytes), path);
}

} // namespace pose
