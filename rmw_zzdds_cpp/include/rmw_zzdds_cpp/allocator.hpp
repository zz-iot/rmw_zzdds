#ifndef RMW_ZZDDS_CPP__ALLOCATOR_HPP_
#define RMW_ZZDDS_CPP__ALLOCATOR_HPP_

#include <new>
#include <utility>

#include "rcutils/allocator.h"

namespace rmw_zzdds_cpp
{

template<typename T, typename ... Args>
T * allocate_object(const rcutils_allocator_t & allocator, Args && ... args)
{
  void * storage = allocator.allocate(sizeof(T), allocator.state);
  if (storage == nullptr) {
    return nullptr;
  }
  try {
    return new (storage) T(std::forward<Args>(args)...);
  } catch (...) {
    allocator.deallocate(storage, allocator.state);
    return nullptr;
  }
}

template<typename T>
void deallocate_object(const rcutils_allocator_t & allocator, T * object) noexcept
{
  if (object == nullptr) {
    return;
  }
  object->~T();
  allocator.deallocate(object, allocator.state);
}

inline char * duplicate_string(const rcutils_allocator_t & allocator, const char * value)
{
  size_t size = 1U;
  while (value[size - 1U] != '\0') {
    ++size;
  }
  auto * copy = static_cast<char *>(allocator.allocate(size, allocator.state));
  if (copy != nullptr) {
    for (size_t i = 0U; i < size; ++i) {
      copy[i] = value[i];
    }
  }
  return copy;
}

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__ALLOCATOR_HPP_
