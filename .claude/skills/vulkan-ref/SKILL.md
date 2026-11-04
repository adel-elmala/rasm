---
name: vulkan-ref
description: Offline Vulkan reference from the installed SDK. Look up a validation message's VUID, list every valid-usage rule of a command or struct before writing code, show definitions (struct members, command params, enum values, which version/extension provides them), decode bitmask values seen in logs or captures, and check what this machine's GPU supports (features, extensions, limits, formats). Use when a validation error appears, before writing new Vulkan/RHI code (barriers, sync, descriptors, pipelines), and before relying on a feature or limit.
---

# Vulkan reference

`scripts/vkref.py` reads `$VULKAN_SDK/share/vulkan/registry/validusage.json` + `vk.xml` and
`vulkaninfo --json` (cached in `build/vkref/`). It works offline and matches the installed SDK version.

```sh
K=.claude/skills/vulkan-ref/scripts/vkref.py
```

| Need | Command | Example |
|---|---|---|
| Explain a validation error | `vuid <VUID>` | `vuid VUID-vkCmdPushConstants-offset-01795` |
| Rules **before** writing a call | `rules <vkCmd...\|Vk...Info> [--grep word]` | `rules vkCmdPipelineBarrier2`, `rules VkImageMemoryBarrier2 --grep layout` |
| Find rules about a topic | `search <words...>` (all must appear in one rule) | `search maxDrawCount` |
| Definition + who provides it | `def <name>` | `def VkRenderingAttachmentInfo`, `def vkCmdDrawIndexedIndirectCount`, `def VkImageLayout` |
| Decode a flags value | `flags <VkXFlags[2]> <value>` | `flags VkAccessFlags2 0x18000` → `MEMORY_READ \| MEMORY_WRITE` |
| Does this GPU support it | `device <words...>` (all must appear in the key) | `device scalarBlockLayout`, `device maxPushConstantsSize`, `device mesh_shader`, `device R16G16B16A16_SFLOAT optimal` |

## How to use it in RASM work

- **Validation error from `verify-change`**: run `vuid` on its ID, then `rules` on the struct or command it names to see the related rules. Fix the cause and don't silence the message. `BestPractices-*`, `SYNC-*` and `UNASSIGNED-*` are layer checks, not spec rules: the layer message is the explanation, and `vuid` says so.
- **Before new RHI/Vulkan code** (barriers in phase 3, compute in phase 2, new descriptors): run `rules` for every new command and struct you fill in. That's cheaper than finding the problems through validation one at a time.
- **Before relying on a feature**: `device <feature>` for this GPU, `def <feature struct>` for which core version or extension provides it. RASM requires Vulkan 1.4 and enables its features in `src/rhi/vulkan/vulkanDevice.cpp`. A new feature must be enabled there, and listed in `docs/architecture.md` §3.
- **Reading captures**: GFXR `.jsonl` and `rdc.py state` print raw hex flags. `flags` turns them into names.
- **Portability (capability tiers in `docs/development_plan.md`)**: `device` only knows this machine (NVIDIA RTX 3060). For other vendors, MoltenVK or mobile, check https://vulkan.gpuinfo.org (WebFetch) and note the source.

## Notes

- `vuid` prints the spec link for the full context (`https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#<VUID>`).
- Rules shown `[when VK_EXT_...]` only apply when that extension or version is in use.
- `device --refresh` re-runs vulkaninfo after a driver update. Use `--gpu N` on multi-GPU machines.
- `device` names are the Vulkan member names: one word works best (`meshShader`, `mesh_shader` for the extension).
