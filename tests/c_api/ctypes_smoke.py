"""Drive the RASM C API from Python with ctypes, as a language binding does: no C compiler, only the shared library.

It makes a headless renderer, a red quad in front of a camera and a sun, and renders a few frames: one view with
rasm_render, then the last frame two views with rasm_render_views (each with the sun's shadow, 1.18), the whole image and a small one in the top-right
corner. The quad and the cameras stand 6370 km from the origin, in Earth-centred coordinates, placed with the 1.2 calls
in doubles, which give the positions back bit for bit. It copies the last two frames back and checks their pixels: red
in the middle of every view, the background at the top-left corner. Then it builds a frame of its own with the render graph (1.17): the scene passes and the tone map into a
target, and a pass whose callback is a Python function (ctypes CFUNCTYPE) that draws the target into the window with red
and blue swapped, so the quad comes out blue. The callback reads the target's texture and its bindless slot in the
callback, checks that a history target's copies swap each frame, and that a call that would start a frame is refused
while the graph records; a rasm_commands kept past its callback is refused too, and a graph whose renderer was
destroyed fails cleanly. With --slangc, the pass's pipeline is made from SPIR-V that slangc compiles from the same
source, which checks the SPIR-V path of rasm_create_pipeline. The 1.18 debug calls (the debug view, frozen culling,
the meshlet path, the inspection) are set and read back. The structs below mirror include/rasm/rasm_c.h field by field.

    python tests/c_api/ctypes_smoke.py <path to rasm_c.dll / librasm_c.dylib / librasm_c.so>
        [--shader-dir DIR] [--frames N] [--out frame.png] [--validation] [--slangc PATH]

Without --shader-dir the library looks for its shaders beside itself, where `cmake --install` puts them.
Exit code 0 when the picture is right.
"""

import argparse
import ctypes
import os
import subprocess
import sys
import tempfile
from ctypes import CFUNCTYPE, POINTER, Structure, byref, c_char_p, c_double, c_float, c_int32, c_uint32, c_uint64, c_uint8, c_void_p

API_VERSION = (1 << 16) | 18
OK = 0
TRUE = 1
LOG_LEVEL_ERROR = 4

ERROR_INVALID_ARGUMENT = -1
ERROR_FAILED = -2
INVALID_INDEX = 0xFFFFFFFF
FORMAT_R8G8B8A8_SRGB = 2
FORMAT_R16G16B16A16_SFLOAT = 6
FORMAT_D32_SFLOAT = 10
FORMAT_BC7_UNORM = 17
PASS_TYPE_GRAPHICS = 0
PASS_ACCESS_READ_RENDER_TARGET = 2
PASS_ACCESS_READ_PREVIOUS_RENDER_TARGET = 3
PASS_ACCESS_WRITE_RENDER_TARGET = 8
PASS_ACCESS_WRITE_SWAPCHAIN = 9

# rasm_log_callback: RASM_CALL is the C default (cdecl), which CFUNCTYPE is.
LogCallback = CFUNCTYPE(None, c_void_p, c_uint32, c_char_p)
# rasm_pass_callback: the user data and the pass's rasm_commands, valid only during the call.
PassCallback = CFUNCTYPE(None, c_void_p, c_void_p)


class Handle(Structure):
    _fields_ = [("id", c_uint64)]


class Vec2(Structure):
    _fields_ = [("x", c_float), ("y", c_float)]


class Vec3(Structure):
    _fields_ = [("x", c_float), ("y", c_float), ("z", c_float)]


class Vec4(Structure):
    _fields_ = [("x", c_float), ("y", c_float), ("z", c_float), ("w", c_float)]


class Transform(Structure):
    _fields_ = [("position", Vec3), ("rotation", Vec4), ("scale", Vec3)]


class DVec3(Structure):
    _fields_ = [("x", c_double), ("y", c_double), ("z", c_double)]


class TransformD(Structure):
    _fields_ = [("position", DVec3), ("rotation", Vec4), ("scale", Vec3)]


class NativeWindow(Structure):
    _fields_ = [("type", c_uint32), ("display", c_void_p), ("window", c_void_p),
                ("pixel_width", c_uint32), ("pixel_height", c_uint32), ("content_scale", c_float)]


class RendererConfig(Structure):
    _fields_ = [("size", c_uint32), ("app_name", c_char_p), ("window_width", c_int32), ("window_height", c_int32),
                ("native_window", NativeWindow), ("enable_validation", c_uint32), ("enable_sync_validation", c_uint32),
                ("enable_gpu_validation", c_uint32), ("preferred_backend", c_uint32), ("shader_dir", c_char_p),
                ("cache_dir", c_char_p)] + \
               [(name, c_uint32) for name in (
                   "geometry_pool_vertices", "geometry_pool_colors", "geometry_pool_skins", "geometry_pool_morphs",
                   "geometry_pool_indices", "geometry_pool_sub_meshes", "geometry_pool_meshlets",
                   "geometry_pool_meshlet_data", "max_instances", "max_lights", "max_materials",
                   "max_animated_entities", "animation_pool_bytes", "max_morph_weights", "show_stats")] + \
               [("fixed_delta_time", c_float), ("occlusion_culling", c_uint32), ("mesh_lods", c_uint32),
                ("lod_error_pixels", c_float), ("forced_lod", c_int32), ("meshlet_path", c_uint32),
                ("max_view_meshlets", c_uint32), ("async_compute", c_uint32), ("transfer_queue", c_uint32),
                ("clustered_lighting", c_uint32), ("host_image_copy", c_uint32), ("bloom", c_uint32),
                ("ui", c_uint32), ("anti_aliasing", c_uint32)]


