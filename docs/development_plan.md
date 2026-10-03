# Development plan for RASM Engine

> The following plan is in no particular order.

1) Make the low-level API as convenient and simple to use as possible, don't expose unnecessary methods
2) Extend the engine by adding more examples, drive the development of the engine by what the examples needs from the engine.
3) Manage gpu resources:
    - ~~use multi-draw-indirect~~ - DONE
    - ~~use a single bindless desc set for textures~~- DONE
    - ~~use a single buffer for all uniforms needed~~ - DONE
    - ~~use a single vertex/index buffer that contains all the vertices of all meshes added to the scene~~ - DONE
    - try out one uber shader, but write the shader in seperate files and use `#include` for better readability and maintainability. - basic skeleton is done
4) Manage cpu resources:
    - build a custom memory areana, make a custom allocator.
5) Move the engine to be built in C instead of C++ so it can be ported easly to other languages, don't use STD
6) Add more examples and more custom render passes.
7) GPU frustom culling
8) Engine Statistics: memory, fps, triangles, entities,...
9) GUI: custom or imgui
10) Make it open source:
    - Licenese
    - alpha release
    - logo, docs, social media.
11) Extend graphics backends to use other APIs such as Metal,D3D12, webGPU


