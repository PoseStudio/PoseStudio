// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Ion Hatzithomas and the PoseStudio contributors.
// Licensed under the GNU AGPL v3 with the additional terms in LICENSE-ADDITIONAL-TERMS.txt.
/**
 * @file report.cpp
 * @brief Report printing and tallying (see report.h).
 */

#include "report.h"

#include <cstdio>

namespace ikharness {

void Report::phase(const std::string& name, const std::vector<Gate>& gates) {
    ++phases;
    bool ok = true;
    for (const Gate& g : gates) {
        if (g.gated && ((g.lessIsBetter && g.value > g.limit) ||
                        (!g.lessIsBetter && g.value < g.limit))) {
            ok = false;
        }
    }
    if (!ok) {
        ++failedPhases;
    }
    std::printf("%s %s\n", ok ? "[PASS]" : "[FAIL]", name.c_str());
    for (const Gate& g : gates) {
        const bool gateOk = !g.gated || (g.lessIsBetter ? g.value <= g.limit : g.value >= g.limit);
        if (g.gated) {
            std::printf("       %-34s %10.4f  %s %.4f%s\n", g.name.c_str(), g.value,
                        g.lessIsBetter ? "<=" : ">=", g.limit, gateOk ? "" : "   <-- FAIL");
        } else if (verbose) {
            std::printf("       %-34s %10.4f\n", g.name.c_str(), g.value);
        }
    }
}

void Report::skip(const std::string& name, const char* why) {
    ++phases;
    ++skippedPhases;
    std::printf("[SKIP] %s (%s)\n", name.c_str(), why);
}

Gate gateMax(const char* name, double value, double limit) { return {name, value, limit, true, true}; }
Gate gateMin(const char* name, double value, double limit) { return {name, value, limit, false, true}; }
Gate info(const char* name, double value) { return {name, value, 0.0, true, false}; }

} // namespace ikharness
