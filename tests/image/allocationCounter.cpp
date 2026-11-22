#include "allocationCounter.h"

#include <cstdlib>
#include <new>

// Counts the heap allocations, through operator new, of the thread that called startCountingAllocations (P27): the
// load workers and the driver's threads do not count. Windows only: there each DLL has its own operator new, so the
// driver's and the validation layer's allocations on the main thread are not counted either. On macOS and Linux a
// replaced operator new also serves the shared libraries, so this file replaces nothing there and the check is off.
// Off with AddressSanitizer too, which brings its own operator new.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define RASM_COUNT_ALLOCATIONS 0
#endif
#endif
#if !defined(RASM_COUNT_ALLOCATIONS) && defined(_WIN32)
#define RASM_COUNT_ALLOCATIONS 1
#endif

#if RASM_COUNT_ALLOCATIONS

namespace
{
    thread_local bool counting = false;
    AllocationCount counted;

    void *allocate(size_t size)
    {
        if (counting)
        {
            counted.firstSize = counted.count == 0 ? size : counted.firstSize;
            ++counted.count;
            counted.bytes += size;
        }
        return std::malloc(size == 0 ? 1 : size);
    }

    void *allocateAligned(size_t size, std::align_val_t align)
    {
        if (counting)
        {
            counted.firstSize = counted.count == 0 ? size : counted.firstSize;
            ++counted.count;
            counted.bytes += size;
        }
        return _aligned_malloc(size == 0 ? 1 : size, static_cast<size_t>(align));
    }

    void *allocateOrThrow(size_t size)
    {
        void *memory = allocate(size);
        if (!memory)
            throw std::bad_alloc();
        return memory;
    }

    void *allocateAlignedOrThrow(size_t size, std::align_val_t align)
    {
        void *memory = allocateAligned(size, align);
        if (!memory)
            throw std::bad_alloc();
        return memory;
    }
}

bool allocationCountingWorks()
{
    return true;
}

void startCountingAllocations()
{
    counted = {};
    counting = true;
}

AllocationCount stopCountingAllocations()
{
    counting = false;
    return counted;
}

void *operator new(size_t size) { return allocateOrThrow(size); }
void *operator new[](size_t size) { return allocateOrThrow(size); }
void *operator new(size_t size, const std::nothrow_t &) noexcept { return allocate(size); }
void *operator new[](size_t size, const std::nothrow_t &) noexcept { return allocate(size); }
void *operator new(size_t size, std::align_val_t align) { return allocateAlignedOrThrow(size, align); }
void *operator new[](size_t size, std::align_val_t align) { return allocateAlignedOrThrow(size, align); }
void *operator new(size_t size, std::align_val_t align, const std::nothrow_t &) noexcept { return allocateAligned(size, align); }
void *operator new[](size_t size, std::align_val_t align, const std::nothrow_t &) noexcept { return allocateAligned(size, align); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, size_t) noexcept { std::free(memory); }
void operator delete(void *memory, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { std::free(memory); }
void operator delete(void *memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete[](void *memory, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete(void *memory, size_t, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete[](void *memory, size_t, std::align_val_t) noexcept { _aligned_free(memory); }
void operator delete(void *memory, std::align_val_t, const std::nothrow_t &) noexcept { _aligned_free(memory); }
void operator delete[](void *memory, std::align_val_t, const std::nothrow_t &) noexcept { _aligned_free(memory); }

#else

bool allocationCountingWorks()
{
    return false;
}

void startCountingAllocations()
{
}

AllocationCount stopCountingAllocations()
{
    return {};
}

#endif
