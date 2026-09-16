struct VSInput
{
    float3 Pos;
    float3 Normal;
    float2 UV;
};

struct ShaderData
{
    float4x4 projection;
    float4x4 view;
    float4x4 model;
};

struct VSOutput
{
    float4 Pos : SV_POSITION;
    float3 Normal : NORMAL;
    float2 UV : TEXCOORD0;
};

[shader("vertex")]
VSOutput vertMain(VSInput input, uniform ShaderData *shaderData)
{
    VSOutput output;
    float4x4 modelMat = shaderData->model;
    float4x4 viewMat = shaderData->view;
    float4x4 projectionMat = shaderData->projection;

    output.Pos = mul(projectionMat, mul(viewMat, mul(modelMat, float4(input.Pos.xyz, 1.0))));
    output.Normal = normalize(mul((float3x3)modelMat, input.Normal));
    output.UV = input.UV;

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
