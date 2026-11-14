/**
 * @file materialxfile.cpp
 * @brief Implementation of the MaterialX material reader (materialxfile.h): a small XML reader,
 *        the tracing of a shading node's inputs to values and image files, and the mapping of
 *        the four standard shading models onto MaterialParams.
 */

#include "materialxfile.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pose {

namespace {

// ============================================================================
// A SMALL XML READER — elements and attributes, which is all a MaterialX document is made of.
// Comments, processing instructions, a doctype and character data are passed over.
// ============================================================================

struct Xml {
    std::string                                      name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<Xml>>                children;
    const Xml*                                       parent = nullptr;

    const std::string* find(const char* key) const {
        for (const auto& attribute : attributes) {
            if (attribute.first == key) {
                return &attribute.second;
            }
        }
        return nullptr;
    }
    std::string get(const char* key, const std::string& fallback = std::string()) const {
        const std::string* value = find(key);
        return value != nullptr ? *value : fallback;
    }
};

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string decodeEntities(const std::string& text) {
    if (text.find('&') == std::string::npos) {
        return text;
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '&') {
            out += text[i];
            continue;
        }
        const std::size_t end = text.find(';', i);
        if (end == std::string::npos || end - i > 10) {
            out += '&';
            continue;
        }
        const std::string entity = text.substr(i + 1, end - i - 1);
        if (entity == "amp") {
            out += '&';
        } else if (entity == "lt") {
            out += '<';
        } else if (entity == "gt") {
            out += '>';
        } else if (entity == "quot") {
            out += '"';
        } else if (entity == "apos") {
            out += '\'';
        } else if (entity.size() > 1 && entity[0] == '#') {
            const bool hex = entity[1] == 'x' || entity[1] == 'X';
            const long code = std::strtol(entity.c_str() + (hex ? 2 : 1), nullptr, hex ? 16 : 10);
            // (UTF-8: a path may hold any character.)
            if (code < 0x80) {
                out += static_cast<char>(code);
            } else if (code < 0x800) {
                out += static_cast<char>(0xC0 | (code >> 6));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else if (code < 0x10000) {
                out += static_cast<char>(0xE0 | (code >> 12));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (code >> 18));
                out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (code & 0x3F));
            }
        } else {
            out += text.substr(i, end - i + 1); // an entity this reader does not know: as written
        }
        i = end;
    }
    return out;
}

/// Parses @p text into a tree under a nameless document node. Null with @p error on malformed
/// markup (an unclosed tag, a closing tag that closes nothing, an attribute without its quotes).
std::unique_ptr<Xml> parseXml(const std::string& text, std::string& error) {
    auto                document = std::make_unique<Xml>();
    std::vector<Xml*>   open{document.get()};
    const std::size_t   size = text.size();
    std::size_t         i = 0;
    const auto          fail = [&](std::size_t at, const std::string& what) {
        const long line = 1 + static_cast<long>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(std::min(at, size)), '\n'));
        error = "line " + std::to_string(line) + ": " + what;
        return std::unique_ptr<Xml>();
    };
    while (i < size) {
        const std::size_t tag = text.find('<', i);
        if (tag == std::string::npos) {
            break;
        }
        i = tag;
        if (text.compare(i, 4, "<!--") == 0) {
            const std::size_t end = text.find("-->", i + 4);
            if (end == std::string::npos) {
                return fail(i, "a comment is not closed");
            }
            i = end + 3;
            continue;
        }
        if (text.compare(i, 9, "<![CDATA[") == 0) {
            const std::size_t end = text.find("]]>", i + 9);
            if (end == std::string::npos) {
                return fail(i, "a CDATA section is not closed");
            }
            i = end + 3;
            continue;
        }
        if (text.compare(i, 2, "<?") == 0 || text.compare(i, 2, "<!") == 0) {
            const std::size_t end = text.find('>', i + 2);
            if (end == std::string::npos) {
                return fail(i, "a declaration is not closed");
            }
            i = end + 1;
            continue;
        }
        if (text.compare(i, 2, "</") == 0) {
            const std::size_t end = text.find('>', i + 2);
            if (end == std::string::npos) {
                return fail(i, "a closing tag is not closed");
            }
            std::string name = text.substr(i + 2, end - i - 2);
            while (!name.empty() && isSpace(name.back())) {
                name.pop_back();
            }
            if (open.size() < 2 || open.back()->name != name) {
                return fail(i, "</" + name + "> closes nothing");
            }
            open.pop_back();
            i = end + 1;
            continue;
        }
        // An element: its name, its attributes, and whether it stays open.
        std::size_t p = i + 1;
        const std::size_t nameStart = p;
        while (p < size && !isSpace(text[p]) && text[p] != '/' && text[p] != '>') {
            ++p;
        }
        if (p == nameStart) {
            return fail(i, "a tag has no name");
        }
        auto element = std::make_unique<Xml>();
        element->name = text.substr(nameStart, p - nameStart);
        element->parent = open.back();
        bool stays = false;
        for (;;) {
            while (p < size && isSpace(text[p])) {
                ++p;
            }
            if (p >= size) {
                return fail(i, "<" + element->name + "> is not closed");
            }
            if (text[p] == '>') {
                ++p;
                stays = true;
                break;
            }
            if (text[p] == '/' && p + 1 < size && text[p + 1] == '>') {
                p += 2;
                break;
            }
            const std::size_t keyStart = p;
            while (p < size && !isSpace(text[p]) && text[p] != '=' && text[p] != '>' && text[p] != '/') {
                ++p;
            }
            if (p == keyStart) {
                return fail(p, "unexpected '" + std::string(1, text[p]) + "' in <" + element->name + ">");
            }
            const std::string key = text.substr(keyStart, p - keyStart);
            while (p < size && isSpace(text[p])) {
                ++p;
            }
            if (p >= size || text[p] != '=') {
                element->attributes.emplace_back(key, std::string());
                continue;
            }
            ++p;
            while (p < size && isSpace(text[p])) {
                ++p;
            }
            if (p >= size || (text[p] != '"' && text[p] != '\'')) {
                return fail(p, "the value of " + key + " in <" + element->name + "> is not quoted");
            }
            const char        quote = text[p];
            const std::size_t end = text.find(quote, p + 1);
            if (end == std::string::npos) {
                return fail(p, "the value of " + key + " in <" + element->name + "> is not closed");
            }
            element->attributes.emplace_back(key, decodeEntities(text.substr(p + 1, end - p - 1)));
            p = end + 1;
        }
        Xml* raw = element.get();
        open.back()->children.push_back(std::move(element));
        if (stays) {
            open.push_back(raw);
        }
        i = p;
    }
    if (open.size() > 1) {
        return fail(size, "<" + open.back()->name + "> is not closed");
    }
    return document;
}

