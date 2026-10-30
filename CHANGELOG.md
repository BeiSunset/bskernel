# Changelog

All notable changes to this repository are documented here.
The release workflow reads `version.txt`; keep it in sync with this file and
with `BSLoaderDriver.template.sh` (`VERSION` / `CURRENT_VERSION`).

## Unreleased

- Removed the unused docs/ARCHITECTURE.md; architecture notes live with the source.

- Loader version check now reads `version.txt` from the repository
  (GitHub Raw with jsDelivr fallback) instead of the QQ share page.

## v1.0.0

### Added

- `README` for the `BSLoaderDriver` source and generator.
- `BSLoaderDriver/` loader template plus `build_single.py`; the package job
  generates `BSLoaderDriver.sh` and attaches it to every release.
- `version.txt` and this changelog.
- CI checks for module ELF metadata and loader syntax/lint.
- `bs_module.h` with the module version and stage enum.

### Changed

- `compat.c` now uses a single central symbol table; adding a runtime symbol
  is one table entry plus its declaration/macro.
- `main.c` uses the stage enum instead of magic numbers and funnels init
  failures through a single cleanup path.
- Loader creates its temporary directory with `mktemp -d` when available,
  sets `umask 077`, and compares versions down to the patch component.
- Release tags now use `v<version>-<run>-<attempt>`.

### Fixed

- 5.10 builds with the compatibility layer enabled (`close_fd` is guarded by
  the 5.11 version check).
