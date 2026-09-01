// Copyright 2026 Zenzen IoT
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "rosidl_typesupport_zzdds_c/message_type_support.hpp"

#include <cstring>

#include "rosidl_typesupport_zzdds_c/identifier.hpp"

namespace rosidl_typesupport_zzdds_c
{
const rosidl_message_type_support_t * get_message_typesupport_handle_function(
  const rosidl_message_type_support_t * handle, const char * identifier)
{
  if (handle != nullptr && identifier != nullptr &&
    std::strcmp(identifier, typesupport_identifier) == 0)
  {
    return handle;
  }
  return nullptr;
}
const rosidl_service_type_support_t * get_service_typesupport_handle_function(
  const rosidl_service_type_support_t * handle, const char * identifier)
{
  return handle != nullptr && identifier != nullptr &&
         std::strcmp(identifier, typesupport_identifier) == 0 ? handle : nullptr;
}
}  // namespace rosidl_typesupport_zzdds_c
