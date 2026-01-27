#pragma once
#include <memory_resource>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <print>
#include <stdexcept>
#include <string_view>
#include <vector>
#include <algorithm>

#include "mem.hpp"

struct buddy_config {
  std::size_t min_block = 64; // must be power of two
};


class buddy_resource final : public mem_resource {
public:
  buddy_resource(mem_config cfg, buddy_config bcfg = {})
    : mem_resource(cfg), cfg_(cfg), bcfg_(bcfg)
  {
    if (!std::has_single_bit(bcfg_.min_block)) {
      throw std::invalid_argument("buddy min_block must be a power of two");
    }

    pool_base_ = align_up(cfg_.region.start, bcfg_.min_block);
    if (pool_base_ >= cfg_.region.end) {
      throw std::bad_alloc();
    }

    const std::size_t avail = static_cast<std::size_t>(cfg_.region.end - pool_base_);
    pool_size_ = std::bit_floor(avail);
    if (pool_size_ < bcfg_.min_block) {
      throw std::bad_alloc();
    }

    pool_end_ = pool_base_ + pool_size_;

    min_order_ = order_for(0);
    max_order_ = order_for(pool_size_);

    free_.assign((max_order_ - min_order_) + 1, nullptr);

    // initial free block = whole pool
    push_free(pool_base_, max_order_);
  }

  void dump_state() const noexcept {
    std::println("buddy:");
    std::println("  cfg.region:   [{:#x}, {:#x}) ({} bytes)",
                 cfg_.region.start, cfg_.region.end,
                 static_cast<std::size_t>(cfg_.region.end - cfg_.region.start));
    std::println("  overbooking:  {}", cfg_.overbooking);

    std::println("  min_block:    {}", bcfg_.min_block);
    std::println("  pool_base:    {:#x}", pool_base_);
    std::println("  pool_end:     {:#x}", pool_end_);
    std::println("  pool_size:    {} bytes (2^{})", pool_size_, max_order_);
    std::println("  min_order:    {}", min_order_);
    std::println("  max_order:    {}", max_order_);
  }

  uintptr_t malloc(size_t bytes) {
    return reinterpret_cast<uintptr_t>(this->strat_allocate(bytes, alignof(max_align_t)));
  }

  void free(uintptr_t addr, size_t bytes) {
    this->strat_deallocate(addr, bytes, alignof(max_align_t));
  }

protected:
  std::string_view name() const noexcept override { return "buddy"; }

  std::uintptr_t strat_allocate(std::size_t bytes, std::size_t alignment) override {
    // mem_resource already validated alignment is power-of-two, this is a requirement

    // buddy needs a block that is at least the size of the alignment
    const std::size_t aligned_bytes = std::max(bytes, alignment);  // should we enforce this from outside?

    const int want = order_for(aligned_bytes);
    if (want > max_order_) {
      throw std::bad_alloc();  // our tree is not big enough
    }

    // find smallest available order >= want
    int have = want;
    while (have <= max_order_ && free_[idx(have)] == nullptr) {
      ++have;
    }

    if (have > max_order_) {
      throw std::bad_alloc();  // couldn't find a free node of this size
    }

    // pop a block of order 'have' and split down to 'want'
    std::uintptr_t addr = pop_free(have);

    while (have > want) {
      --have;
      // split into [addr, addr+2^have) and [addr+2^have, addr+2^(have+1))
      const std::uintptr_t right = addr + (std::uintptr_t(1) << have);
      push_free(right, have);

      // keep left half as addr
    }

    return addr;
  }

