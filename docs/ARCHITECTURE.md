# Architecture

## Layout

| Path | Purpose |
| --- | --- |
| `main.c` | module init/exit, ioctl dispatcher, fd installation, reboot kprobe |
| `compat.c` / `compat.h` | central runtime symbol table + accessor macros |
| `bs_module.h` | module version and stage enum |
| `mem.c`, `process_mem.c` | process memory helpers |
| `file_io.c` | file read/write helpers |
| `touch.c`, `touch_v2.c` | touch input handlers |
| `kgsl.c` | KGSL helpers |
| `bs_hwb.c` | hardware breakpoint helpers |
| `udp_capture.c` | UDP capture helpers |
| `sf_groups.c` | credential/group helpers |
| `syscall.c` | optional syscall-table hook |
| `BSLoaderDriver/` | loader template and single-file generator |
| `.github/workflows/build-kernel.yml` | KMI build matrix, SDK build, release packaging |

## Module lifecycle

`mod_init()`:

1. `bs_compat_init()` resolves all runtime symbols through
   `kallsyms_lookup_name` (obtained with a temporary kprobe).
2. `bs_stage` selects how much is brought up:
   - `1`: compatibility layer only
   - `2`: + memory core
   - `3`: + file I/O
   - `4`: + syscall hook only
   - `5`: + reboot kprobe only
   - `6` (default): full set, but the syscall hook is skipped
   - `0`: full set, syscall hook + reboot kprobe
3. Any failure after the memory/file setup is handled by one cleanup path:
   `err_hooks` -> `err_file` -> `err_mem`.
4. On success the fd-install entry is registered and the module kobject is
   removed from sysfs.

`mod_exit()` mirrors the stage and unwinds only what was initialized.

## Compatibility layer

`compat.c` contains a table of `{ name, slot }` entries. Version-dependent
symbols keep their `LINUX_VERSION_CODE` guard next to the table entry, so the
resolver loop stays version-agnostic. Missing required symbols are reported by
name and cause `bs_compat_init()` to return `-ENOENT`.
