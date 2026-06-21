#pragma once

namespace oss {

// Best-effort: raise the calling thread to a time-critical scheduling class so timing/audio work
// preempts ordinary threads. No-op / failure-tolerant where unsupported or not permitted.
// (macOS: QOS_CLASS_USER_INTERACTIVE.) Reused by the audio worker thread in a later phase.
void setThisThreadTimeCritical();

} // namespace oss