class Vertex(Structure):
    _fields_ = [("position", Vec3), ("normal", Vec3), ("uv", Vec2 * 3), ("tangent", Vec4)]


class TextureTransform(Structure):
    _fields_ = [("offset", Vec2), ("rotation", c_float), ("scale", Vec2)]


class MaterialDesc(Structure):
    _fields_ = [("size", c_uint32), ("type", c_uint32), ("textures", Handle * 6), ("tex_coords", c_uint32 * 6),
                ("texture_transforms", TextureTransform * 6), ("params", c_float * 12), ("double_sided", c_uint32),
                ("alpha_mode", c_uint32), ("alpha_cutoff", c_float)]


class LightDesc(Structure):
    _fields_ = [("size", c_uint32), ("type", c_uint32), ("intensity", c_float), ("range", c_float), ("color", Vec3),
                ("position", Vec3), ("direction", Vec3), ("inner_cone_angle", c_float),
                ("outer_cone_angle", c_float), ("position_d", DVec3)]


class CameraProjection(Structure):
    _fields_ = [("size", c_uint32), ("type", c_uint32)] + [(name, c_float) for name in (
        "fov_y", "aspect", "near_z", "far_z", "left", "right", "bottom", "top")]


class ViewRect(Structure):
    _fields_ = [("x", c_float), ("y", c_float), ("width", c_float), ("height", c_float)]


class ShadowDesc(Structure):
    _fields_ = [("size", c_uint32), ("light", Handle), ("cascade_count", c_uint32), ("map_size", c_uint32),
                ("distance", c_float), ("split_lambda", c_float)]


class View(Structure):
    _fields_ = [("size", c_uint32), ("scene", Handle), ("camera", Handle), ("rect", ViewRect), ("target", Handle),
                ("window", Handle), ("bloom", c_uint32), ("anti_aliasing", c_uint32), ("shadow_count", c_uint32),
                ("shadows", POINTER(ShadowDesc))]


class Inspection(Structure):
    _fields_ = [("size", c_uint32), ("target", c_char_p), ("kind", c_uint32), ("attachment", c_uint32), ("face", c_uint32),
                ("level", c_uint32), ("sample", c_uint32), ("channel", c_uint32), ("exposure", c_float),
                ("depth_range", c_float), ("entity", Handle)]


class RenderTargetDesc(Structure):
    _fields_ = [("size", c_uint32), ("name", c_char_p), ("width", c_uint32), ("height", c_uint32), ("format", c_uint32),
                ("clear_color", Vec4), ("readable", c_uint32), ("swapchain_scale", c_float), ("depth_format", c_uint32),
                ("more_color_formats", c_uint32 * 3), ("samples", c_uint32), ("history", c_uint32), ("storage", c_uint32),
                ("cube", c_uint32)]


class ViewOutput(Structure):
    _fields_ = [("size", c_uint32), ("rect", ViewRect), ("target", Handle), ("window", Handle)]


class PassResource(Structure):
    _fields_ = [("access", c_uint32), ("face", c_uint32), ("id", c_uint64)]


class PassDesc(Structure):
    _fields_ = [("size", c_uint32), ("name", c_char_p), ("type", c_uint32), ("resources", POINTER(PassResource)),
                ("resource_count", c_uint32), ("execute", PassCallback), ("user_data", c_void_p)]


class ShaderStage(Structure):
    _fields_ = [("entry_point", c_char_p), ("spirv", c_void_p), ("spirv_size", c_uint64)]


class PipelineDesc(Structure):
    _fields_ = [("size", c_uint32), ("name", c_char_p), ("type", c_uint32), ("source", c_char_p), ("vertex", ShaderStage),
                ("fragment", ShaderStage), ("compute", ShaderStage), ("target", Handle), ("window", Handle),
                ("blend_mode", c_uint32), ("depth_test", c_uint32), ("depth_write", c_uint32), ("topology", c_uint32)]


# Draws the texture in the push constants over the whole output, red and blue swapped.
SWAP_SHADER = b"""
[[vk::binding(0, 0)]] Sampler2D gTextures[];
struct Push { uint source; uint padding[3]; };
[[vk::push_constant]] ConstantBuffer<Push> pc;
struct VSOutput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
[shader("vertex")] VSOutput vertMain(uint vertexID : SV_VertexID)
{
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv.x * 2.0 - 1.0, 1.0 - output.uv.y * 2.0, 0.0, 1.0);
    return output;
}
[shader("fragment")] float4 fragMain(VSOutput input) : SV_TARGET
{
    return float4(gTextures[pc.source].Sample(input.uv).bgr, 1.0);
}
"""


class ImageData(Structure):
    _fields_ = [("width", c_uint32), ("height", c_uint32), ("format", c_uint32), ("pixels", POINTER(c_uint8)),
                ("byte_size", c_uint64)]


