
Example #0 Key Point:

This simple example demonstrates the basic usage of the rasm engine API. While the example seems simple, under the hood, rasm utilizes modern GPU features, including the following:
- Multi-draw-indirect draw calls: These minimize driver overhead by building a GPU buffer that contains all the parameters needed for the GPU to execute its work. This essentially emits only a single draw call per frame for the entire scene.
- Modern API features: Vertex-pulling, bindless textures, device buffer addresses, push constants, and mega vertex/index buffers all combine to further minimize driver overhead.
