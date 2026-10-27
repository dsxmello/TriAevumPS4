#include "oot3d_native_a32_memory.h"
#include "oot3d_native_a32_memory_device.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>

#include <nlohmann/json.hpp>

namespace Oot3dNativeGame {
namespace {

bool RangeFits(uint32_t baseAddress, size_t size) {
    return size != 0 &&
           static_cast<uint64_t>(baseAddress) + static_cast<uint64_t>(size) <=
               static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1U;
}

bool RangesOverlap(uint32_t leftBase, size_t leftSize, uint32_t rightBase,
                   size_t rightSize) {
    const uint64_t leftEnd = static_cast<uint64_t>(leftBase) + leftSize;
    const uint64_t rightEnd = static_cast<uint64_t>(rightBase) + rightSize;
    return static_cast<uint64_t>(leftBase) < rightEnd &&
           static_cast<uint64_t>(rightBase) < leftEnd;
}

bool IsSupportedSize(uint8_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

void AddWriteRange(std::vector<std::pair<uint32_t, size_t>>& ranges,
                   uint32_t address, size_t size) {
    uint64_t begin = address, end = begin + size;
    auto first = std::lower_bound(ranges.begin(), ranges.end(), begin,
        [](const auto& range, uint64_t value) {
            return uint64_t{range.first} + range.second < value;
        });
    auto last = first;
    while (last != ranges.end() && last->first <= end) {
        begin = std::min(begin, uint64_t{last->first});
        end = std::max(end, uint64_t{last->first} + last->second);
        ++last;
    }
    const auto position = ranges.erase(first, last);
    ranges.insert(position, {static_cast<uint32_t>(begin), static_cast<size_t>(end - begin)});
}

// The GSP worker's side (oot3d_native_a32_memory_device.h). The runner is set
// only on the device thread; the rest is shared by both threads, the vectors
// under the spinlock (a few instructions per access, never a system call).
thread_local NativeA32DeviceAccess::GuestRunner tDeviceRunner = nullptr;
std::atomic<void (*)()> gLayoutHook{nullptr};
std::atomic<uint64_t> gDirectReads{0};
std::atomic<uint64_t> gDirectWrites{0};
std::atomic<uint64_t> gGuestCalls{0};
std::atomic_flag gDeviceLock = ATOMIC_FLAG_INIT;
std::vector<std::pair<uint32_t, size_t>> gOwedWrites;
std::atomic<bool> gOwedPending{false};
// Bumped by every escape of a write pointer; the device's copy of the ranges
// is good while its epoch matches.
std::atomic<uint64_t> gEscapeEpoch{1};
std::atomic<bool> gEscapeCopyWanted{false};
std::shared_ptr<const std::vector<std::pair<uint32_t, size_t>>> gEscapeCopy;
uint64_t gEscapeCopyEpoch = 0;

class DeviceLock final {
  public:
    DeviceLock() noexcept {
        while (gDeviceLock.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    }
    ~DeviceLock() { gDeviceLock.clear(std::memory_order_release); }
    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;
};

} // namespace

NativeA32DeviceAccess::ThreadScope::ThreadScope(GuestRunner runner) noexcept {
    tDeviceRunner = runner;
}

NativeA32DeviceAccess::ThreadScope::~ThreadScope() { tDeviceRunner = nullptr; }

bool NativeA32DeviceAccess::OnDeviceThread() noexcept { return tDeviceRunner != nullptr; }

void NativeA32DeviceAccess::SetLayoutHook(void (*hook)()) noexcept {
    gLayoutHook.store(hook, std::memory_order_release);
}

void NativeA32DeviceAccess::LayoutChanging() {
    if (tDeviceRunner != nullptr)
        throw std::logic_error("the GSP worker reached a guest memory layout change");
    if (const auto hook = gLayoutHook.load(std::memory_order_acquire)) hook();
}

void NativeA32DeviceAccess::EscapesChanged() noexcept {
    gEscapeEpoch.fetch_add(1, std::memory_order_release);
}

template <typename Function>
auto NativeA32DeviceAccess::OnGuestThread(Function&& function) {
    using Result = std::invoke_result_t<Function&>;
    struct Call {
        std::remove_reference_t<Function>* Body;
        std::optional<Result> Value;
        std::exception_ptr Error;
    } call{&function, std::nullopt, nullptr};
    gGuestCalls.fetch_add(1, std::memory_order_relaxed);
    tDeviceRunner(
        [](void* argument) {
            auto& self = *static_cast<Call*>(argument);
            try {
                self.Value.emplace((*self.Body)());
            } catch (...) {
                self.Error = std::current_exception();
            }
        },
        &call);
    if (call.Error) std::rethrow_exception(call.Error);
    return std::move(*call.Value);
}

uint8_t* NativeA32DeviceAccess::InPlace(const NativeA32Memory& memory, uint32_t address,
                                        size_t size, bool write) noexcept {
    // The mapping only changes after the layout hook let the worker finish.
    const auto* region = memory.FindRegion(address, size);
    if (region == nullptr || (write && !region->Writable) || !memory.mFastPages ||
        memory.mTraceWriteFingerprint) {
        return nullptr;
    }
    // Entries go from null to a pointer under the worker (a deferred page is
    // materialized), never the other way: those changes happen in the drain.
    const size_t last = (uint64_t{address} + size - 1U) >> NativeA32Memory::PageShift;
    for (size_t page = address >> NativeA32Memory::PageShift; page <= last; ++page) {
        const auto& entry = memory.mFastPages[page];
        const bool present = write ? __atomic_load_n(&entry.Write, __ATOMIC_ACQUIRE) != nullptr
                                   : __atomic_load_n(&entry.Read, __ATOMIC_ACQUIRE) != nullptr;
        if (!present) return nullptr;
    }
    return const_cast<uint8_t*>(region->Bytes.data()) + (address - region->BaseAddress);
}

void NativeA32DeviceAccess::NoteWritten(NativeA32Memory& memory, uint32_t address,
                                        size_t size) {
    const size_t last = (uint64_t{address} + size - 1U) >> NativeA32Memory::PageShift;
    for (size_t page = address >> NativeA32Memory::PageShift; page <= last; ++page) {
        __atomic_store_n(&memory.mFastPages[page].WriteGeneration, uint64_t{0},
                         __ATOMIC_RELEASE);
    }
    DeviceLock lock;
    AddWriteRange(gOwedWrites, address, size);
    gOwedPending.store(true, std::memory_order_relaxed);
}

std::optional<uint64_t> NativeA32DeviceAccess::RangeWriteGeneration(
    const NativeA32Memory& memory, uint32_t address, size_t size) {
    if (!memory.mRangeVersionsValid || memory.FindRegion(address, size) == nullptr ||
        !memory.mFastPages) {
        return std::nullopt;
    }
    const uint64_t epoch = gEscapeEpoch.load(std::memory_order_acquire);
    {
        DeviceLock lock;
        if (gEscapeCopyEpoch != epoch || !gEscapeCopy) {
            gEscapeCopyWanted.store(true, std::memory_order_relaxed);
            return std::nullopt;
        }
        if (std::any_of(gEscapeCopy->begin(), gEscapeCopy->end(), [&](const auto& range) {
                return RangesOverlap(address, size, range.first, range.second);
            })) {
            return std::nullopt;
        }
    }
    // Read before the bytes (the caller reads them after): a write landing
    // in between leaves a newer generation than the one kept with them.
    uint64_t generation = 0;
    const size_t last = (uint64_t{address} + size - 1U) >> NativeA32Memory::PageShift;
    for (size_t page = address >> NativeA32Memory::PageShift; page <= last; ++page) {
        const uint64_t value =
            __atomic_load_n(&memory.mFastPages[page].WriteGeneration, __ATOMIC_ACQUIRE);
        if (value == 0U) return std::nullopt;
        generation = std::max(generation, value);
    }
    return generation;
}

bool NativeA32DeviceAccess::AlreadyObserved(const NativeA32Memory& memory, uint32_t address,
                                            size_t size) {
    // The observed ranges change only in ObserveWrites and the layout
    // changes, both on the game thread while the worker waits or is idle.
    if (!memory.mWriteObserver || !RangeFits(address, size) || !memory.IsMapped(address, size))
        return false;
    const uint64_t epoch = gEscapeEpoch.load(std::memory_order_acquire);
    {
        DeviceLock lock;
        if (gEscapeCopyEpoch != epoch || !gEscapeCopy ||
            std::any_of(gEscapeCopy->begin(), gEscapeCopy->end(), [&](const auto& range) {
                return RangesOverlap(address, size, range.first, range.second);
            })) {
            return false;
        }
    }
    const auto& ranges = memory.mObservedWriteRanges;
    const auto covered = std::lower_bound(ranges.begin(), ranges.end(), address,
        [](const auto& range, uint32_t value) { return uint64_t{range.first} + range.second <= value; });
    return covered != ranges.end() && covered->first <= address &&
           uint64_t{covered->first} + covered->second >= uint64_t{address} + size;
}

void NativeA32DeviceAccess::Service(NativeA32Memory& memory) {
    if (gOwedPending.load(std::memory_order_relaxed)) {
        static std::vector<std::pair<uint32_t, size_t>> owed;
        {
            DeviceLock lock;
            owed.swap(gOwedWrites);
            gOwedPending.store(false, std::memory_order_relaxed);
        }
        for (const auto& [address, size] : owed) {
            ++memory.mRangeGeneration;
            memory.MarkRangeWritten(address, size, memory.CurrentRangeGeneration());
        }
        owed.clear();
    }
    if (gEscapeCopyWanted.load(std::memory_order_relaxed)) {
        // Escapes happen on this thread: the copy holds every one up to epoch.
        const uint64_t epoch = gEscapeEpoch.load(std::memory_order_acquire);
        auto copy = std::make_shared<const std::vector<std::pair<uint32_t, size_t>>>(
            memory.mEscapedWritePointers);
        DeviceLock lock;
        gEscapeCopy = std::move(copy);
        gEscapeCopyEpoch = epoch;
        gEscapeCopyWanted.store(false, std::memory_order_relaxed);
    }
}

NativeA32DeviceAccess::Counts NativeA32DeviceAccess::ReadCounts() noexcept {
    return {gDirectReads.load(std::memory_order_relaxed),
            gDirectWrites.load(std::memory_order_relaxed),
            gGuestCalls.load(std::memory_order_relaxed)};
}

NativeA32Memory::NativeA32Memory(const NativeA32Memory& other)
    : mRegions((other.MaterializeDeferredWrites()
                    ? other.mRegions
                    : throw std::runtime_error("cannot copy deferred guest memory"))),
      mRangeGeneration(other.ArchitecturalWriteGeneration()),
      mWriteTraceFingerprint(other.mWriteTraceFingerprint),
      mTraceWriteFingerprint(other.mTraceWriteFingerprint),
      mFastReadMismatchCount(other.mFastReadMismatchCount),
      mLastFastReadMismatchAddress(other.mLastFastReadMismatchAddress),
      mLastFastReadMismatchValue(other.mLastFastReadMismatchValue),
      mLastCheckedReadMismatchValue(other.mLastCheckedReadMismatchValue) {
    RebuildPageTable();
}

NativeA32Memory& NativeA32Memory::operator=(const NativeA32Memory& other) {
    if (this != &other) {
        NativeA32DeviceAccess::LayoutChanging();
        if (!other.MaterializeDeferredWrites())
            throw std::runtime_error("cannot copy deferred guest memory");
        const auto nextRangeGeneration = CurrentRangeGeneration() + 1U;
        mDeferredWrites.clear();
        mDeferredPages.clear();
        mEscapedReadPointers.clear();
        mEscapedWritePointers.clear();
        NativeA32DeviceAccess::EscapesChanged();
        mRegions = other.mRegions;
        mRangeGeneration = nextRangeGeneration;
        mRangeGenerationBias =
            nextRangeGeneration - other.ArchitecturalWriteGeneration();
        mRangeVersionsValid = mRangeVersionsValid && nextRangeGeneration != 0U;
        mWriteTraceFingerprint = other.mWriteTraceFingerprint;
        mTraceWriteFingerprint = other.mTraceWriteFingerprint;
        mFastReadMismatchCount = other.mFastReadMismatchCount;
        mLastFastReadMismatchAddress = other.mLastFastReadMismatchAddress;
        mLastFastReadMismatchValue = other.mLastFastReadMismatchValue;
        mLastCheckedReadMismatchValue = other.mLastCheckedReadMismatchValue;
        RebuildPageTable();
    }
    return *this;
}

NativeA32Memory::NativeA32Memory(NativeA32Memory&& other) noexcept {
    *this = std::move(other);
}

NativeA32Memory& NativeA32Memory::operator=(NativeA32Memory&& other) noexcept {
    if (this == &other) return *this;
    NativeA32DeviceAccess::LayoutChanging();
    const auto nextRangeGeneration = CurrentRangeGeneration() + 1U;
    const bool versionsValid =
        mRangeVersionsValid && other.mRangeVersionsValid && nextRangeGeneration != 0U;
    mRegions = std::move(other.mRegions);
    mPageRegions = std::move(other.mPageRegions);
    mFastPages = std::move(other.mFastPages);
    mFastPageSpans = std::move(other.mFastPageSpans);
    other.mFastPageSpans.clear();
    mRangeGenerationBias =
        nextRangeGeneration - other.ArchitecturalWriteGeneration();
    mRangeGeneration = nextRangeGeneration;
    mRangeVersionsValid = versionsValid;
    mWriteTraceFingerprint = other.mWriteTraceFingerprint;
    mTraceWriteFingerprint = other.mTraceWriteFingerprint;
    mFastReadMismatchCount = other.mFastReadMismatchCount;
    mLastFastReadMismatchAddress = other.mLastFastReadMismatchAddress;
    mLastFastReadMismatchValue = other.mLastFastReadMismatchValue;
    mLastCheckedReadMismatchValue = other.mLastCheckedReadMismatchValue;
    mPendingTracedPointerWrites = std::move(other.mPendingTracedPointerWrites);
    mWatchRanges = std::move(other.mWatchRanges);
    mTrappedPages = std::move(other.mTrappedPages);
    mWatchCounters = other.mWatchCounters;
    mWatchActive = other.mWatchActive;
    mWatchSuppressed = other.mWatchSuppressed;
    mWriteObserver = std::move(other.mWriteObserver);
    mObservedWriteRanges = std::move(other.mObservedWriteRanges);
    mObservedWritePages = std::move(other.mObservedWritePages);
    mEscapedWritePointers = std::move(other.mEscapedWritePointers);
    NativeA32DeviceAccess::EscapesChanged();
    mEscapedReadPointers = std::move(other.mEscapedReadPointers);
    mDeferredWrites = std::move(other.mDeferredWrites);
    mDeferredPages = std::move(other.mDeferredPages);
    // Backing allocations and their fast pointers move together. Only the
    // destination's certificate epoch changes; no page-table allocation.
    for (const auto& [first, last] : mFastPageSpans) {
        for (size_t page = first; page <= last; ++page)
            mFastPages[page].WriteGeneration = nextRangeGeneration;
    }
    // A reused moved-from image must not revive its old certificates either.
    other.mRangeVersionsValid = false;
    return *this;
}

void NativeA32Memory::SetError(std::string* error, std::string_view message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool NativeA32Memory::MapRegion(const NativeA32MemoryRegionConfig& config,
                                std::string* error) {
    if (config.Name.empty()) {
        SetError(error, "guest memory region has no name");
        return false;
    }
    if (!RangeFits(config.BaseAddress, config.Size)) {
        SetError(error, "guest memory region has an invalid address range");
        return false;
    }
    if (config.InitialBytes.size() > config.Size) {
        SetError(error, "guest memory initial image exceeds its region");
        return false;
    }
    if (mRegions.size() >= AmbiguousPage - 1U) {
        SetError(error, "guest memory has too many regions for its page table");
        return false;
    }
    for (const auto& region : mRegions) {
        if (RangesOverlap(config.BaseAddress, config.Size, region.BaseAddress,
                          region.Bytes.size())) {
            SetError(error, "guest memory regions overlap");
            return false;
        }
    }

    NativeA32DeviceAccess::LayoutChanging();
    Region region;
    region.Name = config.Name;
    region.BaseAddress = config.BaseAddress;
    region.Bytes.assign(config.Size, 0);
    std::copy(config.InitialBytes.begin(), config.InitialBytes.end(),
              region.Bytes.begin());
    region.Writable = config.Writable;
    region.Executable = config.Executable;
    mRegions.push_back(std::move(region));
    std::sort(mRegions.begin(), mRegions.end(),
              [](const Region& left, const Region& right) {
                  return left.BaseAddress < right.BaseAddress;
              });
    RebuildPageTable();
    return true;
}

void NativeA32Memory::RebuildPageTable() {
    NativeA32DeviceAccess::LayoutChanging();
    mPageRegions.assign(PageCount, 0U);
    if (!mFastPages) {
        mFastPages = std::make_unique<FastPage[]>(PageCount);
    } else {
        for (const auto& [first, last] : mFastPageSpans) {
            std::fill(mFastPages.get() + first, mFastPages.get() + last + 1U, FastPage{});
        }
    }
    mFastPageSpans.clear();
    for (size_t regionIndex = 0; regionIndex < mRegions.size();
         ++regionIndex) {
        auto& region = mRegions[regionIndex];
        const uint64_t lastAddress =
            static_cast<uint64_t>(region.BaseAddress) +
            region.Bytes.size() - 1U;
        const size_t firstPage = region.BaseAddress >> PageShift;
        const size_t lastPage = static_cast<size_t>(lastAddress >> PageShift);
        const uint16_t pageValue =
            static_cast<uint16_t>(regionIndex + 1U);
        mFastPageSpans.emplace_back(firstPage, lastPage);
        for (size_t page = firstPage; page <= lastPage; ++page) {
            auto& entry = mPageRegions[page];
            if (entry == 0U) {
                entry = pageValue;
            } else if (entry != pageValue) {
                entry = AmbiguousPage;
            }
            auto& fastPage = mFastPages[page];
            fastPage.WriteGeneration = CurrentRangeGeneration();

            const uint64_t pageAddress =
                static_cast<uint64_t>(page) << PageShift;
            const uint64_t regionEnd =
                static_cast<uint64_t>(region.BaseAddress) +
                region.Bytes.size();
            if (pageAddress >= region.BaseAddress &&
                pageAddress + PageSize <= regionEnd) {
                const size_t offset = static_cast<size_t>(
                    pageAddress - region.BaseAddress);
                // Watched pages keep no fast entry: typed accesses then take
                // the checked path, which reads and writes the same bytes.
                const bool trapped =
                    std::binary_search(mTrappedPages.begin(), mTrappedPages.end(), page) ||
                    std::binary_search(mDeferredPages.begin(), mDeferredPages.end(), page);
                if (trapped || (!kFastPagesMayBeTraced && mTraceWriteFingerprint)) {
                    continue;
                }
                fastPage.Read = region.Bytes.data() + offset;
                fastPage.Write = region.Writable &&
                                         !std::binary_search(mObservedWritePages.begin(),
                                                             mObservedWritePages.end(), page)
                                     ? region.Bytes.data() + offset : nullptr;
            }
        }
    }
}

NativeA32Memory::Region* NativeA32Memory::FindRegion(uint32_t address,
                                                     size_t size) {
    return const_cast<Region*>(
        static_cast<const NativeA32Memory*>(this)->FindRegion(address, size));
}

const NativeA32Memory::Region* NativeA32Memory::FindRegion(
    uint32_t address, size_t size) const {
    if (!RangeFits(address, size)) {
        return nullptr;
    }
    const uint64_t end = static_cast<uint64_t>(address) + size;
    const auto contains = [&](size_t index) {
        const auto& region = mRegions[index];
        return address >= region.BaseAddress &&
               end <= static_cast<uint64_t>(region.BaseAddress) +
                          region.Bytes.size();
    };
    if (!mPageRegions.empty()) {
        const uint16_t pageEntry = mPageRegions[address >> PageShift];
        if (pageEntry != 0U && pageEntry != AmbiguousPage) {
            const size_t index = pageEntry - 1U;
            if (index < mRegions.size() && contains(index)) {
                return &mRegions[index];
            }
        }
    }
    auto found = std::upper_bound(
        mRegions.begin(), mRegions.end(), address,
        [](uint32_t candidate, const Region& region) {
            return candidate < region.BaseAddress;
        });
    if (found == mRegions.begin()) {
        return nullptr;
    }
    const size_t index =
        static_cast<size_t>(std::distance(mRegions.begin(), found - 1));
    if (contains(index)) {
        return &mRegions[index];
    }
    return nullptr;
}

bool NativeA32Memory::IsMapped(uint32_t address, size_t size) const {
    return FindRegion(address, size) != nullptr;
}

bool NativeA32Memory::IsWritable(uint32_t address, size_t size) const {
    const auto* region = FindRegion(address, size);
    return region != nullptr && region->Writable;
}

bool NativeA32Memory::IsExecutable(uint32_t address, size_t size) const {
    const auto* region = FindRegion(address, size);
    return region != nullptr && region->Executable;
}

size_t NativeA32Memory::RegionCount() const {
    return mRegions.size();
}

uint64_t NativeA32Memory::WriteGeneration() const {
    return ArchitecturalWriteGeneration();
}

std::optional<uint64_t>
NativeA32Memory::RangeWriteGeneration(uint32_t address,
                                      size_t size) const noexcept {
  if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]]
      return NativeA32DeviceAccess::RangeWriteGeneration(*this, address, size);
  if (!mRangeVersionsValid || FindRegion(address, size) == nullptr ||
      HasEscapedWritePointer(address, size)) return std::nullopt;
  uint64_t generation = 0;
  const size_t last = (uint64_t{address} + size - 1U) >> PageShift;
  for (size_t page = address >> PageShift; page <= last; ++page) {
      if (!mFastPages || !mFastPages[page].WriteGeneration) return std::nullopt;
      generation = std::max(generation, mFastPages[page].WriteGeneration);
  }
  return generation;
}

void NativeA32Memory::EnableWriteTraceFingerprint(bool enabled) noexcept {
    NativeA32DeviceAccess::LayoutChanging();
    const bool changed = mTraceWriteFingerprint != enabled;
    mTraceWriteFingerprint = enabled;
    mWriteTraceFingerprint = 14695981039346656037ULL;
    mPendingTracedPointerWrites.clear();
    // Fast pages follow the trace (kFastPagesMayBeTraced).
    if (!kFastPagesMayBeTraced && changed) RebuildPageTable();
}

uint64_t NativeA32Memory::WriteTraceFingerprint() noexcept {
    FlushTracedPointerWrites();
    return mWriteTraceFingerprint;
}

uint64_t NativeA32Memory::FastReadMismatchCount() const noexcept {
    return mFastReadMismatchCount;
}

uint32_t NativeA32Memory::LastFastReadMismatchAddress() const noexcept {
    return mLastFastReadMismatchAddress;
}

uint64_t NativeA32Memory::LastFastReadMismatchValue() const noexcept {
    return mLastFastReadMismatchValue;
}

uint64_t NativeA32Memory::LastCheckedReadMismatchValue() const noexcept {
    return mLastCheckedReadMismatchValue;
}

void NativeA32Memory::RecordTracedWrite(
    uint32_t address, std::span<const uint8_t> bytes) noexcept {
    if (!mTraceWriteFingerprint) {
        return;
    }
    const auto append = [&](uint8_t byte) {
        mWriteTraceFingerprint ^= byte;
        mWriteTraceFingerprint *= 1099511628211ULL;
    };
    for (size_t index = 0; index < sizeof(address); ++index) {
        append(static_cast<uint8_t>(address >> (index * 8U)));
    }
    const uint64_t size = bytes.size();
    for (size_t index = 0; index < sizeof(size); ++index) {
        append(static_cast<uint8_t>(size >> (index * 8U)));
    }
    for (const uint8_t byte : bytes) {
        append(byte);
    }
}

void NativeA32Memory::FlushTracedPointerWrites() noexcept {
    for (const auto& [address, size] : mPendingTracedPointerWrites) {
        const auto* region = FindRegion(address, size);
        if (region == nullptr) {
            continue;
        }
        const size_t offset =
            static_cast<size_t>(address - region->BaseAddress);
        RecordTracedWrite(
            address,
            std::span<const uint8_t>(region->Bytes.data() + offset, size));
    }
    mPendingTracedPointerWrites.clear();
}

uint64_t NativeA32Memory::ContentFingerprint() const {
    if (!MaterializeDeferredWrites())
        throw std::runtime_error("cannot fingerprint deferred guest memory");
    uint64_t hash = 14695981039346656037ULL;
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& region : mRegions) {
        append(region.Name.data(), region.Name.size());
        append(&region.BaseAddress, sizeof(region.BaseAddress));
        append(&region.Writable, sizeof(region.Writable));
        append(&region.Executable, sizeof(region.Executable));
        if (!region.Bytes.empty()) {
            append(region.Bytes.data(), region.Bytes.size());
        }
    }
    return hash;
}

uint64_t NativeA32Memory::StateFingerprint() const {
    if (!MaterializeDeferredWrites())
        throw std::runtime_error("cannot fingerprint deferred guest memory");
    uint64_t hash = 14695981039346656037ULL;
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t index = 0; index < size; ++index) {
            hash ^= bytes[index];
            hash *= 1099511628211ULL;
        }
    };
    const uint64_t writeGeneration = ArchitecturalWriteGeneration();
    append(&writeGeneration, sizeof(writeGeneration));
    for (const auto& region : mRegions) {
        append(region.Name.data(), region.Name.size());
        append(&region.BaseAddress, sizeof(region.BaseAddress));
        append(&region.Writable, sizeof(region.Writable));
        append(&region.Executable, sizeof(region.Executable));
        if (!region.Bytes.empty()) {
            append(region.Bytes.data(), region.Bytes.size());
        }
    }
    return hash;
}

