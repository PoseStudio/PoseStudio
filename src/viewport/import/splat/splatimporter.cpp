/**
 * @file splatimporter.cpp
 * @brief The Gaussian splat readers. See splatimporter.h.
 */

#include "splatimporter.h"

#include "figuredocument.h" // gunzip (miniz): an .spz is one gzip stream

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace pose {

namespace {

constexpr float kShC0 = 0.28209479177387814f;

std::string lowerExtension(const std::string& path) {
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) {
        return {};
    }
    std::string ext = path.substr(dot);
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

std::vector<char> readWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    if (size <= 0) {
        throw std::runtime_error("empty file: " + path);
    }
    std::vector<char> bytes(static_cast<std::size_t>(size));
    in.read(bytes.data(), size);
    if (!in) {
        throw std::runtime_error("could not read " + path);
    }
    return bytes;
}

float sigmoid(float x) {
    return 1.0f / (1.0f + std::exp(-x));
}

void finishRotation(glm::vec4& q) {
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    q = len > 1e-8f ? q / len : glm::vec4(1.0f, 0.0f, 0.0f, 0.0f);
}

/// One PLY vertex property: its name, its byte size and its offset in the record.
struct PlyProperty {
    std::string name;
    int         size = 4;
    bool        isFloat = true;  // float / double; else an integer type
    bool        isDouble = false;
    bool        isSigned = true;
    std::size_t offset = 0;
};

SplatData loadPly(const std::string& path) {
    const std::vector<char> bytes = readWholeFile(path);
    // The header: ASCII lines to "end_header".
    const char* begin = bytes.data();
    const char* end = begin + bytes.size();
    const char* headerEnd = nullptr;
    {
        const char* marker = "end_header";
        const std::size_t limit = std::min<std::size_t>(bytes.size(), 1u << 20);
        for (std::size_t i = 0; i + 10 <= limit; ++i) {
            if (std::memcmp(begin + i, marker, 10) == 0) {
                const char* p = begin + i + 10;
                while (p < end && (*p == '\r' || *p == '\n')) {
                    ++p;
                    if (p[-1] == '\n') {
                        break;
                    }
                }
                headerEnd = p;
                break;
            }
        }
    }
    if (headerEnd == nullptr) {
        throw std::runtime_error("not a PLY file (no end_header): " + path);
    }
    std::istringstream header(std::string(begin, headerEnd));
    std::string        line;
    bool               binaryLittle = false;
    std::size_t        vertexCount = 0;
    bool               inVertex = false;
    bool               chunked = false;
    std::vector<PlyProperty> props;
    std::size_t        recordSize = 0;
    while (std::getline(header, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        std::istringstream ls(line);
        std::string        word;
        ls >> word;
        if (word == "format") {
            std::string fmt;
            ls >> fmt;
            binaryLittle = fmt == "binary_little_endian";
        } else if (word == "element") {
            std::string name;
            std::size_t count = 0;
            ls >> name >> count;
            if (name == "chunk") {
                chunked = true;
            }
            inVertex = name == "vertex";
            if (inVertex) {
                vertexCount = count;
            }
        } else if (word == "property" && inVertex) {
            std::string type, name;
            ls >> type;
            if (type == "list") {
                throw std::runtime_error("a PLY with list properties on its vertices is not a splat scene: " + path);
            }
            ls >> name;
            PlyProperty p;
            p.name = name;
            if (type == "float" || type == "float32") {
                p.size = 4; p.isFloat = true;
            } else if (type == "double" || type == "float64") {
                p.size = 8; p.isFloat = true; p.isDouble = true;
            } else if (type == "uchar" || type == "uint8") {
                p.size = 1; p.isFloat = false; p.isSigned = false;
            } else if (type == "char" || type == "int8") {
                p.size = 1; p.isFloat = false;
            } else if (type == "ushort" || type == "uint16") {
                p.size = 2; p.isFloat = false; p.isSigned = false;
            } else if (type == "short" || type == "int16") {
                p.size = 2; p.isFloat = false;
            } else if (type == "uint" || type == "uint32") {
                p.size = 4; p.isFloat = false; p.isSigned = false;
            } else if (type == "int" || type == "int32") {
                p.size = 4; p.isFloat = false;
            } else {
                throw std::runtime_error("unknown PLY property type '" + type + "' in " + path);
            }
            p.offset = recordSize;
            recordSize += static_cast<std::size_t>(p.size);
            props.push_back(p);
        }
    }
    if (chunked) {
        throw std::runtime_error("a compressed (chunked) splat PLY is not supported yet; export an uncompressed PLY: " + path);
    }
    if (!binaryLittle) {
        throw std::runtime_error("only binary little-endian PLY files are read (this one is ASCII or big-endian): " + path);
    }
    if (vertexCount == 0 || props.empty()) {
        throw std::runtime_error("the PLY has no vertices: " + path);
    }
    std::unordered_map<std::string, const PlyProperty*> byName;
    for (const PlyProperty& p : props) {
        byName.emplace(p.name, &p);
    }
    auto find = [&](const char* name) -> const PlyProperty* {
        const auto it = byName.find(name);
        return it == byName.end() ? nullptr : it->second;
    };
    const PlyProperty* px = find("x");
    const PlyProperty* py = find("y");
    const PlyProperty* pz = find("z");
    const PlyProperty* ps[3] = {find("scale_0"), find("scale_1"), find("scale_2")};
    const PlyProperty* pr[4] = {find("rot_0"), find("rot_1"), find("rot_2"), find("rot_3")};
    const PlyProperty* po = find("opacity");
    const PlyProperty* pdc[3] = {find("f_dc_0"), find("f_dc_1"), find("f_dc_2")};
    if (!px || !py || !pz) {
        throw std::runtime_error("the PLY has no x/y/z positions: " + path);
    }
    if (!ps[0] || !ps[1] || !ps[2] || !pr[0] || !pr[1] || !pr[2] || !pr[3]) {
        throw std::runtime_error("not a Gaussian splat PLY (no scale_0..2 / rot_0..3 per vertex — a plain point cloud?): " + path);
    }
    // The higher harmonics: f_rest_0 .. f_rest_(3*(coeffs-1)-1), channel-major in the reference's
    // layout (every coefficient of R, then G, then B).
    int rest = 0;
    while (find(("f_rest_" + std::to_string(rest)).c_str()) != nullptr) {
        ++rest;
    }
    int degree = 0;
    if (pdc[0] && pdc[1] && pdc[2]) {
        if (rest >= 45) {
            degree = 3;
        } else if (rest >= 24) {
            degree = 2;
        } else if (rest >= 9) {
            degree = 1;
        }
    }
    const std::size_t dataBytes = static_cast<std::size_t>(end - headerEnd);
    if (dataBytes < vertexCount * recordSize) {
        throw std::runtime_error("the PLY is truncated (" + std::to_string(dataBytes) + " bytes of vertex data for " +
                                 std::to_string(vertexCount) + " splats of " + std::to_string(recordSize) + "): " + path);
    }

    auto readValue = [](const char* rec, const PlyProperty& p) -> float {
        const char* at = rec + p.offset;
        if (p.isFloat) {
            if (p.isDouble) {
                double d;
                std::memcpy(&d, at, 8);
                return static_cast<float>(d);
            }
            float f;
            std::memcpy(&f, at, 4);
            return f;
        }
        switch (p.size) {
        case 1: return p.isSigned ? static_cast<float>(*reinterpret_cast<const int8_t*>(at)) : static_cast<float>(*reinterpret_cast<const uint8_t*>(at));
        case 2: { int16_t s; uint16_t u; if (p.isSigned) { std::memcpy(&s, at, 2); return static_cast<float>(s); } std::memcpy(&u, at, 2); return static_cast<float>(u); }
        default: { int32_t s; uint32_t u; if (p.isSigned) { std::memcpy(&s, at, 4); return static_cast<float>(s); } std::memcpy(&u, at, 4); return static_cast<float>(u); }
        }
    };

    SplatData data;
    data.format = "ply";
    data.shDegree = degree;
    const int coeffs = data.coefficients();
    data.positions.resize(vertexCount);
    data.scales.resize(vertexCount);
    data.rotations.resize(vertexCount);
    data.opacities.resize(vertexCount);
    data.sh.resize(vertexCount * static_cast<std::size_t>(coeffs) * 3);
    std::vector<const PlyProperty*> restProps;
    if (degree > 0) {
        restProps.resize(static_cast<std::size_t>(3 * (coeffs - 1)));
        for (std::size_t i = 0; i < restProps.size(); ++i) {
            restProps[i] = find(("f_rest_" + std::to_string(i)).c_str());
        }
    }
    const char* rec = headerEnd;
    for (std::size_t i = 0; i < vertexCount; ++i, rec += recordSize) {
        data.positions[i] = glm::vec3(readValue(rec, *px), readValue(rec, *py), readValue(rec, *pz));
        glm::vec3 scale(std::exp(readValue(rec, *ps[0])), std::exp(readValue(rec, *ps[1])), std::exp(readValue(rec, *ps[2])));
        // (A degenerate splat — a scale of kilometres from an unconverged training — would cover
        // the whole view; a metre is already a wall.)
        data.scales[i] = glm::clamp(scale, glm::vec3(1e-6f), glm::vec3(2.0f));
        glm::vec4 q(readValue(rec, *pr[0]), readValue(rec, *pr[1]), readValue(rec, *pr[2]), readValue(rec, *pr[3]));
        finishRotation(q);
        data.rotations[i] = q;
        data.opacities[i] = po ? sigmoid(readValue(rec, *po)) : 1.0f;
        float* sh = &data.sh[i * static_cast<std::size_t>(coeffs) * 3];
        if (pdc[0] && pdc[1] && pdc[2]) {
            sh[0] = readValue(rec, *pdc[0]);
            sh[1] = readValue(rec, *pdc[1]);
            sh[2] = readValue(rec, *pdc[2]);
        } else {
            sh[0] = sh[1] = sh[2] = 0.0f; // (a mid grey: 0.5 after the constant)
        }
        for (int k = 1; k < coeffs; ++k) {
            for (int c = 0; c < 3; ++c) {
                const PlyProperty* p = restProps[static_cast<std::size_t>(c * (coeffs - 1) + (k - 1))];
                sh[k * 3 + c] = p ? readValue(rec, *p) : 0.0f;
            }
        }
    }
    return data;
}

SplatData loadSplat(const std::string& path) {
    const std::vector<char> bytes = readWholeFile(path);
    constexpr std::size_t kRecord = 32;
    if (bytes.size() % kRecord != 0) {
        throw std::runtime_error("not a .splat file (its size is not a multiple of 32 bytes): " + path);
    }
    const std::size_t count = bytes.size() / kRecord;
    SplatData         data;
    data.format = "splat";
    data.shDegree = 0;
    data.positions.resize(count);
    data.scales.resize(count);
    data.rotations.resize(count);
    data.opacities.resize(count);
    data.sh.resize(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        const char* rec = bytes.data() + i * kRecord;
        float       f[6];
        std::memcpy(f, rec, sizeof(f));
        data.positions[i] = glm::vec3(f[0], f[1], f[2]);
        data.scales[i] = glm::clamp(glm::vec3(f[3], f[4], f[5]), glm::vec3(1e-6f), glm::vec3(2.0f));
        const auto* u = reinterpret_cast<const uint8_t*>(rec + 24);
        // The colour is the degree-0 term evaluated: (c - 0.5) / C0 puts it back as a coefficient.
        for (int c = 0; c < 3; ++c) {
            data.sh[i * 3 + static_cast<std::size_t>(c)] = (static_cast<float>(u[c]) / 255.0f - 0.5f) / kShC0;
        }
        data.opacities[i] = static_cast<float>(u[3]) / 255.0f;
        glm::vec4 q((static_cast<float>(u[4]) - 128.0f) / 128.0f, (static_cast<float>(u[5]) - 128.0f) / 128.0f,
                    (static_cast<float>(u[6]) - 128.0f) / 128.0f, (static_cast<float>(u[7]) - 128.0f) / 128.0f);
        finishRotation(q);
        data.rotations[i] = q;
    }
    return data;
}

/// The Niantic SPZ layout, versions 1-3 (one gzip stream): a 16-byte header — the magic "NGSP"
/// (0x5053474e little-endian), the version, the point count, the harmonics' degree, the
/// positions' fractional bits, a flags byte (bit 0: trained antialiased) and a reserved byte —
/// then the attributes as WHOLE ARRAYS in this order: positions (version 1: three float16; else
/// three 24-bit signed fixed-point integers over 2^fractionalBits), alphas (one byte: the opacity
/// × 255), colours (three bytes: the degree-0 coefficient × 0.15 + 0.5, × 255), scales (three
/// bytes: (log scale + 10) × 16), rotations (version 2: the quaternion's x, y, z as v × 127.5 +
/// 127.5 with w ≥ 0 derived; version 3: "smallest three" in one 32-bit word — the top two bits the
/// index of the largest component, then three 10-bit fields, a sign bit over a 9-bit magnitude
/// of the component over 1/√2, the largest derived) and the harmonics (one byte a coefficient,
/// v × 128 + 128, coefficient-major with r, g, b interleaved, the reference's basis order). The
/// frame is RUB — x right, y up, z toward the viewer — which is where turnOverYDown puts a
/// `.ply`, so an `.spz` is not turned. Version 4 (ZSTD streams behind a plaintext "NGSP" header)
/// is refused with a message.
SplatData loadSpz(const std::string& path) {
    const std::vector<char> file = readWholeFile(path);
    const auto*             raw = reinterpret_cast<const uint8_t*>(file.data());
    if (file.size() >= 4 && raw[0] == 'N' && raw[1] == 'G' && raw[2] == 'S' && raw[3] == 'P') {
        throw std::runtime_error("this .spz is version 4 (ZSTD-compressed), which is not read yet; save it as SPZ version 3 or earlier, or as a .ply: " + path);
    }
    if (file.size() < 2 || raw[0] != 0x1f || raw[1] != 0x8b) {
        throw std::runtime_error("not an .spz file (no gzip stream): " + path);
    }
    const std::vector<uint8_t> bytes = gunzip(raw, file.size());
    if (bytes.size() < 16) {
        throw std::runtime_error("the .spz is truncated (no header): " + path);
    }
    const auto u32 = [&](std::size_t o) {
        return static_cast<uint32_t>(bytes[o]) | (static_cast<uint32_t>(bytes[o + 1]) << 8) |
               (static_cast<uint32_t>(bytes[o + 2]) << 16) | (static_cast<uint32_t>(bytes[o + 3]) << 24);
    };
    if (u32(0) != 0x5053474eu) {
        throw std::runtime_error("not an .spz file (bad magic): " + path);
    }
    const uint32_t version = u32(4);
    if (version < 1 || version > 3) {
        throw std::runtime_error("SPZ version " + std::to_string(version) + " is not read (versions 1 to 3 are): " + path);
    }
    const std::size_t count = u32(8);
    const int         fileDegree = bytes[12];
    const int         fracBits = std::min<int>(bytes[13], 30);
    const uint8_t     flags = bytes[14];
    if (fileDegree < 0 || fileDegree > 4) {
        throw std::runtime_error("the .spz names a harmonics degree of " + std::to_string(fileDegree) + " (0 to 4 are defined): " + path);
    }
    if (count == 0) {
        throw std::runtime_error("the .spz holds no splats: " + path);
    }
    const int         shDim = fileDegree == 0 ? 0 : fileDegree == 1 ? 3 : fileDegree == 2 ? 8 : fileDegree == 3 ? 15 : 24;
    const std::size_t posBytes = version == 1 ? 6 : 9;
    const std::size_t rotBytes = version >= 3 ? 4 : 3;
    const std::size_t perPoint = posBytes + 1 + 3 + 3 + rotBytes + static_cast<std::size_t>(shDim) * 3;
    if (bytes.size() < 16 + count * perPoint) {
        throw std::runtime_error("the .spz is truncated (" + std::to_string(bytes.size()) + " bytes for " + std::to_string(count) +
                                 " splats of " + std::to_string(perPoint) + "): " + path);
    }
    const uint8_t* positions = bytes.data() + 16;
    const uint8_t* alphas = positions + count * posBytes;
    const uint8_t* colors = alphas + count;
    const uint8_t* scales = colors + count * 3;
    const uint8_t* rotations = scales + count * 3;
    const uint8_t* harmonics = rotations + count * rotBytes;

    SplatData data;
    data.format = "spz";
    data.yDown = false;
    data.antialiased = (flags & 0x1u) != 0;
    data.shDegree = std::min(fileDegree, 3); // (a degree-4 band is left out: the renderer evaluates to 3)
    const int coeffs = data.coefficients();
    data.positions.resize(count);
    data.scales.resize(count);
    data.rotations.resize(count);
    data.opacities.resize(count);
    data.sh.resize(count * static_cast<std::size_t>(coeffs) * 3);
    const float posScale = 1.0f / static_cast<float>(1u << fracBits);
    constexpr float kColorScale = 0.15f;
    constexpr float kInvSqrt2 = 0.70710678118654752f;
    for (std::size_t i = 0; i < count; ++i) {
        const uint8_t* p = positions + i * posBytes;
        glm::vec3      pos;
        for (int c = 0; c < 3; ++c) {
            if (version == 1) {
                const uint16_t h = static_cast<uint16_t>(p[c * 2] | (p[c * 2 + 1] << 8));
                pos[c] = glm::unpackHalf1x16(h);
            } else {
                int32_t v = static_cast<int32_t>(p[c * 3]) | (static_cast<int32_t>(p[c * 3 + 1]) << 8) |
                            (static_cast<int32_t>(p[c * 3 + 2]) << 16);
                if (v & 0x800000) {
                    v -= 0x1000000; // sign-extend the 24 bits
                }
                pos[c] = static_cast<float>(v) * posScale;
            }
        }
        data.positions[i] = pos;
        data.opacities[i] = static_cast<float>(alphas[i]) / 255.0f;
        const uint8_t* s = scales + i * 3;
        glm::vec3      scale(std::exp(static_cast<float>(s[0]) / 16.0f - 10.0f), std::exp(static_cast<float>(s[1]) / 16.0f - 10.0f),
                             std::exp(static_cast<float>(s[2]) / 16.0f - 10.0f));
        data.scales[i] = glm::clamp(scale, glm::vec3(1e-6f), glm::vec3(2.0f));
        const uint8_t* r = rotations + i * rotBytes;
        float          q[4]; // x, y, z, w — the SPZ order
        if (version >= 3) {
            const uint32_t word = static_cast<uint32_t>(r[0]) | (static_cast<uint32_t>(r[1]) << 8) |
                                  (static_cast<uint32_t>(r[2]) << 16) | (static_cast<uint32_t>(r[3]) << 24);
            const int largest = static_cast<int>(word >> 30);
            float     sum = 0.0f;
            int       field = 0;
            for (int j = 0; j < 4; ++j) {
                if (j == largest) {
                    continue;
                }
                const uint32_t g = (word >> (20 - 10 * field)) & 0x3ffu;
                ++field;
                const float mag = static_cast<float>(g & 0x1ffu) / 511.0f * kInvSqrt2;
                q[j] = (g & 0x200u) ? -mag : mag;
                sum += q[j] * q[j];
            }
            q[largest] = std::sqrt(std::max(0.0f, 1.0f - sum));
        } else {
            for (int c = 0; c < 3; ++c) {
                q[c] = (static_cast<float>(r[c]) - 127.5f) / 127.5f;
            }
            q[3] = std::sqrt(std::max(0.0f, 1.0f - (q[0] * q[0] + q[1] * q[1] + q[2] * q[2])));
        }
        glm::vec4 rot(q[3], q[0], q[1], q[2]); // (w, x, y, z)
        finishRotation(rot);
        data.rotations[i] = rot;
        float*         sh = &data.sh[i * static_cast<std::size_t>(coeffs) * 3];
        const uint8_t* col = colors + i * 3;
        for (int c = 0; c < 3; ++c) {
            sh[c] = (static_cast<float>(col[c]) / 255.0f - 0.5f) / kColorScale;
        }
        const uint8_t* h = harmonics + i * static_cast<std::size_t>(shDim) * 3;
        for (int k = 1; k < coeffs; ++k) {
            for (int c = 0; c < 3; ++c) {
                sh[k * 3 + c] = (k - 1) < shDim ? (static_cast<float>(h[(k - 1) * 3 + c]) - 128.0f) / 128.0f : 0.0f;
            }
        }
    }
    return data;
}

} // namespace

void turnOverYDown(SplatData& data) {
    // The rotation Rx(180°) as a quaternion (w, x, y, z) = (0, 1, 0, 0); a splat's rotation q
    // becomes r ⊗ q.
    for (std::size_t i = 0; i < data.count(); ++i) {
        glm::vec3& p = data.positions[i];
        p = glm::vec3(p.x, -p.y, -p.z);
        const glm::vec4& q = data.rotations[i]; // (w, x, y, z)
        // (0,1,0,0) ⊗ (w,x,y,z) = (-x, w, -z, y)
        data.rotations[i] = glm::vec4(-q.y, q.x, -q.w, q.z);
    }
    // The harmonics: each basis function is a polynomial in (x, y, z); under y → −y, z → −z a
    // monomial x^a y^b z^c changes sign by (−1)^(b+c). In the reference's coefficient order:
    // degree 1 (y, z, x), degree 2 (xy, yz, 2z²−x²−y², xz, x²−y²), degree 3 (y(3x²−y²), xyz,
    // y(4z²−x²−y²), z(2z²−3x²−3y²), x(4z²−x²−y²), z(x²−y²), x(x²−3y²)).
    static const float kSign[16] = {1.0f,
                                    -1.0f, -1.0f, 1.0f,
                                    -1.0f, 1.0f, 1.0f, -1.0f, 1.0f,
                                    -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f};
    const int coeffs = data.coefficients();
    for (std::size_t i = 0; i < data.count(); ++i) {
        float* sh = &data.sh[i * static_cast<std::size_t>(coeffs) * 3];
        for (int k = 1; k < coeffs && k < 16; ++k) {
            if (kSign[k] < 0.0f) {
                sh[k * 3] = -sh[k * 3];
                sh[k * 3 + 1] = -sh[k * 3 + 1];
                sh[k * 3 + 2] = -sh[k * 3 + 2];
            }
        }
    }
}

SplatData loadSplatFile(const std::string& path) {
    const std::string ext = lowerExtension(path);
    if (ext == ".ply") {
        return loadPly(path);
    }
    if (ext == ".splat") {
        return loadSplat(path);
    }
    if (ext == ".spz") {
        return loadSpz(path);
    }
    if (ext == ".ksplat") {
        throw std::runtime_error("the .ksplat format is not supported yet; export a .ply, a .splat or an .spz: " + path);
    }
    throw std::runtime_error("not a Gaussian splat file (.ply, .splat or .spz): " + path);
}

} // namespace pose
