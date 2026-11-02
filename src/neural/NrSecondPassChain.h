#pragma once

namespace DlssNr {
// The model output is already encoded at the working dimensions. Only clamp
// it between independent temporal features: do not encode or resize it again.
// These are the same callbacks used by the native host, including restoration
// on an optional-stage refusal. No callbacks run in the original one-pass path.
template<class Operations>
bool RunOptionalSecondPass(bool enabled, Operations& operations) {
    if (!enabled) return true;
    operations.first_readable();
    struct Restore {
        Operations& operations;
        bool clamp_readable{};
        ~Restore() {
            if (clamp_readable) operations.restore_clamp();
            operations.restore_first();
        }
    } restore{operations};
    if (!operations.clamp()) return false;
    operations.clamp_readable();
    restore.clamp_readable = true;
    return operations.evaluate();
}
}
