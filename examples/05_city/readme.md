Example #5 Key Point:

A procedural night city, about 140k instances, drawn by the GPU-driven path in one `render` call:
1. Scale: 49 x 49 blocks of towers, apartments, parks, street trees and streetlights. Each entity is one row in the instance table; the CPU does not walk them per frame.
2. Culling: the GPU culls every instance against the frustum and the last frame's depth (two-pass Hi-Z). Down the street canyons, most of the city is hidden behind the towers.
3. LOD: the tree canopies have 5120 triangles. meshoptimizer builds their LODs at load, and the GPU picks one per tree from its size on screen. F2 forces a LOD; the stats overlay (F1) shows the triangles drawn against LOD 0.
4. Updates: about 5000 cars move every frame and rewrite only their own rows. The monument turns, and every aviation light blinks through one material row.
5. Materials: the facade textures (windows, glass, lit rooms) are generated in code and uploaded with `fillTexture`. The monument is the DamagedHelmet glTF.

The camera flies a two-minute tour. Drag the mouse or press WASD to take over.
