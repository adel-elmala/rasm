Example #1 Key Point:

This simple example demostate the usage of rasm lower level API:
- render graph and render passes.

the example contians 2 simple passes:
1. Forward Pass that draws the scene with the GPU-driven path into a offscreen render target
2. overlay pass, which is a simple postproccess pass that samples the render target, converts the background color to green and displays the final image on the swapchain.
