#pragma once

#include <string_view>
#include <optional>
#include <vector>


#ifndef IM_RICHTEXT_DEFAULT_FONTFAMILY
#define IM_RICHTEXT_DEFAULT_FONTFAMILY "default-font-family"
#endif
#ifndef IM_RICHTEXT_MONOSPACE_FONTFAMILY
#define IM_RICHTEXT_MONOSPACE_FONTFAMILY "monospace-family"
#endif

namespace glimmer
{
    // =============================================================================================
    // FONT MANAGEMENT
    // =============================================================================================
    enum FontType
    {
        FT_Normal, FT_Light, FT_Bold, FT_Italics, FT_BoldItalics, FT_Total
    };

    // Determines which UTF-8 characters are present in provided rich text
    // NOTE: Irrespective of the enum value, text is expected to be UTF-8 encoded
    enum class TextContentCharset
    {
        ASCII,        // Standard ASCII characters (0-127)
        ASCIISymbols, // Extended ASCII + certain common characters i.e. math symbols, arrows, ™, etc.
        UTF8Simple,   // Simple UTF8 encoded text without support for GPOS/kerning/ligatures (libgrapheme)
        UnicodeBidir  // Standard compliant Unicode BiDir algorithm implementation (Harfbuzz)
    };

    struct FontCollectionFile
    {
        std::string_view Files[FT_Total];
    };

    struct FontFileNames
    {
        FontCollectionFile Proportional;
        FontCollectionFile Monospace;
        std::string_view BasePath;
    };

    struct RenderConfig;

    enum FontLoadType : uint64_t
    {
        FLT_Proportional = 1,
        FLT_Monospace = 2,

        // Use this to auto-scale fonts, loading the largest size for a family
        // NOTE: For ImGui backend, this will save on memory from texture
        FLT_AutoScale = 2048,

        // TODO: Handle absolute size font-size fonts (Look at imrichtext.cpp: PopulateSegmentStyle function)
    };

    struct FontDescriptor
    {
        std::optional<FontFileNames> names = std::nullopt;
        std::vector<float> sizes;
        TextContentCharset charset = TextContentCharset::ASCII;
        uint64_t flags = FLT_Proportional;
    };

    // Load default fonts based on provided descriptor. Custom paths can also be 
    // specified through FontDescriptor::names member. If not specified, a OS specific
    // default path is selected i.e. C:\Windows\Fonts for Windows and 
    // /usr/share/fonts/ for Linux.
    bool LoadDefaultFonts(const FontDescriptor* descriptors, int totalNames = 1);

    // Find out path to .ttf file for specified font family and font type
    // The exact filepath returned is based on reading TTF OS/2 and name tables
    // The matching is done on best effort basis.
    // NOTE: This is not a replacement of fontconfig library and can only perform
    //       rudimentary font fallback
    [[nodiscard]] std::string_view FindFontFile(std::string_view family, FontType ft,
        std::string_view* lookupPaths = nullptr, int lookupSz = 0);

    //struct GlyphRangeMappedFont
    //{
    //    std::pair<int32_t, int32_t> Range;
    //    std::string_view FontPath;
    //};

    //// Find a set of fonts which match the glyph ranges provided through the charset
    //[[nodiscard]] std::vector<GlyphRangeMappedFont> FindFontFileEx(std::string_view family, 
    //    FontType ft, TextContentCharset charset, std::string_view* lookupPaths = nullptr, 
    //    int lookupSz = 0);

#ifdef IM_RICHTEXT_TARGET_IMGUI
    // Get the closest matching font based on provided parameters. The return type is
    // ImFont* cast to void* to better fit overall library.
    // NOTE: size matching happens with lower_bound calls, this is done because all fonts
    //       should be preloaded for ImGui, dynamic font atlas updates are not supported.
    [[nodiscard]] void* GetFont(std::string_view family, float size, FontType type);

    // Returns whether font is monospaced or proportional
    [[nodiscard]] bool IsFontMonospace(void* font);

    //#ifndef IM_FONTMANAGER_STANDALONE
    //    // Get font to display overlay i.e. style info in side panel
    //    [[nodiscard]] void* GetOverlayFont(const RenderConfig& config);
    //#endif

        // Return status of font atlas construction
    [[nodiscard]] bool IsFontLoaded();
#endif
#ifdef IM_RICHTEXT_TARGET_BLEND2D
    using FontFamilyToFileMapper = std::string_view(*)(std::string_view);
    struct FontExtraInfo { FontFamilyToFileMapper mapper = nullptr; std::string_view filepath; };

    // Preload font lookup info which is used in FindFontFile and GetFont function to perform
    // fast lookup + rudimentary fallback.
    void PreloadFontLookupInfo(int timeoutMs);

    // Get the closest matching font based on provided parameters. The return type is
    // BLFont* cast to void* to better fit overall library.
    // NOTE: The FontExtraInfo::mapper can be assigned to a function which loads fonts based
    //       con content codepoints and can perform better fallback.
    [[nodiscard]] void* GetFont(std::string_view family, float size, FontType type, FontExtraInfo extra);
#endif
}