def declare(lib):
    signatures = {
        "rasm_get_version": (c_uint32, []),
        "rasm_get_last_error": (c_char_p, []),
        "rasm_set_log_callback": (None, [LogCallback, c_void_p]),
        "rasm_renderer_config_init": (c_int32, [POINTER(RendererConfig), c_uint32]),
        "rasm_light_desc_init": (c_int32, [POINTER(LightDesc), c_uint32]),
        "rasm_camera_projection_init": (c_int32, [POINTER(CameraProjection), c_uint32]),
        "rasm_renderer_create": (c_int32, [c_uint32, POINTER(RendererConfig), POINTER(c_void_p)]),
        "rasm_renderer_destroy": (None, [c_void_p]),
        "rasm_running": (c_uint32, [c_void_p]),
        "rasm_render": (None, [c_void_p, Handle, Handle]),
        "rasm_view_init": (c_int32, [POINTER(View), c_uint32]),
        "rasm_render_views": (c_int32, [c_void_p, POINTER(View), c_uint32]),
        "rasm_shadow_desc_init": (c_int32, [POINTER(ShadowDesc), c_uint32]),
        "rasm_set_debug_view": (c_int32, [c_void_p, c_uint32]),
        "rasm_get_debug_view": (c_uint32, [c_void_p]),
        "rasm_set_culling_frozen": (c_int32, [c_void_p, c_uint32]),
        "rasm_set_meshlet_path": (c_int32, [c_void_p, c_uint32]),
        "rasm_get_meshlet_path": (c_uint32, [c_void_p]),
        "rasm_inspection_init": (c_int32, [POINTER(Inspection), c_uint32]),
        "rasm_set_inspection": (c_int32, [c_void_p, POINTER(Inspection)]),
        "rasm_get_inspection": (c_uint32, [c_void_p, POINTER(Inspection), ctypes.c_char_p, c_uint32]),
        "rasm_create_scene": (Handle, [c_void_p]),
        "rasm_create_mesh": (Handle, [c_void_p, POINTER(Vertex), c_uint32, POINTER(c_uint32), c_uint32, POINTER(c_uint32)]),
        "rasm_create_material": (Handle, [c_void_p, c_uint32, POINTER(Handle), c_uint32, c_uint32]),
        "rasm_get_material": (c_int32, [c_void_p, Handle, POINTER(MaterialDesc)]),
        "rasm_update_material": (c_int32, [c_void_p, Handle, POINTER(MaterialDesc)]),
        "rasm_create_entity": (Handle, [c_void_p, c_char_p, Handle, Handle, POINTER(Transform)]),
        "rasm_add_entity_to_scene": (c_int32, [c_void_p, Handle, Handle]),
        "rasm_create_light": (Handle, [c_void_p, POINTER(LightDesc)]),
        "rasm_add_light_to_scene": (c_int32, [c_void_p, Handle, Handle]),
        "rasm_create_camera": (Handle, [c_void_p, POINTER(CameraProjection), POINTER(Transform)]),
        "rasm_add_camera_to_scene": (c_int32, [c_void_p, Handle, Handle]),
        "rasm_read_frame": (Handle, [c_void_p, c_uint32]),
        "rasm_take_readback": (c_int32, [c_void_p, Handle, c_void_p]),
        "rasm_image_create": (c_void_p, []),
        "rasm_image_destroy": (None, [c_void_p]),
        "rasm_image_get": (ImageData, [c_void_p]),
        "rasm_image_write_png": (c_int32, [c_void_p, c_char_p]),
        "rasm_get_light": (c_int32, [c_void_p, Handle, POINTER(LightDesc)]),
        "rasm_get_transform_d": (c_int32, [c_void_p, Handle, POINTER(TransformD)]),
        "rasm_update_transform_d": (c_int32, [c_void_p, Handle, POINTER(TransformD)]),
        "rasm_get_camera_transform_d": (c_int32, [c_void_p, Handle, POINTER(TransformD)]),
        "rasm_set_camera_transform_d": (c_int32, [c_void_p, Handle, POINTER(TransformD)]),
        "rasm_get_light_position_d": (c_int32, [c_void_p, Handle, POINTER(DVec3)]),
        "rasm_set_light_position_d": (c_int32, [c_void_p, Handle, POINTER(DVec3)]),
        "rasm_to_render_position": (Vec3, [DVec3, DVec3]),
        "rasm_get_glyph_orientation": (Vec4, [Vec3]),
        "rasm_render_target_desc_init": (c_int32, [POINTER(RenderTargetDesc), c_uint32]),
        "rasm_view_output_init": (c_int32, [POINTER(ViewOutput), c_uint32]),
        "rasm_pass_desc_init": (c_int32, [POINTER(PassDesc), c_uint32]),
        "rasm_pipeline_desc_init": (c_int32, [POINTER(PipelineDesc), c_uint32]),
        "rasm_render_graph_create": (c_int32, [c_void_p, POINTER(c_void_p)]),
        "rasm_render_graph_destroy": (None, [c_void_p]),
        "rasm_render_graph_create_render_target": (Handle, [c_void_p, POINTER(RenderTargetDesc)]),
        "rasm_render_graph_add_scene_passes": (c_int32, [c_void_p, Handle, Handle, Handle, c_void_p, c_uint32, c_void_p, c_uint32]),
        "rasm_render_graph_add_tone_map_pass": (c_int32, [c_void_p, Handle, Handle, Handle, POINTER(ViewOutput)]),
        "rasm_render_graph_add_pass": (c_int32, [c_void_p, POINTER(PassDesc)]),
        "rasm_render_graph_compile": (c_int32, [c_void_p]),
        "rasm_render_graph_execute": (c_int32, [c_void_p, Handle]),
        "rasm_create_pipeline": (Handle, [c_void_p, POINTER(PipelineDesc)]),
        "rasm_get_render_target_color": (Handle, [c_void_p, Handle, c_uint32]),
        "rasm_get_previous_render_target_color": (Handle, [c_void_p, Handle, c_uint32]),
        "rasm_get_bindless_texture_index": (c_uint32, [c_void_p, Handle]),
        "rasm_cmd_bind_pipeline": (c_int32, [c_void_p, Handle]),
        "rasm_cmd_push_constants": (c_int32, [c_void_p, c_void_p, c_uint32, c_uint32]),
        "rasm_cmd_draw": (c_int32, [c_void_p, c_uint32, c_uint32]),
        "rasm_cmd_end_gpu_timer": (c_int32, [c_void_p, c_uint32]),
        "rasm_cmd_set_scissor": (c_int32, [c_void_p, c_int32, c_int32, c_uint32, c_uint32]),
    }
    for name, (restype, argtypes) in signatures.items():
        function = getattr(lib, name)
        function.restype = restype
        function.argtypes = argtypes


