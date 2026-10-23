// TriAevum PS4 product - C++ exceptions thrown at run time.
//
// The 1.06/1.09 profiles spend ~3% of the game thread unwinding: something
// throws every frame. Every throw passes here (-Wl,--wrap=__cxa_throw,
// CMakeLists.txt); the first ones, then one in a thousand, are logged with
// the exception type and the first frames of the throwing stack (eboot
// addresses, symbolized offline with llvm-symbolizer).

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <typeinfo>

#include <unwind.h>

namespace {

struct Frames {
    uintptr_t addresses[8];
    int count = 0;
};

_Unwind_Reason_Code CollectFrame(_Unwind_Context* context, void* user) {
    auto& frames = *static_cast<Frames*>(user);
    if (frames.count >= 8) return _URC_END_OF_STACK;
    frames.addresses[frames.count++] = _Unwind_GetIP(context);
    return _URC_NO_REASON;
}

} // namespace

extern "C" {

[[noreturn]] void __real___cxa_throw(void* exception, std::type_info* type, void (*destructor)(void*));

[[noreturn]] void __wrap___cxa_throw(void* exception, std::type_info* type,
                                     void (*destructor)(void*)) {
    static std::atomic<uint64_t> count{0};
    const uint64_t index = count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (index <= 20 || index % 1000 == 0) {
        Frames frames;
        _Unwind_Backtrace(CollectFrame, &frames);
        std::fprintf(stderr, "[triaevum] throw #%llu %s, stack:",
                     static_cast<unsigned long long>(index), type != nullptr ? type->name() : "?");
        for (int frame = 1; frame < frames.count; ++frame)
            std::fprintf(stderr, " %#lx", static_cast<unsigned long>(frames.addresses[frame]));
        std::fprintf(stderr, "\n");
    }
    __real___cxa_throw(exception, type, destructor);
}

} // extern "C"
