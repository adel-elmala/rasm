// clang-format off
#pragma once

#include <string>

namespace rasm
{
    // The variant of the shaded pipelines an export's view draws with: its tile window and pixel scale (core.slang).
    constexpr const char*       EXPORT_VIEW_DEFINE      = "#define RASM_EXPORT_VIEW 1\n";

    std::string                 getShaderVariant(const std::string& defines, const std::string& file);
}
// clang-format on
