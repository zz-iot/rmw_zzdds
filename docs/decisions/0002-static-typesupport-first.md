# ADR 0002: Implement static type support first

Status: accepted

Stage 1 generates callbacks for compiled ROS C++ message structures and writes
CDR through the zzdds data plane.  DDS DynamicData, remote TypeObject lookup,
and RMW dynamic-message features are outside Stages 1 and 2.

The callback contract reserves a place for future DDS TypeInformation so full
XTypes support can be added without changing endpoint ownership.

