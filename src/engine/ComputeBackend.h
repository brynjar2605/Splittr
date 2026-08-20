#pragma once

namespace splittr
{

// Where demucs inference runs. `gpu` is Metal (MPSGraph) on the system GPU;
// when the machine or model can't take that path, separation silently falls
// back to `cpu` — callers ask DemucsSeparator::gpuAvailable() to grey the
// setting, not to gate the call.
enum class ComputeBackend
{
    cpu,
    gpu
};

} // namespace splittr
