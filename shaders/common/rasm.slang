enum VertexFormat : uint8_t { Full, PosOnly };

struct VertexFull       { float3 pos; float3 normal; float2 uv; };
struct VertexPosOnly    { float3 pos; };

struct DrawInfo
{
    uint32_t        indexCount;
    uint32_t        instanceCount;
    uint32_t        firstIndex;
    int32_t         vertexOffset;
    uint32_t        firstInstance;
    VertexFormat    vertFormat;
};

struct Modeldata
{
    float4x4 model[];
};

struct SceneData
{
    float4x4   projection;
    float4x4   view;
    uint64_t   vertsPtr;
    DrawInfo*  drawInfoPtr;
    Modeldata* modeldataPtr;
};

struct PushConstants
{
    SceneData* scene;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

// Define a function to pull the native Vulkan DrawIndex built-in
uint getDrawIndex()
{
    return spirv_asm
    {
        OpExtension "SPV_KHR_shader_draw_parameters";
        OpCapability DrawParameters;
        result:$$uint = OpLoad builtin(DrawIndex:uint);
    };
}

struct VSInput
{
    // SV_VertexID automatically includes the baseVertex offset from the indirect call
    uint vertexID : SV_VertexID;
};

struct VSOutput
{
    float4 Pos : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output = {};
    uint drawID = getDrawIndex();

    float4x4 modelMat = pc.scene->modeldataPtr->model[drawID];
    float4x4 viewMat = pc.scene->view;
    float4x4 projectionMat = pc.scene->projection;

    int vertexOffset = pc.scene->drawInfoPtr[drawID].vertexOffset;
    // switch (pc.scene->drawInfoPtr[drawID].vertFormat)
    // {
    //     case VertexFormat::Full:
    //     {
            VertexFull* inputFull = (VertexFull *)(pc.scene->vertsPtr + vertexOffset);
            VertexFull inputFullVal = inputFull[input.vertexID];
            // output.Pos = mul(projectionMat, mul(viewMat, mul(modelMat, float4(inputFullVal.pos, 1.0))));
            // output.Normal = normalize(mul((float3x3)modelMat, inputFull.normal));
            // output.UV = inputFull.uv;
    //     } break;

    //     case VertexFormat::PosOnly:
    //     {
    //         VertexPosOnly inputPosOnly = *(VertexPosOnly *)(pc.scene->vertsPtr + vertexOffset + (input.vertexID - vertexOffset) * sizeof(VertexPosOnly));
    //         output.Pos = mul(projectionMat, mul(viewMat, mul(modelMat, float4(inputPosOnly.pos, 1.0))));
    //     }
    // }

    return output;
}

[[vk::binding(0, 0)]]
Sampler2D gTextures[];


[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    float3 color = gTextures[6].Sample(input.UV).xyz;
    return float4(color, 1.0);
}