// ============================================================================
// WHAT AN INPUT IS — a value, or an image file (with how it is read and placed).
// ============================================================================

struct Signal {
    enum class Kind { None, Constant, Texture };
    Kind      kind = Kind::None;
    glm::vec4 value{0.0f}; ///< Constant: its components (a float in x).
    int       components = 0;
    // Texture:
    std::string file;
    int         channel = -1;   ///< The channel it is read from; -1 = the image as it is.
    glm::vec4   factor{1.0f};   ///< A constant the document multiplies the image by.
    bool        hasUv = false;  ///< It names a placement of its own:
    glm::vec2   tiling{1.0f};
    glm::vec2   offset{0.0f};
    float       rotation = 0.0f;
    bool        normalMap = false; ///< It went through a normal-map node …
    bool        bump = false;      ///< … or is a height read as relief.
    float       strength = 1.0f;   ///< … of this strength.
};

float srgbToLinear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// The numbers of a value attribute: "0.8, 0.8, 0.8", "1", "true".
std::vector<float> numbersOf(const std::string& text) {
    std::vector<float> numbers;
    std::size_t        i = 0;
    while (i < text.size()) {
        while (i < text.size() && (isSpace(text[i]) || text[i] == ',')) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        const std::size_t start = i;
        while (i < text.size() && !isSpace(text[i]) && text[i] != ',') {
            ++i;
        }
        const std::string token = lowered(text.substr(start, i - start));
        if (token == "true") {
            numbers.push_back(1.0f);
        } else if (token == "false") {
            numbers.push_back(0.0f);
        } else {
            char*       end = nullptr;
            const float number = std::strtof(token.c_str(), &end);
            if (end == token.c_str()) {
                return {}; // not a number: a string, a file name
            }
            numbers.push_back(number);
        }
    }
    return numbers;
}

/// The nearest value of an attribute that elements INHERIT (fileprefix, colorspace): the
/// element's own, else its parent's, and so on up to the document.
std::string inherited(const Xml& from, const char* key) {
    for (const Xml* at = &from; at != nullptr; at = at->parent) {
        if (const std::string* value = at->find(key)) {
            return *value;
        }
    }
    return std::string();
}

/// A node's input by name: <input> (1.38), <parameter> (1.36) or a shaderref's <bindinput>.
const Xml* portOf(const Xml& node, const char* name) {
    for (const auto& child : node.children) {
        if ((child->name == "input" || child->name == "parameter" || child->name == "bindinput") && child->get("name") == name) {
            return child.get();
        }
    }
    return nullptr;
}

int channelOf(const std::string& output) {
    if (output.empty()) {
        return -1;
    }
    switch (output.back()) { // outr / outx / r … outa / outw / a
    case 'r': case 'x': return 0;
    case 'g': case 'y': return 1;
    case 'b': case 'z': return 2;
    case 'a': case 'w': return 3;
    default: return -1;
    }
}

class Tracer {
public:
    Tracer(const Xml& root, std::vector<std::string>& notes) : m_root(root), m_notes(notes) {}

    /// What the input (or a nodegraph's output) @p in is, its `nodename` looked up in @p scope.
    Signal input(const Xml& in, const Xml& scope, int depth = 0) {
        if (depth > 48) {
            return {};
        }
        if (const std::string* value = in.find("value")) {
            return constant(in, *value);
        }
        if (const std::string* graphName = in.find("nodegraph")) {
            const Xml* graph = child(m_root, "nodegraph", *graphName);
            if (graph == nullptr) {
                return {};
            }
            const std::string wanted = in.get("output");
            const Xml*        out = nullptr;
            for (const auto& candidate : graph->children) {
                if (candidate->name == "output" && (wanted.empty() || candidate->get("name") == wanted)) {
                    out = candidate.get();
                    break;
                }
            }
            return out != nullptr ? input(*out, *graph, depth + 1) : Signal();
        }
        if (const std::string* nodeName = in.find("nodename")) {
            const Xml* node = findNode(scope, *nodeName);
            return node != nullptr ? traceNode(*node, in.get("output"), depth + 1) : Signal();
        }
        if (const std::string* interfaceName = in.find("interfacename")) {
            // An input of the enclosing nodegraph itself.
            if (const Xml* own = portOf(scope, interfaceName->c_str()); own != nullptr && own != &in) {
                return input(*own, m_root, depth + 1);
            }
        }
        return {};
    }

    const Xml* findNode(const Xml& scope, const std::string& name) const {
        for (const Xml* at : {&scope, &m_root}) {
            for (const auto& candidate : at->children) {
                if (candidate->name != "output" && candidate->name != "input" && candidate->get("name") == name) {
                    return candidate.get();
                }
            }
        }
        return nullptr;
    }

    void note(const std::string& text) {
        if (std::find(m_notes.begin(), m_notes.end(), text) == m_notes.end()) {
            m_notes.push_back(text);
        }
    }

private:
    static const Xml* child(const Xml& parent, const char* element, const std::string& name) {
        for (const auto& candidate : parent.children) {
            if (candidate->name == element && candidate->get("name") == name) {
                return candidate.get();
            }
        }
        return nullptr;
    }

