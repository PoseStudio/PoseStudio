/**
 * @file formulaops.cpp
 * @brief Implementation of readFormulaOps. See formulaops.h.
 */

#include "formulaops.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace pose {

namespace {

// Reads knot element @p index of a pushed array into @p out. An element that is absent leaves
// @p out at 0 (a short knot); one that is present but no number does too, and is reported.
void readKnotValue(const nlohmann::json& array, std::size_t index, double& out, bool& malformed) {
    if (index >= array.size()) {
        return;
    }
    if (array[index].is_number()) {
        out = array[index].get<double>();
    } else {
        malformed = true;
    }
}

} // namespace

FormulaProgram readFormulaOps(const nlohmann::json& operations) {
    FormulaProgram program;
    if (!operations.is_array()) {
        return program;
    }
    std::vector<FormulaKnot> pendingKnots; // knots accumulate until the spline op consumes them
    for (std::size_t i = 0; i < operations.size(); ++i) {
        const nlohmann::json& op = operations[i];
        FormulaOp             out;
        out.name = stringOf(op, "op");
        if (out.name == "push") {
            const auto val = op.find("val");
            const auto url = op.find("url");
            if (val != op.end() && val->is_array()) {
                FormulaKnot knot;
                readKnotValue(*val, 0, knot.x, program.malformedKnot);
                readKnotValue(*val, 1, knot.y, program.malformedKnot);
                pendingKnots.push_back(knot);
                continue;
            }
            if (val != op.end() && val->is_number()) {
                // A bare number directly before a spline op is the knot count, not an operand.
                if (i + 1 < operations.size() &&
                    stringOf(operations[i + 1], "op").rfind("spline", 0) == 0) {
                    continue;
                }
                out.kind = FormulaOp::Kind::PushConst;
                out.value = val->get<double>();
            } else if (url != op.end() && url->is_string()) {
                out.kind = FormulaOp::Kind::PushUrl;
                out.url = url->get<std::string>();
                out.ref = parseChannelRef(out.url);
            } else {
                out.kind = FormulaOp::Kind::PushConst; // a push of nothing readable: 0
                out.value = 0.0;
            }
        } else if (out.name.rfind("spline", 0) == 0) {
            out.kind = FormulaOp::Kind::Spline;
            out.interpolation = out.name == "spline_linear"     ? FormulaOp::Interpolation::Linear
                                : out.name == "spline_constant" ? FormulaOp::Interpolation::Step
                                                                : FormulaOp::Interpolation::Smooth;
            out.knots = std::move(pendingKnots);
            pendingKnots.clear();
            std::stable_sort(out.knots.begin(), out.knots.end(),
                             [](const FormulaKnot& a, const FormulaKnot& b) { return a.x < b.x; });
        } else if (out.name == "mult") {
            out.kind = FormulaOp::Kind::Mult;
        } else if (out.name == "add") {
            out.kind = FormulaOp::Kind::Add;
        } else if (out.name == "sub") {
            out.kind = FormulaOp::Kind::Sub;
        } else if (out.name == "div") {
            out.kind = FormulaOp::Kind::Div;
        } else if (out.name == "inv") {
            out.kind = FormulaOp::Kind::Inv;
        } else if (out.name == "neg") {
            out.kind = FormulaOp::Kind::Neg;
        } else if (program.unknownOp.empty()) {
            program.unknownOp = out.name.empty() ? "(unnamed)" : out.name;
        }
        program.ops.push_back(std::move(out));
    }
    return program;
}

} // namespace pose
