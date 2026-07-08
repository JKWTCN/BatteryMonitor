#pragma once

#include <mutex>

// hidapi is shared by all HID providers. Keep calls serialized across provider
// worker threads to avoid races in the Windows backend and its error buffers.
inline std::recursive_mutex &hidApiMutex()
{
    static std::recursive_mutex mutex;
    return mutex;
}
