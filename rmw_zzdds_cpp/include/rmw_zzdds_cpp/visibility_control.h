#ifndef RMW_ZZDDS_CPP__VISIBILITY_CONTROL_H_
#define RMW_ZZDDS_CPP__VISIBILITY_CONTROL_H_

#if defined _WIN32 || defined __CYGWIN__
  #ifdef __GNUC__
    #define RMW_ZZDDS_CPP_EXPORT __attribute__((dllexport))
    #define RMW_ZZDDS_CPP_IMPORT __attribute__((dllimport))
  #else
    #define RMW_ZZDDS_CPP_EXPORT __declspec(dllexport)
    #define RMW_ZZDDS_CPP_IMPORT __declspec(dllimport)
  #endif
  #ifdef RMW_ZZDDS_CPP_BUILDING_LIBRARY
    #define RMW_ZZDDS_CPP_PUBLIC RMW_ZZDDS_CPP_EXPORT
  #else
    #define RMW_ZZDDS_CPP_PUBLIC RMW_ZZDDS_CPP_IMPORT
  #endif
#else
  #define RMW_ZZDDS_CPP_EXPORT __attribute__((visibility("default")))
  #define RMW_ZZDDS_CPP_IMPORT
  #if __GNUC__ >= 4
    #define RMW_ZZDDS_CPP_PUBLIC __attribute__((visibility("default")))
  #else
    #define RMW_ZZDDS_CPP_PUBLIC
  #endif
#endif

#endif  // RMW_ZZDDS_CPP__VISIBILITY_CONTROL_H_

