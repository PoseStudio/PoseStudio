// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file report.h
 * @brief The harness's reporting: each phase records named metrics (Gate), the ones with a limit
 *        are GATES, and the Report tallies pass/fail/skip and prints one block per phase.
 *
 * Gates are regression guards set from measured behaviour, not specs — a phase gates the metrics
 * that once failed, and prints the rest as `info` rows under --verbose. `--only <substring>`
 * filters phases by name through Report::wants.
 */

#ifndef IKHARNESS_REPORT_H
#define IKHARNESS_REPORT_H

#include <string>
#include <vector>

namespace ikharness {

struct Gate {
    std::string name;
    double      value = 0.0;
    double      limit = 0.0;
    bool        lessIsBetter = true;
    bool        gated = true;
};

struct Report {
    int  phases = 0;
    int  failedPhases = 0;
    int  skippedPhases = 0;
    bool verbose = false;
    std::string only;

    bool wants(const std::string& phase) const {
        return only.empty() || phase.find(only) != std::string::npos;
    }

    /// Prints the phase block and tallies it: a phase FAILS when any gated metric is past its
    /// limit. Info rows print only in verbose mode.
    void phase(const std::string& name, const std::vector<Gate>& gates);

    void skip(const std::string& name, const char* why);
};

/// A gated metric that must stay at or below @p limit.
Gate gateMax(const char* name, double value, double limit);
/// A gated metric that must stay at or above @p limit.
Gate gateMin(const char* name, double value, double limit);
/// An ungated metric, printed under --verbose.
Gate info(const char* name, double value);

} // namespace ikharness

#endif // IKHARNESS_REPORT_H
