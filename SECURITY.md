# Security policy

## Supported versions

RASM is at an early stage and has no long-term support branches. Security fixes go to `main` and into the next release.

| Version | Supported |
|---|---|
| `main` | Yes |
| The latest release | Yes |
| Older releases | No. Please update to the latest release |

## Reporting a vulnerability

**Please do not report security problems in public issues, discussions or pull requests.**

Report them privately through GitHub:

1. Open the repository's [**Security** tab](https://github.com/adel-elmala/rasm/security).
2. Click **Report a vulnerability**.
3. Describe the problem, the version or commit, and how to reproduce it. A sample file that triggers it is the most useful thing you can attach.

Only the maintainer can see the report. You can discuss it there, and the fix can be prepared in a private fork before it is published.

## What to expect

RASM has one maintainer, so these are goals, not guarantees:

- A first answer within 7 days.
- An assessment (confirmed or not, and how serious) within 30 days.
- A fix, or a plan for one, as soon as possible after that. A fix for a confirmed issue is published together with a GitHub security advisory, and you are credited unless you ask not to be.

If you do not hear back within 14 days, please ask in a [discussion](https://github.com/adel-elmala/rasm/discussions) that the maintainer check the Security tab, without giving details.

## Scope

RASM is a library that applications link into their own process, so a bug that lets a crafted input crash it, read or write out of bounds, or run code is in scope. The parts most exposed to untrusted input:

| Area | Where | Examples |
|---|---|---|
| Asset loaders | `src/assets/` | glTF and GLB (tinygltf), OBJ (tinyobjloader), KTX2 and Basis (libktx), Radiance HDR and other images (stb_image), LAS and LAZ point clouds (LASzip), fonts (stb_truetype). A file from an untrusted source must not be able to do more than fail to load |
| The C API | `include/rasm/rasm_c.h`, `src/c/` | Invalid handles, sizes, counts or pointers passed through the stable ABI by bindings in other languages |
| Shader loading | `EngineConfig::shaderDir`, `EngineConfig::cacheDir` | Shader sources and the on-disk shader and pipeline caches are read at run time |
| Paths | `EngineConfig::assetRoot` | Asset paths, including paths inside glTF files that point to other files |

Out of scope:

- Bugs in a dependency that are not caused by how RASM uses it. Please report those to the dependency's project. If RASM ships or pins a vulnerable version, that is in scope.
- GPU driver bugs, and hangs or crashes caused by shaders an application writes itself.
- Denial of service by asking for huge resources through the API (for example a very large texture), unless a small input causes it.
- The example programs and the tools in `tools/`, unless the problem is in the library code they call.

Some known limits are already documented, for example `B34` in [`docs/code_review.md`](docs/code_review.md): font files are trusted. A report that shows a real attack on such a known limit is still welcome.