def compile_spirv(slangc, source):
    """The swap shader's two stages as SPIR-V, compiled by slangc; each module names its entry point main."""
    stages = []
    with tempfile.TemporaryDirectory() as folder:
        path = os.path.join(folder, "swap.slang")
        with open(path, "wb") as file:
            file.write(source)
        for entry, stage in (("vertMain", "vertex"), ("fragMain", "fragment")):
            out = os.path.join(folder, stage + ".spv")
            subprocess.run([os.path.abspath(slangc), path, "-target", "spirv", "-entry", entry, "-stage", stage, "-o", out], check=True)
            with open(out, "rb") as file:
                stages.append(file.read())
    return stages


def draw_graph(lib, renderer, scene, camera, frames, errors, slangc):
    """Builds a graph with a Python pass and draws its frames; the last one is copied back. Returns the graph, the
    readback and the callback's counts."""
    graph = c_void_p()
    check(lib.rasm_render_graph_create(renderer, byref(graph)) == OK, "rasm_render_graph_create", lib)

    def target(name, color, depth=0, history=False):
        desc = RenderTargetDesc()
        lib.rasm_render_target_desc_init(byref(desc), ctypes.sizeof(desc))
        desc.name = name
        desc.format = color
        desc.depth_format = depth
        desc.clear_color = Vec4(0.0, 0.0, 0.0, 1.0)
        if history:
            desc.width, desc.height, desc.history = 1, 1, TRUE
        else:
            desc.swapchain_scale = 1.0
        handle = lib.rasm_render_graph_create_render_target(graph, byref(desc))
        check(handle.id != 0, f"the graph's target {name}", lib)
        return handle

    hdr = target(b"hdr", FORMAT_R16G16B16A16_SFLOAT, FORMAT_D32_SFLOAT)
    ldr = target(b"ldr", FORMAT_R8G8B8A8_SRGB)
    trail = target(b"trail", FORMAT_R8G8B8A8_SRGB, history=True)
    # A block-compressed format cannot be drawn into: refused before it reaches the device.
    bad = RenderTargetDesc()
    lib.rasm_render_target_desc_init(byref(bad), ctypes.sizeof(bad))
    bad.format = FORMAT_BC7_UNORM
    before = len(errors)
    check(lib.rasm_render_graph_create_render_target(graph, byref(bad)).id == 0, "a target of a format it cannot draw into is refused", lib)
    del errors[before:]
    check(lib.rasm_render_graph_add_scene_passes(graph, scene, camera, hdr, None, 0, None, 0) == OK, "rasm_render_graph_add_scene_passes", lib)
    output = ViewOutput()
    lib.rasm_view_output_init(byref(output), ctypes.sizeof(output))
    output.target = ldr
    check(lib.rasm_render_graph_add_tone_map_pass(graph, hdr, camera, Handle(), byref(output)) == OK, "rasm_render_graph_add_tone_map_pass", lib)

    pipeline_desc = PipelineDesc()
    lib.rasm_pipeline_desc_init(byref(pipeline_desc), ctypes.sizeof(pipeline_desc))
    pipeline_desc.name = b"swap"
    pipeline_desc.source = SWAP_SHADER
    pipeline = lib.rasm_create_pipeline(renderer, byref(pipeline_desc))
    check(pipeline.id != 0, "rasm_create_pipeline from Slang source", lib)
    if slangc:
        # The same shader from SPIR-V: the pass draws with this one, so the picture checks the SPIR-V path.
        vertex, fragment = compile_spirv(slangc, SWAP_SHADER)
        spirv = (ctypes.create_string_buffer(vertex, len(vertex)), ctypes.create_string_buffer(fragment, len(fragment)))
        lib.rasm_pipeline_desc_init(byref(pipeline_desc), ctypes.sizeof(pipeline_desc))
        pipeline_desc.name = b"swap_spirv"
        pipeline_desc.vertex = ShaderStage(None, ctypes.cast(spirv[0], c_void_p), len(vertex))
        pipeline_desc.fragment = ShaderStage(None, ctypes.cast(spirv[1], c_void_p), len(fragment))
        pipeline = lib.rasm_create_pipeline(renderer, byref(pipeline_desc))
        check(pipeline.id != 0, "rasm_create_pipeline from SPIR-V", lib)

    counts = {"frames": 0, "history": 0, "commands": 0, "refused": 0}
    state = {"last": 0, "kept": None}

    def swap(user_data, commands):
        # A Python exception stops in ctypes, which prints it: nothing reaches RASM.
        written = lib.rasm_get_render_target_color(renderer, trail, 0)
        previous = lib.rasm_get_previous_render_target_color(renderer, trail, 0)
        if written.id == previous.id or (counts["frames"] > 0 and previous.id != state["last"]):
            counts["history"] += 1
        state["last"] = written.id
        if counts["frames"] == 0:
            before = len(errors)
            lib.rasm_render(renderer, scene, camera)
            if len(errors) == before + 1 and b"recording" in lib.rasm_get_last_error():
                counts["refused"] += 1
            # A command that does not fit is refused, not recorded: a timer never begun, a scissor off the target.
            if lib.rasm_cmd_end_gpu_timer(commands, 5) == ERROR_INVALID_ARGUMENT:
                counts["refused"] += 1
            if lib.rasm_cmd_set_scissor(commands, -1, 0, 8, 8) == ERROR_INVALID_ARGUMENT:
                counts["refused"] += 1
            del errors[before:]
        counts["frames"] += 1
        state["kept"] = commands
        source = lib.rasm_get_bindless_texture_index(renderer, lib.rasm_get_render_target_color(renderer, ldr, 0))
        push = (c_uint32 * 4)(source, 0, 0, 0)
        if (source == INVALID_INDEX or lib.rasm_cmd_bind_pipeline(commands, pipeline) != OK or
                lib.rasm_cmd_push_constants(commands, push, ctypes.sizeof(push), 0) != OK or lib.rasm_cmd_draw(commands, 3, 1) != OK):
            counts["commands"] += 1

    # The callback object must live as long as the graph: it is kept in the returned counts' closure below.
    callback = PassCallback(swap)
    resources = (PassResource * 4)(PassResource(PASS_ACCESS_READ_RENDER_TARGET, 0, ldr.id),
                                   PassResource(PASS_ACCESS_READ_PREVIOUS_RENDER_TARGET, 0, trail.id),
                                   PassResource(PASS_ACCESS_WRITE_RENDER_TARGET, 0, trail.id),
                                   PassResource(PASS_ACCESS_WRITE_SWAPCHAIN, 0, 0))
    # A pass writes one target: the history target in a pass of its own, the window in the swap pass.
    trail_pass = PassDesc()
    lib.rasm_pass_desc_init(byref(trail_pass), ctypes.sizeof(trail_pass))
    trail_pass.name = b"Trail"
    trail_pass.type = PASS_TYPE_GRAPHICS
    trail_resources = (PassResource * 2)(resources[1], resources[2])
    trail_pass.resources = trail_resources
    trail_pass.resource_count = 2
    check(lib.rasm_render_graph_add_pass(graph, byref(trail_pass)) == OK, "the Trail pass", lib)
    swap_resources = (PassResource * 3)(resources[0], PassResource(PASS_ACCESS_READ_RENDER_TARGET, 0, trail.id), resources[3])
    swap_pass = PassDesc()
    lib.rasm_pass_desc_init(byref(swap_pass), ctypes.sizeof(swap_pass))
    swap_pass.name = b"Swap"
    swap_pass.type = PASS_TYPE_GRAPHICS
    swap_pass.resources = swap_resources
    swap_pass.resource_count = 3
    swap_pass.execute = callback
    check(lib.rasm_render_graph_add_pass(graph, byref(swap_pass)) == OK, "the Swap pass with a Python callback", lib)
    check(lib.rasm_render_graph_compile(graph) == OK, "rasm_render_graph_compile", lib)

    readback = Handle()
    for frame in range(1, frames + 1):
        if frame == frames:
            readback = lib.rasm_read_frame(renderer, 0)
        # The first frame's callback made a call that was refused, so that frame reports the error.
        expected = ERROR_FAILED if frame == 1 else OK
        check(lib.rasm_render_graph_execute(graph, camera) == expected, f"rasm_render_graph_execute, frame {frame}", lib)
    # The commands handle of a callback that returned is refused.
    before = len(errors)
    if lib.rasm_cmd_draw(state["kept"], 3, 1) != ERROR_INVALID_ARGUMENT or b"only inside the pass callback" not in lib.rasm_get_last_error():
        counts["commands"] += 1
    del errors[before:]
    counts["callback"] = callback
    return graph, readback, counts


