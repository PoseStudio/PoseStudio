// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file formulaops.h
 * @brief Reads a channel-driver formula's `operations` — the format's small stack language —
 *        once, for the two things that evaluate it.
 *
 * A modifier's formula says "this channel is that expression of other channels", written as a
 * list of stack operations:
 *
 *     { "op": "push", "url": "Figure:/data/…/Control.dsf#Control?value" }   a channel's value
 *     { "op": "push", "val": 0.5 }                                          a constant
 *     { "op": "push", "val": [x, y, 0, 0, 0] } … { "op": "push", "val": N } { "op": "spline_tcb" }
 *     { "op": "mult" }                                                      pop b, a; push a·b
 *     { "op": "inv" }                                                       pop a; push 1/a
 *
 * THE VOCABULARY is what the content uses, counted over every modifier file of two content
 * libraries (66,958 files): push, mult, spline_tcb, spline_linear, inv, spline_constant and neg
 * — and nothing else; add, sub and div are in the format's list and read too. An operation
 * outside it comes out as Unknown, and a formula that has one is NOT evaluated by either reader:
 * run without the operation it gives another number, not a nearby one ("divide by" read as
 * "multiply by").
 *
 * Two readers need it. The morph resolver's walk (morphresolver) evaluates what a preset's dials
 * come to, where every pushed channel is another dial; the corrective translator
 * (correctiveparser) turns the same language, driven by joint rotations, into the program the
 * engine runs at pose time. Each used to carry its own copy of the vocabulary — the knot rule
 * included — and the two drifted apart in what they understood. This file is the vocabulary;
 * what a pushed channel MEANS (a dial of the walk, a joint's angle, a gate folded to a constant)
 * stays with each caller, as does what to do with an operation it cannot express.
 *
 * Pure std + nlohmann — no Qt, no Vulkan.
 */

#ifndef FORMULAOPS_H
#define FORMULAOPS_H

#include "figureutils.h" // ChannelRef

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace pose {

/// One knot of a formula's spline: at input x the output is y. (The format writes TCB knots as
/// [x, y, tension, continuity, bias] with the last three ~0, and linear ones as [x, y].)
struct FormulaKnot {
    double x = 0.0;
    double y = 0.0;
};

/// One operation of a formula, as read.
struct FormulaOp {
    enum class Kind {
        PushUrl,   ///< Push the value of the channel `url` names.
        PushConst, ///< Push `value`.
        Spline,    ///< Pop x; push the spline through `knots` at x.
        Mult,      ///< Pop b, a; push a·b.
        Add,       ///< Pop b, a; push a+b.
        Sub,       ///< Pop b, a; push a−b.
        Div,       ///< Pop b, a; push a/b (0 where b is 0).
        Inv,       ///< Pop a; push 1/a (0 where a is 0) — see invert().
        Neg,       ///< Pop a; push −a.
        Unknown    ///< An operation outside this vocabulary; `name` says which.
    };
    /// How a spline runs between its knots (it is flat outside them).
    enum class Interpolation {
        Smooth, ///< spline_tcb: a Catmull-Rom Hermite, clamped to its segment's end values.
        Linear, ///< spline_linear: straight segments.
        Step    ///< spline_constant: each knot's value holds until the next knot.
    };
    Kind                     kind = Kind::Unknown;
    std::string              url;            ///< PushUrl: the reference as written …
    ChannelRef               ref;            ///< … and split (parseChannelRef).
    double                   value = 0.0;    ///< PushConst.
    std::vector<FormulaKnot> knots;          ///< Spline: ascending in x.
    Interpolation            interpolation = Interpolation::Smooth; ///< Spline.
    std::string              name;           ///< The operation's name as the file wrote it.
};

/// A formula's operations, in order.
struct FormulaProgram {
    std::vector<FormulaOp> ops;
    /// A spline knot was not written as numbers ([x, y, …]); it was read as 0 where it was not.
    bool malformedKnot = false;
    /// The first operation outside the vocabulary, by the name the file wrote ("" = none; an
    /// entry that names no operation at all reads "(unnamed)"). A formula with one must not be
    /// evaluated: see the file comment.
    std::string unknownOp;
};

/// The `inv` operation's result, for both readers: 1/a, and 0 where a is 0 (as a division by 0
/// gives 0 here). The content writes "divide by a channel" as a mult-stage formula
/// `push <channel>?value, inv` — a corrective's weight divided by a fit dial — and every one of
/// those channels found (eight, on two wearables' correctives) is limited to [1, 2] with a default
/// of 1: the content cannot produce a divisor of 0, so no file says what its host does with one.
/// 0 is chosen because it is finite and it switches the thing divided OFF — a corrective gated by
/// it is not live — where an infinity would reach the vertices.
template <typename Real>
Real invert(Real a) {
    return a != Real(0) ? Real(1) / a : Real(0);
}

/// Reads a formula's `operations` array (anything else: an empty program). Never throws — an
/// operation that is no object, or names nothing, comes out as Unknown.
///
/// THE KNOT RULE lives here, once: a spline's knots are pushed as arrays, then their COUNT as a
/// bare number, then the spline operation that consumes them — so an array push is a knot (never
/// an operand), and a bare number directly before a "spline…" operation is that count and is
/// dropped. Read as an operand, the count was the formula's result: 2 or 3 whatever the dial.
/// Knots are returned ascending in x (a stable sort; real content writes them in order).
FormulaProgram readFormulaOps(const nlohmann::json& operations);

} // namespace pose

#endif // FORMULAOPS_H
