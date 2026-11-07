@rem Offline compile check for every pass shader, with the flags of the engine's Slang session (shaderCompiler.cpp).
@rem Run from the repository root. The engine compiles shaders at run time and does not use this output.
@setlocal
@if not exist shaders\build\passes mkdir shaders\build\passes
@set FLAGS=-I ./shaders/lib -target spirv -profile spirv_1_4 -emit-spirv-directly -force-glsl-scalar-layout -matrix-layout-column-major -fvk-use-entrypoint-name
slangc.exe ./shaders/passes/rasm.slang %FLAGS% -entry vertMain -entry fragMain -o ./shaders/build/passes/rasm.spv
slangc.exe ./shaders/passes/statsOverlay.slang %FLAGS% -entry vertMain -entry fragMain -o ./shaders/build/passes/statsOverlay.spv
slangc.exe ./shaders/passes/cull.slang %FLAGS% -entry countMain -entry writeMain -o ./shaders/build/passes/cull.spv
slangc.exe ./shaders/passes/hiz.slang %FLAGS% -entry hizMain -o ./shaders/build/passes/hiz.spv
