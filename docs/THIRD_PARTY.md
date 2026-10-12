# Third-party code and provenance

GoW3R Windows Port is licensed under GPL-2.0-or-later and contains work derived
from GPL-licensed projects.

- The initial architecture was derived from `bloodborne_pc_windows_port`.
- The graphics loader, shader recompiler, and PS4 libraries are derived from shadPS4.
- The vendored core snapshot and its backports are documented in
  [`gpu/VENDOR.txt`](../gpu/VENDOR.txt).

References to the base project in internal names such as `bbport` may remain when
they are part of the ABI, change history, or source attribution. They do not
indicate Bloodborne support in this repository.

The submodules use their own licenses:

- LibAtrac9 — MIT;
- Dear ImGui — MIT;
- FSR-Vulkan — MIT;
- magic_enum — MIT;
- miniz — MIT;
- Xbyak — BSD-3-Clause.

Distributions must preserve `LICENSE`, this document, `gpu/VENDOR.txt`, and the
license notices of all included submodules.
