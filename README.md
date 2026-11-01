# Ananicy Configurator (Qt / C++)

A GUI editor for [Ananicy](https://github.com/Nefelim4ag/Ananicy) and ananicy-cpp configuration.

## Features

- **Types & Rules tab**
  - Table of all types (from every `*.types` file) showing their settings and how many rules use them.
    Double-click a type to show only the rules that use it.
  - Every type setting except the name is a drop-down: `nice`, `latency_nice`, `ioclass`, `ionice`,
    `sched`, `rtprio`, `oom_score_adj`, `cgroup` (cgroups come from your `*.cgroups` files).
  - Table of all rules (every `*.rules` file under the config dir, including all of `00-default/**`),
    with search and a type filter. Assign a type to a process from the types that exist, and
    optionally set per-rule overrides.
  - **Add from running process…** lists the running processes (grouped by `/proc/PID/comm`, with
    executable, command line and any existing rule) and creates a rule from your pick.
- **ananicy.conf tab**: all known keys as drop-downs (`check_freq`, `*_load`, `apply_*`,
  `loglevel`, …). Keys the program doesn't know about are kept and can be edited too.
- **Save** / **Save & Restart service**: writes the changed files, then runs
  `systemctl restart <service>`. The service name is auto-detected (`ananicy` or `ananicy-cpp`)
  and can be changed in the bottom bar.

## Where changes are written

| What | File |
|---|---|
| Changed existing rule | the same file it was loaded from (only that line changes) |
| New rule | `/etc/ananicy.d/00-default/custom.rules` |
| Changed existing type | the same file it was loaded from |
| New type | `/etc/ananicy.d/00-types.types` |
| ananicy.conf settings | `/etc/ananicy.d/ananicy.conf` |

Comments, blank lines, formatting of untouched lines and unknown keys are preserved byte-for-byte.
Only files that actually changed are written. Before each save, the previous version is copied to
`<file>.bak`. Ananicy only loads `*.rules` / `*.types`, so it ignores these backups.

## Root permissions

Run the program as your normal user. On save, it stages the files in a temp dir and runs **one**
`pkexec` command (one password prompt) that installs them and restarts the service. If you start it
as root, it writes directly.

## Build

Dependencies: CMake ≥ 3.16, a C++17 compiler, Qt 6 (or Qt 5.15) Widgets.

```bash
# Arch:          sudo pacman -S cmake qt6-base
# Debian/Ubuntu: sudo apt install cmake g++ qt6-base-dev
# Fedora:        sudo dnf install cmake gcc-c++ qt6-qtbase-devel

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/ananicy-gui

# optional: install to /usr/local (binary + .desktop entry)
sudo cmake --install build
```

To try it on a copy of your config without touching `/etc`:

```bash
cp -r /etc/ananicy.d /tmp/ananicy-test
./build/ananicy-gui --root /tmp/ananicy-test
```

## Arch Linux package

The `PKGBUILD` in the project root builds the package from this checkout. Nothing is downloaded,
and your local changes are included:

```bash
cd AnanicyConfigurator
makepkg -si
```

The build files go to `build/`, and the finished package (`ananicy-configurator-*.pkg.tar.zst`)
is written to the project root. The package installs `/usr/bin/ananicy-gui` and a desktop entry
("Ananicy Configurator").

To rebuild after changing or pulling the code, run `makepkg -sif`. To uninstall, run
`sudo pacman -R ananicy-configurator`.

## Notes

- Ananicy matches rules against the kernel process name (`/proc/PID/comm`), which the kernel cuts
  off at 15 characters. The editor warns you when a name is longer than that.
- If the same process name has rules in more than one file, Ananicy uses the last one it loads.
  The editor's "already exists" warnings point to the existing file.
- Rules or types that point to a missing type, or that have the same name more than once, are
  shown in red.
