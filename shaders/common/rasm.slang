enum VertexFormat : uint32_t { Full, PosOnly };

struct VertexFull       { float3 pos; float3 normal; float2 uv; };
struct VertexPosOnly    { float3 pos; };

struct DrawInfo
{
    uint32_t        indexCount;
    uint32_t        instanceCount;
    uint32_t        firstIndex;
    int32_t         vertexOffset;
    uint32_t        firstInstance;
};

struct MetallicRoughnessMaterial
{
    float3      baseColorFactor;
    uint32_t    baseColorTextureIndex;
    uint32_t    metallicRoughnessTextureIndex;
    uint32_t    metallicRoughtnessTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float       metallicFactor;
    float       roughnessFactor;
};

struct NormalMaterial
{
    uint32_t normalTextureIndex;
    uint32_t normalTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float scale;
};

struct OcclusionMaterial
{
    uint32_t occlusionTextureIndex;
    uint32_t occlusionTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float strength;
};

struct EmissiveMaterial
{
    uint32_t emissiveTextureIndex;
    uint32_t emissiveTextureCoordinate; // TEXCOORD_0 / TEXCOORD_1 ,...
    float3   emissiveFactor;
};

struct Material
{
    MetallicRoughnessMaterial   metallicRoughness;
    NormalMaterial              normal;
    OcclusionMaterial           occlusion;
    EmissiveMaterial            emissive;
};

struct Modeldata
{
    float4x4        worldMatrix; 
    Material        material;
    VertexFormat    vertFormat;
};

struct SceneData
{
    float4x4        projection;
    float4x4        view;
    VertexFull*     vertsPtr;
    DrawInfo*       drawInfoPtr;
    Modeldata*      modeldataPtr;
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
    uint drawID;
};

[shader("vertex")]
VSOutput vertMain(VSInput input)
{
    VSOutput output = {};
    uint drawID = getDrawIndex();
    output.drawID = drawID;

    float4x4 modelMat = pc.scene->modeldataPtr[drawID].worldMatrix;
    float4x4 viewMat = pc.scene->view;
    float4x4 projectionMat = pc.scene->projection;

    // Slang lowers SV_VertexID to (VertexIndex - BaseVertex) to match D3D semantics, so it is the raw
    // index-buffer value and the per-draw vertexOffset must be added back to address the mega buffer.
    VertexFull vert = pc.scene->vertsPtr[input.vertexID + pc.scene->drawInfoPtr[drawID].vertexOffset];

    output.Pos = mul(projectionMat, mul(viewMat, mul(modelMat, float4(vert.pos, 1.0))));
    output.Normal = normalize(mul((float3x3)modelMat, vert.normal));
    output.UV = vert.uv;

    return output;
}

[[vk::binding(0, 0)]]
Sampler2D gTextures[];


[shader("fragment")]
float4 fragMain(VSOutput input) : SV_TARGET
{
    float3 color = gTextures[pc.scene->modeldataPtr[input.drawID].material.metallicRoughness.baseColorTextureIndex].Sample(input.UV).xyz;
    return float4(color, 1.0);
}
