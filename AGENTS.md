# AGENTS.md - Nind Indexing Module

## Developer Commands

### Build
Requires `NIND_DIST` environment variable to be set.
- Default build: `NIND_DIST=/path/to/dist ./gbuild.sh`
- Address Sanitizer: `./gbuild.sh -a ON`
- Native architecture optimizations: `./gbuild.sh -n native`
- Linear build (disable parallel): `./gbuild.sh -p false`
- Change build mode: `./gbuild.sh -m Release` or `./gbuild.sh -m RelWithDebInfo`
- Change CMake generator: `./gbuild.sh -G Unix` (default is `Ninja`)

### Verification
- Build and test (via `gbuild.sh` if generator supports it): `./gbuild.sh`
- Manual test execution: `ctest` within the build directory (`build/<branch>/<mode>/nind`)

## Architecture & Conventions

- **Language**: C++ (targets C++14, falls back to C++11/0x).
- **Build System**: CMake.
- **Project Structure**:
  - `src/`: Core implementation.
  - `tst/`: Test suites (mostly in `tst/cpp/`).
- **Debug Mode**: The `-DDEBUG_NIND` flag is automatically added in `Debug` and `RelWithDebInfo` modes to enable debug output.
- **Binary Format**: Nind uses an EBNF grammar to specify binary file formats. These are verified by Python programs (not in this repo's main source but referenced in README).

## Toolchain Quirks

- **NIND_DIST**: This variable is mandatory for `gbuild.sh` to define the installation prefix.
- **Build Artifacts**: Builds are isolated by branch and mode in `build/<branch>/<mode>/nind`.
- **Linker Flags**: Uses `-Wl,-z,defs,--no-as-needed` to support a plugin mechanism where symbols must remain visible even if not explicitly used.