  void strat_deallocate(std::uintptr_t addr, std::size_t bytes, std::size_t alignment) noexcept override {
    // this assumes addr is both valid and allocated

    if (addr < pool_base_ || addr >= pool_end_) {
      // if caller frees something outside the pool, ignore (we avoid throwing for performance)
      return;
    }

    const std::size_t need = std::max(bytes, alignment);
    int order = order_for(need);
    if (order > max_order_) {
      // the caller tried to deallocate something bigger than us... this is bad!
      return;
    }

    // merge while buddy is free at same order
    while (order < max_order_) {
      const std::uintptr_t bud = buddy_of(addr, order);

      // buddies must also be within pool; if not, stop. (TODO: can this happen?)
      if (bud < pool_base_ || bud >= pool_end_) {
        break;
      }

      // if buddy block is currently free at this order, remove it 
      if (!remove_if_free(bud, order)) {
        // sadly, the bud wasn't free. we have introduced fragmentation (TODO: add stats for this?)
        break;
      }

      // and merge!
      addr = std::min(addr, bud);
      ++order;
    }

    push_free(addr, order);
  }

  std::uintptr_t strat_allocate_at(std::uintptr_t addr, std::size_t bytes, std::size_t alignment) override {
    // mem_resource already validated:
    // - addr is aligned to 'alignment'
    // - addr is within [region.start, region.end)
    // - [addr, addr+bytes) fits in region
    //
    // buddy adds: we can only place at the beginning of a buddy block,
    // and that exact block must be currently free
    //
    // TODO: permit overriding

    if (addr < pool_base_ || addr >= pool_end_) {
      throw std::bad_alloc();  // we have no control over the address requested!
    }

    const std::size_t need = std::max(bytes, alignment);
    const int order = order_for(need);
    if (order > max_order_) {
      throw std::bad_alloc();  // the requested size is too big to fit in our pool
    }

    const std::uintptr_t block_size = (std::uintptr_t(1) << order);
    if ( ((addr - pool_base_) & (block_size - 1)) != 0 ) {
      throw std::bad_alloc();  // address isn't aligned to the requested size
    }

    if (!remove_if_free(addr, order)) {
      throw std::bad_alloc();  // unable to take ownership of the node at this position
    }
    return addr;
  }

private:
  struct FreeNode { FreeNode* next; }; // named as such because... they're free to be used

  mem_config cfg_;
  buddy_config bcfg_;

  std::uintptr_t pool_base_{0};
  std::uintptr_t pool_end_{0};
  std::size_t    pool_size_{0};

  int min_order_{0};
  int max_order_{0};

  std::vector<FreeNode*> free_;

  static std::uintptr_t align_up(std::uintptr_t p, std::size_t alignment) noexcept {
    const std::uintptr_t a = static_cast<std::uintptr_t>(alignment);
    return (p + (a - 1)) & ~(a - 1);
  }

  /**
   * gets the corresponding order k big enough to hold the requested size
   *
   * since buddy is a binary tree, we get:
   *   order k => individual blocks of 2^k bytes
   *
   *  bytes should be aligned
   */
  int order_for(std::size_t bytes) const noexcept {
    std::size_t n = std::max(bytes, bcfg_.min_block);
    if (!std::has_single_bit(n)) {
      n = std::bit_ceil(n);
    }
    return static_cast<int>(std::countr_zero(n));
  }

  std::uintptr_t buddy_of(std::uintptr_t addr, int order) const noexcept {
    const std::uintptr_t off = addr - pool_base_;
    return pool_base_ + (off ^ (std::uintptr_t(1) << order));
  }

  std::size_t idx(int order) const noexcept {
    return static_cast<std::size_t>(order - min_order_);
  }

  void push_free(std::uintptr_t p, int order) noexcept {
    auto* n = reinterpret_cast<FreeNode*>(p);
    n->next = free_[idx(order)];
    free_[idx(order)] = n;
  }

  std::uintptr_t pop_free(int order) noexcept {
    FreeNode* n = free_[idx(order)];
    free_[idx(order)] = n->next;
    return reinterpret_cast<std::uintptr_t>(n);
  }

  /* 
   * removes a node at the specified addr, for the specific order
   *
   * returns true if it was removed
   * returns false if the node was busy
   * */
  bool remove_if_free(std::uintptr_t addr, int order) noexcept {
    auto* target = reinterpret_cast<FreeNode*>(addr);

    FreeNode** cur = &free_[idx(order)];
    while (*cur) {
      if (*cur == target) {
        *cur = (*cur)->next;
        return true;
      }
      cur = &((*cur)->next);
    }
    return false;
  }
};
