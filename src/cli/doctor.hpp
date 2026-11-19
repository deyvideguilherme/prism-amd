#pragma once

namespace prisma {

struct DoctorResult {
    bool healthy = true;
    bool kernel_ok = true;
    bool module_loaded = false;
    bool dev_node_exists = false;
    bool dev_node_accessible = false;
    bool abi_compatible = false;
    bool gpu_detected = false;
};

// Runs the doctor diagnostics and prints structured, human-readable output without emojis.
// Returns 0 if all checks pass, 1 if any critical check fails.
int run_doctor();

} // namespace prisma
