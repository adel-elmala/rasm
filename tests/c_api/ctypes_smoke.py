"""Drive the RASM C API from Python with ctypes, as a language binding does: no C compiler, only the shared library.

It makes a headless renderer, a red quad in front of a camera and a sun, renders a few frames, copies the last one
back and checks its pixels: red in the middle, the background at the corner. The structs below mirror
include/rasm/rasm_c.h field by field.

    python tests/c_api/ctypes_smoke.py <path to rasm_c.dll / librasm_c.dylib / librasm_c.so>
        [--shader-dir DIR] [--frames N] [--out frame.png] [--validation]

Without --shader-dir the library looks for its shaders beside itself, where `cmake --install` puts them.
Exit code 0 when the picture is right.
"""

import argparse
import ctypes
import os
import sys
from ctypes import CFUNCTYPE, POINTER, Structure, byref, c_char_p, c_float, c_int32, c_uint32, c_uint64, c_uint8, c_void_p

API_VERSION = (1 << 16) | 0
OK = 0
TRUE = 1
LOG_LEVEL_ERROR = 4

# rasm_log_callback: RASM_CALL is the C default (cdecl), which CFUNCTYPE is.
LogCallback = CFUNCTYPE(None, c_void_p, c_uint32, c_char_p)


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
                ("outer_cone_angle", c_float)]


class CameraProjection(Structure):
    _fields_ = [("size", c_uint32), ("type", c_uint32)] + [(name, c_float) for name in (
        "fov_y", "aspect", "near_z", "far_z", "left", "right", "bottom", "top")]


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
    }
    for name, (restype, argtypes) in signatures.items():
        function = getattr(lib, name)
        function.restype = restype
        function.argtypes = argtypes


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
        check(lib.rasm_add_light_to_scene(renderer, scene, lib.rasm_create_light(renderer, byref(light))) == OK, "the light", lib)

        projection = CameraProjection()
        lib.rasm_camera_projection_init(byref(projection), ctypes.sizeof(projection))
        projection.fov_y = 45.0
        projection.aspect = width / height
        eye = Transform(Vec3(0.0, 0.0, 3.0), Vec4(0.0, 0.0, 0.0, 1.0), Vec3(1.0, 1.0, 1.0))
        camera = lib.rasm_create_camera(renderer, byref(projection), byref(eye))
        check(camera.id != 0, "rasm_create_camera", lib)
        lib.rasm_add_camera_to_scene(renderer, scene, camera)

        readback = Handle()
        for frame in range(1, args.frames + 1):
            check(lib.rasm_running(renderer) == TRUE, f"the renderer stopped at frame {frame}", lib)
            if frame == args.frames:
                readback = lib.rasm_read_frame(renderer, 0)
                check(readback.id != 0, "rasm_read_frame", lib)
            lib.rasm_render(renderer, scene, camera)

        image = lib.rasm_image_create()
        try:
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
        lib.rasm_set_log_callback(LogCallback(), None)

    def pixel(x, y):
        i = (y * width + x) * 4
        return tuple(pixels[i:i + 3])

    center = pixel(width // 2, height // 2)
    corner = pixel(2, 2)
    print(f"center {center}, corner {corner}")
    check(center[0] > center[1] + 50 and center[0] > center[2] + 50, f"the middle is red, got {center}", lib)
    check(sum(abs(a - b) for a, b in zip(center, corner)) > 60, f"the corner is background, got {corner}", lib)
    check(not errors, "RASM logged errors: " + " | ".join(errors), lib)
    print(f"OK: {os.path.basename(library)} {lib.rasm_get_version() >> 16}.{lib.rasm_get_version() & 0xFFFF} "
          f"drew {args.frames} frames from Python")


if __name__ == "__main__":
    main()