nlohmann::json NativeA32Memory::CaptureState() const {
    if (!MaterializeDeferredWrites())
        throw std::runtime_error("cannot capture deferred guest memory");
    nlohmann::json regions = nlohmann::json::array();
    for (const auto& region : mRegions) {
        regions.push_back({
            {"name", region.Name},
            {"base_address", region.BaseAddress},
            {"writable", region.Writable},
            {"executable", region.Executable},
            {"bytes", nlohmann::json::binary(region.Bytes)},
        });
    }
    return {
        {"format", "oot3d_native_a32_memory_state_v1"},
        {"write_generation", ArchitecturalWriteGeneration()},
        {"regions", std::move(regions)},
    };
}

bool NativeA32Memory::RestoreState(const nlohmann::json& state,
                                   std::string* error) {
    NativeA32DeviceAccess::LayoutChanging();
    try {
        if (!state.is_object() ||
            state.value("format", std::string{}) !=
                "oot3d_native_a32_memory_state_v1" ||
            !state.contains("regions") || !state["regions"].is_array()) {
            SetError(error, "native A32 memory state format is invalid");
            return false;
        }
        NativeA32Memory staged;
        for (const auto& encoded : state["regions"]) {
            if (!encoded.is_object() || !encoded.contains("bytes") ||
                !encoded["bytes"].is_binary()) {
                SetError(error, "native A32 memory region state is invalid");
                return false;
            }
            const auto& bytes = encoded["bytes"].get_binary();
            NativeA32MemoryRegionConfig config;
            config.Name = encoded.at("name").get<std::string>();
            config.BaseAddress = encoded.at("base_address").get<uint32_t>();
            config.Size = bytes.size();
            config.Writable = encoded.at("writable").get<bool>();
            config.Executable = encoded.at("executable").get<bool>();
            config.InitialBytes = std::span<const uint8_t>(bytes.data(),
                                                           bytes.size());
            if (!staged.MapRegion(config, error)) {
                return false;
            }
        }
        staged.SetArchitecturalWriteGeneration(
            state.at("write_generation").get<uint64_t>());
        if (staged.ArchitecturalWriteGeneration() == 0U) {
            SetError(error,
                     "native A32 memory write generation is invalid");
            return false;
        }
        staged.mTraceWriteFingerprint = mTraceWriteFingerprint;
        staged.PreserveAccessObserversFrom(*this);
        *this = std::move(staged);
        return true;
    } catch (const std::exception& exception) {
        if (error != nullptr) {
            *error = std::string("native A32 memory state decode failed: ") +
                     exception.what();
        }
        return false;
    }
}

