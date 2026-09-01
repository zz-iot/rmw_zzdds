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

#ifndef ROSIDL_TYPESUPPORT_ZZDDS_CPP__VISIBILITY_CONTROL_H_
#define ROSIDL_TYPESUPPORT_ZZDDS_CPP__VISIBILITY_CONTROL_H_

#if defined _WIN32 || defined __CYGWIN__
# ifdef __GNUC__
#  define ROSIDL_TYPESUPPORT_ZZDDS_CPP_EXPORT __attribute__((dllexport))
#  define ROSIDL_TYPESUPPORT_ZZDDS_CPP_IMPORT __attribute__((dllimport))
# else
#  define ROSIDL_TYPESUPPORT_ZZDDS_CPP_EXPORT __declspec(dllexport)
#  define ROSIDL_TYPESUPPORT_ZZDDS_CPP_IMPORT __declspec(dllimport)
# endif
# ifdef ROSIDL_TYPESUPPORT_ZZDDS_CPP_BUILDING_LIBRARY
#  define ROSIDL_TYPESUPPORT_ZZDDS_CPP_PUBLIC ROSIDL_TYPESUPPORT_ZZDDS_CPP_EXPORT
# else
#  define ROSIDL_TYPESUPPORT_ZZDDS_CPP_PUBLIC ROSIDL_TYPESUPPORT_ZZDDS_CPP_IMPORT
# endif
#else
# define ROSIDL_TYPESUPPORT_ZZDDS_CPP_EXPORT __attribute__((visibility("default")))
# define ROSIDL_TYPESUPPORT_ZZDDS_CPP_IMPORT
# define ROSIDL_TYPESUPPORT_ZZDDS_CPP_PUBLIC __attribute__((visibility("default")))
#endif

#endif  // ROSIDL_TYPESUPPORT_ZZDDS_CPP__VISIBILITY_CONTROL_H_
