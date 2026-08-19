#pragma once

namespace splittr
{

// Per the design handoff's state management section.
enum class Phase
{
    NeedsModel,
    Downloading,
    Idle,
    Decoding,
    Separating,
    Rendering,
    Ready,
    Error
};

} // namespace splittr
