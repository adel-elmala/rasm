Example #1 Key Point:

This simple example demostate the usage of rasm lower level API:
- render graph and render passes.

the example contians 2 simple passes:
1. Forward Pass that blits to a offscreen buffer
2. overlay pass, which is a simple postproccess pass that converts the background red color in the offscreen buffer to green color and displays the final image on the swapchain.