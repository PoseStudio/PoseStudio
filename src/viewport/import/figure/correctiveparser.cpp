/**
 * @file correctiveparser.cpp
 * @brief Implementation of parseCorrective. See correctiveparser.h.
 */

#include "correctiveparser.h"

#include "figureutils.h" // parseChannelRef, valuesArray
#include "formulaops.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pose {

namespace {

// Channel-driver URLs here take the forms
//   "lForeArm:/data/…/basefigure.dsf#lForeArm?rotation/y"     -> (node "lForeArm", "rotation/y")
//   "figure:/data/…/charShape.dsf#charShape?value"            -> (node "charShape", "value")
// and are split by figureutils' parseChannelRef: the node is the id after '#', compared DECODED
// with the skeleton's joints and the modifier's own id alike (a url writes an id with a space in
// it percent-encoded, the file that defines it plain), and the property is the part after '?'.

// "rotation/x|y|z" -> 0/1/2, else -1.
int rotationAxis(const std::string& prop) {
    if (prop.size() != 10 || prop.rfind("rotation/", 0) != 0) {
        return -1;
    }
    return axisIndex(prop.back());
}

// What reading one formula of a corrective's own weight found.
struct FormulaFindings {
    bool        hasRotation = false;   ///< A joint's rotation drives it …
    bool        unknownDriver = false; ///< … and one of those joints is not in this skeleton.
    std::string problem;               ///< Why the formula cannot be carried ("" = it can).
};

// The joint a rotation driver names, noting one the skeleton does not have.
const std::string& driverJoint(const FormulaOp& op, const CorrectiveContext& ctx, FormulaFindings& found) {
    found.hasRotation = true;
    if (ctx.boneNames && ctx.boneNames->find(op.ref.decodedKey) == ctx.boneNames->end()) {
        found.unknownDriver = true;
    }
    return op.ref.decodedKey;
}

// What a pushed channel that is no joint rotation reads: a character / enable gate, resolved to a
// constant now so that the runtime program deals only in joint angles.
float gateValue(const FormulaOp& op, const CorrectiveContext& ctx) {
    return ctx.resolveValue ? ctx.resolveValue(op.url) : 0.0f;
}

// Translates a DRIVER formula (the "sum" stage) into the runtime's stack program: a rotation of a
// joint becomes PushRotation (the one thing that moves at pose time), every other pushed channel a
// constant. The runtime has no inv / neg / sub / div: applied to constants they are folded here —
// the operand is then the last thing the program pushes — and applied to anything a joint's angle
// went into they make the formula one this program cannot say (@p found.problem), as a step
// spline does. An operation outside the vocabulary is left to the caller (FormulaProgram::
// unknownOp): the program built here is then not one to run.
void translateDriver(const FormulaProgram& program, const CorrectiveContext& ctx, CorrectiveFormula& out,
                     FormulaFindings& found) {
    // The constant @p back places under the top of the stack, where a plain push put it there.
    const auto constantAt = [&out](std::size_t back) -> CorrectiveOp* {
        if (out.ops.size() <= back) {
            return nullptr;
        }
        CorrectiveOp& o = out.ops[out.ops.size() - 1 - back];
        return o.kind == CorrectiveOp::Kind::PushConst ? &o : nullptr;
    };
    const auto cannotSay = [&found](const std::string& what) {
        if (found.problem.empty()) {
            found.problem = "its driver " + what + ", which the pose-time program has no operation for";
        }
    };
    for (const FormulaOp& op : program.ops) {
        CorrectiveOp o;
        switch (op.kind) {
            case FormulaOp::Kind::PushUrl: {
                const int axis = rotationAxis(op.ref.property);
                if (axis >= 0) {
                    o.kind = CorrectiveOp::Kind::PushRotation;
                    o.bone = driverJoint(op, ctx, found);
                    o.axis = axis;
                } else {
                    o.kind = CorrectiveOp::Kind::PushConst;
                    o.value = gateValue(op, ctx);
                }
                break;
            }
            case FormulaOp::Kind::PushConst:
                o.kind = CorrectiveOp::Kind::PushConst;
                o.value = static_cast<float>(op.value);
                break;
            case FormulaOp::Kind::Spline:
                if (op.interpolation == FormulaOp::Interpolation::Step) {
                    cannotSay("is a step spline (" + op.name + ")");
                    continue;
                }
                o.kind = CorrectiveOp::Kind::Spline;
                o.linear = op.interpolation == FormulaOp::Interpolation::Linear;
                o.knots.reserve(op.knots.size());
                for (const FormulaKnot& k : op.knots) {
                    o.knots.push_back({static_cast<float>(k.x), static_cast<float>(k.y)});
                }
                break;
            case FormulaOp::Kind::Mult:
                o.kind = CorrectiveOp::Kind::Mult;
                break;
            case FormulaOp::Kind::Add:
                o.kind = CorrectiveOp::Kind::Add;
                break;
            case FormulaOp::Kind::Inv:
            case FormulaOp::Kind::Neg:
                if (CorrectiveOp* a = constantAt(0)) {
                    a->value = op.kind == FormulaOp::Kind::Inv ? invert(a->value) : -a->value;
                } else {
                    cannotSay("applies '" + op.name + "' to a joint's angle");
                }
                continue;
            case FormulaOp::Kind::Sub:
            case FormulaOp::Kind::Div: {
                CorrectiveOp* b = constantAt(0);
                CorrectiveOp* a = constantAt(1);
                if (a && b) {
                    a->value = op.kind == FormulaOp::Kind::Sub ? a->value - b->value
                                                                : (b->value != 0.0f ? a->value / b->value : 0.0f);
                    out.ops.pop_back();
                } else {
                    cannotSay("applies '" + op.name + "' to a joint's angle");
                }
                continue;
            }
            case FormulaOp::Kind::Unknown:
                continue;
        }
        out.ops.push_back(std::move(o));
    }
}

// Evaluates a GATE formula (the "mult" stage) to the constant it folds to — in the whole
// vocabulary, since nothing of a gate reaches the runtime: every channel it reads is resolved now,
// and a joint's rotation, should one be in a gate, reads as rest (angle 0). An empty program is
// no gate (1).
// NB: the Spline case is a deliberate STEP lookup (the first knot with x >= d wins, whatever the
// spline's interpolation) — not the runtime evaluator's curve, and not to be "upgraded" to it
// in passing: a gate is folded from a rest angle (0) or a switch at one of its keyed values,
// where a knot sits exactly. (A dial left BETWEEN two knots reads the later knot's value.)
float evalGate(const FormulaProgram& program, const CorrectiveContext& ctx, FormulaFindings& found) {
    std::vector<float> st;
    for (const FormulaOp& op : program.ops) {
        switch (op.kind) {
            case FormulaOp::Kind::PushUrl:
                if (rotationAxis(op.ref.property) >= 0) {
                    driverJoint(op, ctx, found);
                    st.push_back(0.0f);
                } else {
                    st.push_back(gateValue(op, ctx));
                }
                break;
            case FormulaOp::Kind::PushConst:
                st.push_back(static_cast<float>(op.value));
                break;
            case FormulaOp::Kind::Spline: {
                const float d = st.empty() ? 0.0f : st.back();
                if (!st.empty()) {
                    st.pop_back();
                }
                float y = op.knots.empty() ? 0.0f : static_cast<float>(op.knots.back().y);
                for (const FormulaKnot& k : op.knots) {
                    if (d <= static_cast<float>(k.x)) {
                        y = static_cast<float>(k.y);
                        break;
                    }
                }
                st.push_back(y);
                break;
            }
            case FormulaOp::Kind::Mult:
            case FormulaOp::Kind::Add:
            case FormulaOp::Kind::Sub:
            case FormulaOp::Kind::Div:
                if (st.size() >= 2) {
                    const float b = st.back();
                    st.pop_back();
                    float& a = st.back();
                    switch (op.kind) {
                        case FormulaOp::Kind::Mult: a *= b; break;
                        case FormulaOp::Kind::Add:  a += b; break;
                        case FormulaOp::Kind::Sub:  a -= b; break;
                        default:                    a = b != 0.0f ? a / b : 0.0f; break;
                    }
                }
                break;
            case FormulaOp::Kind::Inv:
                if (!st.empty()) {
                    st.back() = invert(st.back());
                }
                break;
            case FormulaOp::Kind::Neg:
                if (!st.empty()) {
                    st.back() = -st.back();
                }
                break;
            case FormulaOp::Kind::Unknown:
                break; // the caller reads FormulaProgram::unknownOp and does not use the result
        }
    }
    return st.empty() ? 1.0f : st.back();
}

} // namespace

