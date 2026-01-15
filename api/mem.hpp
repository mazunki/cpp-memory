#pragma once
#include <memory_resource>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <bit>

struct mem_stats {
  std::size_t total_bytes{};          // total capacity of the allocator
  std::size_t busy_bytes{};           // including overhead of the underlying implementation
  std::size_t peak_busy_bytes{};

  std::size_t requested_bytes{};       // sum of all requested bytes
  std::size_t peak_requested_bytes{};

  // uint64_t should be big enough. could also use uintmax_t, but this could
  // cause unpredictable behaviour on specific architectures
  std::uint64_t count_alloc{};
  std::uint64_t count_alloc_at{};
  std::uint64_t count_dealloc{};
};

struct mem_region {
  std::uintptr_t start;
  std::uintptr_t end; // one-past

  std::size_t size() const noexcept { return end - start; }

  std::byte* start_ptr() const noexcept { return reinterpret_cast<std::byte*>(start); }
  std::byte* end_ptr()   const noexcept { return reinterpret_cast<std::byte*>(end); }
};

struct mem_config {
  mem_region region;
  bool overbooking{false};
};


class mem_resource : public std::pmr::memory_resource {
public:
  explicit mem_resource(mem_config cfg) : config_(cfg) {
    if (config_.region.start > config_.region.end) {
      throw std::invalid_argument("allocator can't end before it begins");
    }
    stats_.total_bytes = config_.region.size();
  }
  virtual ~mem_resource() = default;

  const mem_stats& stats() const noexcept {
    return stats_;
  }

  /**
   * std::pmr::memory_resource
   * https://en.cppreference.com/w/cpp/memory/memory_resource.html
   *
   *   void* allocate(size_t bytes, size_t alignment)
   *   void deallocate(void *p, size_t bytes, size_t alignment)
   *   bool is_equal(const memory_resource& other)
   *
   * this interface does not provide allocate_at(), which is necessary for
   * MAP_FIXED allocations
   */
  void* allocate_at(void* where, std::size_t bytes, std::size_t alignment=alignof(std::max_align_t)) {
    return do_allocate_at(where, bytes, alignment);
  }

protected:
  /**
   * these are the *strategy hooks*, need to be implemented by your allocator
   */
  virtual uintptr_t strat_allocate(std::size_t bytes, std::size_t alignment) = 0;
  virtual void  strat_deallocate(uintptr_t p, std::size_t bytes, std::size_t alignment) noexcept = 0;

  virtual uintptr_t strat_allocate_at(uintptr_t where, std::size_t bytes, std::size_t alignment) {
    /*
     * the default implementation explicitly ignores the requested position in order
     * to make it easier to implement different strategies
     **/
    (void)where;
    return strat_allocate(bytes, alignment);
  }

  virtual std::string_view name() const noexcept = 0;

private:
  /**
   * the default implementation from std::pmr::memory_resource are the following wrappers
   *   allocate => do_allocate(size_t bytes, size_t alignment)
   *   deallocate => void do_deallocate(void* p, size_t bytes, size_t alignment)
   *   is_equal => bool do_is_equal(size_t bytes, size_t alignment)
   * 
   *  we override these here to handle statistics, final to guarantee nobody
   *  accidentally overrides these
   *
   *  we also add do_allocate_at() here for consistency
   */

  void* do_allocate(std::size_t bytes, std::size_t alignment) final {
    // pre: validate alignment
    if (!std::has_single_bit(alignment))
      throw std::invalid_argument("alignment must be a power of 2");

    // pre: validate overbooking
    if (!config_.overbooking && stats_.requested_bytes + bytes > stats_.total_bytes) {
        // HACK: using requested bytes here is wrong, just a temporary workaround
        // if (!config_.overbooking && stats_.busy_bytes + bytes > stats_.total_bytes)
        throw std::bad_alloc();
    }

    uintptr_t p = strat_allocate(bytes, alignment);

    // post: update stats
    stats_.requested_bytes += bytes;
    if (stats_.requested_bytes > stats_.peak_requested_bytes) {
      stats_.peak_requested_bytes = stats_.requested_bytes;
    }
    stats_.count_alloc++;

    return reinterpret_cast<void*>(p);
  }

  void* do_allocate_at(void* where, std::size_t bytes, std::size_t alignment) {  // not final since it's not a virtual: can't be overriden anyway
    auto addr = reinterpret_cast<std::uintptr_t>(where);

    // pre: validate alignment
    if (!std::has_single_bit(alignment))
      throw std::invalid_argument("alignment must be a power of 2");
    if (addr & (alignment - 1))
      throw std::invalid_argument("address must be aligned");

    // pre: validate range
    if (addr < config_.region.start || addr >= config_.region.end)
      throw std::invalid_argument("address must be in range");
    if (bytes > config_.region.end - addr)
      throw std::bad_alloc();  // allocation won't fit in memory

    uintptr_t p = strat_allocate_at(reinterpret_cast<uintptr_t>(where), bytes, alignment);

    // post: update stats
    stats_.requested_bytes += bytes;
    if (stats_.requested_bytes > stats_.peak_requested_bytes) {
      stats_.peak_requested_bytes = stats_.requested_bytes;
    }
    stats_.count_alloc_at++;

    return reinterpret_cast<void*>(p);
  }

  void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) final {
    // pre: TODO: verify region was actually allocated?

    if (p != nullptr)
      strat_deallocate(reinterpret_cast<uintptr_t>(p), bytes, alignment);

    // post: update stats
    if (bytes > stats_.requested_bytes) stats_.requested_bytes = 0;  // this branch shouldn't be necessary
    else stats_.requested_bytes -= bytes;
    stats_.count_dealloc++;
  }

  bool do_is_equal(const std::pmr::memory_resource& other) const noexcept final {
    return this == &other;
  }

  mem_stats stats_{};
  mem_config config_{};
};
