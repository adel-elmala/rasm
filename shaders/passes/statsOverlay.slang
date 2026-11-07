// clang-format off
// The stats overlay: an opaque panel in the top-left corner with a grid of characters in an 8x8 bitmap font.
// Two triangles cover the panel, and the fragment shader looks up each pixel's character and glyph bit.

struct PushConstants
{
    uint*   textPtr;    // columns * rows characters, row by row, 4 per uint with the first in the low byte
    uint    grid;       // columns | rows << 8 | scale << 16, scale being screen pixels per font pixel
    uint    screen;     // width | height << 16, in pixels
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> pc;

static const uint   GLYPH_SIZE  = 8;
static const uint   MARGIN      = 8;    // screen pixels between the window corner and the panel
static const uint   PADDING     = 4;    // font pixels between the panel's edge and the text
static const uint   FIRST_CHAR  = 33;   // '!'; the font has no glyph for the space before it
static const uint   LAST_CHAR   = 126;  // '~'
static const float3 BACKGROUND  = float3(0.004, 0.004, 0.006);
static const float3 FOREGROUND  = float3(0.85, 0.85, 0.8);

// The IBM VGA 8x8 font (public domain), from '!' to '~'. Each glyph is two uints of four rows each, the top row
// in the low byte of the first, and in each row the lowest bit is the leftmost pixel.
static const uint FONT[188] =
{
    0x183C3C18, 0x00180018, 0x00003636, 0x00000000, 0x367F3636, 0x0036367F, 0x1E033E0C, 0x000C1F30,  // ! " # $
    0x18336300, 0x0063660C, 0x6E1C361C, 0x006E333B, 0x00030606, 0x00000000, 0x06060C18, 0x00180C06,  // % & ' (
    0x18180C06, 0x00060C18, 0xFF3C6600, 0x0000663C, 0x3F0C0C00, 0x00000C0C, 0x00000000, 0x060C0C00,  // ) * + ,
    0x3F000000, 0x00000000, 0x00000000, 0x000C0C00, 0x0C183060, 0x00010306, 0x7B73633E, 0x003E676F,  // - . / 0
    0x0C0C0E0C, 0x003F0C0C, 0x1C30331E, 0x003F3306, 0x1C30331E, 0x001E3330, 0x33363C38, 0x0078307F,  // 1 2 3 4
    0x301F033F, 0x001E3330, 0x1F03061C, 0x001E3333, 0x1830333F, 0x000C0C0C, 0x1E33331E, 0x001E3333,  // 5 6 7 8
    0x3E33331E, 0x000E1830, 0x000C0C00, 0x000C0C00, 0x000C0C00, 0x060C0C00, 0x03060C18, 0x00180C06,  // 9 : ; <
    0x003F0000, 0x00003F00, 0x30180C06, 0x00060C18, 0x1830331E, 0x000C000C, 0x7B7B633E, 0x001E037B,  // = > ? @
    0x33331E0C, 0x0033333F, 0x3E66663F, 0x003F6666, 0x0303663C, 0x003C6603, 0x6666361F, 0x001F3666,  // A B C D
    0x1E16467F, 0x007F4616, 0x1E16467F, 0x000F0616, 0x0303663C, 0x007C6673, 0x3F333333, 0x00333333,  // E F G H
    0x0C0C0C1E, 0x001E0C0C, 0x30303078, 0x001E3333, 0x1E366667, 0x00676636, 0x0606060F, 0x007F6646,  // I J K L
    0x7F7F7763, 0x0063636B, 0x7B6F6763, 0x00636373, 0x6363361C, 0x001C3663, 0x3E66663F, 0x000F0606,  // M N O P
    0x3333331E, 0x00381E3B, 0x3E66663F, 0x00676636, 0x0E07331E, 0x001E3338, 0x0C0C2D3F, 0x001E0C0C,  // Q R S T
    0x33333333, 0x003F3333, 0x33333333, 0x000C1E33, 0x6B636363, 0x0063777F, 0x1C366363, 0x0063361C,  // U V W X
    0x1E333333, 0x001E0C0C, 0x1831637F, 0x007F664C, 0x0606061E, 0x001E0606, 0x180C0603, 0x00406030,  // Y Z [ backslash
    0x1818181E, 0x001E1818, 0x63361C08, 0x00000000, 0x00000000, 0xFF000000, 0x00180C0C, 0x00000000,  // ] ^ _ `
    0x301E0000, 0x006E333E, 0x3E060607, 0x003B6666, 0x331E0000, 0x001E3303, 0x3E303038, 0x006E3333,  // a b c d
    0x331E0000, 0x001E033F, 0x0F06361C, 0x000F0606, 0x336E0000, 0x1F303E33, 0x6E360607, 0x00676666,  // e f g h
    0x0C0E000C, 0x001E0C0C, 0x30300030, 0x1E333330, 0x36660607, 0x0067361E, 0x0C0C0C0E, 0x001E0C0C,  // i j k l
    0x7F330000, 0x00636B7F, 0x331F0000, 0x00333333, 0x331E0000, 0x001E3333, 0x663B0000, 0x0F063E66,  // m n o p
    0x336E0000, 0x78303E33, 0x6E3B0000, 0x000F0666, 0x033E0000, 0x001F301E, 0x0C3E0C08, 0x00182C0C,  // q r s t
    0x33330000, 0x006E3333, 0x33330000, 0x000C1E33, 0x6B630000, 0x00367F7F, 0x36630000, 0x0063361C,  // u v w x
    0x33330000, 0x1F303E33, 0x193F0000, 0x003F260C, 0x070C0C38, 0x00380C0C, 0x00181818, 0x00181818,  // y z { |
    0x380C0C07, 0x00070C0C, 0x00003B6E, 0x00000000,                                                  // } ~
};

uint getColumns()   { return pc.grid & 0xFF; }
uint getRows()      { return (pc.grid >> 8) & 0xFF; }
uint getScale()     { return pc.grid >> 16; }

[shader("vertex")]
float4 vertMain(uint vertexID : SV_VertexID) : SV_Position
{
    static const uint2 CORNERS[6] = { uint2(0, 0), uint2(1, 0), uint2(0, 1), uint2(0, 1), uint2(1, 0), uint2(1, 1) };

    float2 screen   = float2(pc.screen & 0xFFFF, pc.screen >> 16);
    float2 size     = (float2(getColumns(), getRows()) * GLYPH_SIZE + 2 * PADDING) * getScale();
    float2 pixel    = MARGIN + CORNERS[vertexID] * size;
    // Clip space is Y-up and pixel rows go down. Depth 1 is the near plane under reversed-Z, so the panel
    // passes the depth test over anything the pass drew.
    return float4(pixel.x / screen.x * 2.0 - 1.0, 1.0 - pixel.y / screen.y * 2.0, 1.0, 1.0);
}

[shader("fragment")]
float4 fragMain(float4 position : SV_Position) : SV_Target
{
    uint scale      = getScale();
    int2 textPixel  = int2(position.xy) - int2(MARGIN + PADDING * scale);
    if (any(textPixel < 0))
        return float4(BACKGROUND, 1.0);

    uint2 fontPixel = uint2(textPixel) / scale;
    uint2 cell      = fontPixel / GLYPH_SIZE;
    if (cell.x >= getColumns() || cell.y >= getRows())
        return float4(BACKGROUND, 1.0);

    uint index      = cell.y * getColumns() + cell.x;
    uint character  = (pc.textPtr[index / 4] >> ((index % 4) * 8)) & 0xFF;
    if (character < FIRST_CHAR || character > LAST_CHAR)
        return float4(BACKGROUND, 1.0);

    uint2 glyphPixel    = fontPixel % GLYPH_SIZE;
    uint rows           = FONT[(character - FIRST_CHAR) * 2 + glyphPixel.y / 4];
    uint row            = (rows >> ((glyphPixel.y % 4) * 8)) & 0xFF;
    bool lit            = ((row >> glyphPixel.x) & 1) != 0;
    return float4(lit ? FOREGROUND : BACKGROUND, 1.0);
}
// clang-format on
