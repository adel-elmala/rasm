#include "rasm/threadPriority.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#elif defined(__linux__)
#include <algorithm>
#include <cerrno>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace rasm
{
    // Windows: a thread woken at the main thread's priority gets a wake boost above it and takes the core of the thread
    // that woke it, which then waits in notify_all for the whole job (P34). One level below normal it takes a free core.
    // macOS: the same QoS class as the main thread's default, so the same cores, at a lower priority within it. Linux:
    // nice is per thread, and 5 more gives the main thread about three times the share of a contended core.
    void lowerThreadPriority()
    {
#if defined(_WIN32)
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#elif defined(__APPLE__)
        pthread_set_qos_class_self_np(QOS_CLASS_DEFAULT, -5);
#elif defined(__linux__)
        const auto thread = static_cast<id_t>(syscall(SYS_gettid));
        errno = 0;
        const int nice = getpriority(PRIO_PROCESS, thread);
        if (errno == 0)
            setpriority(PRIO_PROCESS, thread, std::min(19, nice + 5));
#endif
    }
}