void NativeA32Memory::SetRangeWatch(
    std::span<const std::pair<uint32_t, uint32_t>> ranges) {
    mWatchRanges.assign(ranges.begin(), ranges.end());
    mWatchActive = !mWatchRanges.empty();
    std::vector<size_t> trapped;
    for (const auto& range : mWatchRanges) {
        if (range.second <= range.first) {
            continue;
        }
        const size_t firstPage = range.first >> PageShift;
        const size_t lastPage = static_cast<size_t>(
            (static_cast<uint64_t>(range.second) - 1U) >> PageShift);
        for (size_t page = firstPage; page <= lastPage; ++page) {
            trapped.push_back(page);
        }
    }
    std::sort(trapped.begin(), trapped.end());
    trapped.erase(std::unique(trapped.begin(), trapped.end()), trapped.end());
    // Trapping costs a page-table rebuild, so rebuild only when the trapped
    // page set changes: the same destination every frame costs nothing after
    // the first time.
    if (trapped != mTrappedPages) {
        mTrappedPages = std::move(trapped);
        RebuildPageTable();
    }
}

void NativeA32Memory::ClearRangeWatch() {
    if (mWatchRanges.empty() && mTrappedPages.empty() && !mWatchActive) {
        return;
    }
    mWatchRanges.clear();
    mWatchActive = false;
    if (!mTrappedPages.empty()) {
        mTrappedPages.clear();
        RebuildPageTable();
    }
}


