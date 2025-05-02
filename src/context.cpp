#include "context.h"
#include "im_font_manager.h"
#include "renderer.h"
#include <chrono>

namespace glimmer
{
    void AnimationData::moveByPixel(float amount, float max, float reset)
    {
        auto currts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock().now().time_since_epoch()).count();
        if (timestamp == 0) timestamp = currts;

        if (currts - timestamp >= 33)
        {
            offset += amount;
            if (offset >= max) offset = reset;
        }
    }
     
    void ItemGridInternalState::swapColumns(int16_t from, int16_t to, std::vector<std::vector<ItemGridState::ColumnConfig>>& headers, int level)
    {
        auto lfrom = colmap[level].vtol[from];
        auto lto = colmap[level].vtol[to];
        colmap[level].vtol[from] = lto; colmap[level].ltov[lfrom] = to;
        colmap[level].vtol[to] = lfrom; colmap[level].ltov[lto] = from;

        std::pair<int16_t, int16_t> movingColRangeFrom = { lfrom, lfrom }, nextMovingRangeFrom = { INT16_MAX, -1 };
        std::pair<int16_t, int16_t> movingColRangeTo = { lto, lto }, nextMovingRangeTo = { INT16_MAX, -1 };

        for (auto l = level + 1; l < (int)headers.size(); ++l)
        {
            for (int16_t col = 0; col < (int16_t)headers[l].size(); ++col)
            {
                auto& hdr = headers[l][col];
                if (hdr.parent >= movingColRangeFrom.first && hdr.parent <= movingColRangeFrom.second)
                {
                    nextMovingRangeFrom.first = std::min(nextMovingRangeFrom.first, col);
                    nextMovingRangeFrom.second = std::max(nextMovingRangeFrom.second, col);
                }
                else if (hdr.parent >= movingColRangeTo.first && hdr.parent <= movingColRangeTo.second)
                {
                    nextMovingRangeTo.first = std::min(nextMovingRangeTo.first, col);
                    nextMovingRangeTo.second = std::max(nextMovingRangeTo.second, col);
                }
            }

            auto startTo = colmap[l].ltov[nextMovingRangeFrom.first];
            auto startFrom = colmap[l].ltov[nextMovingRangeTo.first];

            for (auto col = nextMovingRangeTo.first, idx = startTo; col <= nextMovingRangeTo.second; ++col, ++idx)
            {
                colmap[l].ltov[col] = idx;
                colmap[l].vtol[idx] = col;
            }

            for (auto col = nextMovingRangeFrom.first, idx = startFrom; col <= nextMovingRangeFrom.second; ++col, ++idx)
            {
                colmap[l].ltov[col] = idx;
                colmap[l].vtol[idx] = col;
            }

            movingColRangeFrom = nextMovingRangeFrom;
            movingColRangeTo = nextMovingRangeTo;
            nextMovingRangeFrom = nextMovingRangeTo = { INT16_MAX, -1 };
        }
    }

    StyleDescriptor& WidgetContextData::GetStyle(int32_t state)
    {
        auto style = log2((unsigned)state);
        auto& res = (state & currStyleStates) ? currStyle[style] : pushedStyles[style].top();
        if (res.font.font == nullptr) res.font.font = GetFont(res.font.family, res.font.size, FT_Normal);
        return res;
    }

    void WidgetContextData::ToggleDeferedRendering(bool defer)
    {
        usingDeferred = defer;

        if (defer)
        {
            auto renderer = CreateDeferredRenderer(&ImGuiMeasureText);
            renderer->Reset();
            deferedRenderer = renderer;
            adhocLayout.push();
        }
        else
        {
            adhocLayout.pop();
            deferedRenderer = nullptr;
        }
    }
    
    void WidgetContextData::AddItemGeometry(int id, const ImRect& geometry)
    {
        auto index = id & 0xffff;
        auto wtype = (WidgetType)(id >> 16);
        itemGeometries[wtype][index] = geometry;
        adhocLayout.top().lastItemId = id;

        if (currSpanDepth > 0 && spans[currSpanDepth].popWhenUsed)
        {
            spans[currSpanDepth] = ElementSpan{};
            --currSpanDepth;
        }
    }

    const ImRect& WidgetContextData::GetGeometry(int32_t id) const
    {
        auto index = id & 0xffff;
        auto wtype = (WidgetType)(id >> 16);
        return itemGeometries[wtype][index];
    }
    
    WidgetContextData::WidgetContextData()
    {
        std::memset(maxids, 0, WT_TotalTypes);
        std::memset(tempids, INT_MAX, WT_TotalTypes);
        constexpr auto totalStyles = 16 * WSI_Total;

        for (auto idx = 0; idx < WSI_Total; ++idx)
        {
            pushedStyles[idx].push();
            radioButtonStyles[idx].push();
            auto& toggle = toggleButtonStyles[idx].push();
            toggle.fontsz *= Config.fontScaling;

            if (idx != WSI_Checked)
            {
                toggle.trackColor = ToRGBA(200, 200, 200);
                toggle.indicatorTextColor = ToRGBA(100, 100, 100);
            }
            else
            {
                toggle.trackColor = ToRGBA(152, 251, 152);
                toggle.indicatorTextColor = ToRGBA(0, 100, 0);
            }
        }

        gridStates.resize(8);
        tabStates.resize(16);
        toggleStates.resize(32);
        radioStates.resize(32);
        inputTextStates.resize(32);

        for (auto idx = 0; idx < WT_TotalTypes; ++idx)
        {
            maxids[idx] = 0;
            WidgetStateData data{ (WidgetType)idx };
            states[idx].resize(32, data);
            itemGeometries[idx].resize(32, ImRect{ { 0.f, 0.f },{ 0.f, 0.f } });
        }

        KeyMappings.resize(1024, { 0, 0 });
        KeyMappings[ImGuiKey_0] = { '0', ')' }; KeyMappings[ImGuiKey_1] = { '1', '!' }; KeyMappings[ImGuiKey_2] = { '2', '@' };
        KeyMappings[ImGuiKey_3] = { '3', '#' }; KeyMappings[ImGuiKey_4] = { '4', '$' }; KeyMappings[ImGuiKey_5] = { '5', '%' };
        KeyMappings[ImGuiKey_6] = { '6', '^' }; KeyMappings[ImGuiKey_7] = { '7', '&' }; KeyMappings[ImGuiKey_8] = { '8', '*' };
        KeyMappings[ImGuiKey_9] = { '9', '(' };

        KeyMappings[ImGuiKey_A] = { 'A', 'a' }; KeyMappings[ImGuiKey_B] = { 'B', 'b' }; KeyMappings[ImGuiKey_C] = { 'C', 'c' };
        KeyMappings[ImGuiKey_D] = { 'D', 'd' }; KeyMappings[ImGuiKey_E] = { 'E', 'e' }; KeyMappings[ImGuiKey_F] = { 'F', 'f' };
        KeyMappings[ImGuiKey_G] = { 'G', 'g' }; KeyMappings[ImGuiKey_H] = { 'H', 'h' }; KeyMappings[ImGuiKey_I] = { 'I', 'i' };
        KeyMappings[ImGuiKey_J] = { 'J', 'j' }; KeyMappings[ImGuiKey_K] = { 'K', 'k' }; KeyMappings[ImGuiKey_L] = { 'L', 'l' };
        KeyMappings[ImGuiKey_M] = { 'M', 'm' }; KeyMappings[ImGuiKey_N] = { 'N', 'n' }; KeyMappings[ImGuiKey_O] = { 'O', 'o' };
        KeyMappings[ImGuiKey_P] = { 'P', 'p' }; KeyMappings[ImGuiKey_Q] = { 'Q', 'q' }; KeyMappings[ImGuiKey_R] = { 'R', 'r' };
        KeyMappings[ImGuiKey_S] = { 'S', 's' }; KeyMappings[ImGuiKey_T] = { 'T', 't' }; KeyMappings[ImGuiKey_U] = { 'U', 'u' };
        KeyMappings[ImGuiKey_V] = { 'V', 'v' }; KeyMappings[ImGuiKey_W] = { 'W', 'w' }; KeyMappings[ImGuiKey_X] = { 'X', 'x' };
        KeyMappings[ImGuiKey_Y] = { 'Y', 'y' }; KeyMappings[ImGuiKey_Z] = { 'Z', 'z' };

        KeyMappings[ImGuiKey_Apostrophe] = { '\'', '"' }; KeyMappings[ImGuiKey_Backslash] = { '\\', '|' };
        KeyMappings[ImGuiKey_Slash] = { '/', '?' }; KeyMappings[ImGuiKey_Comma] = { ',', '<' };
        KeyMappings[ImGuiKey_Minus] = { '-', '_' }; KeyMappings[ImGuiKey_Period] = { '.', '>' };
        KeyMappings[ImGuiKey_Semicolon] = { ';', ':' }; KeyMappings[ImGuiKey_Equal] = { '=', '+' };
        KeyMappings[ImGuiKey_LeftBracket] = { '[', '{' }; KeyMappings[ImGuiKey_RightBracket] = { ']', '}' };
        KeyMappings[ImGuiKey_Space] = { ' ', ' ' }; KeyMappings[ImGuiKey_Tab] = { '\t', '\t' };
        KeyMappings[ImGuiKey_GraveAccent] = { '`', '~' };

        // TODO: Add mappings for keypad buttons...
    }
}