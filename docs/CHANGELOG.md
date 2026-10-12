# Changelog

All notable changes to the GoW3R Windows Port will be documented here.

## Unreleased

### Added

- `GOW3IMG1` loader, relocation support, per-module TLS, and canonical import resolution;
- Win64 runtime for the PS4 services used by CUSA01623 01.02;
- Liverpool/Vulkan renderer and video/audio decoding;
- Windows launcher with validation, caching, logging, and optional GoW3R patches;
- `analysis.json`, `imports.json`, `link.json`, and `coverage.json` reports;
- allowlist-based packaging without game content.

### Fixed

- gameplay performance under the GoW3R profile;
- image aliases, mip tails, tiling, and texture uploads observed during the opening sequence;
- lighting, distant backgrounds, weapons, and enemies in the tested scenarios;
- visual regressions caused by broad alias synchronization and asynchronous pipelines.

### Pending

- final Gaia texture and vegetation corrections;
- save/reload, 30-minute session, and AMD/NVIDIA validation.
