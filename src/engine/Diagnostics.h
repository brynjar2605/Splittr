#pragma once

#include <juce_core/juce_core.h>

namespace splittr
{

class SeparationEngine;

// The DIAGNOSTICS report (issue #28): build identity, machine, compute state,
// models and cache, the last run, and the engine's event log — everything a
// bug report needs, as plain text the user copies and pastes. Assembled fresh
// on every open; nothing here leaves the machine on its own.
juce::String buildDiagnosticsReport (SeparationEngine&);

} // namespace splittr