    Signal constant(const Xml& in, const std::string& text) const {
        const std::vector<float> numbers = numbersOf(text);
        if (numbers.empty()) {
            return {};
        }
        Signal s;
        s.kind = Signal::Kind::Constant;
        s.components = static_cast<int>(std::min<std::size_t>(numbers.size(), 4));
        for (int i = 0; i < s.components; ++i) {
            s.value[i] = numbers[static_cast<std::size_t>(i)];
        }
        // A COLOR is in the document's working space (linear) unless a color space is named.
        if (in.get("type").rfind("color", 0) == 0 && s.components >= 3) {
            const std::string space = lowered(inherited(in, "colorspace"));
            if (space.find("srgb") != std::string::npos && space.find("lin") == std::string::npos) {
                for (int i = 0; i < 3; ++i) {
                    s.value[i] = srgbToLinear(s.value[i]);
                }
            } else if (space.find("g22") != std::string::npos) {
                for (int i = 0; i < 3; ++i) {
                    s.value[i] = std::pow(std::max(s.value[i], 0.0f), 2.2f);
                }
            }
        }
        return s;
    }

    /// The constant an input of @p node is, or @p fallback.
    glm::vec4 numberOf(const Xml& node, const char* name, const glm::vec4& fallback, int depth) {
        const Xml* port = portOf(node, name);
        if (port == nullptr) {
            return fallback;
        }
        const Signal s = input(*port, *node.parent, depth + 1);
        if (s.kind != Signal::Kind::Constant) {
            return fallback;
        }
        return s.components == 1 ? glm::vec4(s.value.x) : s.value;
    }

    /// The placement a texture node's `texcoord` input names (a place2d, a scaled texcoord).
    void placement(const Xml& port, const Xml& scope, Signal& s, int depth) {
        const std::string* nodeName = port.find("nodename");
        const Xml*         node = nodeName != nullptr && depth < 48 ? findNode(scope, *nodeName) : nullptr;
        if (node == nullptr) {
            return;
        }
        const std::string& kind = node->name;
        const glm::vec4    one(1.0f);
        const glm::vec4    zero(0.0f);
        if (kind == "place2d") {
            // uv' = rotate((uv - pivot) / scale) + pivot - offset
            const glm::vec4 scale = numberOf(*node, "scale", one, depth);
            const glm::vec4 offset = numberOf(*node, "offset", zero, depth);
            s.tiling *= glm::vec2(scale.x != 0.0f ? 1.0f / scale.x : 1.0f, scale.y != 0.0f ? 1.0f / scale.y : 1.0f);
            s.offset -= glm::vec2(offset.x, offset.y);
            s.rotation += numberOf(*node, "rotate", zero, depth).x;
            s.hasUv = true;
        } else if (kind == "UsdTransform2d") {
            const glm::vec4 scale = numberOf(*node, "scale", one, depth);
            const glm::vec4 translation = numberOf(*node, "translation", zero, depth);
            s.tiling *= glm::vec2(scale.x, scale.y);
            s.offset += glm::vec2(translation.x, translation.y);
            s.rotation += numberOf(*node, "rotation", zero, depth).x;
            s.hasUv = true;
        } else if (kind == "multiply" || kind == "divide" || kind == "add" || kind == "subtract") {
            // texcoord (in1) scaled or shifted by a constant (in2).
            const Xml* amount = portOf(*node, "in2");
            const Signal by = amount != nullptr ? input(*amount, *node->parent, depth + 1) : Signal();
            if (by.kind == Signal::Kind::Constant) {
                const glm::vec2 v = by.components == 1 ? glm::vec2(by.value.x) : glm::vec2(by.value.x, by.value.y);
                if (kind == "multiply") {
                    s.tiling *= v;
                } else if (kind == "divide") {
                    s.tiling *= glm::vec2(v.x != 0.0f ? 1.0f / v.x : 1.0f, v.y != 0.0f ? 1.0f / v.y : 1.0f);
                } else if (kind == "add") {
                    s.offset += v;
                } else {
                    s.offset -= v;
                }
                s.hasUv = true;
            }
            if (const Xml* upstream = portOf(*node, "in1")) {
                placement(*upstream, *node->parent, s, depth + 1);
            }
        }
    }

