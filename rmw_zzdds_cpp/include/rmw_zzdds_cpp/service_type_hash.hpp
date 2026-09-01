#ifndef RMW_ZZDDS_CPP__SERVICE_TYPE_HASH_HPP_
#define RMW_ZZDDS_CPP__SERVICE_TYPE_HASH_HPP_

#include "rosidl_runtime_c/type_hash.h"

namespace rmw_zzdds_cpp
{

const rosidl_type_hash_t * service_type_hash_override() noexcept;
void set_service_type_hash_override(const rosidl_type_hash_t * type_hash) noexcept;

}  // namespace rmw_zzdds_cpp

#endif  // RMW_ZZDDS_CPP__SERVICE_TYPE_HASH_HPP_
