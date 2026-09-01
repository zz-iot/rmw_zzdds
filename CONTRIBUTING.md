# Contributing

This implementation is intentionally developed in narrow vertical slices.
Code must not advertise an RMW feature until its success and failure paths are
tested.  Optional functionality should return `RMW_RET_UNSUPPORTED` while it is
absent.

Changes must:

- build without warnings;
- preserve allocator and ownership contracts;
- add tests at the lowest layer capable of reproducing the behavior;
- avoid private zzdds implementation APIs and hard-coded build paths;
- identify whether a failure belongs to generated type support, the zzdds C
  binding, the zzdds core, or the RMW adapter.

