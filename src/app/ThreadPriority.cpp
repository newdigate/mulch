#include "app/ThreadPriority.h"

#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

namespace oss {

void setThisThreadTimeCritical() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    // Other platforms: no-op for now (SCHED_FIFO/RR needs privileges; a later phase can add them).
}

} // namespace oss