void NativeA32Memory::SetWriteObserver(WriteObserver observer) {
    NativeA32DeviceAccess::LayoutChanging();
    mWriteObserver = std::move(observer);
    if (!mWriteObserver && !mObservedWritePages.empty()) {
        mObservedWriteRanges.clear();
        mObservedWritePages.clear();
        RebuildPageTable();
    }
}

bool NativeA32Memory::HasEscapedWritePointer(uint32_t address, size_t size) const noexcept {
    return std::any_of(mEscapedWritePointers.begin(), mEscapedWritePointers.end(),
        [&](const auto& range) { return RangesOverlap(address, size, range.first, range.second); });
}

bool NativeA32Memory::ObserveWrites(uint32_t address, size_t size) {
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]] {
        if (NativeA32DeviceAccess::AlreadyObserved(*this, address, size)) return true;
        return NativeA32DeviceAccess::OnGuestThread([&] { return ObserveWrites(address, size); });
    }
    if (!mWriteObserver || !RangeFits(address, size) ||
        !IsMapped(address, size) || HasEscapedWritePointer(address, size)) return false;
    const auto covered = std::lower_bound(mObservedWriteRanges.begin(), mObservedWriteRanges.end(),
        address, [](const auto& range, uint32_t value) {
            return uint64_t{range.first} + range.second <= value;
        });
    if (covered != mObservedWriteRanges.end() && covered->first <= address &&
        uint64_t{covered->first} + covered->second >= uint64_t{address} + size) return true;
    AddWriteRange(mObservedWriteRanges, address, size);
    const size_t first = address >> PageShift;
    const size_t last = (uint64_t{address} + size - 1) >> PageShift;
    for (size_t page = first; page <= last; ++page) {
        const auto position = std::lower_bound(mObservedWritePages.begin(), mObservedWritePages.end(), page);
        if (position == mObservedWritePages.end() || *position != page)
            mObservedWritePages.insert(position, page);
        if (mFastPages) mFastPages[page].Write = nullptr;
    }
    return true;
}