    Signal traceNode(const Xml& node, const std::string& output, int depth) {
        if (depth > 48 || node.parent == nullptr) {
            return {};
        }
        const Xml&         scope = *node.parent;
        const std::string& kind = node.name;
        const auto         arg = [&](const char* name) {
            const Xml* port = portOf(node, name);
            return port != nullptr ? input(*port, scope, depth + 1) : Signal();
        };
        const glm::vec4 one(1.0f);

        // --- an image file ---
        if (kind == "image" || kind == "tiledimage" || kind == "gltf_image" || kind == "gltf_colorimage" || kind == "gltf_normalmap" ||
            kind == "UsdUVTexture") {
            const Xml*        file = portOf(node, "file");
            const std::string name = file != nullptr ? file->get("value") : std::string();
            if (name.empty()) {
                return {};
            }
            Signal s;
            s.kind = Signal::Kind::Texture;
            s.file = inherited(*file, "fileprefix") + name;
            if (kind == "tiledimage") {
                // uv' = uv * uvtiling - uvoffset
                const glm::vec4 tiling = numberOf(node, "uvtiling", one, depth);
                const glm::vec4 offset = numberOf(node, "uvoffset", glm::vec4(0.0f), depth);
                s.tiling = glm::vec2(tiling.x, tiling.y);
                s.offset = glm::vec2(0.0f) - glm::vec2(offset.x, offset.y);
                s.hasUv = s.tiling != glm::vec2(1.0f) || s.offset != glm::vec2(0.0f);
            }
            for (const char* uvPort : {"texcoord", "st"}) {
                if (const Xml* port = portOf(node, uvPort)) {
                    placement(*port, scope, s, depth + 1);
                }
            }
            if (kind == "gltf_colorimage") {
                const glm::vec4 color = numberOf(node, "color", one, depth);
                s.factor = glm::vec4(color.x, color.y, color.z, numberOf(node, "alpha", one, depth).x);
                if (output == "outa") {
                    s.channel = 3;
                }
            } else if (kind == "gltf_image") {
                s.factor = numberOf(node, "factor", one, depth);
            } else if (kind == "gltf_normalmap") {
                s.normalMap = true;
            } else if (kind == "UsdUVTexture") {
                s.factor = numberOf(node, "scale", one, depth);
                if (output == "r" || output == "g" || output == "b" || output == "a") {
                    s.channel = channelOf(output);
                }
            }
            return s;
        }

        // --- which slot, and how strong ---
        if (kind == "normalmap") {
            Signal s = arg("in");
            s.normalMap = true;
            s.strength = numberOf(node, "scale", one, depth).x;
            return s;
        }
        if (kind == "bump" || kind == "heighttonormal") {
            Signal s = arg(kind == "bump" ? "height" : "in");
            s.bump = true;
            s.strength = numberOf(node, "scale", one, depth).x;
            return s;
        }

        // --- which channel ---
        if (kind == "extract" || kind == "swizzle" || kind.rfind("separate", 0) == 0) {
            Signal s = arg("in");
            int    channel = -1;
            if (kind == "extract") {
                channel = static_cast<int>(numberOf(node, "index", glm::vec4(0.0f), depth).x);
            } else if (kind == "swizzle") {
                const Xml*        port = portOf(node, "channels");
                const std::string channels = port != nullptr ? port->get("value") : std::string();
                channel = channels.size() == 1 ? channelOf(channels) : -1;
            } else {
                channel = channelOf(output);
            }
            if (channel >= 0 && channel < 4) {
                if (s.kind == Signal::Kind::Texture) {
                    s.channel = channel;
                } else if (s.kind == Signal::Kind::Constant && channel < s.components) {
                    s.value = glm::vec4(s.value[channel]);
                    s.components = 1;
                }
            }
            return s;
        }

        // --- a value ---
        if (kind == "constant") {
            return arg("value");
        }
        if (kind == "multiply" || kind == "add" || kind == "subtract" || kind == "divide") {
            const Signal a = arg("in1");
            const Signal b = arg("in2");
            if (a.kind == Signal::Kind::Constant && b.kind == Signal::Kind::Constant) {
                Signal s = a;
                s.components = std::max(a.components, b.components);
                const glm::vec4 x = a.components == 1 ? glm::vec4(a.value.x) : a.value;
                const glm::vec4 y = b.components == 1 ? glm::vec4(b.value.x) : b.value;
                if (kind == "multiply") {
                    s.value = x * y;
                } else if (kind == "add") {
                    s.value = x + y;
                } else if (kind == "subtract") {
                    s.value = x - y;
                } else {
                    for (int i = 0; i < 4; ++i) {
                        s.value[i] = y[i] != 0.0f ? x[i] / y[i] : x[i];
                    }
                }
                return s;
            }
            const Signal& image = a.kind == Signal::Kind::Texture ? a : b;
            const Signal& other = a.kind == Signal::Kind::Texture ? b : a;
            if (image.kind != Signal::Kind::Texture) {
                return a.kind != Signal::Kind::None ? a : b; // one side is unknown: the other stands
            }
            Signal s = image;
            if (other.kind == Signal::Kind::Constant && kind == "multiply") {
                s.factor *= other.components == 1 ? glm::vec4(other.value.x) : other.value;
            } else if (other.kind == Signal::Kind::Texture) {
                note("Two images are combined by a \"" + kind + "\" node; the first one is used.");
            } else if (other.kind == Signal::Kind::Constant) {
                note("An image is adjusted by a \"" + kind + "\" node, which is not carried: the image is used as it is.");
            }
            return s;
        }
        if (kind == "mix") {
            const Signal fg = arg("fg");
            const Signal bg = arg("bg");
            const Signal amount = arg("mix");
            if (fg.kind == Signal::Kind::Constant && bg.kind == Signal::Kind::Constant && amount.kind == Signal::Kind::Constant) {
                Signal s = fg;
                s.components = std::max(fg.components, bg.components);
                const glm::vec4 x = fg.components == 1 ? glm::vec4(fg.value.x) : fg.value;
                const glm::vec4 y = bg.components == 1 ? glm::vec4(bg.value.x) : bg.value;
                s.value = glm::mix(y, x, amount.value.x);
                return s;
            }
            note("Two inputs are blended by a \"mix\" node; one of them is used.");
            if (fg.kind == Signal::Kind::Texture) {
                return fg;
            }
            if (bg.kind == Signal::Kind::Texture) {
                return bg;
            }
            return fg.kind != Signal::Kind::None ? fg : bg;
        }
        if (kind == "invert") {
            Signal s = arg("in");
            if (s.kind == Signal::Kind::Constant) {
                const glm::vec4 amount = numberOf(node, "amount", one, depth);
                s.value = amount - s.value;
            } else if (s.kind == Signal::Kind::Texture) {
                note("An image is inverted in the file (\"" + s.file + "\"), which is not carried: it is used as it is.");
            }
            return s;
        }
        // --- a node that passes its input on (a conversion, a clamp, a color correction) ---
        if (const Xml* through = portOf(node, "in")) {
            static const char* const kPlain[] = {"convert", "dot", "clamp", "normalize", "absval", "luminance"};
            const bool               plain = std::any_of(std::begin(kPlain), std::end(kPlain), [&](const char* name) { return kind == name; });
            Signal                   s = input(*through, scope, depth + 1);
            if (!plain && s.kind == Signal::Kind::Texture) {
                note("An image is adjusted by a \"" + kind + "\" node, which is not carried: the image is used as it is.");
            }
            return s;
        }
        note("A \"" + kind + "\" node has no image to name (a procedural pattern, or a node this reader does not know): that input keeps its default.");
        return {};
    }

    const Xml&                m_root;
    std::vector<std::string>& m_notes;
};

// ============================================================================
// THE SHADING MODELS — each input's place in the material.
// ============================================================================

enum class Target {
    BaseWeight, BaseColor, DiffuseRoughness, Metalness, SpecularWeight, SpecularColor, Roughness, Ior, Anisotropy, AnisotropyTurns,
    Transmission, TransmissionColor, TransmissionDepth, Dispersion, ThinWalled,
    Subsurface, SubsurfaceColor, ScatterColor, ScatterScale, ScatterAnisotropy,
    Sheen, SheenColor, SheenRoughness,
    Coat, CoatColor, CoatRoughness, CoatIor, CoatAnisotropy, CoatTurns,
    FilmWeight, FilmNanometres, FilmMicrons, FilmIor,
    EmissionWeight, EmissionNits, EmissionColor,
    Opacity, Alpha, AlphaMode, Normal, Occlusion
};