bool parseCorrective(const nlohmann::json& modifier, const CorrectiveContext& ctx, PoseCorrective& out,
                     std::string* refused) {
    if (refused) {
        refused->clear();
    }
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
    out.id = stringOf(modifier, "id", stringOf(modifier, "name"));
    out.gateScale = 1.0f;
    if (const auto ch = modifier.find("channel"); ch != modifier.end() && ch->is_object()) {
        out.clamped = boolOf(*ch, "clamped", false);
        out.clampMin = floatOf(*ch, "min", 0.0f);
        out.clampMax = floatOf(*ch, "max", 1.0f);
    }

    bool        anyRotation = false;
    std::string problem; // the first reason a formula of its weight cannot be carried
    for (const auto& formula : *formulas) {
        // Only formulas whose OUTPUT is this modifier's own `?value` channel belong to its
        // weight. Base corrective packs carry exactly that one driver formula, but vendor
        // character corrective files bundle DOZENS of others in the same modifier — joint-center /
        // orientation / end-point adjustments driven BY the morph, and cross-drivers of sibling
        // morphs. Summing those into the weight produced garbage (and their gate stages zeroed
        // it); they are simply skipped here (per-morph bone adjustments are transient, tiny, and
        // not modeled — the identity-morph path handles the character's static ones).
        // The output id is compared DECODED: a vendor morph id with spaces is written
        // percent-encoded in the url ("Some%20Character%20Body%201") but plain in the modifier's
        // own "id" ("Some Character Body 1").
        const ChannelRef outRef = parseChannelRef(stringOf(formula, "output"));
        if (outRef.property != "value" || outRef.decodedKey != out.id) {
            continue;
        }
        FormulaProgram program;
        if (const auto ops = formula.find("operations"); ops != formula.end() && ops->is_array()) {
            program = readFormulaOps(*ops);
        }
        if (program.malformedKnot) {
            return false; // an unreadable formula of its own weight: no corrective
        }
        FormulaFindings found;
        if (stringOf(formula, "stage", "sum") == "mult") {
            const float gate = evalGate(program, ctx, found);
            if (program.unknownOp.empty()) {
                out.gateScale *= gate;
            }
        } else { // "sum" (default)
            CorrectiveFormula cf;
            translateDriver(program, ctx, cf, found);
            anyRotation = anyRotation || found.hasRotation;
            out.sumFormulas.push_back(std::move(cf));
        }
        if (found.unknownDriver) {
            return false; // driver joint not in this skeleton — can't evaluate correctly
        }
        if (problem.empty()) {
            problem = !program.unknownOp.empty()
                          ? "a formula of its weight uses an operation this importer does not know (" +
                                program.unknownOp + ")"
                          : found.problem;
        }
    }

    // Not pose-driven, or a character gate resolved it to zero -> not a live corrective for this figure.
    if (!anyRotation || std::fabs(out.gateScale) < 1e-4f) {
        return false;
    }

    // A live corrective one of whose formulas could not be read or said. Carried with the
    // operation left out, its weight would be another number — "divide by" read as "multiply by".
    if (!problem.empty()) {
        if (refused) {
            *refused = problem;
        }
        return false;
    }

    // A morph says how many vertices the geometry it was made for has, and the shape bake refuses
    // one made for another geometry; so does this. A joint-driven morph found in a dialed morph's
    // folder may be another figure's with the same joint names: its deltas, indexed by THAT cage's
    // vertices, would move whichever of ours happen to share the numbers.
    if (ctx.cageVertexCount != 0) {
        if (const auto vc = morph->find("vertex_count"); vc != morph->end() && vc->is_number_integer()) {
            const long long authoredFor = vc->get<long long>();
            if (authoredFor > 0 && static_cast<std::size_t>(authoredFor) != ctx.cageVertexCount) {
                if (refused) {
                    *refused = "made for another geometry (" + std::to_string(authoredFor) +
                               " vertices, this one has " + std::to_string(ctx.cageVertexCount) + ")";
                }
                return false;
            }
        }
    }

    // Sparse deltas: values = [[baseVertexIndex, dx, dy, dz], …].
    const nlohmann::json& values = valuesArray(*deltas);
    if (!values.is_array()) {
        return false;
    }
    out.deltas.reserve(values.size());
    for (const auto& d : values) {
        if (d.is_array() && d.size() >= 4) {
            out.deltas.emplace_back(d[0].get<uint32_t>(),
                                    glm::vec3(d[1].get<float>(), d[2].get<float>(), d[3].get<float>()));
        }
    }
    return !out.deltas.empty();
}

} // namespace pose
