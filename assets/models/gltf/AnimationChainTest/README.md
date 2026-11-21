# AnimationChainTest

A chain of 100 boxes, each the child of the one below it, under a static root node scaled by 0.5. One clip, "Sway",
turns every link by up to 1.5 degrees around Z over 3 seconds, all through one shared sampler, so the tip bends by up
to 150 degrees.

It tests what a character with many bones needs: more nodes than one 64-thread group of the Animate pass, a parent
walk 100 nodes deep, a static parent with a scale above the animated nodes, and one sampler used by many channels.

Made for RASM by a small generator script; the file has no other source.
