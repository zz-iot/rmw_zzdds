cmake_minimum_required(VERSION 3.20)
project(rmw_zzdds_cpp VERSION 0.1.0 LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
find_package(rcutils REQUIRED)
find_package(rmw REQUIRED)
find_package(rmw_dds_common REQUIRED)
find_package(rosidl_typesupport_zzdds_cpp REQUIRED)
find_package(rosidl_typesupport_zzdds_c REQUIRED)
find_package(ZZDDS CONFIG REQUIRED)


get_target_property(_zzdds_library ZZDDS::zzdds IMPORTED_LOCATION)
get_filename_component(_zzdds_library_dir "${_zzdds_library}" DIRECTORY)

add_library(rmw_zzdds_cpp SHARED
  src/identifier.cpp
  src/rmw_guard_condition.cpp
  src/rmw_init.cpp
  src/rmw_node.cpp
  src/rmw_qos.cpp
  src/rmw_data_plane.cpp
  src/rmw_event.cpp
  src/rmw_graph.cpp
  src/graph_discovery.cpp
  src/rmw_serialization.cpp
  src/rmw_service.cpp
  src/rmw_wait.cpp
  src/type_support.cpp
  src/unsupported.cpp
)

target_compile_features(rmw_zzdds_cpp PUBLIC cxx_std_17)
target_compile_options(rmw_zzdds_cpp PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wsign-conversion>
)
target_compile_definitions(rmw_zzdds_cpp PRIVATE RMW_ZZDDS_CPP_BUILDING_LIBRARY)
target_include_directories(rmw_zzdds_cpp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
ament_target_dependencies(
  rmw_zzdds_cpp rcutils rmw rmw_dds_common
  rosidl_typesupport_zzdds_c rosidl_typesupport_zzdds_cpp)
target_link_libraries(
  rmw_zzdds_cpp
  ZZDDS::zzdds
  rmw_dds_common::rmw_dds_common__rosidl_typesupport_zzdds_cpp)

install(
  DIRECTORY include/
  DESTINATION include
)
install(
  TARGETS rmw_zzdds_cpp
  EXPORT export_rmw_zzdds_cpp
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib
  RUNTIME DESTINATION bin
)

ament_export_include_directories(include)
ament_export_targets(export_rmw_zzdds_cpp HAS_LIBRARY_TARGET)
ament_export_dependencies(
  rcutils rmw rmw_dds_common rosidl_typesupport_zzdds_c rosidl_typesupport_zzdds_cpp)

register_rmw_implementation(
  "c:rosidl_typesupport_zzdds_c"
  "cpp:rosidl_typesupport_zzdds_cpp")

if(BUILD_TESTING)
  find_package(ament_cmake_gtest REQUIRED)
  ament_add_gtest(test_lifecycle test/test_lifecycle.cpp)
  if(TARGET test_lifecycle)
    target_compile_features(test_lifecycle PRIVATE cxx_std_17)
    target_link_libraries(test_lifecycle rmw_zzdds_cpp)
    ament_target_dependencies(test_lifecycle rcutils rmw)
    set_tests_properties(test_lifecycle PROPERTIES
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:${_zzdds_library_dir}"
    )
  endif()

  ament_add_gtest(test_zzdds_c_binding test/test_zzdds_c_binding.cpp)
  if(TARGET test_zzdds_c_binding)
    target_compile_features(test_zzdds_c_binding PRIVATE cxx_std_17)
    target_link_libraries(test_zzdds_c_binding ZZDDS::zzdds)
    set_tests_properties(test_zzdds_c_binding PROPERTIES
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:${_zzdds_library_dir}"
    )
  endif()

  ament_add_gtest(test_symbol_coverage test/test_symbol_coverage.cpp)
  if(TARGET test_symbol_coverage)
    target_compile_features(test_symbol_coverage PRIVATE cxx_std_17)
    target_compile_definitions(test_symbol_coverage PRIVATE
      RMW_ZZDDS_LIBRARY_PATH="$<TARGET_FILE:rmw_zzdds_cpp>"
    )
    target_link_libraries(test_symbol_coverage ${CMAKE_DL_LIBS})
    set_tests_properties(test_symbol_coverage PROPERTIES
      ENVIRONMENT_MODIFICATION
        "LD_LIBRARY_PATH=path_list_prepend:${_zzdds_library_dir}"
    )
  endif()
endif()

ament_package()
