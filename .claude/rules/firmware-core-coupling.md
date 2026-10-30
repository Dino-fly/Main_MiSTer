---
paths:
  - "user_io.h"
  - "user_io.cpp"
  - "snacpad.cpp"
  - "snacpad.h"
  - "**/sys/**"
---
# Firmware and cores are coupled

`UIO_*` numbers in `user_io.h` must match the decoder in each core's
`sys/sys_top.v`, which lives in the core's repository. A feature that reads new
data from the fabric needs the shared `sys/` change **and every core rebuilt
against it**. Degrade gracefully on old cores: `snacpad.cpp` uses a magic byte
in the reply so an old core reads as "not supported", never as garbage.
