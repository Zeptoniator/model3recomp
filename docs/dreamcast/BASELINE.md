# Baseline (desktop), before the Dreamcast port

Taken on 2026-10-05 from `origin/main` at `0a3d75b`, in a separate worktree
on branch `feature/dreamcast-port`.

| | |
|---|---|
| Host | Ubuntu, x86-64 |
| Compiler | GCC 15.2.0 (`gcc (Ubuntu 15.2.0-16ubuntu1)`) |
| CMake | 4.2.3 |
| SDL2 | 2.32.10 (found: `platform_sdl.c` is built) |
| Configure | `cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Release` |

## Result as found: **broken, pre-existing**

```
src/real3d.c:491:36: warning: 'struct tri_s' declared inside parameter list
src/real3d.c:515:16: error: conflicting types for 'sample'
```

`sample()` was forward-declared with `const struct tri_s *` before
`struct tri_s` existed, so the parameter named a different, file-local type.
MSVC (the repository's development compiler) accepts it; GCC 15 does not.
Nothing in the repository was changed to cause it.

Fix (commit `fe32f1e`): `typedef struct tri_s tri_t;` before the prototype,
definition after. No behaviour change.

## Result after the fix

| | |
|---|---|
| Build | clean (warnings only: Musashi's unused variables, two `-Wformat-truncation` in `netplay.c`) |
| Tests | 1 / 1 passed (`ppc_semantics`: "all PPC semantic checks passed") |
| `libmodel3recomp.a` | 1 548 466 bytes |
| `model3recomp_test_ppc` | 821 384 bytes |

After the port's commits the host suite is 2 / 2 (`guest_smoke` added) and is
re-run before every commit.
