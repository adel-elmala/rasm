Example #4 Key Point:

This example shows a compute pass in the render graph:
1. Animate (compute pass): a compute shader writes 4096 particle positions into a storage buffer, through its device address.
2. Draw: a graphics pass reads the same buffer in its vertex shader and draws a small triangle per particle.

The pass records no barriers itself. The render graph adds them: graphics → compute before the dispatch, because the previous frame may still draw from the buffer, and compute → graphics after it, so the draw sees the new positions.
