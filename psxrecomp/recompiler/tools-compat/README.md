# Recompiler tools: configuration loader from the pinned framework

This project's `psxrecomp/` is a local compatibility layer over the framework
(`framework_pins.txt`): the runtime is built against an older configuration
loader than the one the rest of `recompiler/` was written for, so
`psxrecomp-game` and `psxrecomp-bios` do not compile from this tree as it is.

The files here are the unmodified ones from the pinned framework revision
(`psxrecomp-upstream`, ffbd9d1c). `scripts/build_recompiler_tools.sh` builds
the tools from a copy of this tree with these files laid over it, at the same
relative paths:

    recompiler/src/config_loader.h
    recompiler/src/config_loader.cpp
    runtime/include/internal_resolution.h
    runtime/include/fast_forward.h

None of the four is in `runtime/codegen_hash_sources.cmake`, so the tools
carry the same code-generation hash as the game built from this tree, and the
per-game configuration hash both sides compute is the same (checked for
Master of Arena). That is what lets the game load the code these tools
compile; the build script verifies the first of the two on every run.

The game itself never uses these files.
