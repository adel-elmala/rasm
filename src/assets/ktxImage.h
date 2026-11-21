// clang-format off
#pragma once

#include "rasm/types.h"
#include "rasm/render/renderContext.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rasm
{
    struct KtxInfo
    {
        uint32_t                    width               = 0;
        uint32_t                    height              = 0;
        uint32_t                    levelCount          = 0;
        uint32_t                    components          = 0;                // 1 to 4, from the data format descriptor
        bool                        basis               = false;            // ETC1S or UASTC: transcoded to a GPU format
        bool                        yInGreen            = false;            // two channels as R and G (UASTC RG), not R and A
        Format                      format              = Format::UNKNOWN;  // a file that is not Basis: its format
    };

    constexpr uint64_t              formatBit(Format format) { return 1ull << static_cast<uint32_t>(format); }

    uint64_t                        getSupportedTextureFormats(const RenderContext& ctx);
    bool                            isKtx2(const unsigned char* bytes, size_t size);
    bool                            readKtxInfo(const std::string& path, KtxInfo& info, std::string& error);
    bool                            readKtxInfo(const unsigned char* bytes, size_t size, KtxInfo& info, std::string& error);
    Format                          chooseKtxFormat(const KtxInfo& info, bool srgb, uint64_t supportedFormats);
    uint32_t                        getKtxMipCount(const KtxInfo& info, Format format);
    void                            warnAboutKtxFormat(const std::string& name, const KtxInfo& info, Format format);
    bool                            decodeKtx(const unsigned char* bytes, size_t size, Format format, TextureRaw& raw, std::string& error);
}
// clang-format on
