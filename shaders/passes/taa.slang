// clang-format off
// Temporal anti-aliasing (src/renderer/taa.cpp): each thread blends one pixel of this frame, drawn with a sub-pixel
// jitter, into the history of the frames before, found through the pixel's motion. The history is clipped to the
// colors around the pixel this frame, so what the history saw that is no longer there (a surface that moved away, a
// disocclusion) is cut back. The blend works on colors compressed by c / (1 + luminance), so a bright sample cannot
// outweigh the others (Karis's weights), and turns the result back to HDR.

// Must match gfx::TaaPushConstants in src/renderer/uberMaterialTypes.h; validateRasmStructs checks it by name.
struct TaaPushConstants
{
    uint32_t    colorAndMotion;     // bindless indices: this frame's color (low 16 bits), its motion (high 16)
    uint32_t    depthAndHistory;    // bindless indices: this frame's depth (low 16), the history's last frame (high 16)
    uint32_t    targetAndReset;     // storage slot of this frame's history (low 16); bit 16: drop the history; bit 17: no sky
    float       exposure;           // the camera's, so "bright" means bright on screen
};

[[vk::push_constant]]
ConstantBuffer<TaaPushConstants> pc;

[[vk::binding(0, 0)]]
Sampler2D gTextures[];

[[vk::binding(1, 0)]]
[format("rgba16f")]
RWTexture2DArray<float4> gStorageImages[];

// The share of this frame in the blend: about 10 frames of history at rest, fewer when the pixel moves fast, where the
// history is resampled more and blurs.
static const float  CURRENT_WEIGHT_STILL    = 0.1;
static const float  CURRENT_WEIGHT_MOVING   = 0.3;
static const float  MOVING_PIXELS           = 8.0;
// The history is clipped to the 3x3 colors' mean plus or minus this many standard deviations.
static const float  CLIP_SIGMAS             = 1.0;

float getLuma(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

// Reversible: compressed colors stay below 1 / exposure in luminance.
float3 compress(float3 color)
{
    return color / (1.0 + getLuma(color) * pc.exposure);
}

float3 decompress(float3 color)
{
    return color / max(1.0 - getLuma(color) * pc.exposure, 1e-4);
}

float3 toYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b, -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 fromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

// The history at uv with a Catmull-Rom filter: sharper than bilinear, so resampling every frame does not blur. Five
// bilinear taps instead of 16 loads (the corner taps of the 4x4 weigh little and are left out).
float3 sampleHistory(Sampler2D history, float2 uv, float2 size)
{
    float2 position = uv * size;
    float2 center   = floor(position - 0.5) + 0.5;
    float2 f        = position - center;
    float2 w0       = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1       = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2       = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3       = f * f * (-0.5 + 0.5 * f);
    float2 w12      = w1 + w2;
    float2 p0       = (center - 1.0) / size;
    float2 p3       = (center + 2.0) / size;
    float2 p12      = (center + w2 / w12) / size;
    float3 sum      = history.SampleLevel(float2(p12.x, p0.y), 0.0).rgb * (w12.x * w0.y)
                    + history.SampleLevel(float2(p0.x, p12.y), 0.0).rgb * (w0.x * w12.y)
                    + history.SampleLevel(p12, 0.0).rgb * (w12.x * w12.y)
                    + history.SampleLevel(float2(p3.x, p12.y), 0.0).rgb * (w3.x * w12.y)
                    + history.SampleLevel(float2(p12.x, p3.y), 0.0).rgb * (w12.x * w3.y);
    float weights   = w12.x * w0.y + w0.x * w12.y + w12.x * w12.y + w3.x * w12.y + w12.x * w3.y;
    return max(sum / weights, 0.0);
}

// Moves the history toward the box's center until it is inside the box (Playdead's clip), which keeps its direction
// from the mean, unlike a clamp per channel.
float3 clipToBox(float3 history, float3 boxMin, float3 boxMax)
{
    float3 center   = 0.5 * (boxMax + boxMin);
    float3 extent   = 0.5 * (boxMax - boxMin) + 1e-5;
    float3 offset   = history - center;
    float3 units    = abs(offset / extent);
    float largest   = max(units.x, max(units.y, units.z));
    return largest > 1.0 ? center + offset / largest : history;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void taaMain(uint3 id : SV_DispatchThreadID)
{
    uint target     = pc.targetAndReset & 0xFFFF;
    uint width, height, layers;
    gStorageImages[target].GetDimensions(width, height, layers);
    if (id.x >= width || id.y >= height)
        return;
    Sampler2D color = gTextures[pc.colorAndMotion & 0xFFFF];
    Sampler2D motion = gTextures[pc.colorAndMotion >> 16];
    Sampler2D depth = gTextures[pc.depthAndHistory & 0xFFFF];
    int2 pixel      = int2(id.xy);
    int2 last       = int2(width, height) - 1;

    // The 3x3 colors' mean and spread, and the nearest surface (reversed-Z: the largest depth), whose motion an edge
    // pixel takes, so an object's outline moves with it and not with what is behind it.
    float3 sum      = float3(0.0);
    float3 squares  = float3(0.0);
    float3 current  = float3(0.0);
    float nearest   = -1.0;
    int2 nearestPixel = pixel;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 neighbor = clamp(pixel + int2(x, y), int2(0), last);
            float3 c    = toYCoCg(compress(max(color.Load(int3(neighbor, 0)).rgb, 0.0)));
            sum        += c;
            squares    += c * c;
            if (x == 0 && y == 0)
                current = c;
            float d     = depth.Load(int3(neighbor, 0)).r;
            if (d > nearest)
            {
                nearest = d;
                nearestPixel = neighbor;
            }
        }
    }

    float2 size     = float2(width, height);
    float2 uv       = (float2(pixel) + 0.5) / size;
    float2 pixelMotion = motion.Load(int3(nearestPixel, 0)).xy;
    // Without a sky, a pixel with nothing drawn around it (reversed-Z: depth 0) holds the clear color in both
    // targets, which is no motion: the background stays where it is.
    if ((pc.targetAndReset & 0x20000) != 0 && nearest <= 0.0)
        pixelMotion = float2(0.0);
    float2 previousUV = uv - pixelMotion;
    bool reset      = (pc.targetAndReset & 0x10000) != 0;
    if (reset || any(previousUV < 0.0) || any(previousUV > 1.0))
    {
        gStorageImages[target][uint3(id.xy, 0)] = float4(decompress(fromYCoCg(current)), 1.0);
        return;
    }

    float3 mean     = sum / 9.0;
    float3 sigma    = sqrt(max(squares / 9.0 - mean * mean, 0.0));
    float3 history  = toYCoCg(compress(sampleHistory(gTextures[pc.depthAndHistory >> 16], previousUV, size)));
    history         = clipToBox(history, mean - CLIP_SIGMAS * sigma, mean + CLIP_SIGMAS * sigma);

    float currentWeight = lerp(CURRENT_WEIGHT_STILL, CURRENT_WEIGHT_MOVING, saturate(length(pixelMotion * size) / MOVING_PIXELS));
    float3 blended  = lerp(history, current, currentWeight);
    gStorageImages[target][uint3(id.xy, 0)] = float4(decompress(fromYCoCg(blended)), 1.0);
}

// clang-format on