struct Port {
    const char* name;
    Target      target;
};

const Port kStandardSurface[] = {
    {"base", Target::BaseWeight}, {"base_color", Target::BaseColor}, {"diffuse_roughness", Target::DiffuseRoughness},
    {"metalness", Target::Metalness}, {"specular", Target::SpecularWeight}, {"specular_color", Target::SpecularColor},
    {"specular_roughness", Target::Roughness}, {"specular_IOR", Target::Ior}, {"specular_anisotropy", Target::Anisotropy},
    {"specular_rotation", Target::AnisotropyTurns}, {"transmission", Target::Transmission}, {"transmission_color", Target::TransmissionColor},
    {"transmission_depth", Target::TransmissionDepth}, {"thin_walled", Target::ThinWalled}, {"subsurface", Target::Subsurface},
    {"subsurface_color", Target::SubsurfaceColor}, {"subsurface_radius", Target::ScatterColor}, {"subsurface_scale", Target::ScatterScale},
    {"subsurface_anisotropy", Target::ScatterAnisotropy}, {"sheen", Target::Sheen}, {"sheen_color", Target::SheenColor},
    {"sheen_roughness", Target::SheenRoughness}, {"coat", Target::Coat}, {"coat_color", Target::CoatColor},
    {"coat_roughness", Target::CoatRoughness}, {"coat_IOR", Target::CoatIor}, {"coat_anisotropy", Target::CoatAnisotropy},
    {"coat_rotation", Target::CoatTurns}, {"thin_film_thickness", Target::FilmNanometres}, {"thin_film_IOR", Target::FilmIor},
    {"emission", Target::EmissionWeight}, {"emission_color", Target::EmissionColor}, {"opacity", Target::Opacity}, {"normal", Target::Normal},
};

const Port kOpenPbr[] = {
    {"base_weight", Target::BaseWeight}, {"base_color", Target::BaseColor}, {"base_diffuse_roughness", Target::DiffuseRoughness},
    {"base_metalness", Target::Metalness}, {"specular_weight", Target::SpecularWeight}, {"specular_color", Target::SpecularColor},
    {"specular_roughness", Target::Roughness}, {"specular_ior", Target::Ior}, {"specular_roughness_anisotropy", Target::Anisotropy},
    {"transmission_weight", Target::Transmission}, {"transmission_color", Target::TransmissionColor},
    {"transmission_depth", Target::TransmissionDepth}, {"transmission_dispersion_scale", Target::Dispersion},
    {"subsurface_weight", Target::Subsurface}, {"subsurface_color", Target::SubsurfaceColor}, {"subsurface_radius", Target::ScatterScale},
    {"subsurface_radius_scale", Target::ScatterColor}, {"subsurface_scatter_anisotropy", Target::ScatterAnisotropy},
    {"fuzz_weight", Target::Sheen}, {"fuzz_color", Target::SheenColor}, {"fuzz_roughness", Target::SheenRoughness},
    {"coat_weight", Target::Coat}, {"coat_color", Target::CoatColor}, {"coat_roughness", Target::CoatRoughness}, {"coat_ior", Target::CoatIor},
    {"coat_roughness_anisotropy", Target::CoatAnisotropy}, {"thin_film_weight", Target::FilmWeight},
    {"thin_film_thickness", Target::FilmMicrons}, {"thin_film_ior", Target::FilmIor}, {"emission_luminance", Target::EmissionNits},
    {"emission_color", Target::EmissionColor}, {"geometry_opacity", Target::Opacity}, {"geometry_thin_walled", Target::ThinWalled},
    {"geometry_normal", Target::Normal},
};

const Port kGltfPbr[] = {
    {"base_color", Target::BaseColor}, {"metallic", Target::Metalness}, {"roughness", Target::Roughness}, {"normal", Target::Normal},
    {"occlusion", Target::Occlusion}, {"transmission", Target::Transmission}, {"specular", Target::SpecularWeight},
    {"specular_color", Target::SpecularColor}, {"ior", Target::Ior}, {"alpha", Target::Alpha}, {"alpha_mode", Target::AlphaMode},
    {"iridescence", Target::FilmWeight}, {"iridescence_ior", Target::FilmIor}, {"iridescence_thickness", Target::FilmNanometres},
    {"sheen_color", Target::SheenColor}, {"sheen_roughness", Target::SheenRoughness}, {"clearcoat", Target::Coat},
    {"clearcoat_roughness", Target::CoatRoughness}, {"emissive", Target::EmissionColor}, {"emissive_strength", Target::EmissionWeight},
    {"attenuation_distance", Target::TransmissionDepth}, {"attenuation_color", Target::TransmissionColor},
};

const Port kUsdPreview[] = {
    {"diffuseColor", Target::BaseColor}, {"emissiveColor", Target::EmissionColor}, {"metallic", Target::Metalness},
    {"roughness", Target::Roughness}, {"clearcoat", Target::Coat}, {"clearcoatRoughness", Target::CoatRoughness},
    {"opacity", Target::Opacity}, {"ior", Target::Ior}, {"normal", Target::Normal}, {"occlusion", Target::Occlusion},
};

enum class Model { None, StandardSurface, OpenPbr, GltfPbr, UsdPreview };

Model modelOf(const std::string& category) {
    if (category == "standard_surface") {
        return Model::StandardSurface;
    }
    if (category == "open_pbr_surface") {
        return Model::OpenPbr;
    }
    if (category == "gltf_pbr") {
        return Model::GltfPbr;
    }
    if (category == "UsdPreviewSurface") {
        return Model::UsdPreview;
    }
    return Model::None;
}

/// One material being put together: the values, the maps, and what is settled at the end.
struct Build {
    MaterialXMaterial& material;
    Tracer&            tracer;
    float              baseWeight = 1.0f;
    float              emissionWeight = 0.0f;
    bool               emissionWeightGiven = false;
    bool               emissionLit = false;   ///< An emission color that is not black (or an image).
    bool               sheenLit = false;
    bool               filmThick = false;     ///< A film thickness above nothing (or an image).
    bool               filmWeightGiven = false;
    bool               placed = false;        ///< The material's tiling has been taken from a map.
    Signal             alpha;                 ///< glTF: used only when the alpha mode says so.
    int                alphaMode = 0;