void NativeA32Memory::NotifyObservedWrite(uint32_t address, size_t size) {
    DiscardDeferredWrites(address, size);
    if (!mWriteObserver || mWatchSuppressed) return;
    const auto overlap = std::lower_bound(mObservedWriteRanges.begin(), mObservedWriteRanges.end(),
        address, [](const auto& range, uint32_t value) {
            return uint64_t{range.first} + range.second <= value;
        });
    if (overlap != mObservedWriteRanges.end() && overlap->first < uint64_t{address} + size)
        mWriteObserver(address, size);
}

void NativeA32Memory::PreserveAccessObserversFrom(const NativeA32Memory& memory) {
    NativeA32DeviceAccess::LayoutChanging();
    mWriteObserver = memory.mWriteObserver;
    mObservedWriteRanges = memory.mObservedWriteRanges;
    mObservedWritePages = memory.mObservedWritePages;
    mWatchRanges = memory.mWatchRanges;
    mTrappedPages = memory.mTrappedPages;
    mWatchCounters = memory.mWatchCounters;
    mWatchActive = memory.mWatchActive;
    // Escaped pointers referred to the replaced allocation, not this image.
    RebuildPageTable();
}

void NativeA32Memory::ReleaseDeferredPageTraps() const {
    if (!mDeferredWrites.empty()) return;
    if (!kFastPagesMayBeTraced && mTraceWriteFingerprint) {
        mDeferredPages.clear();
        return;
    }
    for (const size_t page : mDeferredPages) {
        if (!mFastPages || std::binary_search(mTrappedPages.begin(), mTrappedPages.end(), page))
            continue;
        const uint32_t address = static_cast<uint32_t>(page << PageShift);
        const auto* region = FindRegion(address, PageSize);
        if (!region) continue;
        auto& entry = mFastPages[page];
        auto* bytes = region->Bytes.data() + (address - region->BaseAddress);
        entry.Read = bytes;
        entry.Write = region->Writable &&
                              !std::binary_search(mObservedWritePages.begin(),
                                                  mObservedWritePages.end(), page)
                          ? const_cast<uint8_t*>(bytes) : nullptr;
    }
    mDeferredPages.clear();
}

