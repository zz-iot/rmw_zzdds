#ifndef RMW_ZZDDS_CPP__SERVICE_TYPE_HASH_HPP_
#define RMW_ZZDDS_CPP__SERVICE_TYPE_HASH_HPP_

#include <cstddef>
#include <cstdint>

#include "rosidl_runtime_c/type_hash.h"

namespace rmw_zzdds_cpp
{
constexpr size_t service_guid_size = 16;
constexpr size_t service_request_header_size = service_guid_size + sizeof(int64_t);

const rosidl_type_hash_t * service_type_hash_override() noexcept;
void set_service_type_hash_override(const rosidl_type_hash_t * type_hash) noexcept;

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__SERVICE_TYPE_HASH_HPP_