    MaterialParams& p() { return material.params; }

    void addTexture(MaterialMap slot, const Signal& s) {
        for (const MaterialXTexture& texture : material.textures) {
            if (texture.slot == slot) {
                return;
            }
        }
        material.textures.push_back({slot, s.file, s.channel < 0 ? 0 : s.channel});
        // The material has ONE placement for all its maps: the base color's, else the first map's.
        if (s.hasUv && (!placed || slot == MaterialMap::BaseColor)) {
            p().uvTiling = s.tiling;
            p().uvOffset = s.offset;
            p().uvRotation = s.rotation;
            placed = true;
        }
    }

    /// A value, or a map in @p slot that the value then scales (the constant the file gives
    /// it, else 1). `MaterialMap::Count` = the material has no map for this.
    void scalar(float& field, MaterialMap slot, const Signal& s, const char* what) {
        if (s.kind == Signal::Kind::Constant) {
            field = s.value.x;
        } else if (s.kind == Signal::Kind::Texture) {
            if (slot == MaterialMap::Count) {
                tracer.note(std::string("The ") + what + " is given by an image, which the material has no slot for: it keeps its default.");
                return;
            }
            addTexture(slot, s);
            field = s.factor[s.channel >= 0 && s.channel < 4 ? s.channel : 0];
        }
    }

    void color(glm::vec3& field, MaterialMap slot, const Signal& s, const char* what) {
        if (s.kind == Signal::Kind::Constant) {
            field = s.components == 1 ? glm::vec3(s.value.x) : glm::vec3(s.value);
        } else if (s.kind == Signal::Kind::Texture) {
            if (slot == MaterialMap::Count) {
                tracer.note(std::string("The ") + what + " is given by an image, which the material has no slot for: it keeps its default.");
                return;
            }
            addTexture(slot, s);
            field = glm::vec3(s.factor);
        }
    }

    void apply(Target target, const Signal& s) {
        if (s.kind == Signal::Kind::None) {
            return;
        }
        const bool   constant = s.kind == Signal::Kind::Constant;
        const float  x = s.value.x;
        const auto   lit = [&]() { return !constant || std::max(s.value.x, std::max(s.value.y, s.value.z)) > 0.0f; };
        switch (target) {
        case Target::BaseWeight:
            if (constant) {
                baseWeight = x;
            } else {
                tracer.note("The base weight is given by an image, which the material has no slot for: it is taken as 1.");
            }
            break;
        case Target::BaseColor: color(p().baseColor, MaterialMap::BaseColor, s, "base color"); break;
        case Target::DiffuseRoughness: scalar(p().diffuseRoughness, MaterialMap::DiffuseRoughness, s, "diffuse roughness"); break;
        case Target::Metalness: scalar(p().metalness, MaterialMap::Metalness, s, "metalness"); break;
        case Target::SpecularWeight: scalar(p().specular, MaterialMap::SpecularMask, s, "specular weight"); break;
        case Target::SpecularColor: color(p().specularColor, MaterialMap::SpecularColor, s, "specular color"); break;
        case Target::Roughness: scalar(p().roughness, MaterialMap::Roughness, s, "roughness"); break;
        case Target::Ior: scalar(p().ior, MaterialMap::Count, s, "index of refraction"); break;
        case Target::Anisotropy: scalar(p().anisotropy, MaterialMap::Anisotropy, s, "anisotropy"); break;
        case Target::AnisotropyTurns: {
            float turns = 0.0f; // (0..1 of a full turn in the file; degrees in the material)
            scalar(turns, MaterialMap::AnisotropyRotation, s, "anisotropy rotation");
            p().anisotropyRotation = constant ? turns * 360.0f : 0.0f;
            break;
        }
        case Target::Transmission: scalar(p().transmission, MaterialMap::TransmissionWeight, s, "transmission"); break;
        case Target::TransmissionColor: color(p().transmissionColor, MaterialMap::TransmissionColor, s, "transmission color"); break;
        case Target::TransmissionDepth: scalar(p().transmissionDepth, MaterialMap::Count, s, "transmission depth"); break;
        case Target::Dispersion: scalar(p().dispersion, MaterialMap::Count, s, "dispersion"); break;
        case Target::ThinWalled:
            if (constant) {
                p().thinWalled = x > 0.5f;
            }
            break;
        case Target::Subsurface: scalar(p().translucency, MaterialMap::TranslucencyWeight, s, "subsurface weight"); break;
        case Target::SubsurfaceColor: color(p().subsurfaceColor, MaterialMap::Translucency, s, "subsurface color"); break;
        case Target::ScatterColor: color(p().subsurfaceRadiusColor, MaterialMap::Count, s, "subsurface radius"); break;
        case Target::ScatterScale:
            if (constant) {
                p().subsurfaceRadius = x * 100.0f; // (scene units — metres — in the file; centimetres in the material)
            }
            break;
        case Target::ScatterAnisotropy: scalar(p().subsurfaceAnisotropy, MaterialMap::Count, s, "subsurface anisotropy"); break;
        case Target::Sheen: scalar(p().sheen, MaterialMap::SheenWeight, s, "sheen"); break;
        case Target::SheenColor:
            color(p().sheenColor, MaterialMap::SheenColor, s, "sheen color");
            sheenLit = lit();
            break;
        case Target::SheenRoughness: scalar(p().sheenRoughness, MaterialMap::SheenRoughness, s, "sheen roughness"); break;
        case Target::Coat: scalar(p().topCoat, MaterialMap::CoatWeight, s, "coat"); break;
        case Target::CoatColor: color(p().coatColor, MaterialMap::CoatColor, s, "coat color"); break;
        case Target::CoatRoughness: scalar(p().topCoatRoughness, MaterialMap::CoatRoughness, s, "coat roughness"); break;
        case Target::CoatIor: scalar(p().coatIor, MaterialMap::Count, s, "coat index of refraction"); break;
        case Target::CoatAnisotropy: scalar(p().coatAnisotropy, MaterialMap::CoatAnisotropy, s, "coat anisotropy"); break;
        case Target::CoatTurns: {
            float turns = 0.0f;
            scalar(turns, MaterialMap::CoatAnisotropyRotation, s, "coat rotation");
            p().coatAnisotropyRotation = constant ? turns * 360.0f : 0.0f;
            break;
        }
        case Target::FilmWeight:
            scalar(p().thinFilm, MaterialMap::ThinFilmWeight, s, "thin film weight");
            filmWeightGiven = true;
            break;
        case Target::FilmNanometres:
            if (constant) {
                p().thinFilmThickness = x > 0.0f ? x : p().thinFilmThickness;
                filmThick = x > 0.0f;
            } else {
                addTexture(MaterialMap::ThinFilmThickness, s);
                filmThick = true;
            }
            break;
        case Target::FilmMicrons:
            if (constant) {
                p().thinFilmThickness = x > 0.0f ? x * 1000.0f : p().thinFilmThickness;
                filmThick = x > 0.0f;
            } else {
                addTexture(MaterialMap::ThinFilmThickness, s);
                filmThick = true;
            }
            break;
        case Target::FilmIor: scalar(p().thinFilmIor, MaterialMap::Count, s, "thin film index of refraction"); break;
        case Target::EmissionWeight:
            if (constant) {
                emissionWeight = x;
                emissionWeightGiven = true;
            } else {
                tracer.note("The emission weight is given by an image, which the material has no slot for: it is taken as 1.");
                emissionWeight = 1.0f;
                emissionWeightGiven = true;
            }
            break;
        case Target::EmissionNits:
            if (constant) {
                emissionWeight = x / 1000.0f; // (1000 cd/m2 is the material's luminance factor 1)
                emissionWeightGiven = true;
            }
            break;
        case Target::EmissionColor:
            color(p().emissionColor, MaterialMap::Emission, s, "emission color");
            emissionLit = lit();
            break;
        case Target::Opacity:
            if (constant) {
                p().opacity = s.components >= 3 ? (s.value.x + s.value.y + s.value.z) / 3.0f : x;
            } else {
                addTexture(MaterialMap::Opacity, s);
            }
            break;
        case Target::Alpha: alpha = s; break;
        case Target::AlphaMode:
            if (constant) {
                alphaMode = static_cast<int>(x);
            }
            break;
        case Target::Normal:
            if (!constant) {
                if (s.bump) {
                    addTexture(MaterialMap::Bump, s);
                    p().bumpStrength = s.strength;
                } else {
                    addTexture(MaterialMap::Normal, s);
                    p().normalStrength = s.strength;
                }
            }
            break;
        case Target::Occlusion:
            if (!constant) {
                addTexture(MaterialMap::AmbientOcclusion, s);
            }
            break;
        }
    }
};

