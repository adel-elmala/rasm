@rem Offline compile check for the uber and culling shaders. Run from the repository root.
slangc.exe ./shaders/passes/rasm.slang -I ./shaders/lib -target spirv -profile spirv_1_4 -emit-spirv-directly  -force-glsl-scalar-layout -fvk-use-entrypoint-name -entry vertMain -entry fragMain -o ./shaders/build/passes/rasm.spv
slangc.exe ./shaders/passes/cull.slang -I ./shaders/lib -target spirv -profile spirv_1_4 -emit-spirv-directly  -force-glsl-scalar-layout -fvk-use-entrypoint-name -entry compMain -o ./shaders/build/passes/cull.spv
