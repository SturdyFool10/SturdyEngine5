# IDE setup

SturdyEngine5 builds through a single **CMake preset matrix** — one preset per
`<arch>-<os>-<profile>` combination (see `cmake/SturdyMatrix.cmake` for the axis lists). That file
is `CMakePresets.json`, the portable contract every IDE reads. Nothing below is required to build
from the command line; it just makes each editor pleasant.

## Prerequisites (all platforms)

- **CMake ≥ 3.28** and **Ninja** — the project uses C++20/23 module file sets, which need Ninja's
  module dependency scanning. Ninja is the only supported generator.
- **Clang** (C++23 path; the presets pin `clang`/`clang++`). Put them on `PATH`.
- **Vulkan SDK**. Slang is fetched and built from source, which needs **Python 3** on `PATH`.

Configurations are named `<arch>-<os>-<profile>`, e.g. `x86-64-linux-debug`,
`arm64-win-relwithdebinfo`, `riscv-freebsd-dist`. Build trees live in `build/<arch>/<os>/<profile>`.
Regenerate the presets after editing the axis lists:

```sh
cmake -P cmake/GeneratePresets.cmake
```

## CLion

CLion consumes `CMakePresets.json` natively — **open the repository root** and it imports every
configure preset as a read-only CMake profile.

1. **Enable your profiles.** `Settings | Build, Execution, Deployment | CMake` lists all presets.
   Tick the ones for your platform (e.g. `x86-64 Linux Debug`). Enabled profiles are per-developer
   and stored in `workspace.xml`, which is **not** committed (see `.idea/.gitignore`).
2. **Build / run.** The committed run configurations under `.idea/runConfigurations/` (`Debug Linux`,
   `RelWithDebInfo Linux`, `Release Linux`, `Dist Linux`) each bind the CMake **`Runtime` target** to
   a matrix profile — no hardcoded executable paths — so building one builds only `Runtime` (and its
   dependencies) for that profile and runs whatever binary the build system produced, wherever the
   matrix placed it. The run **working directory defaults to the project root**, which is what the
   Runtime expects for loading `Shaders/` and other assets. For platforms other than
   `x86-64 Linux`, CLion **auto-creates** an equivalent `Runtime` configuration for each profile you
   enable — or copy one of the committed configs and change its profile.
3. **Formatting.** CLion auto-detects the project `.clang-format` and formats with it.
4. **Personal presets.** Put machine-specific tweaks in `CMakeUserPresets.json` (gitignored); CLion
   overlays it on top of `CMakePresets.json` automatically.

Notes:
- The shared run configurations bind to a profile by its **display name** (e.g. `x86-64 Linux Dist`).
  CLion **2026.1+** matches profiles by display name; on older CLion the match is the preset `name`
  (`x86-64-linux-dist`) instead, so re-select the profile in the run configuration once.
- If CLion offers to register **nested VCS roots** from third-party checkouts under `build/` or
  `.cache/deps/`, decline — `vcs.xml` should map only the project root. Those directories are
  gitignored FetchContent sources, not part of this repository.
- The presets pin the Ninja generator and Clang. CLion respects both as long as they are on `PATH`
  (on Windows, install Clang and Ninja and select a matching toolchain).
- C++ module **code intelligence** in CLion is still experimental; it does not affect building or
  running, which go through CMake + Ninja as usual.
- The committed `.idea/` files are only the portable ones (`vcs.xml`, `.gitignore`, and the
  `runConfigurations/`). Do not force-add `workspace.xml`, `editor.xml`, or other per-developer files.

## Zed

`.zed/tasks.json` and `.zed/debug.json` drive the matrix through two generic scripts:

- `cmake/IDEBuild.cmake` — configure + build (+ run) a target for a chosen `-DSTURDY_PROFILE` (and
  optional `-DSTURDY_ARCH` / `-DSTURDY_OS`, both defaulting to the host).
- `cmake/ClangdView.cmake` — point clangd's `.clangd` compile database at a chosen OS/profile view.

The debug configs reference a stable `build/host/<profile>/...` path — a symlink `IDEBuild.cmake`
maintains to the host's `build/<arch>/<os>` tree so the committed configs stay portable.

## Visual Studio, VS Code, Qt Creator, Neovim (cmake-tools)

These consume `CMakePresets.json` directly — open the folder and pick a preset. No extra files.

## Editors without preset support (Vim, Helix, Emacs, Atom, …)

Configure any preset once (or run a Zed "View As" task) to produce
`build/<arch>/<os>/<profile>/compile_commands.json` for your language server, and build with:

```sh
cmake -DSTURDY_PROFILE=Debug -P cmake/IDEBuild.cmake      # host arch/os, builds + optionally runs
cmake -DSTURDY_TARGET=Core -DSTURDY_PROFILE=Debug -P cmake/IDEBuild.cmake   # a single layer
```

## Shared dependency cache

`STURDY_SHARED_DEPS_CACHE` (ON) keeps one copy of each third-party source checkout in `.cache/deps/`
instead of re-downloading it into every build tree; per-configuration build artifacts stay isolated.
Turn it OFF for CI matrix jobs that configure the same sources concurrently.
