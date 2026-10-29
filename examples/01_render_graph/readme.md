Example #1 Key Point:

This example demonstrates the render graph and the lower-level API (`RenderContext`, render targets, custom pipelines).

It renders the Damaged Helmet with bloom, in 5 passes. Each pass declares the render targets it reads and writes, and the graph orders the passes from those declarations:

1. **Forward**: draws the helmet with the GPU-driven scene path (`Engine::drawScene`) into an offscreen target.
2. **Bloom bright**: downsamples the scene to half resolution and keeps what is brighter than a threshold. The forward pass is tone-mapped (Reinhard), so the shader inverts the tone mapping and thresholds in HDR.
3. **Bloom blur H** and 4. **Bloom blur V**: a separable Gaussian blur of the bright parts.
5. **Composite**: adds the blurred bloom to the scene, tone-maps again and draws to the swapchain.

The bloom passes draw a fullscreen triangle with pipelines from `Engine::createPipeline`. They read their inputs from the bindless texture array and their parameters through the push-constant pointer.

Controls: drag to orbit the helmet, mouse wheel to zoom.
