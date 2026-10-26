@rem Offline compile check for the uber shader. Run from the repository root.
slangc.exe ./shaders/passes/rasm.slang -I ./shaders/lib -target spirv -profile spirv_1_4 -emit-spirv-directly  -force-glsl-scalar-layout -fvk-use-entrypoint-name -entry vertMain -entry fragMain -o ./shaders/build/passes/rasm.spv
