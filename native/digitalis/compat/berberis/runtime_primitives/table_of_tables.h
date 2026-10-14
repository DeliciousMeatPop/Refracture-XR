// Windows version of Digitalis' TableOfTables (the translation cache's guest pc -> host code map).
//
// Same layout and interface as upstream (generated code indexes main_table_[pc >> 24][pc & 0xffffff]
// directly, see EmitIndirectDispatch), different memory: upstream maps one memfd filled with the
// default value as private copy-on-write views. Here the default child table is one pagefile
// section filled with the default value, and each child table is a FILE_MAP_COPY view of it:
// untouched pages stay shared, written pages become private. The main table starts as views of a
// section filled with the default child table's address.
#ifndef BERBERIS_RUNTIME_PRIMITIVES_TABLE_OF_TABLES_H_
#define BERBERIS_RUNTIME_PRIMITIVES_TABLE_OF_TABLES_H_

#include <atomic>
#include <cstdint>
#include <mutex>

#include "berberis/base/logging.h"

namespace berberis {

// src/platform_win.cpp. Returns a section handle (as void*) of `size` bytes, every `value_size`
// bytes holding `value`.
void* CreateFilledSectionOrDie(size_t size, uint64_t value, size_t value_size);
// A view of the whole section: copy-on-write, or read-only.
void* MapSectionOrDie(void* section, size_t size, bool copy_on_write);
void UnmapSectionView(void* view);
void CloseSection(void* section);

template <typename Key, typename T>
class TableOfTables {
 public:
  explicit TableOfTables(T default_value) : default_value_(default_value) {
    static_assert(sizeof(T) == sizeof(uintptr_t) || sizeof(T) == sizeof(uint32_t));
    static_assert(sizeof(Key) == 8);
    CHECK_NE(default_value, T{0});
    child_section_ = CreateFilledSectionOrDie(kChildTableBytes, uint64_t{default_value}, sizeof(T));
    default_table_ = static_cast<std::atomic<T>*>(
        MapSectionOrDie(child_section_, kChildTableBytes, /* copy_on_write */ false));
    main_section_ = CreateFilledSectionOrDie(
        kTableSize * sizeof(T*), reinterpret_cast<uint64_t>(default_table_), sizeof(T*));
    main_table_ = static_cast<std::atomic<std::atomic<T>*>*>(
        MapSectionOrDie(main_section_, kTableSize * sizeof(T*), /* copy_on_write */ true));
  }

  ~TableOfTables() {
    for (size_t i = 0; i < kTableSize; ++i) {
      if (main_table_[i] != default_table_) {
        UnmapSectionView(main_table_[i]);
      }
    }
    UnmapSectionView(main_table_);
    UnmapSectionView(default_table_);
    CloseSection(main_section_);
    CloseSection(child_section_);
  }

  /*may_discard*/ std::atomic<T>* Put(Key key, T value) {
    SplitKey split_key(key);
    AllocateIfNecessary(split_key.high);
    main_table_[split_key.high][split_key.low] = value;
    return &main_table_[split_key.high][split_key.low];
  }

  [[nodiscard]] T Get(Key key) const {
    SplitKey split_key(key);
    return main_table_[split_key.high][split_key.low];
  }

  [[nodiscard]] std::atomic<T>* GetPointer(Key key) {
    SplitKey split_key(key);
    AllocateIfNecessary(split_key.high);
    return &main_table_[split_key.high][split_key.low];
  }

  [[nodiscard]] const std::atomic<std::atomic<T>*>* main_table() const { return main_table_; }

  void CloseDefaultMemfdUnsafe() {}

 private:
  struct SplitKey {
    // Top bits (TBI/PAC tags) are masked off, as upstream does.
    explicit SplitKey(Key key)
        : low(key & (kTableSize - 1)), high((key >> kTableBits) & (kTableSize - 1)) {}
    const uint32_t low;
    const uint32_t high;
  };

  void __attribute__((noinline)) AllocateIfNecessary(uint32_t high_word) {
    if (main_table_[high_word] != default_table_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (main_table_[high_word] == default_table_) {
      auto* tmp = static_cast<std::atomic<T>*>(
          MapSectionOrDie(child_section_, kChildTableBytes, /* copy_on_write */ true));
      std::atomic_thread_fence(std::memory_order_release);
      main_table_[high_word] = tmp;
    }
  }

  static constexpr size_t kTableBits = 24;
  static constexpr size_t kTableSize = 1 << kTableBits;
  static constexpr size_t kChildTableBytes = kTableSize * sizeof(T);
  std::mutex mutex_;
  std::atomic<std::atomic<T>*>* main_table_;
  std::atomic<T>* default_table_;
  void* child_section_;
  void* main_section_;
  T default_value_;
};

}  // namespace berberis

#endif  // BERBERIS_RUNTIME_PRIMITIVES_TABLE_OF_TABLES_H_
