# GFXReconstruct (Windows, Linux, macOS — Vulkan)

Ships with the Vulkan SDK (`gfxrecon-replay`, `-convert`, `-info`, and the capture layer).
`scripts/gfxr.py` finds them via `VULKAN_SDK`, else `PATH`. No engine change is needed:
capture is a Vulkan layer turned on through environment variables, and the app quits by
itself after the frame.

```sh
G=.claude/skills/frame-debug/scripts/gfxr.py
python $G capture build/x64-debug/examples/02_gltf.exe --frame 60 --out-dir <scratch>
# -> <scratch>/02_gltf_frame_60.gfxr
```

## Commands

| Command | Shows |
|---|---|
| `frame <gfxr>` | The captured frame's command buffers with block indices: barriers, passes (area, load ops), binds, push constants, draws/dispatches (`*`). It writes a `.jsonl` next to the capture (full API calls with every create-info, for grep). |
| `dump <gfxr> <outdir> [--draws i,j] [--before] [--vertex] [--no-descriptors]` | Replays and dumps, for every draw/dispatch (or only the listed indices): parameters (indirect args decoded), color + depth attachments as PNG (`--before` also gives the image before the draw), every bound descriptor image (bindless textures included), and vertex/index buffers with `--vertex`. Prints file paths. Read the PNGs. |
| `screenshot <gfxr> <outdir>` | Replays and saves the presented frame. |

## Limits

- Only **descriptor-bound** resources are dumped. Buffers reached through buffer device addresses (RASM's `SceneData`, vertex pool, draw info, model data) are not. Use RenderDoc for those, or grep the `.jsonl` for the upload (`InitBufferCommand` / `vkCmdCopyBuffer`).
- Depth PNGs are hard to read with reversed-Z, where far depth is close to 0 (most pixels look black). RenderDoc's `targets` stretches the range.
- `--before` zeroes the reported indirect params (GFXR quirk). Run without it for the args.
- The `--dump-resources` text in `gfxrecon-replay --help` describes D3D12. For Vulkan the argument is a JSON config, which `dump` writes for you (`dump_config.json` in the out dir).

## macOS

Same commands. `capture` adds `VK_ADD_LAYER_PATH=$VULKAN_SDK/share/vulkan/explicit_layer.d` off Windows.
This is untested on macOS and Linux so far. If the layer isn't found, check `vulkaninfo --summary` for `VK_LAYER_LUNARG_gfxreconstruct`.
For a Metal-level view, MoltenVK can write an Xcode trace:
`MVK_CONFIG_AUTO_GPU_CAPTURE_SCOPE=2 MVK_CONFIG_AUTO_GPU_CAPTURE_OUTPUT_FILE=~/frame.gputrace <exe>`
(captures the first frame). Only Xcode can open it, so hand the file to the user.
