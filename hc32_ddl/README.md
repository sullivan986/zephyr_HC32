# Vendor device driver library (DDL) shipped by XiaoHua Semiconductor.

Everything below this directory is unmodified vendor source, taken from
`HC32A052_DLL_Rev1.0.1` (see `ChangeLog.md`).

- Origin: XHSC HC32A052 device driver library, Rev 1.0.1
- License: BSD-3-Clause, see `LICENSE` in this directory. Each source file also
  carries the SPDX-style license header from the vendor.
- Local additions inside this directory, MIT licensed like the rest of this
  repository:
  - `drivers/inc/ddl_device.h` - selects the device series/package, which the
    vendor DDL expects the application to provide.
  - `CMakeLists.txt` - builds only the driver sources the port currently uses.

Keep vendor files byte-identical where possible so that a future DDL update can
be diffed against upstream.
