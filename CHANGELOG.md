# Changelog

All notable changes to this repository are documented here.
Releases follow the enkernel scheme: the name is a UTC timestamp
(`YYYY-MM-DD HH:MM:SS UTC`) and the tag is
`YYYY-MM-DD-HHMMSS-<run_id>-<attempt>`. `BS_MODULE_VERSION` in `bs_module.h`
and the loader's `VERSION` / `CURRENT_VERSION` only describe the build.

## Unreleased

- Loader version check is served by the QQ share page again; the jsDelivr and
  GitHub Raw fallbacks and `version.txt` were removed.
- Removed the unused docs/ARCHITECTURE.md; architecture notes live with the source.
- Module logs are quiet by default; `bs_log_verbose=1` brings the trace back.
- rmmod panic fixed: the module no longer deletes its own sysfs kobject.
- Memory reads use the cached linear alias for MT_NORMAL pages (2-4x on 4 MiB).

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