void NativeA32Memory::DiscardDeferredWrites(uint32_t address, size_t size) const {
    if (mDeferredWrites.empty() || size == 0U) return;
    const uint64_t end = uint64_t{address} + size;
    for (size_t index = 0; index < mDeferredWrites.size();) {
        auto& write = mDeferredWrites[index];
        const uint64_t writeEnd = uint64_t{write.Address} + write.Size;
        if (writeEnd <= address || write.Address >= end) { ++index; continue; }
        if (write.Address < address && writeEnd > end) {
            DeferredWrite right{static_cast<uint32_t>(end),
                                static_cast<size_t>(writeEnd - end), write.Reader};
            write.Size = address - write.Address;
            mDeferredWrites.insert(mDeferredWrites.begin() + index + 1, std::move(right));
            index += 2;
        } else if (write.Address < address) {
            write.Size = address - write.Address;
            ++index;
        } else if (writeEnd > end) {
            write.Address = static_cast<uint32_t>(end);
            write.Size = static_cast<size_t>(writeEnd - end);
            ++index;
        } else {
            mDeferredWrites.erase(mDeferredWrites.begin() + index);
        }
    }
    ReleaseDeferredPageTraps();
}

bool NativeA32Memory::DeferWrites(
    std::span<const std::pair<uint32_t, size_t>> ranges, DeferredRead reader) {
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]]
        return NativeA32DeviceAccess::OnGuestThread(
            [&] { return DeferWrites(ranges, std::move(reader)); });
    NativeA32DeviceAccess::LayoutChanging();
    if (!reader || ranges.empty() || mTraceWriteFingerprint) return false;
    for (const auto& [address, size] : ranges) {
        if (!RangeFits(address, size) || !IsWritable(address, size) ||
            HasEscapedWritePointer(address, size) ||
            std::any_of(mEscapedReadPointers.begin(), mEscapedReadPointers.end(),
                [&](const auto& escaped) {
                    return RangesOverlap(address, size, escaped.first, escaped.second);
                })) return false;
    }
    auto owner = std::make_shared<const DeferredRead>(std::move(reader));
    for (const auto& [address, size] : ranges) {
        DiscardDeferredWrites(address, size);
        mDeferredWrites.push_back({address, size, owner});
        const size_t last = (uint64_t{address} + size - 1U) >> PageShift;
        for (size_t page = address >> PageShift; page <= last; ++page) {
            const auto at = std::lower_bound(mDeferredPages.begin(), mDeferredPages.end(), page);
            if (at == mDeferredPages.end() || *at != page) mDeferredPages.insert(at, page);
            if (mFastPages) {
                mFastPages[page].Read = nullptr;
                mFastPages[page].Write = nullptr;
            }
        }
        // A logical device write invalidates exclusives now, not at readback.
        ++mRangeGeneration;
        MarkRangeWritten(address, size, CurrentRangeGeneration());
    }
    return true;
}

bool NativeA32Memory::ResolveDeferredRead(uint32_t address, size_t size) const {
    if (mDeferredWrites.empty() || size == 0U) return true;
    const uint64_t end = uint64_t{address} + size;
    try {
        for (;;) {
            const auto found = std::find_if(mDeferredWrites.begin(), mDeferredWrites.end(),
                [&](const auto& write) {
                    return write.Address < end && uint64_t{write.Address} + write.Size > address;
                });
            if (found == mDeferredWrites.end()) return true;
            const uint32_t low = std::max(address, found->Address);
            const size_t count = static_cast<size_t>(
                std::min(end, uint64_t{found->Address} + found->Size) - low);
            const auto reader = found->Reader;
            auto* region = const_cast<NativeA32Memory*>(this)->FindRegion(low, count);
            if (!region || !(*reader)(low, std::span<uint8_t>(
                    region->Bytes.data() + (low - region->BaseAddress), count)))
                return false;
            DiscardDeferredWrites(low, count);
        }
    } catch (...) {
        // The typed AOT bus is noexcept: fail the memory access, never expose
        // stale bytes or let a renderer exception cross the title ABI.
        return false;
    }
}