def check(condition, what, lib):
    if not condition:
        error = lib.rasm_get_last_error().decode("utf-8", "replace")
        sys.exit(f"FAIL: {what}" + (f": {error}" if error else ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("library")
    parser.add_argument("--shader-dir")
    parser.add_argument("--frames", type=int, default=8)
    parser.add_argument("--out")
    parser.add_argument("--validation", action="store_true")
    parser.add_argument("--slangc", help="also make the pass's pipeline from SPIR-V that this slangc compiles")
    args = parser.parse_args()

    library = os.path.abspath(args.library)
    if sys.platform == "win32":
        os.add_dll_directory(os.path.dirname(library))
    lib = ctypes.CDLL(library)
    declare(lib)
    check(lib.rasm_get_version() >> 16 == API_VERSION >> 16, "the library's major version", lib)

    # Every RASM error line, from any thread; the callback object must live as long as it is set.
    errors = []

    def on_log(user_data, level, message):
        if level >= LOG_LEVEL_ERROR:
            errors.append(message.decode("utf-8", "replace"))

    log_callback = LogCallback(on_log)
    lib.rasm_set_log_callback(log_callback, None)

    width, height = 256, 256
    config = RendererConfig()
    check(lib.rasm_renderer_config_init(byref(config), ctypes.sizeof(config)) == OK, "rasm_renderer_config_init", lib)
    config.app_name = b"ctypes_smoke"
    config.window_width = width
    config.window_height = height
    config.enable_validation = TRUE if args.validation else 0
    config.cache_dir = None
    if args.shader_dir:
        config.shader_dir = os.path.abspath(args.shader_dir).encode("utf-8")
    renderer = c_void_p()
    check(lib.rasm_renderer_create(API_VERSION, byref(config), byref(renderer)) == OK, "rasm_renderer_create", lib)

    try:
        scene = lib.rasm_create_scene(renderer)
        # A 2 x 2 quad at z = 0, facing the camera at z = 3.
        vertices = (Vertex * 4)()
        for i, (x, y) in enumerate(((-1, -1), (1, -1), (1, 1), (-1, 1))):
            vertices[i].position = Vec3(x, y, 0.0)
            vertices[i].normal = Vec3(0.0, 0.0, 1.0)
        indices = (c_uint32 * 6)(0, 1, 2, 0, 2, 3)
        mesh = lib.rasm_create_mesh(renderer, vertices, 4, indices, 6, None)
        check(mesh.id != 0, "rasm_create_mesh", lib)

        material = lib.rasm_create_material(renderer, 0, None, 0, 0)
        desc = MaterialDesc()
        desc.size = ctypes.sizeof(desc)
        check(lib.rasm_get_material(renderer, material, byref(desc)) == OK, "rasm_get_material", lib)
        desc.params[0], desc.params[1], desc.params[2], desc.params[3] = 1.0, 0.05, 0.05, 1.0
        desc.params[4], desc.params[5] = 0.0, 0.6
        check(lib.rasm_update_material(renderer, material, byref(desc)) == OK, "rasm_update_material", lib)

        at = Transform(Vec3(0.0, 0.0, 0.0), Vec4(0.0, 0.0, 0.0, 1.0), Vec3(1.0, 1.0, 1.0))
        entity = lib.rasm_create_entity(renderer, b"quad", mesh, material, byref(at))
        check(lib.rasm_add_entity_to_scene(renderer, scene, entity) == OK, "rasm_add_entity_to_scene", lib)

        light = LightDesc()
        lib.rasm_light_desc_init(byref(light), ctypes.sizeof(light))
        light.direction = Vec3(0.0, 0.0, -1.0)
        light.intensity = 3.0
        light.range = 0.0
        sun = lib.rasm_create_light(renderer, byref(light))
        check(lib.rasm_add_light_to_scene(renderer, scene, sun) == OK, "the light", lib)

        projection = CameraProjection()
        lib.rasm_camera_projection_init(byref(projection), ctypes.sizeof(projection))
        projection.fov_y = 45.0
        projection.aspect = width / height
        eye = Transform(Vec3(0.0, 0.0, 3.0), Vec4(0.0, 0.0, 0.0, 1.0), Vec3(1.0, 1.0, 1.0))
        camera = lib.rasm_create_camera(renderer, byref(projection), byref(eye))
        check(camera.id != 0, "rasm_create_camera", lib)
        lib.rasm_add_camera_to_scene(renderer, scene, camera)

        # Far from the origin: Paris in Earth-centred coordinates, with digits a float cannot hold. Each view draws in its
        # camera's render space, so the picture is the same as at the origin.
        paris = (4197000.123456789, 168000.987654321, 4786000.000001)

        def far(x, y, z, rotation=Vec4(0.0, 0.0, 0.0, 1.0)):
            return TransformD(DVec3(paris[0] + x, paris[1] + y, paris[2] + z), rotation, Vec3(1.0, 1.0, 1.0))

        def same(a, b):
            return (a.x, a.y, a.z) == (b.x, b.y, b.z)

        placed = far(0.0, 0.0, 0.0)
        check(lib.rasm_update_transform_d(renderer, entity, byref(placed)) == OK, "rasm_update_transform_d", lib)
        back = TransformD()
        check(lib.rasm_get_transform_d(renderer, entity, byref(back)) == OK and same(back.position, placed.position),
              "the entity's position comes back bit for bit", lib)
        eye_far = far(0.0, 0.0, 3.0)
        check(lib.rasm_set_camera_transform_d(renderer, camera, byref(eye_far)) == OK, "rasm_set_camera_transform_d", lib)
        check(lib.rasm_get_camera_transform_d(renderer, camera, byref(back)) == OK and same(back.position, eye_far.position),
              "the camera's position comes back bit for bit", lib)
        lamp_desc = LightDesc()
        lib.rasm_light_desc_init(byref(lamp_desc), ctypes.sizeof(lamp_desc))
        lamp_desc.type = 1
        lamp = lib.rasm_create_light(renderer, byref(lamp_desc))
        lamp_at = DVec3(paris[0] + 1.0, paris[1], paris[2] + 2.0)
        check(lib.rasm_set_light_position_d(renderer, lamp, byref(lamp_at)) == OK, "rasm_set_light_position_d", lib)
        lamp_back = DVec3()
        check(lib.rasm_get_light_position_d(renderer, lamp, byref(lamp_back)) == OK and same(lamp_back, lamp_at),
              "the light's position comes back bit for bit", lib)
        check(lib.rasm_get_light(renderer, lamp, byref(lamp_desc)) == OK and
              (lamp_desc.position.x + lamp_desc.position_d.x, lamp_desc.position.z + lamp_desc.position_d.z) == (lamp_at.x, lamp_at.z),
              "rasm_get_light's position + position_d is the double", lib)
        render = lib.rasm_to_render_position(lamp_at, eye_far.position)
        check((render.x, render.y, render.z) == (1.0, 0.0, -1.0), f"rasm_to_render_position, got {render.x} {render.y} {render.z}", lib)
        # A struct passed and returned by value: +Y turned to +X is a quarter turn about -Z.
        turn = lib.rasm_get_glyph_orientation(Vec3(1.0, 0.0, 0.0))
        check(abs(turn.z + 0.70710678) < 1e-6 and abs(turn.w - 0.70710678) < 1e-6, f"rasm_get_glyph_orientation, got {turn.x} {turn.y} {turn.z} {turn.w}", lib)

        # Every element has the size of the first: the library steps through the array by it.
        views = (View * 2)()
        for view in views:
            lib.rasm_view_init(byref(view), ctypes.sizeof(view))
            view.scene = scene
            view.camera = camera
        # A view is a camera and an output: the small one needs a camera of its own.
        views[1].camera = lib.rasm_create_camera(renderer, byref(projection), byref(eye))
        check(views[1].camera.id != 0, "the second camera", lib)
        check(lib.rasm_set_camera_transform_d(renderer, views[1].camera, byref(eye_far)) == OK, "the second camera, far", lib)
        views[1].rect = ViewRect(0.75, 0.0, 0.25, 0.25)
        # 1.18: both views draw the sun's shadow, each fitted to its own camera, into one set of maps in turn.
        shadows = (ShadowDesc * 5)()
        for shadow in shadows:
            check(lib.rasm_shadow_desc_init(byref(shadow), ctypes.sizeof(shadow)) == OK, "rasm_shadow_desc_init", lib)
            shadow.light = sun
            shadow.cascade_count = 2
            shadow.map_size = 512
        for view in views:
            view.shadow_count = 1
            view.shadows = shadows
        # More shadows than RASM_MAX_VIEW_SHADOWS are refused before anything is drawn.
        expected = len(errors)
        views[0].shadow_count = 5
        check(lib.rasm_render_views(renderer, views, 2) == ERROR_INVALID_ARGUMENT, "a view with 5 shadows is refused", lib)
        check(b"RASM_MAX_VIEW_SHADOWS" in lib.rasm_get_last_error(), "the refused shadows say why", lib)
        del errors[expected:]
        views[0].shadow_count = 1

        # 1.18: the debug toggles go and come back, and the inspection keeps its own copy of the target's name.
        check(lib.rasm_set_debug_view(renderer, 1) == OK and lib.rasm_get_debug_view(renderer) == 1, "rasm_set_debug_view NORMALS", lib)
        check(lib.rasm_set_debug_view(renderer, 0) == OK and lib.rasm_get_debug_view(renderer) == 0, "rasm_set_debug_view NONE", lib)
        check(lib.rasm_set_culling_frozen(renderer, TRUE) == OK and lib.rasm_set_culling_frozen(renderer, 0) == OK, "rasm_set_culling_frozen", lib)
        path = lib.rasm_get_meshlet_path(renderer)
        check(lib.rasm_set_meshlet_path(renderer, path) == OK and lib.rasm_get_meshlet_path(renderer) == path, "rasm_set_meshlet_path", lib)
        inspection = Inspection()
        check(lib.rasm_inspection_init(byref(inspection), ctypes.sizeof(inspection)) == OK, "rasm_inspection_init", lib)
        name = ctypes.create_string_buffer(b"scene_hdr")
        inspection.target = ctypes.cast(name, c_char_p)
        inspection.exposure = 2.0
        check(lib.rasm_set_inspection(renderer, byref(inspection)) == OK, "rasm_set_inspection", lib)
        name.value = b"changed"
        back = Inspection()
        lib.rasm_inspection_init(byref(back), ctypes.sizeof(back))
        buffer = ctypes.create_string_buffer(6)
        length = lib.rasm_get_inspection(renderer, byref(back), buffer, 6)
        check(length == 9 and buffer.value == b"scene" and back.exposure == 2.0, f"rasm_get_inspection, got {length} {buffer.value} {back.exposure}", lib)
        inspection.target = None
        check(lib.rasm_set_inspection(renderer, byref(inspection)) == OK, "rasm_set_inspection of nothing", lib)

        # Frame N - 1 is one view through rasm_render, frame N two through rasm_render_views.
        single_readback = Handle()
        readback = Handle()
        for frame in range(1, args.frames + 1):
            check(lib.rasm_running(renderer) == TRUE, f"the renderer stopped at frame {frame}", lib)
            if frame == args.frames - 1:
                single_readback = lib.rasm_read_frame(renderer, 0)
                check(single_readback.id != 0, "rasm_read_frame", lib)
            if frame == args.frames:
                readback = lib.rasm_read_frame(renderer, 0)
                check(readback.id != 0, "rasm_read_frame", lib)
                check(lib.rasm_render_views(renderer, views, 2) == OK, "rasm_render_views", lib)
            else:
                lib.rasm_render(renderer, scene, camera)

        graph, graph_readback, graph_checks = draw_graph(lib, renderer, scene, camera, args.frames, errors, args.slangc)
        image = lib.rasm_image_create()
        try:
            check(lib.rasm_take_readback(renderer, graph_readback, image) == OK, "rasm_take_readback of the graph's frame", lib)
            data = lib.rasm_image_get(image)
            graph_pixels = ctypes.string_at(data.pixels, data.byte_size)
            check(lib.rasm_take_readback(renderer, single_readback, image) == OK, "rasm_take_readback", lib)
            data = lib.rasm_image_get(image)
            single_pixels = ctypes.string_at(data.pixels, data.byte_size)
            check(lib.rasm_take_readback(renderer, readback, image) == OK, "rasm_take_readback", lib)
            data = lib.rasm_image_get(image)
            check(data.width == width and data.height == height and data.byte_size == width * height * 4, "the image's size", lib)
            pixels = ctypes.string_at(data.pixels, data.byte_size)
            if args.out:
                check(lib.rasm_image_write_png(image, os.path.abspath(args.out).encode("utf-8")) == OK, "rasm_image_write_png", lib)
        finally:
            lib.rasm_image_destroy(image)
    finally:
        lib.rasm_renderer_destroy(renderer)
    # The renderer took its graph's passes with it: calls on the graph fail, and destroying it frees it.
    expected = len(errors)
    check(lib.rasm_render_graph_compile(graph) == ERROR_INVALID_ARGUMENT, "a graph whose renderer was destroyed refuses calls", lib)
    check(b"renderer was destroyed" in lib.rasm_get_last_error(), "a detached graph says why", lib)
    lib.rasm_render_graph_destroy(graph)
    del errors[expected:]
    lib.rasm_set_log_callback(LogCallback(), None)

    def pixel(x, y, image=None):
        i = (y * width + x) * 4
        return tuple((pixels if image is None else image)[i:i + 3])

    center = pixel(width // 2, height // 2)
    small = pixel(width * 7 // 8, height // 8)
    corner = pixel(2, 2)
    print(f"center {center}, small view {small}, corner {corner}")
    check(center[0] > center[1] + 50 and center[0] > center[2] + 50, f"the middle is red, got {center}", lib)
    check(small[0] > small[1] + 50 and small[0] > small[2] + 50, f"the small view's middle is red, got {small}", lib)
    single = pixel(width // 2, height // 2, single_pixels)
    check(single[0] > single[1] + 50 and single[0] > single[2] + 50, f"rasm_render's middle is red, got {single}", lib)
    check(sum(abs(a - b) for a, b in zip(center, corner)) > 60, f"the corner is background, got {corner}", lib)
    swapped = pixel(width // 2, height // 2, graph_pixels)
    print(f"graph frame center {swapped}, {graph_checks['frames']} Python pass callbacks")
    check(swapped[2] > swapped[1] + 50 and swapped[2] > swapped[0] + 50, f"the Python pass swapped red and blue, got {swapped}", lib)
    check(graph_checks["frames"] >= 2 and graph_checks["history"] == 0 and graph_checks["commands"] == 0, f"the pass callback's checks: {graph_checks['history']} history, {graph_checks['commands']} command errors", lib)
    check(graph_checks["refused"] == 3, f"a call that starts a frame, and commands that do not fit, are refused in a pass callback: {graph_checks['refused']} of 3", lib)
    check(not errors, "RASM logged errors: " + " | ".join(errors), lib)
    print(f"OK: {os.path.basename(library)} {lib.rasm_get_version() >> 16}.{lib.rasm_get_version() & 0xFFFF} "
          f"drew {args.frames} frames from Python, 6370 km from the origin")


if __name__ == "__main__":
    main()
