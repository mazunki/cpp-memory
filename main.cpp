#include <sys/mman.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <print>

#include "mem.hpp"
#include "mem/buddy.hpp"

static bool in_pool(void* p, std::uintptr_t lo, std::uintptr_t hi) {
  auto x = reinterpret_cast<std::uintptr_t>(p);
  return x >= lo && x < hi;
}

int main() {
  std::size_t sz = 1 << 20; // 1 MiB
  void* p = mmap(nullptr, sz, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) { perror("mmap"); return 1; }

  {
    auto pool_lo = reinterpret_cast<std::uintptr_t>(p);
    auto pool_hi = pool_lo + sz;

    mem_config cfg{
      .region = mem_region{ .start = pool_lo, .end = pool_hi },
      .overbooking = false
    };

    buddy_resource buddy(cfg, {.min_block = 64});

    std::pmr::vector<int> v(&buddy);

    std::size_t last_cap = v.capacity();
    for (int i = 0; i < 100'000; ++i) {
      v.push_back(i);

      if (v.capacity() != last_cap) {
        last_cap = v.capacity();
        std::println("cap={}\tsize={}\tdata={}\tin_pool={}",
          v.capacity(), v.size(), (void*)v.data(),
          in_pool(v.data(), pool_lo, pool_hi) ? 1 : 0);
      }
    }

    std::println("final:\tsize={}\tcap={}\tdata={}\tin_pool={}",
      v.size(), v.capacity(), (void*)v.data(),
      in_pool(v.data(), pool_lo, pool_hi) ? 1 : 0);

    auto show = [&](std::size_t idx) {
      std::println("v[{}]={}\t&v[{}]={}\tin_pool={}",
        idx, v[idx], idx, (void*)&v[idx],
        in_pool(&v[idx], pool_lo, pool_hi) ? 1 : 0);
    };

    show(0);
    show(1);
    show(2);
    show(v.size() / 2);
    show(v.size() - 1);
  }

  munmap(p, sz);
}