float clamp01(float v) {
    return std::min(std::max(v, 0.0f), 1.0f);
}

/// The material of the shading node @p shader (its inputs looked up in @p scope).
bool buildMaterial(const Xml& shader, Model model, const std::string& name, const Xml& root, MaterialXMaterial& out) {
    out = MaterialXMaterial();
    out.name = name;
    Tracer tracer(root, out.notes);
    Build  build{out, tracer};

    // Each model's own defaults (an input the file leaves out has its model's value, not the
    // material's): a surface that is opaque through and through unless the file says otherwise.
    MaterialParams& p = out.params;
    p.translucency = 0.0f;
    p.topCoatRoughness = 0.1f;
    const Port* ports = nullptr;
    std::size_t count = 0;
    switch (model) {
    case Model::StandardSurface:
        out.model = "standard_surface";
        ports = kStandardSurface;
        count = std::size(kStandardSurface);
        build.baseWeight = 0.8f;
        p.roughness = 0.2f;
        p.sheenRoughness = 0.3f;
        p.thinFilmIor = 1.5f;
        break;
    case Model::OpenPbr:
        out.model = "open_pbr_surface";
        ports = kOpenPbr;
        count = std::size(kOpenPbr);
        p.baseColor = glm::vec3(0.8f);
        p.roughness = 0.3f;
        p.topCoatRoughness = 0.0f;
        p.coatIor = 1.6f;
        p.sheenRoughness = 0.5f;
        break;
    case Model::GltfPbr:
        out.model = "gltf_pbr";
        ports = kGltfPbr;
        count = std::size(kGltfPbr);
        p.metalness = 1.0f;
        p.roughness = 1.0f;
        p.topCoatRoughness = 0.0f;
        break;
    case Model::UsdPreview:
        out.model = "UsdPreviewSurface";
        ports = kUsdPreview;
        count = std::size(kUsdPreview);
        p.baseColor = glm::vec3(0.18f);
        p.roughness = 0.5f;
        p.topCoatRoughness = 0.01f;
        break;
    case Model::None: return false;
    }

    const Xml& scope = shader.parent != nullptr ? *shader.parent : root;
    for (std::size_t i = 0; i < count; ++i) {
        if (const Xml* port = portOf(shader, ports[i].name)) {
            build.apply(ports[i].target, tracer.input(*port, scope));
        }
    }

    // --- what is settled once every input is known ---
    p.baseColor *= build.baseWeight;
    switch (model) {
    case Model::StandardSurface:
        p.emission = build.emissionWeight;                 // (the model's emission is off until its weight says otherwise)
        p.thinFilm = build.filmThick ? 1.0f : 0.0f;         // (… and its film is there when it has a thickness)
        break;
    case Model::OpenPbr:
        p.emission = build.emissionWeight;
        if (!build.filmWeightGiven) {
            p.thinFilm = 0.0f;
        }
        break;
    case Model::GltfPbr:
        p.emission = build.emissionLit ? (build.emissionWeightGiven ? build.emissionWeight : 1.0f) : 0.0f;
        p.sheen = build.sheenLit ? 1.0f : 0.0f;
        if (build.alphaMode != 0) { // (0 = opaque: the alpha is then not used)
            build.apply(Target::Opacity, build.alpha);
        }
        break;
    case Model::UsdPreview:
        p.emission = build.emissionLit ? 1.0f : 0.0f;
        break;
    case Model::None: break;
    }
    p.metalness = clamp01(p.metalness);
    p.roughness = clamp01(p.roughness);
    p.lobe1Roughness = p.roughness;
    p.lobe2Roughness = p.roughness;
    p.lobeRatio = 1.0f;
    p.opacity = clamp01(p.opacity);
    p.transmission = clamp01(p.transmission);
    p.translucency = clamp01(p.translucency);
    p.topCoat = clamp01(p.topCoat);
    p.sheen = clamp01(p.sheen);
    p.thinFilm = clamp01(p.thinFilm);
    p.specular = std::max(p.specular, 0.0f);
    p.emission = std::max(p.emission, 0.0f);
    p.ior = p.ior > 0.0f ? p.ior : 1.5f;
    return true;
}

