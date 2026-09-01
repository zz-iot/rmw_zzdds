cmake_minimum_required(VERSION 3.20)
project(rosidl_typesupport_zzdds_c VERSION 0.1.0 LANGUAGES CXX)

find_package(ament_cmake REQUIRED)
find_package(ament_cmake_ros_core REQUIRED)
find_package(rmw REQUIRED)
find_package(rosidl_runtime_c REQUIRED)
find_package(rosidl_typesupport_zzdds_cpp REQUIRED)
find_package(ZZDDS CONFIG REQUIRED)

add_library(${PROJECT_NAME} SHARED src/identifier.cpp src/message_type_support.cpp)
target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_17)
target_compile_options(${PROJECT_NAME} PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
target_include_directories(${PROJECT_NAME} PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
  "$<INSTALL_INTERFACE:include/${PROJECT_NAME}>")
target_link_libraries(${PROJECT_NAME} PUBLIC
  ZZDDS::zzdds rmw::rmw rosidl_runtime_c::rosidl_runtime_c
  rosidl_typesupport_zzdds_cpp::rosidl_typesupport_zzdds_cpp)

ament_export_dependencies(rmw rosidl_runtime_c rosidl_typesupport_zzdds_cpp ZZDDS)
ament_export_targets(${PROJECT_NAME})
ament_index_register_resource("rosidl_typesupport_c")

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  ament_lint_auto_find_test_dependencies()
endif()

if(BUILD_SHARED_LIBS)
  set(${PROJECT_NAME}_LIBRARY_TYPE SHARED)
else()
  set(${PROJECT_NAME}_LIBRARY_TYPE STATIC)
endif()

ament_package(CONFIG_EXTRAS "rosidl_typesupport_zzdds_c-extras.cmake.in")
install(PROGRAMS bin/rosidl_typesupport_zzdds_c DESTINATION lib/${PROJECT_NAME})
install(DIRECTORY cmake resource DESTINATION share/${PROJECT_NAME})
install(DIRECTORY include/ DESTINATION include/${PROJECT_NAME})
install(TARGETS ${PROJECT_NAME} EXPORT ${PROJECT_NAME}
  ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