bool NativeA32Memory::MaterializeDeferredWrites() const {
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]]
        return NativeA32DeviceAccess::OnGuestThread([&] { return MaterializeDeferredWrites(); });
    while (!mDeferredWrites.empty()) {
        const auto address = mDeferredWrites.front().Address;
        const auto size = mDeferredWrites.front().Size;
        if (!ResolveDeferredRead(address, size)) return false;
    }
    return true;
}

std::span<const uint8_t> NativeA32Memory::SynchronousReadView(
    uint32_t address, size_t size) const {
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]] {
        if (const auto* bytes = NativeA32DeviceAccess::InPlace(*this, address, size, false)) {
            gDirectReads.fetch_add(1, std::memory_order_relaxed);
            return {bytes, size};
        }
        return NativeA32DeviceAccess::OnGuestThread(
            [&] { return SynchronousReadView(address, size); });
    }
    const auto* region = FindRegion(address, size);
    if (!region || !ResolveDeferredRead(address, size)) return {};
    return {region->Bytes.data() + (address - region->BaseAddress), size};
}

bool NativeA32Memory::ReadBytes(uint32_t address,
                                std::span<uint8_t> bytes) const {
    if (bytes.empty()) {
        return true;
    }
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]] {
        if (const auto* source = NativeA32DeviceAccess::InPlace(*this, address, bytes.size(), false)) {
            std::memcpy(bytes.data(), source, bytes.size());
            gDirectReads.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return NativeA32DeviceAccess::OnGuestThread([&] { return ReadBytes(address, bytes); });
    }
    const auto* region = FindRegion(address, bytes.size());
    if (region == nullptr) {
        return false;
    }
    if (!ResolveDeferredRead(address, bytes.size())) return false;
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::copy_n(region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.size(), bytes.begin());
    if (!mWatchSuppressed && Watches(address, bytes.size())) [[unlikely]] {
        ++mWatchCounters.ByteReads;
    }
    return true;
}

const uint8_t* NativeA32Memory::GetReadPointer(uint32_t address,
                                               size_t size) const {
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]]
        return NativeA32DeviceAccess::OnGuestThread([&] { return GetReadPointer(address, size); });
    const auto* region = FindRegion(address, size);
    if (region == nullptr) {
        return nullptr;
    }
    if (!ResolveDeferredRead(address, size)) return nullptr;
    AddWriteRange(mEscapedReadPointers, address, size);
    if (!mWatchSuppressed && Watches(address, size)) [[unlikely]] {
        // Handing out a live pointer ends observability for this range: every
        // later read through it happens outside any hook we own.
        ++mWatchCounters.PointerReads;
    }
    return region->Bytes.data() +
           static_cast<size_t>(address - region->BaseAddress);
}

uint8_t* NativeA32Memory::GetWritePointer(uint32_t address, size_t size) {
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]]
        return NativeA32DeviceAccess::OnGuestThread([&] { return GetWritePointer(address, size); });
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        return nullptr;
    }
    if (!ResolveDeferredRead(address, size)) return nullptr;
    if (!mWatchSuppressed && Watches(address, size)) [[unlikely]] {
        ++mWatchCounters.PointerWrites;
    }
    uint8_t* destination = region->Bytes.data() +
                           static_cast<size_t>(address - region->BaseAddress);
    if (mTraceWriteFingerprint) {
        mPendingTracedPointerWrites.emplace_back(address, size);
    }
    AddWriteRange(mEscapedWritePointers, address, size);
    NativeA32DeviceAccess::EscapesChanged();
    NotifyObservedWrite(address, size);
    ++mRangeGeneration;
    MarkRangeWritten(address, size, CurrentRangeGeneration());
    return destination;
}

std::optional<uint32_t>
NativeA32Memory::GetGuestAddress(const void* pointer,
                                 size_t size) const noexcept {
    if (pointer == nullptr || size == 0U) {
        return std::nullopt;
    }
    const uintptr_t candidate = reinterpret_cast<uintptr_t>(pointer);
    for (const auto& region : mRegions) {
        const uintptr_t base =
            reinterpret_cast<uintptr_t>(region.Bytes.data());
        if (candidate < base) {
            continue;
        }
        const uintptr_t rawOffset = candidate - base;
        if (rawOffset > region.Bytes.size()) {
            continue;
        }
        const size_t offset = static_cast<size_t>(rawOffset);
        if (size > region.Bytes.size() - offset) {
            continue;
        }
        const uint64_t guestAddress =
            static_cast<uint64_t>(region.BaseAddress) + offset;
        if (guestAddress <= std::numeric_limits<uint32_t>::max()) {
            return static_cast<uint32_t>(guestAddress);
        }
    }
    return std::nullopt;
}

bool NativeA32Memory::WriteBytes(uint32_t address,
                                 std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return true;
    }
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]] {
        if (auto* destination = NativeA32DeviceAccess::InPlace(*this, address, bytes.size(), true)) {
            std::memcpy(destination, bytes.data(), bytes.size());
            NativeA32DeviceAccess::NoteWritten(*this, address, bytes.size());
            gDirectWrites.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return NativeA32DeviceAccess::OnGuestThread([&] { return WriteBytes(address, bytes); });
    }
    auto* region = FindRegion(address, bytes.size());
    if (region == nullptr || !region->Writable) {
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::copy(bytes.begin(), bytes.end(),
              region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    if (Watches(address, bytes.size())) [[unlikely]] {
        // Counted apart from typed writes. The renderer suppresses itself
        // around its own materialization of these bytes: that traffic lands in
        // SuppressedByteWrites, which doubles as the control that the trap is
        // live in a run where no guest or service touched the range.
        if (mWatchSuppressed) {
            ++mWatchCounters.SuppressedByteWrites;
        } else {
            ++mWatchCounters.ByteWrites;
        }
    }
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(address, bytes);
    }
    ++mRangeGeneration;
    MarkRangeWritten(address, bytes.size(), CurrentRangeGeneration());
    NotifyObservedWrite(address, bytes.size());
    return true;
}

bool NativeA32Memory::WriteHostBytes(
    uint32_t address, std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        return true;
    }
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]]
        return NativeA32DeviceAccess::OnGuestThread([&] { return WriteHostBytes(address, bytes); });
    auto* region = FindRegion(address, bytes.size());
    if (region == nullptr) {
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::copy(bytes.begin(), bytes.end(),
              region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(address, bytes);
    }
    ++mRangeGeneration;
    MarkRangeWritten(address, bytes.size(), CurrentRangeGeneration());
    if (!mWatchSuppressed && Watches(address, bytes.size())) [[unlikely]] {
        ++mWatchCounters.HostByteWrites;
    }
    NotifyObservedWrite(address, bytes.size());
    return true;
}

