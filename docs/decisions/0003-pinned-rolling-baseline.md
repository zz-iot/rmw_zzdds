# ADR 0003: Pin the Rolling development baseline

Status: accepted

Development follows ROS 2 Rolling, but builds and tests use exact revisions
recorded in `ci/rolling.repos`.  Updating that lock is an explicit, tested
change.  A stable ROS distribution becomes a second compatibility target in
Stage 2.

