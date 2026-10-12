# Third-party code and provenance

GoW3R Windows Port is licensed under GPL-2.0-or-later and contains work derived
from GPL-licensed projects.

- The initial architecture was derived from `bloodborne_pc_windows_port`.
- The graphics loader, shader recompiler, and PS4 libraries are derived from shadPS4.
- The stereo AudioOut matrix and stereo-device detection are adapted from the
  GPL-2.0-or-later cuesta4/shadPS4 `eltutz` snapshot, commit
  `eb325fbd64c1696389b60813df125f517736573f`, file
  `src/core/libraries/audio/sdl_audio_out.cpp` (Copyright 2024-2026 shadPS4
  Emulator Project). Front channels use 1.0 gain;
  center and surround channels use 0.7071; LFE is omitted. This port preserves
  guest per-channel volumes rather than the fork's maximum-channel stream gain.
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