bool NativeA32Memory::Fill(uint32_t address, size_t size, uint8_t value) {
    if (size == 0) {
        return true;
    }
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]] {
        if (auto* destination = NativeA32DeviceAccess::InPlace(*this, address, size, true)) {
            std::memset(destination, value, size);
            NativeA32DeviceAccess::NoteWritten(*this, address, size);
            gDirectWrites.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        return NativeA32DeviceAccess::OnGuestThread([&] { return Fill(address, size, value); });
    }
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    std::fill_n(region->Bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                size, value);
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(
            address,
            std::span<const uint8_t>(region->Bytes.data() + offset, size));
    }
    ++mRangeGeneration;
    MarkRangeWritten(address, size, CurrentRangeGeneration());
    if (!mWatchSuppressed && Watches(address, size)) [[unlikely]] {
        ++mWatchCounters.Fills;
    }
    NotifyObservedWrite(address, size);
    return true;
}

bool NativeA32Memory::ReadSized(uint32_t address, uint8_t size,
                                uint64_t* value,
                                uint32_t* faultAddress) const {
    // Typed accesses belong to the guest; the device never makes one.
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]] {
        try {
            return NativeA32DeviceAccess::OnGuestThread(
                [&] { return ReadSized(address, size, value, faultAddress); });
        } catch (...) {
            return false;
        }
    }
    if (value == nullptr || !IsSupportedSize(size)) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    const auto* region = FindRegion(address, size);
    if (region == nullptr) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    if (!ResolveDeferredRead(address, size)) {
        if (faultAddress) *faultAddress = address;
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    uint64_t decoded = 0;
    for (uint8_t index = 0; index < size; ++index) {
        decoded |= static_cast<uint64_t>(region->Bytes[offset + index])
                   << (index * 8U);
    }
    *value = decoded;
    if (Watches(address, size)) [[unlikely]] {
        NoteWatchedRead(address, size);
    }
    return true;
}

bool NativeA32Memory::WriteSized(uint32_t address, uint8_t size,
                                 uint64_t value, uint32_t* faultAddress) {
    if (NativeA32DeviceAccess::OnDeviceThread()) [[unlikely]] {
        try {
            return NativeA32DeviceAccess::OnGuestThread(
                [&] { return WriteSized(address, size, value, faultAddress); });
        } catch (...) {
            return false;
        }
    }
    if (!IsSupportedSize(size)) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    auto* region = FindRegion(address, size);
    if (region == nullptr || !region->Writable) {
        if (faultAddress != nullptr) {
            *faultAddress = address;
        }
        return false;
    }
    const size_t offset = static_cast<size_t>(address - region->BaseAddress);
    switch (size) {
    case 1: std::memcpy(region->Bytes.data() + offset, &value, 1); break;
    case 2: std::memcpy(region->Bytes.data() + offset, &value, 2); break;
    case 4: std::memcpy(region->Bytes.data() + offset, &value, 4); break;
    case 8: std::memcpy(region->Bytes.data() + offset, &value, 8); break;
    }
    if (mTraceWriteFingerprint) [[unlikely]] {
        RecordTracedWrite(
            address,
            std::span<const uint8_t>(region->Bytes.data() + offset, size));
    }
    ++mRangeGeneration;
    MarkRangeWritten(address, size, CurrentRangeGeneration());
    if (Watches(address, size)) [[unlikely]] {
        NoteWatchedWrite(address, size);
    }
    NotifyObservedWrite(address, size);
    return true;
}

bool NativeA32Memory::Read8(uint32_t address, uint8_t* value) {
    return ReadFast(address, value);
}

bool NativeA32Memory::Read16(uint32_t address, uint16_t* value) {
    return ReadFast(address, value);
}

bool NativeA32Memory::Read32(uint32_t address, uint32_t* value) {
    return ReadFast(address, value);
}

bool NativeA32Memory::Read64(uint32_t address, uint64_t* value,
                             uint32_t* faultAddress) {
    return ReadFast(address, value, faultAddress);
}

bool NativeA32Memory::Write8(uint32_t address, uint8_t value) {
    return WriteFast(address, value);
}

bool NativeA32Memory::Write16(uint32_t address, uint16_t value) {
    return WriteFast(address, value);
}

bool NativeA32Memory::Write32(uint32_t address, uint32_t value) {
    return WriteFast(address, value);
}

bool NativeA32Memory::Write64(uint32_t address, uint64_t value,
                             uint32_t* faultAddress) {
    return WriteFast(address, value, faultAddress);
}

bool NativeA32Memory::LoadExclusive(uint32_t address, uint8_t size,
                                    uint64_t* value, uint64_t* token,
                                    uint32_t* faultAddress) {
    if (token == nullptr || !ReadSized(address, size, value, faultAddress)) {
        return false;
    }
    *token = ArchitecturalWriteGeneration();
    return true;
}

oot3d::recomp::a32::ExclusiveStoreResult NativeA32Memory::StoreExclusive(
    uint32_t address, uint8_t size, uint64_t value, uint64_t token,
    uint32_t* faultAddress) {
    if (token != ArchitecturalWriteGeneration()) {
        return oot3d::recomp::a32::ExclusiveStoreResult::ReservationLost;
    }
    if (!WriteSized(address, size, value, faultAddress)) {
        return oot3d::recomp::a32::ExclusiveStoreResult::MemoryFault;
    }
    return oot3d::recomp::a32::ExclusiveStoreResult::Success;
}

bool NativeA32Memory::AtomicSwap(uint32_t address, uint8_t size,
                                 uint32_t replacement, uint32_t* previous,
                                 uint32_t* faultAddress) {
    uint64_t oldValue = 0;
    if (previous == nullptr || !ReadSized(address, size, &oldValue, faultAddress) ||
        !WriteSized(address, size, replacement, faultAddress)) {
        return false;
    }
    *previous = static_cast<uint32_t>(oldValue);
    return true;
}

} // namespace Oot3dNativeGame
