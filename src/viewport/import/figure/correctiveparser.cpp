/**
 * @file correctiveparser.cpp
 * @brief Implementation of parseCorrective. See correctiveparser.h.
 */

#include "correctiveparser.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <utility>

namespace pose {

namespace {

// Splits a channel-driver URL into (nodeId, property). Forms:
//   "lForeArm:/data/…/basefigure.dsf#lForeArm?rotation/y"     -> ("lForeArm", "rotation/y")
//   "figure:/data/…/charShape.dsf#charShape?value"            -> ("charShape", "value")
// Takes the fragment after '#', then splits on '?'.
void splitChannel(const std::string& url, std::string& node, std::string& prop) {
    const auto hash = url.rfind('#');
    const std::string frag = (hash == std::string::npos) ? url : url.substr(hash + 1);
    const auto q = frag.find('?');
    if (q == std::string::npos) {
        node = frag;
        prop.clear();
        return;
    }
    node = frag.substr(0, q);
    prop = frag.substr(q + 1);
}

// "rotation/x|y|z" -> 0/1/2, else -1.
int rotationAxis(const std::string& prop) {
    if (prop.size() != 10 || prop.rfind("rotation/", 0) != 0) {
        return -1;
    }
    switch (prop.back()) {
        case 'x': return 0;
        case 'y': return 1;
        case 'z': return 2;
        default:  return -1;
    }
}

// Translates one formula's `operations` into a stack program of CorrectiveOps. Rotation drivers of
// known joints become PushRotation (dynamic); every other channel driver is resolved to a constant
// via ctx.resolveValue (baked as PushConst). Sets @p hasRotation if a rotation driver was seen and
// @p unknownDriver if a rotation driver named a joint absent from the skeleton.
void translateFormula(const nlohmann::json& formula, const CorrectiveContext& ctx,
                      CorrectiveFormula& out, bool& hasRotation, bool& unknownDriver) {
    const auto opsIt = formula.find("operations");
    if (opsIt == formula.end() || !opsIt->is_array()) {
        return;
    }
    const nlohmann::json& ops = *opsIt;
    std::vector<CorrectiveKnot> pendingKnots; // spline knots accumulate until spline_* consumes them

    for (std::size_t i = 0; i < ops.size(); ++i) {
        const nlohmann::json& op = ops[i];
        const std::string name = op.value("op", std::string());

        if (name == "push") {
            if (const auto url = op.find("url"); url != op.end() && url->is_string()) {
                std::string node;
                std::string prop;
                splitChannel(url->get<std::string>(), node, prop);
                const int axis = rotationAxis(prop);
                if (axis >= 0) {
                    if (ctx.boneNames && ctx.boneNames->find(node) == ctx.boneNames->end()) {
                        unknownDriver = true;
                    }
                    CorrectiveOp o;
                    o.kind = CorrectiveOp::Kind::PushRotation;
                    o.bone = node;
                    o.axis = axis;
                    out.ops.push_back(std::move(o));
                    hasRotation = true;
                } else {
                    // A value (or other) channel driver — a character/enable gate. Resolve it to a
                    // constant now so the runtime formula deals only in joint angles.
                    CorrectiveOp o;
                    o.kind = CorrectiveOp::Kind::PushConst;
                    o.value = ctx.resolveValue ? ctx.resolveValue(url->get<std::string>()) : 0.0f;
                    out.ops.push_back(std::move(o));
                }
            } else if (const auto val = op.find("val"); val != op.end()) {
                if (val->is_array()) {
                    // A spline knot [x, y, tension, continuity, bias]; keep (x, y).
                    CorrectiveKnot k;
                    if (val->size() > 0) k.x = (*val)[0].get<float>();
                    if (val->size() > 1) k.y = (*val)[1].get<float>();
                    pendingKnots.push_back(k);
                } else if (val->is_number()) {
                    // A bare number is either the spline knot-count (immediately before a spline op,
                    // discard) or a real multiplicand.
                    const bool isCount = (i + 1 < ops.size()) &&
                                         ops[i + 1].value("op", std::string()).rfind("spline", 0) == 0;
                    if (!isCount) {
                        CorrectiveOp o;
                        o.kind = CorrectiveOp::Kind::PushConst;
                        o.value = val->get<float>();
                        out.ops.push_back(std::move(o));
                    }
                }
            }
        } else if (name.rfind("spline", 0) == 0) { // spline_tcb / spline_linear
            CorrectiveOp o;
            o.kind = CorrectiveOp::Kind::Spline;
            o.knots = std::move(pendingKnots);
            pendingKnots.clear();
            out.ops.push_back(std::move(o));
        } else if (name == "mult") {
            CorrectiveOp o;
            o.kind = CorrectiveOp::Kind::Mult;
            out.ops.push_back(std::move(o));
        } else if (name == "add") {
            CorrectiveOp o;
            o.kind = CorrectiveOp::Kind::Add;
            out.ops.push_back(std::move(o));
        }
        // Other ops (sub/div/spline flavours we don't model) are absent from the base correctives.
    }
}

// Evaluates a mult-stage gate formula (constants after translation) to a scalar. Rotation drivers, if
// any slipped into a gate, are treated as rest (angle 0); splines clamp to their last knot's value.
float evalGate(const CorrectiveFormula& f) {
    std::vector<float> st;
    for (const CorrectiveOp& o : f.ops) {
        switch (o.kind) {
            case CorrectiveOp::Kind::PushRotation:
                st.push_back(0.0f);
                break;
            case CorrectiveOp::Kind::PushConst:
                st.push_back(o.value);
                break;
            case CorrectiveOp::Kind::Spline: {
                const float d = st.empty() ? 0.0f : (st.back());
                if (!st.empty()) st.pop_back();
                float y = o.knots.empty() ? 0.0f : o.knots.back().y;
                for (const CorrectiveKnot& k : o.knots) {
                    if (d <= k.x) { y = k.y; break; }
                }
                st.push_back(y);
                break;
            }
            case CorrectiveOp::Kind::Mult: {
                if (st.size() >= 2) { const float b = st.back(); st.pop_back(); st.back() *= b; }
                break;
            }
            case CorrectiveOp::Kind::Add: {
                if (st.size() >= 2) { const float b = st.back(); st.pop_back(); st.back() += b; }
                break;
            }
        }
    }
    return st.empty() ? 1.0f : st.back();
}

} // namespace

bool parseCorrective(const nlohmann::json& modifier, const CorrectiveContext& ctx, PoseCorrective& out) {
    // Must carry actual geometry (sparse deltas) and driver formulas.
    const auto morph = modifier.find("morph");
    if (morph == modifier.end()) {
        return false;
    }
    const auto deltas = morph->find("deltas");
    if (deltas == morph->end()) {
        return false;
    }
    const auto formulas = modifier.find("formulas");
    if (formulas == modifier.end() || !formulas->is_array() || formulas->empty()) {
        return false;
    }

    out = PoseCorrective{};
    out.id = modifier.value("id", modifier.value("name", std::string()));
    out.gateScale = 1.0f;

    bool anyRotation = false;
    for (const auto& formula : *formulas) {
        const std::string stage = formula.value("stage", std::string("sum"));
        CorrectiveFormula cf;
        bool hasRotation = false;
        bool unknownDriver = false;
        translateFormula(formula, ctx, cf, hasRotation, unknownDriver);
        if (unknownDriver) {
            return false; // driver joint not in this skeleton — can't evaluate correctly
        }
        if (stage == "mult") {
            out.gateScale *= evalGate(cf);
        } else { // "sum" (default)
            anyRotation = anyRotation || hasRotation;
            out.sumFormulas.push_back(std::move(cf));
        }
    }

    // Not pose-driven, or a character gate resolved it to zero -> not a live corrective for this figure.
    if (!anyRotation || std::fabs(out.gateScale) < 1e-4f) {
        return false;
    }

    // Sparse deltas: values = [[baseVertexIndex, dx, dy, dz], …].
    const nlohmann::json* values = &(*deltas);
    if (deltas->is_object()) {
        if (const auto v = deltas->find("values"); v != deltas->end()) {
            values = &(*v);
        }
    }
    if (!values->is_array()) {
        return false;
    }
    out.deltas.reserve(values->size());
    for (const auto& d : *values) {
        if (d.is_array() && d.size() >= 4) {
            out.deltas.emplace_back(d[0].get<uint32_t>(),
                                    glm::vec3(d[1].get<float>(), d[2].get<float>(), d[3].get<float>()));
        }
    }
    return !out.deltas.empty();
}

} // namespace pose