/// A `displacement` shader's map and scale, onto the material.
void addDisplacement(const Xml& displacement, const Xml& root, MaterialXMaterial& material) {
    Tracer tracer(root, material.notes);
    Build  build{material, tracer};
    const Xml& scope = displacement.parent != nullptr ? *displacement.parent : root;
    const Xml* height = portOf(displacement, "displacement");
    const Signal s = height != nullptr ? tracer.input(*height, scope) : Signal();
    if (s.kind != Signal::Kind::Texture) {
        return;
    }
    build.addTexture(MaterialMap::Displacement, s);
    float scale = 1.0f;
    if (const Xml* port = portOf(displacement, "scale")) {
        const Signal amount = tracer.input(*port, scope);
        if (amount.kind == Signal::Kind::Constant) {
            scale = amount.value.x;
        }
    }
    // (Scene units — metres — from the map's black to its white, measured from black; the
    // material's is centimetres about a mid level.)
    material.params.displacementStrength = scale * 100.0f;
    material.params.displacementMidLevel = 0.0f;
}

/// Splices the documents named by xi:include elements into @p root (their top-level elements
/// become its own), a few levels deep.
void spliceIncludes(Xml& root, const MaterialXInclude& include, int depth) {
    if (!include || depth > 8) {
        return;
    }
    std::vector<std::unique_ptr<Xml>> merged;
    for (auto& child : root.children) {
        if (child->name != "xi:include" && child->name != "include") {
            merged.push_back(std::move(child));
            continue;
        }
        std::string text;
        std::string error;
        if (!include(child->get("href"), text)) {
            continue;
        }
        std::unique_ptr<Xml> document = parseXml(text, error);
        if (!document) {
            continue;
        }
        for (auto& top : document->children) {
            if (top->name != "materialx") {
                continue;
            }
            spliceIncludes(*top, include, depth + 1);
            for (auto& element : top->children) {
                element->parent = &root;
                merged.push_back(std::move(element));
            }
        }
    }
    root.children = std::move(merged);
}

} // namespace

bool readMaterialXText(const std::string& text, MaterialXDocument& out, std::string& error, const MaterialXInclude& include) {
    out = MaterialXDocument();
    error.clear();
    std::string          why;
    std::unique_ptr<Xml> document = parseXml(text, why);
    if (!document) {
        error = "The file is not a MaterialX document (it is not valid XML: " + why + ").";
        return false;
    }
    Xml* root = nullptr;
    for (auto& child : document->children) {
        if (child->name == "materialx") {
            root = child.get();
            break;
        }
    }
    if (root == nullptr) {
        error = "The file is not a MaterialX document (it has no <materialx> element).";
        return false;
    }
    out.version = root->get("version");
    spliceIncludes(*root, include, 0);

    std::vector<std::string> unknown; // shading models met that this reader does not read
    const auto               taken = [&out](const std::string& name) {
        return std::any_of(out.materials.begin(), out.materials.end(), [&](const MaterialXMaterial& m) { return m.name == name; });
    };
    const auto addFrom = [&](const Xml& shader, const std::string& category, const std::string& name, const Xml* displacement) {
        const Model model = modelOf(category);
        if (model == Model::None) {
            if (std::find(unknown.begin(), unknown.end(), category) == unknown.end()) {
                unknown.push_back(category);
            }
            return;
        }
        MaterialXMaterial material;
        if (taken(name) || !buildMaterial(shader, model, name, *root, material)) {
            return;
        }
        if (displacement != nullptr) {
            addDisplacement(*displacement, *root, material);
        }
        out.materials.push_back(std::move(material));
    };
    Tracer lookup(*root, unknown); // (for its node lookup alone)

    // 1. Materials as the current form names them: a surfacematerial node and its shader.
    for (const auto& child : root->children) {
        if (child->name != "surfacematerial") {
            continue;
        }
        const Xml* port = portOf(*child, "surfaceshader");
        const Xml* shader = port != nullptr && port->find("nodename") != nullptr ? lookup.findNode(*root, port->get("nodename")) : nullptr;
        if (shader == nullptr) {
            continue;
        }
        const Xml* displacementPort = portOf(*child, "displacementshader");
        const Xml* displacement = displacementPort != nullptr && displacementPort->find("nodename") != nullptr
                                      ? lookup.findNode(*root, displacementPort->get("nodename"))
                                      : nullptr;
        addFrom(*shader, shader->name, child->get("name", shader->get("name")), displacement);
    }
    // 2. The older form: a material element holding shader references with bound inputs.
    for (const auto& child : root->children) {
        if (child->name != "material") {
            continue;
        }
        for (const auto& reference : child->children) {
            if (reference->name == "shaderref") {
                addFrom(*reference, reference->get("node", reference->get("name")), child->get("name"), nullptr);
            }
        }
    }
    // 3. A file of shading nodes and no material: each node is one.
    if (out.materials.empty()) {
        for (const auto& child : root->children) {
            if (modelOf(child->name) != Model::None) {
                addFrom(*child, child->name, child->get("name", child->name), nullptr);
            }
        }
    }

    if (out.materials.empty()) {
        if (!unknown.empty()) {
            std::string names;
            for (const std::string& name : unknown) {
                names += (names.empty() ? "" : ", ") + name;
            }
            error = "The materials of this file use a shading model PoseStudio does not read (" + names +
                    "). It reads standard_surface, open_pbr_surface, gltf_pbr and UsdPreviewSurface.";
        } else {
            error = "The file holds no material (no surface shader of a kind PoseStudio reads).";
        }
        return false;
    }
    return true;
}

} // namespace pose
