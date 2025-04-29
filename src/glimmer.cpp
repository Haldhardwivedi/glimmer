#include "glimmer.h"

#define _USE_MATH_DEFINES
#include <math.h>

#ifdef IM_RICHTEXT_TARGET_IMGUI
#include "imgui.h"
#endif
#ifdef IM_RICHTEXT_TARGET_BLEND2D
#include "blend2d.h"
#endif

#include <cstring>
#include <unordered_map>
#include <chrono>
#include <cassert>
#include <bit>
#include <cstdlib>
#include <span>

#ifdef _DEBUG
#define LOG(FMT, ...) std::fprintf(stderr, FMT, __VA_ARGS__)
#define HIGHLIGHT(FMT, ...) std::fprintf(stderr, "\x1B[93m" FMT "\x1B[0m", __VA_ARGS__)
#define ERROR(FMT, ...) std::fprintf(stderr, "\x1B[31m" FMT "\x1B[0m", __VA_ARGS__)
#else
#define LOG(FMT, ...)
#define HIGHLIGHT(FMT, ...)
#define ERROR(FMT, ...)
#endif

template <typename T>
T clamp(T val, T min, T max)
{
    return val > max ? max : val < min ? min : val;
}

namespace glimmer
{
    // =============================================================================================
    // STATIC DATA
    // =============================================================================================

    static WindowConfig Config{};

    struct AnimationData
    {
        int32_t elements = 0;
        int32_t types = 0;
        ImGuiDir direction = ImGuiDir::ImGuiDir_Right;
        float offset = 0.f;
        long long timestamp = 0;

        void moveByPixel(float amount, float max, float reset)
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
    };

    constexpr int AnimationsPreallocSz = 32;
    constexpr int ShadowsPreallocSz = 64;
    constexpr int FontStylePreallocSz = 64;
    constexpr int BorderPreallocSz = 64;
    constexpr int GradientPreallocSz = 64;

    inline int log2(auto i) { return i <= 0 ? 0 : 8 * sizeof(i) - std::countl_zero(i) - 1; }

    struct ToggleButtonStyleDescriptor
    {
        uint32_t trackColor;
        uint32_t trackBorderColor;
        uint32_t thumbColor;
        int32_t trackGradient = -1;
        int32_t indicatorTextColor; // Text color of ON/OFF text
        float trackBorderThickness = 0.f;
        float thumbOffset = -2.f;
        float thumbExpand = 0.f;
        float animate = 0.3f;
        bool showText = true; // Show ON/OFF text inside button
        void* fontptr = nullptr;
        float fontsz = 12.f;
    };

    struct RadioButtonStyleDescriptor
    {
        uint32_t checkedColor = ToRGBA(0, 0, 0);
        uint32_t outlineColor = ToRGBA(0, 0, 0);
        float outlineThickness = 2.f;
        float checkedRadius = 0.6f; // relative to total
        float animate = 0.3f;
    };

    union CommonWidgetStyleDescriptor
    {
        ToggleButtonStyleDescriptor toggle;

        CommonWidgetStyleDescriptor() {}
    };

    struct LayoutItemDescriptor
    {
        WidgetType wtype = WidgetType::WT_Invalid;
        int32_t id = -1;
        ImRect border, margin, padding, content, text;
        //ImVec2 viewport;
        int32_t sizing = 0;
        int16_t row = 0, col = 0;
        int16_t from = -1, to = -1;
        bool isComputed = false;
        // keep adding members
    };

    struct LayoutDescriptor
    {
        Layout type = Layout::Invalid;
        int32_t fill = FD_None;
        int32_t alignment = TextAlignLeading;
        int16_t from = -1, to = -1, itemidx = -1;
        int16_t currow = -1, currcol = -1;
        int32_t sizing;
        ImRect geometry{ { FIT_SZ, FIT_SZ }, { FIT_SZ, FIT_SZ } };
        ImVec2 nextpos{ 0.f, 0.f }, prevpos{ 0.f, 0.f };
        ImVec2 spacing{ 0.f, 0.f };
        ImVec2 maxdim{ 0.f, 0.f };
        ImVec2 cumulative{ 0.f, 0.f };
        ImVec2 rows[8];
        ImVec2 cols[8];
        OverflowMode hofmode = OverflowMode::Scroll;
        OverflowMode vofmode = OverflowMode::Scroll;
        FourSidedBorder border;
        bool popSizingOnEnd = false;
    };

    struct Sizing
    {
        float horizontal = FIT_SZ;
        float vertical = FIT_SZ;
        bool relativeh = false;
        bool relativev = false;
    };

    struct ElementSpan
    {
        int32_t direction = 0;
        bool popWhenUsed = false;
    };

    struct ItemGridStyleDescriptor
    {
        uint32_t gridcolor = IM_COL32(100, 100, 100, 255);
    };

#ifndef GLIMMER_MAX_LAYOUT_NESTING 
#define GLIMMER_MAX_LAYOUT_NESTING 16
#endif

    enum WidgetStateIndex
    {
        WSI_Default, WSI_Focused, WSI_Hovered, WSI_Pressed, WSI_Checked, WSI_Disabled,
        WSI_PartiallyChecked, WSI_Selected,
        WSI_Total
    };

    template <typename T, typename Sz, Sz blocksz = 128>
    struct Vector
    {
        template <typename T, typename S, S v> friend struct DynamicStack;

        static_assert(blocksz > 0, "Block size has to non-zero");
        static_assert(std::is_integral_v<Sz>, "Sz must be integral type");
        static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");
        static_assert(!std::is_same_v<T, bool>, "T must not be bool, consider using std::bitset");

        using value_type = T;
        using size_type = Sz;
        using difference_type = std::conditional_t<std::is_unsigned_v<Sz>, std::make_signed_t<Sz>, Sz>;
        using Iterator = T*;

        /*struct Iterator
        {
            Sz current = 0;
            Vector& parent;

            using DiffT = std::make_signed_t<Sz>;

            Iterator(Vector& p, Sz curr) : current{ curr }, parent{ p } {}
            Iterator(const Iterator& copy) : current{ copy.current }, parent{ copy.parent } {}

            Iterator& operator++() { current++; return *this; }
            Iterator& operator+=(Sz idx) { current += idx; return *this; }
            Iterator operator++(int) { auto temp = *this; current++; return temp; }
            Iterator operator+(Sz idx) { auto temp = *this; temp.current += idx; return temp; }

            Iterator& operator--() { current--; return *this; }
            Iterator& operator-=(Sz idx) { current -= idx; return *this; }
            Iterator operator--(int) { auto temp = *this; current--; return temp; }
            Iterator operator-(Sz idx) { auto temp = *this; temp.current -= idx; return temp; }

            difference_type operator-(const Iterator& other) const 
            { return (difference_type)current - (difference_type)other.current; }

            bool operator!=(const Iterator& other) const 
            { return current != other.current || parent._data != other.parent._data; }

            bool operator<(const Iterator& other) const { current < other.current; }

            T& operator*() { return parent._data[current]; }
            T* operator->() { return parent._data + current; }
        };*/

        ~Vector() 
        { 
            if constexpr (std::is_destructible_v<T>) for (auto idx = 0; idx < _size; ++idx) _data[idx].~T();
            std::free(_data);
        }

        Vector(Sz initialsz = blocksz) 
            : _capacity{ initialsz }, _data{ (T*)std::malloc(sizeof(T) * initialsz) }
        { 
            _default_init(0, _capacity);
        }

        Vector(Sz initialsz, const T& el)
            : _capacity{ initialsz }, _size{ initialsz }, _data{ (T*)std::malloc(sizeof(T) * initialsz) }
        {
            std::fill(_data, _data + _capacity, el);
        }

        void resize(Sz count, bool initialize = true)
        {
            if (_data == nullptr)
            {
                _data = (T*)std::malloc(sizeof(T) * count);
            }
            else if (_capacity < count)
            {
                auto ptr = (T*)std::realloc(_data, sizeof(T) * count);
                if (ptr != _data) std::memmove(ptr, _data, sizeof(T) * _size);
                _data = ptr;
            }

            if (initialize) _default_init(_data + _size, _data + count);
            _size = _capacity = count;
        }

        void resize(Sz count, const T& el)
        {
            if (_data == nullptr)
            {
                _data = (T*)std::malloc(sizeof(T) * count);
            }
            else if (_capacity < count)
            {
                auto ptr = (T*)std::realloc(_data, sizeof(T) * count);
                if (ptr != _data) std::memmove(ptr, _data, sizeof(T) * _size);
                _data = ptr;
            }
            
            std::fill(_data + _size, _data + count, el);
            _size = _capacity = count;
        }

        void fill(const T& el)
        {
            std::fill(_data + _size, _data + _capacity, el);
            _size = _capacity;
        }

        void expand(Sz count, bool initialize = true)
        {
            auto targetsz = _size + count;

            if (_capacity < targetsz)
            {
                auto ptr = (T*)std::realloc(_data, sizeof(T) * targetsz);
                if (ptr != _data) std::memmove(ptr, _data, sizeof(T) * _size);
                _data = ptr;
                if (initialize) _default_init(_size, targetsz);
            }
        }

        template <typename... ArgsT>
        T& emplace_back(ArgsT&&... args) 
        { 
            _reallocate(true); 
            ::new(_data + _size) T{ std::forward<ArgsT>(args)... }; 
            _size++;
            return _data[_size - 1];
        }

        void push_back(const T& el) { _reallocate(true); _data[_size] = el; _size++; }
        void pop_back() { if constexpr (std::is_default_constructible_v<T>) _data[_size - 1] = T{}; --_size; }
        void clear() { _default_init(0, _size); _size = 0; }
        void reset(const T& el) { std::fill(_data, _data + _size, el); }
        void shrink_to_fit() { _data = (T*)std::realloc(_data, _size * sizeof(T)); }

        Iterator begin() { return _data; }
        Iterator end() { return _data + _size; }

        const T& operator[](Sz idx) const { assert(idx < _size); return _data[idx]; }
        T& operator[](Sz idx) { assert(idx < _size); return _data[idx]; }

        T& front() { assert(_size > 0); return _data[0]; }
        T& back() { assert(_size > 0); return _data[_size - 1]; }
        T* data() { return _data; }

        Sz size() const { return _size; }
        Sz capacity() const { return _capacity; }
        bool empty() const { return _size == 0; }
        std::span<T> span() const { return std::span<T>{ _data, _size }; }

    private:

        void _default_init(Sz from, Sz to)
        {
            if constexpr (std::is_default_constructible_v<T> && !std::is_scalar_v<T>)
                std::fill(_data + from, _data + to, T{});
            else if constexpr (std::is_arithmetic_v<T>)
                std::fill(_data + from, _data + to, T{ 0 });
        }

        void _reallocate(bool initialize)
        {
            T* ptr = nullptr;
            if (_size == _capacity) ptr = (T*)std::realloc(_data, (_capacity + blocksz) * sizeof(T));

            if (ptr != nullptr)
            {
                if (ptr != _data) std::memmove(ptr, _data, sizeof(T) * _capacity);
                _data = ptr;
                if (initialize) _default_init(_capacity, _capacity + blocksz);
                _capacity += blocksz;
            }
        }

        T* _data = nullptr;
        Sz _size = 0;
        Sz _capacity = 0;
    };

    template <typename T, int capacity> 
    struct Stack
    {
        T* _data = nullptr;
        int _size = 0;

        static_assert(capacity > 0, "capacity has to be a +ve value");

        Stack()
        {
            _data = (T*)std::malloc(sizeof(T) * capacity);
            if constexpr (std::is_default_constructible_v<T>)
                std::fill(_data, _data + capacity, T{});
        }

        ~Stack() 
        { 
            if constexpr (std::is_destructible_v<T>) for (auto idx = 0; idx < _size; ++idx) _data[idx].~T();
            std::free(_data);
        }

        template <typename... ArgsT>
        T& push(ArgsT&&... args)
        {
            assert(_size < (capacity - 1));
            ::new(_data + _size) T{ std::forward<ArgsT>(args)... };
            ++_size;
            return _data[_size - 1];
        }

        void pop(int depth = 1)
        {
            while (depth >= 0 && _size > 0)
            {
                --_size;
                if constexpr (std::is_default_constructible_v<T>)
                    ::new(_data + _size) T{};
                --depth;
            }
        }

        void clear() { pop(_size); }

        int size() const { return _size; }
        T* begin() const { return _data; }
        T* end() const { return _data + _size; }

        T& operator[](int idx) { return _data[idx]; }
        T const& operator[](int idx) const { return _data[idx]; }

        T& top() { return _data[_size - 1]; }
        T const& top() const { return _data[_size - 1]; }

        T& next(int amount) { return _data[_data.size() - 1 - amount]; }
        T const& next(int amount) const { return _data[_data.size() - 1 - amount]; }
    };

    template <typename T, typename Sz, Sz blocksz = 128>
    struct DynamicStack
    {
        using IteratorT = typename Vector<T, Sz, blocksz>::Iterator;

        DynamicStack(Sz capacity, const T& el)
            : _data{ capacity, el }
        {}

        DynamicStack(Sz capacity = blocksz)
            : _data{ capacity }
        {}

        template <typename... ArgsT>
        T& push(ArgsT&&... args)
        {
            return _data.emplace_back(std::forward<ArgsT>(args)...);
        }

        void pop(int depth = 1)
        {
            while (depth >= 0 && _data._size > 0)
            {
                --_data._size;
                if constexpr (std::is_default_constructible_v<T>)
                    ::new(_data._data + _data._size) T{};
                --depth;
            }
        }

        void clear() { pop(_data._size); }

        Sz size() const { return _data.size(); }
        bool empty() const { return _data.size() == 0; }
        IteratorT begin() { return _data.begin(); }
        IteratorT end() { return _data.end(); }

        T& operator[](int idx) { return _data[idx]; }
        T const& operator[](int idx) const { return _data[idx]; }

        T& top() { return _data.back(); }
        T const& top() const { return _data.back(); }

        T& next(int amount) { return _data[_data.size() - 1 - amount]; }
        T const& next(int amount) const { return _data[_data.size() - 1 - amount]; }

    private:

        Vector<T, Sz, blocksz> _data;
    };

    WidgetStateData::WidgetStateData(WidgetType ty)
        : type{ ty }
    {
        switch (type)
        {
        case WT_Label: ::new (&state.label) LabelState{}; break;
        case WT_Button: ::new (&state.button) ButtonState{}; break;
        case WT_RadioButton: new (&state.radio) RadioButtonState{}; break;
        case WT_ToggleButton: new (&state.toggle) ToggleButtonState{}; break;
        case WT_TextInput: new (&state.input) TextInputState{}; break;
        case WT_TabBar: new (&state.tab) TabBarState{}; break;
        case WT_ItemGrid: ::new (&state.grid) ItemGridState{}; break;
        default: break;
        }
    }

    WidgetStateData::WidgetStateData(const WidgetStateData& src)
        : WidgetStateData{ src.type }
    {
        switch (type)
        {
        case WT_Label: state.label = src.state.label; break;
        case WT_Button: state.button = src.state.button; break;
        case WT_RadioButton: state.radio = src.state.radio; break;
        case WT_ToggleButton: state.toggle = src.state.toggle; break;
        case WT_TextInput: state.input = src.state.input; break;
        case WT_TabBar: state.tab = src.state.tab; break;
        case WT_ItemGrid: state.grid = src.state.grid; break;
        default: break;
        }
    }

    WidgetStateData::~WidgetStateData()
    {
        switch (type)
        {
        case WT_Label: state.label.~LabelState(); break;
        case WT_Button: state.button.~ButtonState(); break;
        case WT_RadioButton: break;
        case WT_ToggleButton: break;
        case WT_ItemGrid: state.grid.~ItemGridState(); break;
        default: break;
        }
    }

    struct ScrollBarState
    {
        ImVec2 pos;
        ImVec2 lastMousePos;
        float opacity = 0.f;
        bool mouseDownOnVGrip = false;
        bool mouseDownOnHGrip = false;
    };

    enum class ItemGridCurrentState
    {
        Default, ResizingColumns, ReorderingColumns
    };

    struct ItemGridInternalState
    {
        struct HeaderCellResizeState
        {
            ImVec2 lastPos; // Last mouse position while dragging
            float modified = 0.f; // Records already modified column width
            bool mouseDown = false; // If mouse is down on column boundary
        };

        struct HeaderCellDragState
        {
            ItemGridState::ColumnConfig config;
            ImVec2 lastPos;
            ImVec2 startPos;
            int16_t column = -1;
            int16_t level = -1;
            bool mouseDown = false;
        };

        struct BiDirMap 
        { 
            Vector<int16_t, int16_t> ltov{ 128, -1 };
            Vector<int16_t, int16_t> vtol{ 128, -1 };
        };

        Vector<HeaderCellResizeState, int16_t> cols[8];
        BiDirMap colmap[8];
        HeaderCellDragState drag;
        ScrollBarState scroll;
        ImVec2 totalsz;
        ItemGridCurrentState state = ItemGridCurrentState::Default;

        void swapColumns(int16_t from, int16_t to, std::vector<std::vector<ItemGridState::ColumnConfig>>& headers, int level)
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
    };

    struct TabBarInternalState
    {
        struct BiDirMap
        {
            Vector<int16_t, int16_t> ltov{ 128, -1 };
            Vector<int16_t, int16_t> vtol{ 128, -1 };
        };

        BiDirMap map;
    };

    struct ToggleButtonInternalState
    {
        float btnpos = -1.f;
        float progress = 0.f;
        bool animate = false;
    };

    struct RadioButtonInternalState
    {
        float radius = -1.f;
        float progress = 0.f;
        bool animate = false;
    };

    struct InputTextInternalState
    {
        int caretpos = 0;
        float offset = 0.f;
        bool caretVisible = true;
        bool capslock = false;
        bool isSelecting = false;
        float lastCaretShowTime = 0.f;
        float selectionStart = -1.f;
        std::pair<float, float> visibleRegion;
        Vector<float, int16_t> pixelpos; // Cumulative pixel position of characters
    };

    static std::vector<std::pair<char, char>> KeyMappings;

    struct WidgetContextData
    {
        // This is quasi-persistent
        std::vector<WidgetStateData> states[WT_TotalTypes];
        std::vector<ItemGridInternalState> gridStates;
        std::vector<TabBarInternalState> tabStates;
        std::vector<ToggleButtonInternalState> toggleStates;
        std::vector<RadioButtonInternalState> radioStates;
        std::vector<InputTextInternalState> inputTextStates;

        DynamicStack<StyleDescriptor, int16_t> pushedStyles[WSI_Total];
        StyleDescriptor currStyle[WSI_Total];
        int32_t currStyleStates = 0;

        Vector<LayoutItemDescriptor, int16_t> layoutItems{ 128 };
        Vector<ImRect, int16_t> itemGeometries[WT_TotalTypes];

        // This has to persistent
        std::vector<AnimationData> animations{ AnimationsPreallocSz, AnimationData{} };

        // Per widget specific style objects
        DynamicStack<ToggleButtonStyleDescriptor, int16_t> toggleButtonStyles[WSI_Total];
        DynamicStack<RadioButtonStyleDescriptor, int16_t>  radioButtonStyles[WSI_Total];

        // Keep track of widget IDs
        int maxids[WT_TotalTypes];
        int tempids[WT_TotalTypes];

        // Whether we are in a frame being rendered
        bool InsideFrame = false;

        int32_t currLayoutDepth = -1;
        LayoutDescriptor layouts[GLIMMER_MAX_LAYOUT_NESTING];

        int32_t currSizingDepth = 0;
        Sizing sizing[GLIMMER_MAX_LAYOUT_NESTING];

        int32_t currSpanDepth = -1;
        ElementSpan spans[GLIMMER_MAX_LAYOUT_NESTING];
        ImVec2 nextpos{ 0.f, 0.f };
        int32_t lastItemId = -1;

        WidgetStateData& GetState(int32_t id)
        {
            auto index = id & 0xffff;
            auto wtype = (WidgetType)(id >> 16);
            return states[wtype][index];
        }

        ItemGridInternalState& GridState(int32_t id)
        {
            auto index = id & 0xffff;
            return gridStates[index];
        }

        TabBarInternalState& TabStates(int32_t id)
        {
            auto index = id & 0xffff;
            return tabStates[index];
        }

        ToggleButtonInternalState& ToggleState(int32_t id)
        {
            auto index = id & 0xffff;
            return toggleStates[index];
        }

        RadioButtonInternalState& RadioState(int32_t id)
        {
            auto index = id & 0xffff;
            return radioStates[index];
        }

        InputTextInternalState& InputTextState(int32_t id)
        {
            auto index = id & 0xffff;
            return inputTextStates[index];
        }

        StyleDescriptor& GetStyle(int32_t state)
        {
            auto style = log2((unsigned)state);
            auto& res = (state & currStyleStates) ? currStyle[style] : pushedStyles[style].top();
            if (res.font.font == nullptr) res.font.font = GetFont(res.font.family, res.font.size, FT_Normal);
            return res;
        }

        void AddItemGeometry(int id, const ImRect& geometry)
        {
            auto index = id & 0xffff;
            auto wtype = (WidgetType)(id >> 16);
            itemGeometries[wtype][index] = geometry;
            lastItemId = id;

            if (currSpanDepth > 0 && spans[currSpanDepth].popWhenUsed)
            {
                spans[currSpanDepth] = ElementSpan{};
                --currSpanDepth;
            }
        }

        const ImRect& GetGeometry(int32_t id) const
        {
            auto index = id & 0xffff;
            auto wtype = (WidgetType)(id >> 16);
            return itemGeometries[wtype][index];
        }

        WidgetContextData()
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
                itemGeometries[idx].resize(32, ImRect{ {0.f, 0.f}, {0.f, 0.f} });
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
    };

    static WidgetContextData Context{};

    // =============================================================================================
    // INTERFACE IMPLEMENTATIONS
    // =============================================================================================

#pragma region Interface Impls

    ImGuiRenderer::ImGuiRenderer()
    {
        //UserData = ImGui::GetWindowDrawList();
    }

    void ImGuiRenderer::SetClipRect(ImVec2 startpos, ImVec2 endpos)
    {
        ImGui::PushClipRect(startpos, endpos, true);
    }

    void ImGuiRenderer::ResetClipRect()
    {
        ImGui::PopClipRect();
    }

    void ImGuiRenderer::DrawLine(ImVec2 startpos, ImVec2 endpos, uint32_t color, float thickness)
    {
        ((ImDrawList*)UserData)->AddLine(startpos, endpos, color, thickness);
    }

    void ImGuiRenderer::DrawPolyline(ImVec2* points, int sz, uint32_t color, float thickness)
    {
        ((ImDrawList*)UserData)->AddPolyline(points, sz, color, 0, thickness);
    }

    void ImGuiRenderer::DrawTriangle(ImVec2 pos1, ImVec2 pos2, ImVec2 pos3, uint32_t color, bool filled, bool thickness)
    {
        filled ? ((ImDrawList*)UserData)->AddTriangleFilled(pos1, pos2, pos3, color) :
            ((ImDrawList*)UserData)->AddTriangle(pos1, pos2, pos3, color, thickness);
    }

    void ImGuiRenderer::DrawRect(ImVec2 startpos, ImVec2 endpos, uint32_t color, bool filled, float thickness)
    {
        if (thickness > 0.f || filled)
        {
            filled ? ((ImDrawList*)UserData)->AddRectFilled(startpos, endpos, color) :
                ((ImDrawList*)UserData)->AddRect(startpos, endpos, color, 0.f, 0, thickness);
        }
    }

    void ImGuiRenderer::DrawRoundedRect(ImVec2 startpos, ImVec2 endpos, uint32_t color, bool filled,
        float topleftr, float toprightr, float bottomrightr, float bottomleftr, float thickness)
    {
        auto isUniformRadius = (topleftr == toprightr && toprightr == bottomrightr && bottomrightr == bottomleftr) ||
            ((topleftr + toprightr + bottomrightr + bottomleftr) == 0.f);

        if (isUniformRadius)
        {
            auto drawflags = 0;

            if (topleftr > 0.f) drawflags |= ImDrawFlags_RoundCornersTopLeft;
            if (toprightr > 0.f) drawflags |= ImDrawFlags_RoundCornersTopRight;
            if (bottomrightr > 0.f) drawflags |= ImDrawFlags_RoundCornersBottomRight;
            if (bottomleftr > 0.f) drawflags |= ImDrawFlags_RoundCornersBottomLeft;

            filled ? ((ImDrawList*)UserData)->AddRectFilled(startpos, endpos, color, toprightr, drawflags) :
                ((ImDrawList*)UserData)->AddRect(startpos, endpos, color, toprightr, drawflags, thickness);
        }
        else
        {
            auto& dl = *((ImDrawList*)UserData);
            ConstructRoundedRect(startpos, endpos, topleftr, toprightr, bottomrightr, bottomleftr);
            filled ? dl.PathFillConvex(color) : dl.PathStroke(color, ImDrawFlags_Closed, thickness);
        }
    }

    void ImGuiRenderer::DrawRectGradient(ImVec2 startpos, ImVec2 endpos, uint32_t colorfrom, uint32_t colorto, Direction dir)
    {
        dir == DIR_Horizontal ? ((ImDrawList*)UserData)->AddRectFilledMultiColor(startpos, endpos, colorfrom, colorto, colorto, colorfrom) :
            ((ImDrawList*)UserData)->AddRectFilledMultiColor(startpos, endpos, colorfrom, colorfrom, colorto, colorto);
    }

    void ImGuiRenderer::DrawRoundedRectGradient(ImVec2 startpos, ImVec2 endpos, float topleftr, float toprightr, float bottomrightr, float bottomleftr, uint32_t colorfrom, uint32_t colorto, Direction dir)
    {
        auto& dl = *((ImDrawList*)UserData);
        ConstructRoundedRect(startpos, endpos, topleftr, toprightr, bottomrightr, bottomleftr);

        DrawPolyGradient(dl._Path.Data, NULL, dl._Path.Size);
    }

    void ImGuiRenderer::DrawCircle(ImVec2 center, float radius, uint32_t color, bool filled, bool thickness)
    {
        filled ? ((ImDrawList*)UserData)->AddCircleFilled(center, radius, color) :
            ((ImDrawList*)UserData)->AddCircle(center, radius, color, 0, thickness);
    }

    void ImGuiRenderer::DrawSector(ImVec2 center, float radius, int start, int end, uint32_t color, bool filled, bool inverted, bool thickness)
    {
        constexpr float DegToRad = M_PI / 180.f;

        if (inverted)
        {
            auto& dl = *((ImDrawList*)UserData);
            dl.PathClear();
            dl.PathArcTo(center, radius, DegToRad * (float)start, DegToRad * (float)end, 32);
            auto start = dl._Path.front(), end = dl._Path.back();

            ImVec2 exterior[4] = { { std::min(start.x, end.x), std::min(start.y, end.y) },
                { std::max(start.x, end.x), std::min(start.y, end.y) },
                { std::min(start.x, end.x), std::max(start.y, end.y) },
                { std::max(start.x, end.x), std::max(start.y, end.y) }
            };

            auto maxDistIdx = 0;
            float dist = 0.f;
            for (auto idx = 0; idx < 4; ++idx)
            {
                auto curr = std::sqrt((end.x - start.x) * (end.x - start.x) +
                    (end.y - start.y) * (end.y - start.y));
                if (curr > dist)
                {
                    dist = curr;
                    maxDistIdx = idx;
                }
            }

            dl.PathLineTo(exterior[maxDistIdx]);
            filled ? dl.PathFillConcave(color) : dl.PathStroke(color, ImDrawFlags_Closed, thickness);
        }
        else
        {
            auto& dl = *((ImDrawList*)UserData);
            dl.PathClear();
            dl.PathArcTo(center, radius, DegToRad * (float)start, DegToRad * (float)end, 32);
            dl.PathLineTo(center);
            filled ? dl.PathFillConcave(color) : dl.PathStroke(color, ImDrawFlags_Closed, thickness);
        }
    }

    void ImGuiRenderer::DrawPolygon(ImVec2* points, int sz, uint32_t color, bool filled, float thickness)
    {
        filled ? ((ImDrawList*)UserData)->AddConvexPolyFilled(points, sz, color) :
            ((ImDrawList*)UserData)->AddPolyline(points, sz, color, ImDrawFlags_Closed, thickness);
    }

    void ImGuiRenderer::DrawPolyGradient(ImVec2* points, uint32_t* colors, int sz)
    {
        auto drawList = ((ImDrawList*)UserData);
        const ImVec2 uv = drawList->_Data->TexUvWhitePixel;

        if (drawList->Flags & ImDrawListFlags_AntiAliasedFill)
        {
            // Anti-aliased Fill
            const float AA_SIZE = 1.0f;
            const int idx_count = (sz - 2) * 3 + sz * 6;
            const int vtx_count = (sz * 2);
            drawList->PrimReserve(idx_count, vtx_count);

            // Add indexes for fill
            unsigned int vtx_inner_idx = drawList->_VtxCurrentIdx;
            unsigned int vtx_outer_idx = drawList->_VtxCurrentIdx + 1;
            for (int i = 2; i < sz; i++)
            {
                drawList->_IdxWritePtr[0] = (ImDrawIdx)(vtx_inner_idx);
                drawList->_IdxWritePtr[1] = (ImDrawIdx)(vtx_inner_idx + ((i - 1) << 1));
                drawList->_IdxWritePtr[2] = (ImDrawIdx)(vtx_inner_idx + (i << 1));
                drawList->_IdxWritePtr += 3;
            }

            // Compute normals
            ImVec2* temp_normals = (ImVec2*)alloca(sz * sizeof(ImVec2));
            for (int i0 = sz - 1, i1 = 0; i1 < sz; i0 = i1++)
            {
                const ImVec2& p0 = points[i0];
                const ImVec2& p1 = points[i1];
                ImVec2 diff = p1 - p0;
                diff *= ImInvLength(diff, 1.0f);
                temp_normals[i0].x = diff.y;
                temp_normals[i0].y = -diff.x;
            }

            for (int i0 = sz - 1, i1 = 0; i1 < sz; i0 = i1++)
            {
                // Average normals
                const ImVec2& n0 = temp_normals[i0];
                const ImVec2& n1 = temp_normals[i1];
                ImVec2 dm = (n0 + n1) * 0.5f;
                float dmr2 = dm.x * dm.x + dm.y * dm.y;
                if (dmr2 > 0.000001f)
                {
                    float scale = 1.0f / dmr2;
                    if (scale > 100.0f) scale = 100.0f;
                    dm *= scale;
                }
                dm *= AA_SIZE * 0.5f;

                // Add vertices
                drawList->_VtxWritePtr[0].pos = (points[i1] - dm);
                drawList->_VtxWritePtr[0].uv = uv; drawList->_VtxWritePtr[0].col = colors[i1];        // Inner
                drawList->_VtxWritePtr[1].pos = (points[i1] + dm);
                drawList->_VtxWritePtr[1].uv = uv; drawList->_VtxWritePtr[1].col = colors[i1] & ~IM_COL32_A_MASK;  // Outer
                drawList->_VtxWritePtr += 2;

                // Add indexes for fringes
                drawList->_IdxWritePtr[0] = (ImDrawIdx)(vtx_inner_idx + (i1 << 1));
                drawList->_IdxWritePtr[1] = (ImDrawIdx)(vtx_inner_idx + (i0 << 1));
                drawList->_IdxWritePtr[2] = (ImDrawIdx)(vtx_outer_idx + (i0 << 1));
                drawList->_IdxWritePtr[3] = (ImDrawIdx)(vtx_outer_idx + (i0 << 1));
                drawList->_IdxWritePtr[4] = (ImDrawIdx)(vtx_outer_idx + (i1 << 1));
                drawList->_IdxWritePtr[5] = (ImDrawIdx)(vtx_inner_idx + (i1 << 1));
                drawList->_IdxWritePtr += 6;
            }

            drawList->_VtxCurrentIdx += (ImDrawIdx)vtx_count;
        }
        else
        {
            // Non Anti-aliased Fill
            const int idx_count = (sz - 2) * 3;
            const int vtx_count = sz;
            drawList->PrimReserve(idx_count, vtx_count);
            for (int i = 0; i < vtx_count; i++)
            {
                drawList->_VtxWritePtr[0].pos = points[i];
                drawList->_VtxWritePtr[0].uv = uv;
                drawList->_VtxWritePtr[0].col = colors[i];
                drawList->_VtxWritePtr++;
            }
            for (int i = 2; i < sz; i++)
            {
                drawList->_IdxWritePtr[0] = (ImDrawIdx)(drawList->_VtxCurrentIdx);
                drawList->_IdxWritePtr[1] = (ImDrawIdx)(drawList->_VtxCurrentIdx + i - 1);
                drawList->_IdxWritePtr[2] = (ImDrawIdx)(drawList->_VtxCurrentIdx + i);
                drawList->_IdxWritePtr += 3;
            }

            drawList->_VtxCurrentIdx += (ImDrawIdx)vtx_count;
        }

        drawList->_Path.Size = 0;
    }

    void ImGuiRenderer::DrawRadialGradient(ImVec2 center, float radius, uint32_t in, uint32_t out, int start, int end)
    {
        auto drawList = ((ImDrawList*)UserData);
        if (((in | out) & IM_COL32_A_MASK) == 0 || radius < 0.5f)
            return;
        auto startrad = ((float)M_PI / 180.f) * (float)start;
        auto endrad = ((float)M_PI / 180.f) * (float)end;

        // Use arc with 32 segment count
        drawList->PathArcTo(center, radius, startrad, endrad, 32);
        const int count = drawList->_Path.Size - 1;

        unsigned int vtx_base = drawList->_VtxCurrentIdx;
        drawList->PrimReserve(count * 3, count + 1);

        // Submit vertices
        const ImVec2 uv = drawList->_Data->TexUvWhitePixel;
        drawList->PrimWriteVtx(center, uv, in);
        for (int n = 0; n < count; n++)
            drawList->PrimWriteVtx(drawList->_Path[n], uv, out);

        // Submit a fan of triangles
        for (int n = 0; n < count; n++)
        {
            drawList->PrimWriteIdx((ImDrawIdx)(vtx_base));
            drawList->PrimWriteIdx((ImDrawIdx)(vtx_base + 1 + n));
            drawList->PrimWriteIdx((ImDrawIdx)(vtx_base + 1 + ((n + 1) % count)));
        }

        drawList->_Path.Size = 0;
    }

    bool ImGuiRenderer::SetCurrentFont(std::string_view family, float sz, FontType type)
    {
        auto font = GetFont(family, sz, type);

        if (font != nullptr)
        {
            _currentFontSz = sz;
            ImGui::PushFont((ImFont*)font);
            return true;
        }

        return false;
    }

    bool ImGuiRenderer::SetCurrentFont(void* fontptr, float sz)
    {
        if (fontptr != nullptr)
        {
            _currentFontSz = sz;
            ImGui::PushFont((ImFont*)fontptr);
            return true;
        }

        return false;
    }

    void ImGuiRenderer::ResetFont()
    {
        ImGui::PopFont();
    }

    ImVec2 ImGuiRenderer::GetTextSize(std::string_view text, void* fontptr, float sz, float wrapWidth)
    {
        auto imfont = (ImFont*)fontptr;
        ImGui::PushFont(imfont);
        auto txtsz = ImGui::CalcTextSize(text.data(), text.data() + text.size(), false, wrapWidth);
        ImGui::PopFont();

        auto ratio = (sz / imfont->FontSize);
        txtsz.x *= ratio;
        txtsz.y *= ratio;
        return txtsz;
    }

    void ImGuiRenderer::DrawText(std::string_view text, ImVec2 pos, uint32_t color, float wrapWidth)
    {
        auto font = ImGui::GetFont();
        ((ImDrawList*)UserData)->AddText(font, _currentFontSz, pos, color, text.data(), text.data() + text.size(), 
            wrapWidth);
    }

    void ImGuiRenderer::DrawTooltip(ImVec2 pos, std::string_view text)
    {
        if (!text.empty())
        {
            //SetCurrentFont(config.DefaultFontFamily, Config.defaultFontSz, FT_Normal);
            ImGui::SetTooltip("%.*s", (int)text.size(), text.data());
            ResetFont();
        }
    }

    float ImGuiRenderer::EllipsisWidth(void* fontptr, float sz)
    {
        return ((ImFont*)fontptr)->EllipsisWidth;
    }

    void ImGuiRenderer::ConstructRoundedRect(ImVec2 startpos, ImVec2 endpos, float topleftr, float toprightr, float bottomrightr, float bottomleftr)
    {
        auto& dl = *((ImDrawList*)UserData);
        auto minlength = std::min(endpos.x - startpos.x, endpos.y - startpos.y);
        topleftr = std::min(topleftr, minlength);
        toprightr = std::min(toprightr, minlength);
        bottomrightr = std::min(bottomrightr, minlength);
        bottomleftr = std::min(bottomleftr, minlength);

        dl.PathClear();
        dl.PathLineTo(ImVec2{ startpos.x, endpos.y - bottomleftr });
        dl.PathLineTo(ImVec2{ startpos.x, startpos.y + topleftr });
        if (topleftr > 0.f) dl.PathArcToFast(ImVec2{ startpos.x + topleftr, startpos.y + topleftr }, topleftr, 6, 9);
        dl.PathLineTo(ImVec2{ endpos.x - toprightr, startpos.y });
        if (toprightr > 0.f) dl.PathArcToFast(ImVec2{ endpos.x - toprightr, startpos.y + toprightr }, toprightr, 9, 12);
        dl.PathLineTo(ImVec2{ endpos.x, endpos.y - bottomrightr });
        if (bottomrightr > 0.f) dl.PathArcToFast(ImVec2{ endpos.x - bottomrightr, endpos.y - bottomrightr }, bottomrightr, 0, 3);
        dl.PathLineTo(ImVec2{ startpos.x - bottomleftr, endpos.y });
        if (bottomleftr > 0.f) dl.PathArcToFast(ImVec2{ startpos.x + bottomleftr, endpos.y - bottomleftr }, bottomleftr, 3, 6);
    }

    float IRenderer::EllipsisWidth(void* fontptr, float sz)
    {
        return GetTextSize("...", fontptr, sz).x;
    }

#pragma endregion

    // =============================================================================================
    // STYLING FUNCTIONS
    // =============================================================================================

#pragma region Style parsing

#pragma optimize( "", on )
    [[nodiscard]] int SkipSpace(const char* text, int idx, int end)
    {
        while ((idx < end) && std::isspace(text[idx])) idx++;
        return idx;
    }

    [[nodiscard]] int SkipSpace(const std::string_view text, int from = 0)
    {
        auto end = (int)text.size();
        while ((from < end) && (std::isspace(text[from]))) from++;
        return from;
    }

    [[nodiscard]] int WholeWord(const std::string_view text, int from = 0)
    {
        auto end = (int)text.size();
        while ((from < end) && (!std::isspace(text[from]))) from++;
        return from;
    }

    [[nodiscard]] int SkipDigits(const std::string_view text, int from = 0)
    {
        auto end = (int)text.size();
        while ((from < end) && (std::isdigit(text[from]))) from++;
        return from;
    }

    [[nodiscard]] int SkipFDigits(const std::string_view text, int from = 0)
    {
        auto end = (int)text.size();
        while ((from < end) && ((std::isdigit(text[from])) || (text[from] == '.'))) from++;
        return from;
    }
#pragma optimize( "", off )

    [[nodiscard]] bool AreSame(const std::string_view lhs, const char* rhs)
    {
        auto rlimit = (int)std::strlen(rhs);
        auto llimit = (int)lhs.size();
        if (rlimit != llimit)
            return false;

        for (auto idx = 0; idx < rlimit; ++idx)
            if (std::tolower(lhs[idx]) != std::tolower(rhs[idx]))
                return false;

        return true;
    }

    [[nodiscard]] bool StartsWith(const std::string_view lhs, const char* rhs)
    {
        auto rlimit = (int)std::strlen(rhs);
        auto llimit = (int)lhs.size();
        if (rlimit > llimit)
            return false;

        for (auto idx = 0; idx < rlimit; ++idx)
            if (std::tolower(lhs[idx]) != std::tolower(rhs[idx]))
                return false;

        return true;
    }

    [[nodiscard]] bool AreSame(const std::string_view lhs, const std::string_view rhs)
    {
        auto rlimit = (int)rhs.size();
        auto llimit = (int)lhs.size();
        if (rlimit != llimit)
            return false;

        for (auto idx = 0; idx < rlimit; ++idx)
            if (std::tolower(lhs[idx]) != std::tolower(rhs[idx]))
                return false;

        return true;
    }

    [[nodiscard]] int ExtractInt(std::string_view input, int defaultVal)
    {
        int result = defaultVal;
        int base = 1;
        auto idx = (int)input.size() - 1;
        while (!std::isdigit(input[idx]) && idx >= 0) idx--;

        for (; idx >= 0; --idx)
        {
            result += (input[idx] - '0') * base;
            base *= 10;
        }

        return result;
    }

    [[nodiscard]] int ExtractIntFromHex(std::string_view input, int defaultVal)
    {
        int result = defaultVal;
        int base = 1;
        auto idx = (int)input.size() - 1;
        while (!std::isdigit(input[idx]) && !std::isalpha(input[idx]) && idx >= 0) idx--;

        for (; idx >= 0; --idx)
        {
            result += std::isdigit(input[idx]) ? (input[idx] - '0') * base :
                std::islower(input[idx]) ? ((input[idx] - 'a') + 10) * base :
                ((input[idx] - 'A') + 10) * base;
            base *= 16;
        }

        return result;
    }

    [[nodiscard]] IntOrFloat ExtractNumber(std::string_view input, float defaultVal)
    {
        float result = 0.f, base = 1.f;
        bool isInt = false;
        auto idx = (int)input.size() - 1;

        while (idx >= 0 && input[idx] != '.') idx--;
        auto decimal = idx;

        if (decimal != -1)
        {
            for (auto midx = decimal; midx >= 0; --midx)
            {
                result += (input[midx] - '0') * base;
                base *= 10.f;
            }

            base = 0.1f;
            for (auto midx = decimal + 1; midx < (int)input.size(); ++midx)
            {
                result += (input[midx] - '0') * base;
                base *= 0.1f;
            }
        }
        else
        {
            for (auto midx = (int)input.size() - 1; midx >= 0; --midx)
            {
                result += (input[midx] - '0') * base;
                base *= 10.f;
            }

            isInt = true;
        }

        return { result, !isInt };
    }

    [[nodiscard]] float ExtractFloatWithUnit(std::string_view input, float defaultVal, float ems, float parent, float scale)
    {
        float result = defaultVal, base = 1.f;
        auto idx = (int)input.size() - 1;
        while (!std::isdigit(input[idx]) && idx >= 0) idx--;

        if (AreSame(input.substr(idx + 1), "pt")) scale = 1.3333f;
        else if (AreSame(input.substr(idx + 1), "em")) scale = ems;
        else if (input[idx + 1] == '%') scale = parent * 0.01f;

        auto num = ExtractNumber(input.substr(0, idx + 1), defaultVal);
        result = num.value;

        return result * scale;
    }

    uint32_t ToRGBA(int r, int g, int b, int a)
    {
        return (((uint32_t)(a) << 24) |
            ((uint32_t)(b) << 16) |
            ((uint32_t)(g) << 8) |
            ((uint32_t)(r) << 0));
    }

    uint32_t ToRGBA(const std::tuple<int, int, int>& rgb)
    {
        return ToRGBA(std::get<0>(rgb), std::get<1>(rgb), std::get<2>(rgb), 255);
    }

    std::tuple<int, int, int, int> DecomposeColor(uint32_t color)
    {
        return { color & 0xff, (color & 0xff00) >> 8, (color & 0xff0000) >> 16, (color & 0xff000000) >> 24 };
    }

    uint32_t ToRGBA(const std::tuple<int, int, int, int>& rgba)
    {
        return ToRGBA(std::get<0>(rgba), std::get<1>(rgba), std::get<2>(rgba), std::get<3>(rgba));
    }

    uint32_t ToRGBA(float r, float g, float b, float a)
    {
        return ToRGBA((int)(r * 255.f), (int)(g * 255.f), (int)(b * 255.f), (int)(a * 255.f));
    }

#ifndef IM_RICHTEXT_TARGET_IMGUI
    static void ColorConvertHSVtoRGB(float h, float s, float v, float& out_r, float& out_g, float& out_b)
    {
        if (s == 0.0f)
        {
            // gray
            out_r = out_g = out_b = v;
            return;
        }

        h = std::fmodf(h, 1.0f) / (60.0f / 360.0f);
        int   i = (int)h;
        float f = h - (float)i;
        float p = v * (1.0f - s);
        float q = v * (1.0f - s * f);
        float t = v * (1.0f - s * (1.0f - f));

        switch (i)
        {
        case 0: out_r = v; out_g = t; out_b = p; break;
        case 1: out_r = q; out_g = v; out_b = p; break;
        case 2: out_r = p; out_g = v; out_b = t; break;
        case 3: out_r = p; out_g = q; out_b = v; break;
        case 4: out_r = t; out_g = p; out_b = v; break;
        case 5: default: out_r = v; out_g = p; out_b = q; break;
        }
    }
#endif

    [[nodiscard]] std::tuple<IntOrFloat, IntOrFloat, IntOrFloat, IntOrFloat> GetCommaSeparatedNumbers(std::string_view stylePropVal, int& curr)
    {
        std::tuple<IntOrFloat, IntOrFloat, IntOrFloat, IntOrFloat> res;
        auto hasFourth = curr == 4;
        curr = SkipSpace(stylePropVal, curr);
        assert(stylePropVal[curr] == '(');
        curr++;

        auto valstart = curr;
        curr = SkipFDigits(stylePropVal, curr);
        std::get<0>(res) = ExtractNumber(stylePropVal.substr(valstart, curr - valstart), 0);
        curr = SkipSpace(stylePropVal, curr);
        assert(stylePropVal[curr] == ',');
        curr++;
        curr = SkipSpace(stylePropVal, curr);

        valstart = curr;
        curr = SkipFDigits(stylePropVal, curr);
        std::get<1>(res) = ExtractNumber(stylePropVal.substr(valstart, curr - valstart), 0);
        curr = SkipSpace(stylePropVal, curr);
        assert(stylePropVal[curr] == ',');
        curr++;
        curr = SkipSpace(stylePropVal, curr);

        valstart = curr;
        curr = SkipFDigits(stylePropVal, curr);
        std::get<2>(res) = ExtractNumber(stylePropVal.substr(valstart, curr - valstart), 0);
        curr = SkipSpace(stylePropVal, curr);

        if (hasFourth)
        {
            assert(stylePropVal[curr] == ',');
            curr++;
            curr = SkipSpace(stylePropVal, curr);

            valstart = curr;
            curr = SkipFDigits(stylePropVal, curr);
            std::get<3>(res) = ExtractNumber(stylePropVal.substr(valstart, curr - valstart), 0);
        }

        return res;
    }

    [[nodiscard]] uint32_t ExtractColor(std::string_view stylePropVal, uint32_t(*NamedColor)(const char*, void*), void* userData)
    {
        if (stylePropVal.size() >= 3u && AreSame(stylePropVal.substr(0, 3), "rgb"))
        {
            IntOrFloat r, g, b, a;
            auto hasAlpha = stylePropVal[3] == 'a' || stylePropVal[3] == 'A';
            int curr = hasAlpha ? 4 : 3;
            std::tie(r, g, b, a) = GetCommaSeparatedNumbers(stylePropVal, curr);
            auto isRelative = r.isFloat && g.isFloat && b.isFloat;
            a.value = isRelative ? hasAlpha ? a.value : 1.f :
                hasAlpha ? a.value : 255;

            assert(stylePropVal[curr] == ')');
            return isRelative ? ToRGBA(r.value, g.value, b.value, a.value) :
                ToRGBA((int)r.value, (int)g.value, (int)b.value, (int)a.value);
        }
        else if (stylePropVal.size() >= 3u && AreSame(stylePropVal.substr(0, 3), "hsv"))
        {
            IntOrFloat h, s, v;
            auto curr = 3;
            std::tie(h, s, v, std::ignore) = GetCommaSeparatedNumbers(stylePropVal, curr);

            assert(stylePropVal[curr] == ')');
            return ImColor::HSV(h.value, s.value, v.value);
        }
        else if (stylePropVal.size() >= 3u && AreSame(stylePropVal.substr(0, 3), "hsl"))
        {
            IntOrFloat h, s, l;
            auto curr = 3;
            std::tie(h, s, l, std::ignore) = GetCommaSeparatedNumbers(stylePropVal, curr);
            auto v = l.value + s.value * std::min(l.value, 1.f - l.value);
            s.value = v == 0.f ? 0.f : 2.f * (1.f - (l.value / v));

            assert(stylePropVal[curr] == ')');
            return ImColor::HSV(h.value, s.value, v);
        }
        else if (stylePropVal.size() >= 1u && stylePropVal[0] == '#')
        {
            return (uint32_t)ExtractIntFromHex(stylePropVal.substr(1), 0);
        }
        else if (NamedColor != nullptr)
        {
            static char buffer[32] = { 0 };
            std::memset(buffer, 0, 32);
            std::memcpy(buffer, stylePropVal.data(), std::min((int)stylePropVal.size(), 31));
            return NamedColor(buffer, userData);
        }
        else
        {
            return IM_COL32_BLACK;
        }
    }

    std::pair<uint32_t, float> ExtractColorStop(std::string_view input, uint32_t(*NamedColor)(const char*, void*), void* userData)
    {
        auto idx = 0;
        std::pair<uint32_t, float> colorstop;

        idx = WholeWord(input, idx);
        colorstop.first = ExtractColor(input.substr(0, idx), NamedColor, userData);
        idx = SkipSpace(input, idx);

        if ((idx < (int)input.size()) && std::isdigit(input[idx]))
        {
            auto start = idx;
            idx = SkipDigits(input, start);
            colorstop.second = ExtractNumber(input.substr(start, idx - start), -1.f).value;
        }
        else colorstop.second = -1.f;

        return colorstop;
    }

    ColorGradient ExtractLinearGradient(std::string_view input, uint32_t(*NamedColor)(const char*, void*), void* userData)
    {
        ColorGradient gradient;
        auto idx = 15; // size of "linear-gradient" string

        if (idx < (int)input.size())
        {
            idx = SkipSpace(input, idx);
            assert(input[idx] == '(');
            idx++;
            idx = SkipSpace(input, idx);

            std::optional<std::pair<uint32_t, float>> lastStop = std::nullopt;
            auto firstPart = true;
            auto start = idx;
            auto total = 0.f, unspecified = 0.f;

            do
            {
                idx = SkipSpace(input, idx);

                auto start = idx;
                while ((idx < (int)input.size()) && (input[idx] != ',') && (input[idx] != ')')
                    && !std::isspace(input[idx])) idx++;
                auto part = input.substr(start, idx - start);
                idx = SkipSpace(input, idx);
                auto isEnd = input[idx] == ')';

                if ((idx < (int)input.size()) && (input[idx] == ',' || isEnd)) {
                    if (firstPart)
                    {
                        if (AreSame(input, "to right")) {
                            gradient.dir = ImGuiDir::ImGuiDir_Right;
                        }
                        else if (AreSame(input, "to left")) {
                            gradient.dir = ImGuiDir::ImGuiDir_Left;
                        }
                        else {
                            auto colorstop = ExtractColorStop(part, NamedColor, userData);
                            if (colorstop.second != -1.f) total += colorstop.second;
                            else unspecified += 1.f;
                            lastStop = colorstop;
                        }
                        firstPart = false;
                    }
                    else {
                        auto colorstop = ExtractColorStop(part, NamedColor, userData);
                        if (colorstop.second != -1.f) total += colorstop.second;
                        else unspecified += 1.f;

                        if (lastStop.has_value())
                        {
                            gradient.colorStops[gradient.totalStops] =
                                ColorStop{ lastStop.value().first, colorstop.first, colorstop.second };
                            gradient.totalStops = std::min(gradient.totalStops + 1, IM_RICHTEXT_MAX_COLORSTOPS - 1);
                        }

                        lastStop = colorstop;
                    }
                }
                else break;

                if (isEnd) break;
                else if (input[idx] == ',') idx++;
            } while ((idx < (int)input.size()) && (input[idx] != ')'));

            unspecified -= 1.f;
            for (auto& colorstop : gradient.colorStops)
                if (colorstop.pos == -1.f) colorstop.pos = (100.f - total) / (100.f * unspecified);
                else colorstop.pos /= 100.f;
        }

        return gradient;
    }

    template <int maxsz>
    struct CaseInsensitiveHasher
    {
        std::size_t operator()(std::string_view key) const
        {
            thread_local static char buffer[maxsz] = { 0 };
            std::memset(buffer, 0, maxsz);
            auto limit = std::min((int)key.size(), maxsz - 1);

            for (auto idx = 0; idx < limit; ++idx)
                buffer[idx] = std::tolower(key[idx]);

            return std::hash<std::string_view>()(buffer);
        }
    };

    uint32_t GetColor(const char* name, void*)
    {
        const static std::unordered_map<std::string_view, uint32_t, CaseInsensitiveHasher<32>> Colors{
            { "black", ToRGBA(0, 0, 0) },
            { "silver", ToRGBA(192, 192, 192) },
            { "gray", ToRGBA(128, 128, 128) },
            { "white", ToRGBA(255, 255, 255) },
            { "maroon", ToRGBA(128, 0, 0) },
            { "red", ToRGBA(255, 0, 0) },
            { "purple", ToRGBA(128, 0, 128) },
            { "fuchsia", ToRGBA(255, 0, 255) },
            { "green", ToRGBA(0, 128, 0) },
            { "lime", ToRGBA(0, 255, 0) },
            { "olive", ToRGBA(128, 128, 0) },
            { "yellow", ToRGBA(255, 255, 0) },
            { "navy", ToRGBA(0, 0, 128) },
            { "blue", ToRGBA(0, 0, 255) },
            { "teal", ToRGBA(0, 128, 128) },
            { "aqua", ToRGBA(0, 255, 255) },
            { "aliceblue", ToRGBA(240, 248, 255) },
            { "antiquewhite", ToRGBA(250, 235, 215) },
            { "aquamarine", ToRGBA(127, 255, 212) },
            { "azure", ToRGBA(240, 255, 255) },
            { "beige", ToRGBA(245, 245, 220) },
            { "bisque", ToRGBA(255, 228, 196) },
            { "blanchedalmond", ToRGBA(255, 235, 205) },
            { "blueviolet", ToRGBA(138, 43, 226) },
            { "brown", ToRGBA(165, 42, 42) },
            { "burlywood", ToRGBA(222, 184, 135) },
            { "cadetblue", ToRGBA(95, 158, 160) },
            { "chartreuse", ToRGBA(127, 255, 0) },
            { "chocolate", ToRGBA(210, 105, 30) },
            { "coral", ToRGBA(255, 127, 80) },
            { "cornflowerblue", ToRGBA(100, 149, 237) },
            { "cornsilk", ToRGBA(255, 248, 220) },
            { "crimson", ToRGBA(220, 20, 60) },
            { "darkblue", ToRGBA(0, 0, 139) },
            { "darkcyan", ToRGBA(0, 139, 139) },
            { "darkgoldenrod", ToRGBA(184, 134, 11) },
            { "darkgray", ToRGBA(169, 169, 169) },
            { "darkgreen", ToRGBA(0, 100, 0) },
            { "darkgrey", ToRGBA(169, 169, 169) },
            { "darkkhaki", ToRGBA(189, 183, 107) },
            { "darkmagenta", ToRGBA(139, 0, 139) },
            { "darkolivegreen", ToRGBA(85, 107, 47) },
            { "darkorange", ToRGBA(255, 140, 0) },
            { "darkorchid", ToRGBA(153, 50, 204) },
            { "darkred", ToRGBA(139, 0, 0) },
            { "darksalmon", ToRGBA(233, 150, 122) },
            { "darkseagreen", ToRGBA(143, 188, 143) },
            { "darkslateblue", ToRGBA(72, 61, 139) },
            { "darkslategray", ToRGBA(47, 79, 79) },
            { "darkslategray", ToRGBA(47, 79, 79) },
            { "darkturquoise", ToRGBA(0, 206, 209) },
            { "darkviolet", ToRGBA(148, 0, 211) },
            { "deeppink", ToRGBA(255, 20, 147) },
            { "deepskyblue", ToRGBA(0, 191, 255) },
            { "dimgray", ToRGBA(105, 105, 105) },
            { "dimgrey", ToRGBA(105, 105, 105) },
            { "dodgerblue", ToRGBA(30, 144, 255) },
            { "firebrick", ToRGBA(178, 34, 34) },
            { "floralwhite", ToRGBA(255, 250, 240) },
            { "forestgreen", ToRGBA(34, 139, 34) },
            { "gainsboro", ToRGBA(220, 220, 220) },
            { "ghoshtwhite", ToRGBA(248, 248, 255) },
            { "gold", ToRGBA(255, 215, 0) },
            { "goldenrod", ToRGBA(218, 165, 32) },
            { "greenyellow", ToRGBA(173, 255, 47) },
            { "honeydew", ToRGBA(240, 255, 240) },
            { "hotpink", ToRGBA(255, 105, 180) },
            { "indianred", ToRGBA(205, 92, 92) },
            { "indigo", ToRGBA(75, 0, 130) },
            { "ivory", ToRGBA(255, 255, 240) },
            { "khaki", ToRGBA(240, 230, 140) },
            { "lavender", ToRGBA(230, 230, 250) },
            { "lavenderblush", ToRGBA(255, 240, 245) },
            { "lawngreen", ToRGBA(124, 252, 0) },
            { "lemonchiffon", ToRGBA(255, 250, 205) },
            { "lightblue", ToRGBA(173, 216, 230) },
            { "lightcoral", ToRGBA(240, 128, 128) },
            { "lightcyan", ToRGBA(224, 255, 255) },
            { "lightgoldenrodyellow", ToRGBA(250, 250, 210) },
            { "lightgray", ToRGBA(211, 211, 211) },
            { "lightgreen", ToRGBA(144, 238, 144) },
            { "lightgrey", ToRGBA(211, 211, 211) },
            { "lightpink", ToRGBA(255, 182, 193) },
            { "lightsalmon", ToRGBA(255, 160, 122) },
            { "lightseagreen", ToRGBA(32, 178, 170) },
            { "lightskyblue", ToRGBA(135, 206, 250) },
            { "lightslategray", ToRGBA(119, 136, 153) },
            { "lightslategrey", ToRGBA(119, 136, 153) },
            { "lightsteelblue", ToRGBA(176, 196, 222) },
            { "lightyellow", ToRGBA(255, 255, 224) },
            { "lilac", ToRGBA(200, 162, 200) },
            { "limegreen", ToRGBA(50, 255, 50) },
            { "linen", ToRGBA(250, 240, 230) },
            { "mediumaquamarine", ToRGBA(102, 205, 170) },
            { "mediumblue", ToRGBA(0, 0, 205) },
            { "mediumorchid", ToRGBA(186, 85, 211) },
            { "mediumpurple", ToRGBA(147, 112, 219) },
            { "mediumseagreen", ToRGBA(60, 179, 113) },
            { "mediumslateblue", ToRGBA(123, 104, 238) },
            { "mediumspringgreen", ToRGBA(0, 250, 154) },
            { "mediumturquoise", ToRGBA(72, 209, 204) },
            { "mediumvioletred", ToRGBA(199, 21, 133) },
            { "midnightblue", ToRGBA(25, 25, 112) },
            { "mintcream", ToRGBA(245, 255, 250) },
            { "mistyrose", ToRGBA(255, 228, 225) },
            { "moccasin", ToRGBA(255, 228, 181) },
            { "navajowhite", ToRGBA(255, 222, 173) },
            { "oldlace", ToRGBA(253, 245, 230) },
            { "olivedrab", ToRGBA(107, 142, 35) },
            { "orange", ToRGBA(255, 165, 0) },
            { "orangered", ToRGBA(255, 69, 0) },
            { "orchid", ToRGBA(218, 112, 214) },
            { "palegoldenrod", ToRGBA(238, 232, 170) },
            { "palegreen", ToRGBA(152, 251, 152) },
            { "paleturquoise", ToRGBA(175, 238, 238) },
            { "palevioletred", ToRGBA(219, 112, 147) },
            { "papayawhip", ToRGBA(255, 239, 213) },
            { "peachpuff", ToRGBA(255, 218, 185) },
            { "peru", ToRGBA(205, 133, 63) },
            { "pink", ToRGBA(255, 192, 203) },
            { "plum", ToRGBA(221, 160, 221) },
            { "powderblue", ToRGBA(176, 224, 230) },
            { "rosybrown", ToRGBA(188, 143, 143) },
            { "royalblue", ToRGBA(65, 105, 225) },
            { "saddlebrown", ToRGBA(139, 69, 19) },
            { "salmon", ToRGBA(250, 128, 114) },
            { "sandybrown", ToRGBA(244, 164, 96) },
            { "seagreen", ToRGBA(46, 139, 87) },
            { "seashell", ToRGBA(255, 245, 238) },
            { "sienna", ToRGBA(160, 82, 45) },
            { "skyblue", ToRGBA(135, 206, 235) },
            { "slateblue", ToRGBA(106, 90, 205) },
            { "slategray", ToRGBA(112, 128, 144) },
            { "slategrey", ToRGBA(112, 128, 144) },
            { "snow", ToRGBA(255, 250, 250) },
            { "springgreen", ToRGBA(0, 255, 127) },
            { "steelblue", ToRGBA(70, 130, 180) },
            { "tan", ToRGBA(210, 180, 140) },
            { "thistle", ToRGBA(216, 191, 216) },
            { "tomato", ToRGBA(255, 99, 71) },
            { "violet", ToRGBA(238, 130, 238) },
            { "wheat", ToRGBA(245, 222, 179) },
            { "whitesmoke", ToRGBA(245, 245, 245) },
            { "yellowgreen", ToRGBA(154, 205, 50) }
        };

        auto it = Colors.find(name);
        return it != Colors.end() ? it->second : uint32_t{ IM_COL32_BLACK };
    }

    bool IsColorVisible(uint32_t color)
    {
        return (color & 0xFF000000) != 0;
    }

    Border ExtractBorder(std::string_view input, float ems, float percent,
        uint32_t(*NamedColor)(const char*, void*), void* userData)
    {
        Border result;
        auto idx = WholeWord(input);

        if (AreSame(input.substr(0, idx), "none")) return result;
        result.thickness = ExtractFloatWithUnit(input.substr(0, idx), 1.f, ems, percent, 1.f);
        idx = SkipSpace(input, idx);

        auto idx2 = WholeWord(input, idx + 1);
        auto type = input.substr(idx + 1, idx2);
        if (AreSame(type, "solid")) result.lineType = LineType::Solid;
        else if (AreSame(type, "dashed")) result.lineType = LineType::Dashed;
        else if (AreSame(type, "dotted")) result.lineType = LineType::Dotted;

        idx2 = SkipSpace(input, idx2);
        auto idx3 = WholeWord(input, idx2 + 1);
        auto color = input.substr(idx2 + 1, idx3);
        result.color = ExtractColor(color, NamedColor, userData);

        return result;
    }

    static bool IsColor(std::string_view input, int from)
    {
        return input[from] != '-' && !std::isdigit(input[from]);
    }

    BoxShadow ExtractBoxShadow(std::string_view input, float ems, float percent,
        uint32_t(*NamedColor)(const char*, void*), void* userData)
    {
        BoxShadow result;
        auto idx = WholeWord(input);

        if (AreSame(input.substr(0, idx), "none")) return result;
        result.offset.x = ExtractFloatWithUnit(input.substr(0, idx), 0.f, ems, percent, 1.f);
        idx = SkipSpace(input, idx);

        auto prev = idx;
        idx = WholeWord(input, idx);

        if (!IsColor(input, prev))
        {
            result.offset.y = ExtractFloatWithUnit(input.substr(prev, (idx - prev)), 0.f, ems, percent, 1.f);
            idx = SkipSpace(input, idx);

            prev = idx;
            idx = WholeWord(input, idx);

            if (!IsColor(input, prev))
            {
                result.blur = ExtractFloatWithUnit(input.substr(prev, (idx - prev)), 0.f, ems, percent, 1.f);
                idx = SkipSpace(input, idx);

                prev = idx;
                idx = WholeWord(input, idx);

                if (!IsColor(input, prev))
                {
                    result.spread = ExtractFloatWithUnit(input.substr(prev, (idx - prev)), 0.f, ems, percent, 1.f);
                    idx = SkipSpace(input, idx);

                    prev = idx;
                    idx = WholeWord(input, idx);
                    result.color = ExtractColor(input.substr(prev, (idx - prev)), NamedColor, userData);
                }
                else
                    result.color = ExtractColor(input.substr(prev, (idx - prev)), NamedColor, userData);
            }
            else
                result.color = ExtractColor(input.substr(prev, (idx - prev)), NamedColor, userData);
        }
        else
            result.color = ExtractColor(input.substr(prev, (idx - prev)), NamedColor, userData);
        return result;
    }

    std::pair<std::string_view, bool> ExtractTag(const char* text, int end, char TagEnd,
        int& idx, bool& tagStart)
    {
        std::pair<std::string_view, bool> result;
        result.second = true;

        if (text[idx] == '/')
        {
            tagStart = false;
            idx++;
        }
        else if (!std::isalnum(text[idx]))
        {
            result.second = false;
            return result;
        }

        auto begin = idx;
        while ((idx < end) && !std::isspace(text[idx]) && (text[idx] != TagEnd)) idx++;

        if (idx - begin == 0)
        {
            result.second = false;
            return result;
        }

        result.first = std::string_view{ text + begin, (std::size_t)(idx - begin) };
        if (result.first.back() == '/') result.first = result.first.substr(0, result.first.size() - 1u);

        if (!tagStart)
        {
            if (text[idx] == TagEnd) idx++;

            if (result.first.empty())
            {
                result.second = false;
                return result;
            }
        }

        idx = SkipSpace(text, idx, end);
        return result;
    }

    [[nodiscard]] std::optional<std::string_view> GetQuotedString(const char* text, int& idx, int end)
    {
        auto insideQuotedString = false;
        auto begin = idx;

        if ((idx < end) && text[idx] == '\'')
        {
            idx++;

            while (idx < end)
            {
                if (text[idx] == '\\' && ((idx + 1 < end) && text[idx + 1] == '\''))
                {
                    insideQuotedString = !insideQuotedString;
                    idx++;
                }
                else if (!insideQuotedString && text[idx] == '\'') break;
                idx++;
            }
        }
        else if ((idx < end) && text[idx] == '"')
        {
            idx++;

            while (idx < end)
            {
                if (text[idx] == '\\' && ((idx + 1 < end) && text[idx + 1] == '"'))
                {
                    insideQuotedString = !insideQuotedString;
                    idx++;
                }
                else if (!insideQuotedString && text[idx] == '"') break;
                idx++;
            }
        }

        if ((idx < end) && (text[idx] == '"' || text[idx] == '\''))
        {
            std::string_view res{ text + begin + 1, (std::size_t)(idx - begin - 1) };
            idx++;
            return res;
        }

        return std::nullopt;
    }

    bool FourSidedBorder::isRounded() const
    {
        return cornerRadius[TopLeftCorner] > 0.f ||
            cornerRadius[TopRightCorner] > 0.f ||
            cornerRadius[BottomRightCorner] > 0.f ||
            cornerRadius[BottomLeftCorner] > 0.f;
    }

    bool FourSidedBorder::exists() const
    {
        return top.thickness > 0.f || bottom.thickness > 0.f ||
            left.thickness > 0.f || right.thickness > 0.f;
    }

    FourSidedBorder& FourSidedBorder::setColor(uint32_t color)
    {
        left.color = right.color = top.color = bottom.color = color;
        return *this;
    }

    FourSidedBorder& FourSidedBorder::setThickness(float thickness)
    {
        left.thickness = right.thickness = top.thickness = bottom.thickness = thickness;
        return *this;
    }

    FourSidedBorder& FourSidedBorder::setRadius(float radius)
    {
        cornerRadius[0] = cornerRadius[1] = cornerRadius[2] = cornerRadius[3] = radius;
        return *this;
    }

    static int PopulateSegmentStyle(StyleDescriptor& style, CommonWidgetStyleDescriptor& specific, 
        std::string_view stylePropName, std::string_view stylePropVal)
    {
        int prop = NoStyleChange;

        if (AreSame(stylePropName, "font-size"))
        {
            if (AreSame(stylePropVal, "xx-small")) style.font.size = Config.defaultFontSz * 0.6f * Config.fontScaling;
            else if (AreSame(stylePropVal, "x-small")) style.font.size = Config.defaultFontSz * 0.75f * Config.fontScaling;
            else if (AreSame(stylePropVal, "small")) style.font.size = Config.defaultFontSz * 0.89f * Config.fontScaling;
            else if (AreSame(stylePropVal, "medium")) style.font.size = Config.defaultFontSz * Config.fontScaling;
            else if (AreSame(stylePropVal, "large")) style.font.size = Config.defaultFontSz * 1.2f * Config.fontScaling;
            else if (AreSame(stylePropVal, "x-large")) style.font.size = Config.defaultFontSz * 1.5f * Config.fontScaling;
            else if (AreSame(stylePropVal, "xx-large")) style.font.size = Config.defaultFontSz * 2.f * Config.fontScaling;
            else if (AreSame(stylePropVal, "xxx-large")) style.font.size = Config.defaultFontSz * 3.f * Config.fontScaling;
            else
                style.font.size = ExtractFloatWithUnit(stylePropVal, Config.defaultFontSz * Config.fontScaling,
                    Config.defaultFontSz * Config.fontScaling, 1.f, Config.fontScaling);
            prop = StyleFontSize;
        }
        else if (AreSame(stylePropName, "font-weight"))
        {
            auto idx = SkipDigits(stylePropVal);

            if (idx == 0)
            {
                if (AreSame(stylePropVal, "bold")) style.font.flags |= FontStyleBold;
                else if (AreSame(stylePropVal, "light")) style.font.flags |= FontStyleLight;
                else ERROR("Invalid font-weight property value... [%.*s]\n",
                    (int)stylePropVal.size(), stylePropVal.data());
            }
            else
            {
                int weight = ExtractInt(stylePropVal.substr(0u, idx), 400);
                if (weight >= 600) style.font.flags |= FontStyleBold;
                if (weight < 400) style.font.flags |= FontStyleLight;
            }

            prop = StyleFontWeight;
        }
        else if (AreSame(stylePropName, "text-wrap"))
        {
            if (AreSame(stylePropVal, "nowrap")) style.font.flags |= FontStyleNoWrap;
            prop = StyleTextWrap;
        }
        else if (AreSame(stylePropName, "background-color") || AreSame(stylePropName, "background"))
        {
            if (StartsWith(stylePropVal, "linear-gradient"))
                style.gradient = ExtractLinearGradient(stylePropVal, GetColor, Config.userData);
            else style.bgcolor = ExtractColor(stylePropVal, GetColor, Config.userData);
            prop = StyleBackground;
        }
        else if (AreSame(stylePropName, "color"))
        {
            style.fgcolor = ExtractColor(stylePropVal, GetColor, Config.userData);
            prop = StyleFgColor;
        }
        else if (AreSame(stylePropName, "width"))
        {
            style.dimension.x = ExtractFloatWithUnit(stylePropVal, 0, Config.defaultFontSz * Config.fontScaling, 1.f, Config.scaling);
            prop = StyleWidth;
        }
        else if (AreSame(stylePropName, "height"))
        {
            style.dimension.y = ExtractFloatWithUnit(stylePropVal, 0, Config.defaultFontSz * Config.fontScaling, 1.f, Config.scaling);
            prop = StyleHeight;
        }
        else if (AreSame(stylePropName, "alignment") || AreSame(stylePropName, "text-align"))
        {
            style.alignment |= AreSame(stylePropVal, "justify") ? TextAlignJustify :
                AreSame(stylePropVal, "right") ? TextAlignRight :
                AreSame(stylePropVal, "center") ? TextAlignHCenter :
                TextAlignLeft;
            prop = StyleHAlignment;
        }
        else if (AreSame(stylePropName, "vertical-align"))
        {
            style.alignment |= AreSame(stylePropVal, "top") ? TextAlignTop :
                AreSame(stylePropVal, "bottom") ? TextAlignBottom :
                TextAlignVCenter;
            prop = StyleVAlignment;
        }
        else if (AreSame(stylePropName, "font-family"))
        {
            style.font.family = stylePropVal;
            prop = StyleFontFamily;
        }
        else if (AreSame(stylePropName, "padding"))
        {
            auto val = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, Config.scaling);
            style.padding.top = style.padding.right = style.padding.left = style.padding.bottom = val;
            prop = StylePadding;
        }
        else if (AreSame(stylePropName, "padding-top"))
        {
            auto val = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, Config.scaling);
            style.padding.top = val;
            prop = StylePadding;
        }
        else if (AreSame(stylePropName, "padding-bottom"))
        {
            auto val = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, Config.scaling);
            style.padding.bottom = val;
            prop = StylePadding;
        }
        else if (AreSame(stylePropName, "padding-left"))
        {
            auto val = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, Config.scaling);
            style.padding.left = val;
            prop = StylePadding;
        }
        else if (AreSame(stylePropName, "padding-right"))
        {
            auto val = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, Config.scaling);
            style.padding.right = val;
            prop = StylePadding;
        }
        else if (AreSame(stylePropName, "text-overflow"))
        {
            if (AreSame(stylePropVal, "ellipsis"))
            {
                style.font.flags |= FontStyleOverflowEllipsis;
                prop = StyleTextOverflow;
            }
        }
        else if (AreSame(stylePropName, "border"))
        {
            style.border.top = style.border.bottom = style.border.left = style.border.right = ExtractBorder(stylePropVal,
                Config.defaultFontSz * Config.fontScaling, 1.f, GetColor, Config.userData);
            style.border.isUniform = true;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-top"))
        {
            style.border.top = ExtractBorder(stylePropVal, Config.defaultFontSz * Config.fontScaling,
                1.f, GetColor, Config.userData);
            style.border.isUniform = false;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-left"))
        {
            style.border.left = ExtractBorder(stylePropVal, Config.defaultFontSz * Config.fontScaling,
                1.f, GetColor, Config.userData);
            style.border.isUniform = false;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-right"))
        {
            style.border.right = ExtractBorder(stylePropVal, Config.defaultFontSz * Config.fontScaling,
                1.f, GetColor, Config.userData);
            style.border.isUniform = false;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-bottom"))
        {
            style.border.bottom = ExtractBorder(stylePropVal, Config.defaultFontSz * Config.fontScaling,
                1.f, GetColor, Config.userData);
            prop = StyleBorder;
            style.border.isUniform = false;
        }
        else if (AreSame(stylePropName, "border-radius"))
        {
            auto radius = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling,
                1.f, 1.f);
            if (stylePropVal.back() == '%') style.relativeProps |= (RSP_BorderTopLeftRadius | RSP_BorderTopRightRadius | RSP_BorderBottomLeftRadius | 
                RSP_BorderBottomRightRadius);
            style.border.setRadius(radius);
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-width"))
        {
            auto width = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling,
                1.f, 1.f);
            style.border.setThickness(width);
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-color"))
        {
            auto color = ExtractColor(stylePropVal, GetColor, Config.userData);
            style.border.setColor(color);
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-top-left-radius"))
        {
            style.border.cornerRadius[TopLeftCorner] = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling,
                1.f, 1.f);
            if (stylePropVal.back() == '%') style.relativeProps |= RSP_BorderTopLeftRadius;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-top-right-radius"))
        {
            style.border.cornerRadius[TopRightCorner] = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling,
                1.f, 1.f);
            if (stylePropVal.back() == '%') style.relativeProps |= RSP_BorderTopRightRadius;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-bottom-right-radius"))
        {
            style.border.cornerRadius[BottomRightCorner] = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling,
                1.f, 1.f);
            if (stylePropVal.back() == '%') style.relativeProps |= RSP_BorderBottomRightRadius;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "border-bottom-left-radius"))
        {
            style.border.cornerRadius[BottomLeftCorner] = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling,
                1.f, 1.f);
            if (stylePropVal.back() == '%') style.relativeProps |= RSP_BorderBottomLeftRadius;
            prop = StyleBorder;
        }
        else if (AreSame(stylePropName, "margin"))
        {
            style.margin.left = style.margin.right = style.margin.top = style.margin.bottom =
                ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, 1.f);
            prop = StyleMargin;
        }
        else if (AreSame(stylePropName, "margin-top"))
        {
            style.margin.top = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, 1.f);
            prop = StyleMargin;
        }
        else if (AreSame(stylePropName, "margin-left"))
        {
            style.margin.left = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, 1.f);
            prop = StyleMargin;
        }
        else if (AreSame(stylePropName, "margin-right"))
        {
            style.margin.right = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, 1.f);
            prop = StyleMargin;
        }
        else if (AreSame(stylePropName, "margin-bottom"))
        {
            style.margin.bottom = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz * Config.fontScaling, 1.f, 1.f);
            prop = StyleMargin;
        }
        else if (AreSame(stylePropName, "font-style"))
        {
            if (AreSame(stylePropVal, "normal")) style.font.flags |= FontStyleNormal;
            else if (AreSame(stylePropVal, "italic") || AreSame(stylePropVal, "oblique"))
                style.font.flags |= FontStyleItalics;
            else ERROR("Invalid font-style property value [%.*s]\n",
                (int)stylePropVal.size(), stylePropVal.data());
            prop = StyleFontStyle;
        }
        else if (AreSame(stylePropName, "box-shadow"))
        {
            style.shadow = ExtractBoxShadow(stylePropVal, Config.defaultFontSz, 1.f, GetColor, Config.userData);
            prop = StyleBoxShadow;
        }
        else if (AreSame(stylePropName, "thumb-color"))
        {
            if (StartsWith(stylePropVal, "linear-gradient"))
            {
                style.gradient = ExtractLinearGradient(stylePropVal, GetColor, Config.userData);
            }
            else specific.toggle.thumbColor = ExtractColor(stylePropVal, GetColor, Config.userData);
            prop = StyleThumbColor;
        }
        else if (AreSame(stylePropName, "track-color"))
        {
            if (StartsWith(stylePropVal, "linear-gradient"))
            {
                style.gradient = ExtractLinearGradient(stylePropVal, GetColor, Config.userData);
            }
            else specific.toggle.trackColor = ExtractColor(stylePropVal, GetColor, Config.userData);
            prop = StyleTrackColor;
        }
        else if (AreSame(stylePropName, "track-outline"))
        {
            auto brd = ExtractBorder(stylePropVal, Config.defaultFontSz, 1.f, GetColor, Config.userData);
            specific.toggle.trackBorderColor = brd.color;
            specific.toggle.trackBorderThickness = brd.thickness;
            prop = StyleTrackOutlineColor;
        }
        else if (AreSame(stylePropName, "thumb-offset"))
        {
            specific.toggle.thumbOffset = ExtractFloatWithUnit(stylePropVal, 0.f, Config.defaultFontSz* Config.fontScaling, 1.f, 1.f);
            prop = StyleThumbOffset;
        }
        else
        {
            ERROR("Invalid style property... [%.*s]\n", (int)stylePropName.size(), stylePropName.data());
        }

        return prop;
    }

    static void CopyStyle(const StyleDescriptor& src, StyleDescriptor& dest)
    {
        if (&src == &dest || dest.specified & StyleUpdatedFromBase) return;

        for (int64_t idx = 0; idx <= StyleTotal; ++idx)
        {
            auto prop = (StyleProperty)((1ll << idx));
            if ((dest.specified & prop) == 0)
            {
                switch (prop)
                {
                case glimmer::StyleBackground:
                    dest.bgcolor = src.bgcolor;
                    dest.gradient = src.gradient;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleFgColor:
                    dest.fgcolor = src.fgcolor;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleFontSize:
                case glimmer::StyleFontFamily:
                case glimmer::StyleFontWeight:
                case glimmer::StyleFontStyle:
                    dest.font = src.font;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleHeight:
                    dest.dimension.y = src.dimension.y;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleWidth:
                    dest.dimension.x = src.dimension.x;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleHAlignment:
                    (src.alignment & TextAlignLeft) ? dest.alignment |= TextAlignLeft : dest.alignment &= ~TextAlignLeft;
                    (src.alignment & TextAlignRight) ? dest.alignment |= TextAlignRight : dest.alignment &= ~TextAlignRight;
                    (src.alignment & TextAlignHCenter) ? dest.alignment |= TextAlignHCenter : dest.alignment &= ~TextAlignHCenter;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleVAlignment:
                    (src.alignment & TextAlignTop) ? dest.alignment |= TextAlignTop : dest.alignment &= ~TextAlignTop;
                    (src.alignment & TextAlignBottom) ? dest.alignment |= TextAlignBottom : dest.alignment &= ~TextAlignBottom;
                    (src.alignment & TextAlignVCenter) ? dest.alignment |= TextAlignVCenter : dest.alignment &= ~TextAlignVCenter;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StylePadding:
                    dest.padding = src.padding;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleMargin:
                    dest.margin = src.margin;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleBorder:
                    dest.border = src.border;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleOverflow:
                    break;
                case glimmer::StyleBorderRadius:
                {
                    const auto& srcborder = src.border;
                    auto& dstborder = dest.border;
                    dstborder.cornerRadius[0] = srcborder.cornerRadius[0];
                    dstborder.cornerRadius[1] = srcborder.cornerRadius[1];
                    dstborder.cornerRadius[2] = srcborder.cornerRadius[2];
                    dstborder.cornerRadius[3] = srcborder.cornerRadius[3];
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                }
                case glimmer::StyleCellSpacing:
                    break;
                case glimmer::StyleTextWrap:
                    break;
                case glimmer::StyleBoxShadow:
                    dest.shadow = src.shadow;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleTextOverflow:
                    break;
                case glimmer::StyleMinWidth:
                    dest.mindim.x = src.mindim.x;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleMaxWidth:
                    dest.maxdim.x = src.maxdim.x;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleMinHeight:
                    dest.mindim.y = src.mindim.y;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                case glimmer::StyleMaxHeight:
                    dest.maxdim.y = src.maxdim.y;
                    if (src.specified & prop) dest.specified |= prop;
                    break;
                default:
                    break;
                }
            }
        }

        dest.specified |= StyleUpdatedFromBase;
    }

    void PushStyle(std::string_view defcss, std::string_view hovercss, std::string_view pressedcss, 
        std::string_view focusedcss, std::string_view checkedcss, std::string_view disblcss)
    {
        std::string_view css[WSI_Total] = { defcss, focusedcss, hovercss, pressedcss, checkedcss, disblcss };

        // When pushing style, the default style behaves slightly differently then rest
        // The default style inherits from parent in the stack if present, parse the CSS and gets pushed
        // The other styles, inherit from default and then parse the CSS and get pushed
        for (auto style = 0; style < WSI_Total; ++style)
        {
            if (!css[style].empty())
            {
                auto& pushed = Context.pushedStyles[style].push();

                if (Context.pushedStyles[style].size() >= 2)
                {
                    pushed = Context.pushedStyles[WSI_Default].next(1);
                    pushed.bgcolor = IM_COL32_BLACK_TRANS;
                }
                else if (style != WSI_Default)
                    pushed = Context.pushedStyles[WSI_Default].top();
                
                pushed.From(css[style]);
            }
        }
    }

    void PushStyle(WidgetState state, std::string_view css)
    {
        auto idx = log2((unsigned)state);
        
        if (idx == WSI_Default)  Context.pushedStyles[idx].push().From(css);
        else
        {
            const auto& defstyle = Context.pushedStyles[WSI_Default].top();
            auto& style = Context.pushedStyles[idx].push(defstyle);
            style.From(css);
            style.specified |= defstyle.specified;
        }
    }

    void SetNextStyle(std::string_view defcss, std::string_view hovercss, std::string_view pressedcss, 
        std::string_view focusedcss, std::string_view checkedcss, std::string_view disblcss)
    {
        std::string_view css[WSI_Total] = { defcss, focusedcss, hovercss, pressedcss, checkedcss, disblcss };

        for (auto style = 0; style < WSI_Total; ++style)
        {
            if (!css[style].empty())
            {
                Context.currStyleStates |= (1 << style);

                if (style != WSI_Default)
                {
                    Context.currStyle[style] = GetCurrentStyle(WS_Default);
                    Context.currStyle[style].bgcolor = IM_COL32_BLACK_TRANS;
                }

                Context.currStyle[style].From(css[style]);
            }
        }
    }

    StyleDescriptor& GetCurrentStyle(int32_t state)
    {
        return Context.GetStyle(state);
    }

    void PopStyle(int depth, int32_t state)
    {
        for (auto style = 0; style < WSI_Total; ++style)
        {
            if ((1 << style) & state)
            {
                auto limit = depth;
                while (!Context.pushedStyles[style].empty() && limit > 0)
                {
                    Context.pushedStyles[style].pop();
                    limit--;
                }
            }
        }
    }

    StyleDescriptor::StyleDescriptor()
    {
        font.size = Config.defaultFontSz;
        index.animation = index.custom = 0;
        border.cornerRadius[0] = border.cornerRadius[1] = border.cornerRadius[2] = border.cornerRadius[3] = 0.f;
    }

    StyleDescriptor::StyleDescriptor(std::string_view css)
        : StyleDescriptor{}
    {
        From(css);
    }

    StyleDescriptor& StyleDescriptor::Border(float thick, std::tuple<int, int, int, int> color)
    {
        border.setThickness(thick); border.setColor(ToRGBA(color));
        return *this;
    }

    StyleDescriptor& StyleDescriptor::Raised(float amount)
    {
        return *this;
    }

    StyleDescriptor& StyleDescriptor::From(std::string_view css, bool checkForDuplicate)
    {
        if (css.empty()) return *this;

        auto sidx = 0;
        int prop = 0;
        CommonWidgetStyleDescriptor desc{};

        while (sidx < (int)css.size())
        {
            sidx = SkipSpace(css, sidx);
            auto stbegin = sidx;
            while ((sidx < (int)css.size()) && (css[sidx] != ':') &&
                !std::isspace(css[sidx])) sidx++;
            auto stylePropName = css.substr(stbegin, sidx - stbegin);

            sidx = SkipSpace(css, sidx);
            if (css[sidx] == ':') sidx++;
            sidx = SkipSpace(css, sidx);

            auto stylePropVal = GetQuotedString(css.data(), sidx, (int)css.size());
            if (!stylePropVal.has_value() || stylePropVal.value().empty())
            {
                stbegin = sidx;
                while ((sidx < (int)css.size()) && css[sidx] != ';') sidx++;
                stylePropVal = css.substr(stbegin, sidx - stbegin);

                if ((sidx < (int)css.size()) && css[sidx] == ';') sidx++;
            }

            if (stylePropVal.has_value())
                prop |= PopulateSegmentStyle(*this, desc, stylePropName, stylePropVal.value());
        }

        if (prop != 0)
        {
            /*if (prop & StyleThumbColor || prop & StyleTrackColor || prop & StyleTrackOutlineColor || prop & StyleThumbOffset)
            {
                auto found = false;

                if (checkForDuplicate)
                {
                    for (auto idx = 0; idx < Context.toggleButtonStyles.size(); ++idx)
                        if (std::memcmp(&(Context.toggleButtonStyles[idx]), &(desc.toggle), sizeof(ToggleButtonStyleDescriptor)) == 0)
                        {
                            found = true;
                            index.custom = idx;
                        }
                }

                if (!found)
                {
                    index.custom = (uint16_t)Context.toggleButtonStyles.size();
                    Context.toggleButtonStyles.push_back(desc.toggle);
                }
            }*/
        }

        specified = prop;
        return *this;
    }

#pragma endregion

    // =============================================================================================
    // DRAWING FUNCTIONS
    // =============================================================================================

#pragma region Drawing functions

    IntersectRects ComputeIntersectRects(ImRect rect, ImVec2 startpos, ImVec2 endpos)
    {
        IntersectRects res;

        res.intersects[0].Max.x = startpos.x;
        res.intersects[1].Max.y = startpos.y;
        res.intersects[2].Min.x = endpos.x;
        res.intersects[3].Min.y = endpos.y;

        if (rect.Min.x < startpos.x)
        {
            res.intersects[0].Min.x = res.intersects[1].Min.x = res.intersects[3].Min.x = rect.Min.x;
        }
        else
        {
            res.intersects[1].Min.x = res.intersects[3].Min.x = rect.Min.x;
            res.visibleRect[0] = false;
        }

        if (rect.Min.y < startpos.y)
        {
            res.intersects[0].Min.y = res.intersects[1].Min.y = res.intersects[2].Min.y = rect.Min.y;
        }
        else
        {
            res.intersects[0].Min.y = res.intersects[2].Min.y = rect.Min.y;
            res.visibleRect[1] = false;
        }

        if (rect.Max.x > endpos.x)
        {
            res.intersects[1].Max.x = res.intersects[2].Max.x = res.intersects[3].Max.x = rect.Max.x;
        }
        else
        {
            res.intersects[1].Max.x = res.intersects[3].Max.x = rect.Max.x;
            res.visibleRect[2] = false;
        }

        if (rect.Max.y > endpos.y)
        {
            res.intersects[0].Max.y = res.intersects[2].Max.y = res.intersects[3].Max.y = rect.Max.y;
        }
        else
        {
            res.intersects[0].Max.y = res.intersects[2].Max.y = rect.Max.y;
            res.visibleRect[3] = false;
        }

        return res;
    }

    RectBreakup ComputeRectBreakups(ImRect rect, float amount)
    {
        RectBreakup res;

        res.rects[0].Min = ImVec2{ rect.Min.x - amount, rect.Min.y };
        res.rects[0].Max = ImVec2{ rect.Min.x, rect.Max.y };

        res.rects[1].Min = ImVec2{ rect.Min.x, rect.Min.y - amount };
        res.rects[1].Max = ImVec2{ rect.Max.x, rect.Min.y };

        res.rects[2].Min = ImVec2{ rect.Max.x, rect.Min.y };
        res.rects[2].Max = ImVec2{ rect.Max.x + amount, rect.Max.y };

        res.rects[3].Min = ImVec2{ rect.Min.x, rect.Max.y };
        res.rects[3].Max = ImVec2{ rect.Max.x, rect.Max.y + amount };

        ImVec2 delta{ amount, amount };
        res.corners[0].Min = rect.Min - delta;
        res.corners[0].Max = rect.Min;
        res.corners[1].Min = ImVec2{ rect.Max.x, rect.Min.y - amount };
        res.corners[1].Max = res.corners[1].Min + delta;
        res.corners[2].Min = rect.Max;
        res.corners[2].Max = rect.Max + delta;
        res.corners[3].Min = ImVec2{ rect.Min.x - amount, rect.Max.y };
        res.corners[3].Max = res.corners[3].Min + delta;

        return res;
    }

    void DrawBorderRect(ImVec2 startpos, ImVec2 endpos, const FourSidedBorder& border, uint32_t bgcolor, IRenderer& renderer)
    {
        if (border.isUniform && border.top.thickness > 0.f && IsColorVisible(border.top.color) &&
            border.top.color != bgcolor)
        {
            if (!border.isRounded())
                renderer.DrawRect(startpos, endpos, border.top.color, false, border.top.thickness);
            else
                renderer.DrawRoundedRect(startpos, endpos, border.top.color, false,
                    border.cornerRadius[TopLeftCorner], border.cornerRadius[TopRightCorner],
                    border.cornerRadius[BottomRightCorner], border.cornerRadius[BottomLeftCorner],
                    border.top.thickness);
        }
        else
        {
            auto width = endpos.x - startpos.x, height = endpos.y - startpos.y;

            if (border.top.thickness > 0.f && border.top.color != bgcolor && IsColorVisible(border.top.color))
                renderer.DrawLine(startpos, startpos + ImVec2{ width, 0.f }, border.top.color, border.top.thickness);
            if (border.right.thickness > 0.f && border.right.color != bgcolor && IsColorVisible(border.right.color))
                renderer.DrawLine(startpos + ImVec2{ width - border.right.thickness, 0.f }, endpos -
                    ImVec2{ border.right.thickness, 0.f }, border.right.color, border.right.thickness);
            if (border.left.thickness > 0.f && border.left.color != bgcolor && IsColorVisible(border.left.color))
                renderer.DrawLine(startpos, startpos + ImVec2{ 0.f, height }, border.left.color, border.left.thickness);
            if (border.bottom.thickness > 0.f && border.bottom.color != bgcolor && IsColorVisible(border.bottom.color))
                renderer.DrawLine(startpos + ImVec2{ 0.f, height - border.bottom.thickness }, endpos -
                    ImVec2{ 0.f, border.bottom.thickness }, border.bottom.color, border.bottom.thickness);
        }
    }

    void DrawBoxShadow(ImVec2 startpos, ImVec2 endpos, const StyleDescriptor& style, IRenderer& renderer)
    {
        // In order to draw box-shadow, the following steps are used:
        // 1. Draw the underlying rectangle with shadow color, spread and offset.
        // 2. Decompose the blur region into 8 rects i.e. 4 for corners, 4 for the sides
        // 3. For each rect, determine the vertex color i.e. shadow color or transparent,
        //    and draw a rect gradient accordingly.
        if ((style.shadow.blur > 0.f || style.shadow.spread > 0.f || style.shadow.offset.x != 0.f ||
            style.shadow.offset.y != 0) && IsColorVisible(style.shadow.color))
        {
            ImRect rect{ startpos, endpos };
            rect.Expand(style.shadow.spread);
            rect.Translate(style.shadow.offset);

            if (style.shadow.blur > 0.f)
            {
                auto outercol = style.shadow.color & ~IM_COL32_A_MASK;
                auto brk = ComputeRectBreakups(rect, style.shadow.blur);

                // Sides: Left -> Top -> Right -> Bottom
                renderer.DrawRectGradient(brk.rects[0].Min, brk.rects[0].Max, outercol, style.shadow.color, DIR_Horizontal);
                renderer.DrawRectGradient(brk.rects[1].Min, brk.rects[1].Max, outercol, style.shadow.color, DIR_Vertical);
                renderer.DrawRectGradient(brk.rects[2].Min, brk.rects[2].Max, style.shadow.color, outercol, DIR_Horizontal);
                renderer.DrawRectGradient(brk.rects[3].Min, brk.rects[3].Max, style.shadow.color, outercol, DIR_Vertical);

                // Corners: Top-left -> Top-right -> Bottom-right -> Bottom-left
                switch (Config.shadowQuality)
                {
                case BoxShadowQuality::Balanced:
                {
                    ImVec2 center = brk.corners[0].Max;
                    auto radius = brk.corners[0].GetHeight() - 0.5f; // all corners of same size
                    ImVec2 offset{ radius / 2.f, radius / 2.f };
                    radius *= 1.75f;

                    renderer.SetClipRect(brk.corners[0].Min, brk.corners[0].Max);
                    renderer.DrawRadialGradient(center + offset, radius, style.shadow.color, outercol, 180, 270);
                    renderer.ResetClipRect();

                    center = ImVec2{ brk.corners[1].Min.x, brk.corners[1].Max.y };
                    renderer.SetClipRect(brk.corners[1].Min, brk.corners[1].Max);
                    renderer.DrawRadialGradient(center + ImVec2{ -offset.x, offset.y }, radius, style.shadow.color, outercol, 270, 360);
                    renderer.ResetClipRect();

                    center = brk.corners[2].Min;
                    renderer.SetClipRect(brk.corners[2].Min, brk.corners[2].Max);
                    renderer.DrawRadialGradient(center - offset, radius, style.shadow.color, outercol, 0, 90);
                    renderer.ResetClipRect();

                    center = ImVec2{ brk.corners[3].Max.x, brk.corners[3].Min.y };
                    renderer.SetClipRect(brk.corners[3].Min, brk.corners[3].Max);
                    renderer.DrawRadialGradient(center + ImVec2{ offset.x, -offset.y }, radius, style.shadow.color, outercol, 90, 180);
                    renderer.ResetClipRect();
                    break;
                }
                default:
                    break;
                }
            }

            rect.Expand(style.shadow.blur > 0.f ? 1.f : 0.f);
            if (!style.border.isRounded())
                renderer.DrawRect(rect.Min, rect.Max, style.shadow.color, true);
            else
                renderer.DrawRoundedRect(rect.Min, rect.Max, style.shadow.color, true,
                    style.border.cornerRadius[TopLeftCorner], style.border.cornerRadius[TopRightCorner],
                    style.border.cornerRadius[BottomRightCorner], style.border.cornerRadius[BottomLeftCorner]);

            auto diffcolor = Config.bgcolor - 1;
            auto border = style.border;
            border.setColor(Config.bgcolor);
            DrawBorderRect(startpos, endpos, style.border, diffcolor, renderer);
            if (!border.isRounded())
                renderer.DrawRect(startpos, endpos, Config.bgcolor, true);
            else
                renderer.DrawRoundedRect(startpos, endpos, Config.bgcolor, true,
                    style.border.cornerRadius[TopLeftCorner], style.border.cornerRadius[TopRightCorner],
                    style.border.cornerRadius[BottomRightCorner], style.border.cornerRadius[BottomLeftCorner]);
        }
    }

    template <typename ItrT>
    void DrawLinearGradient(ImVec2 initpos, ImVec2 endpos, float angle, ImGuiDir dir,
        ItrT start, ItrT end, const FourSidedBorder& border, IRenderer& renderer)
    {
        if (!border.isRounded())
        {
            auto width = endpos.x - initpos.x;
            auto height = endpos.y - initpos.y;

            if (dir == ImGuiDir::ImGuiDir_Left)
            {
                for (auto it = start; it != end; ++it)
                {
                    auto extent = width * it->pos;
                    renderer.DrawRectGradient(initpos, initpos + ImVec2{ extent, height },
                        it->from, it->to, DIR_Horizontal);
                    initpos.x += extent;
                }
            }
            else if (dir == ImGuiDir::ImGuiDir_Down)
            {
                for (auto it = start; it != end; ++it)
                {
                    auto extent = height * it->pos;
                    renderer.DrawRectGradient(initpos, initpos + ImVec2{ width, extent },
                        it->from, it->to, DIR_Vertical);
                    initpos.y += extent;
                }
            }
        }
        else
        {

        }
    }

    void DrawBackground(ImVec2 startpos, ImVec2 endpos, const StyleDescriptor& style, IRenderer& renderer)
    {
        if (style.gradient.totalStops != 0)
            (style.gradient.dir == ImGuiDir_Down || style.gradient.dir == ImGuiDir_Left) ?
            DrawLinearGradient(startpos, endpos, style.gradient.angleDegrees, style.gradient.dir,
                std::begin(style.gradient.colorStops), std::begin(style.gradient.colorStops) + style.gradient.totalStops, style.border, renderer) :
            DrawLinearGradient(startpos, endpos, style.gradient.angleDegrees, style.gradient.dir,
                std::rbegin(style.gradient.colorStops), std::rbegin(style.gradient.colorStops) + style.gradient.totalStops, style.border, renderer);
        else if (IsColorVisible(style.bgcolor))
            if (!style.border.isRounded())
                renderer.DrawRect(startpos, endpos, style.bgcolor, true);
            else
                renderer.DrawRoundedRect(startpos, endpos, style.bgcolor, true,
                    style.border.cornerRadius[TopLeftCorner], style.border.cornerRadius[TopRightCorner],
                    style.border.cornerRadius[BottomRightCorner], style.border.cornerRadius[BottomLeftCorner]);
    }

    void DrawText(ImVec2 startpos, ImVec2 endpos, const ImRect& textrect, std::string_view text, bool disabled, 
        const StyleDescriptor& style, IRenderer& renderer, std::optional<int32_t> txtflags = std::nullopt)
    {
        ImRect content{ startpos, endpos };
        
        renderer.SetClipRect(content.Min, content.Max);
        renderer.SetCurrentFont(style.font.font, style.font.size);
        auto flags = txtflags.has_value() ? txtflags.value() : style.font.flags;

        if ((flags & FontStyleOverflowMarquee) != 0 &&
            !disabled && style.index.animation != InvalidIdx)
        {
            auto& animation = Context.animations[style.index.animation];
            ImVec2 textsz = textrect.GetWidth() == 0.f ? renderer.GetTextSize(text, style.font.font, style.font.size) :
                textrect.GetSize();

            if (textsz.x > content.GetWidth())
            {
                animation.moveByPixel(1.f, content.GetWidth(), -textsz.x);
                content.Min.x += animation.offset;
                renderer.DrawText(text, content.Min, style.fgcolor, style.dimension.x > 0 ? style.dimension.x : -1.f);
            }
            else
                renderer.DrawText(text, textrect.Min, style.fgcolor, textrect.GetWidth());
        }
        else if (flags & FontStyleOverflowEllipsis)
        {
            ImVec2 textsz = textrect.GetWidth() == 0.f ? renderer.GetTextSize(text, style.font.font, style.font.size) :
                textrect.GetSize();

            if (textsz.x > content.GetWidth())
            {
                float width = 0.f, available = content.GetWidth() - renderer.EllipsisWidth(style.font.font, style.font.size);

                for (auto chidx = 0; chidx < (int)text.size(); ++chidx)
                {
                    // TODO: This is only valid for ASCII, this should be fixed for UTF-8
                    auto w = renderer.GetTextSize(text.substr(chidx, 1), style.font.font, style.font.size).x;
                    if (width + w > available)
                    {
                        renderer.DrawText(text.substr(0, chidx), content.Min, style.fgcolor, -1.f);
                        renderer.DrawText("...", content.Min + ImVec2{ width, 0.f }, style.fgcolor, -1.f);
                        break;
                    }
                    width += w;
                }
            }
            else renderer.DrawText(text, textrect.Min, style.fgcolor, textrect.GetWidth());
        }
        else
            renderer.DrawText(text, textrect.Min, style.fgcolor, textrect.GetWidth());

        renderer.ResetFont();
        renderer.ResetClipRect();
    }

#pragma endregion

    // =============================================================================================
    // LAYOUT FUNCTIONS
    // =============================================================================================

#pragma region Layout functions

    void PushSpan(int32_t direction)
    {
        Context.currSpanDepth++;
        Context.spans[Context.currSpanDepth].direction = direction;
    }

    void SetSpan(int32_t direction)
    {
        PushSpan(direction);
        Context.spans[Context.currSpanDepth].popWhenUsed = true;
    }

    void Move(int32_t direction)
    {
        assert(Context.lastItemId != -1);
        Move(Context.lastItemId, direction);
    }

    void Move(int32_t id, int32_t direction)
    {
        const auto& geometry = Context.GetGeometry(id);
        Context.nextpos = geometry.Min;
        if (direction & FD_Horizontal) Context.nextpos.x = geometry.Max.x;
        if (direction & FD_Vertical) Context.nextpos.y = geometry.Max.y;
    }

    void Move(int32_t hid, int32_t vid, bool toRight, bool toBottom)
    {
        const auto& hgeometry = Context.GetGeometry(hid);
        const auto& vgeometry = Context.GetGeometry(vid);
        Context.nextpos.x = toRight ? hgeometry.Max.x : hgeometry.Min.x;
        Context.nextpos.y = toBottom ? vgeometry.Max.y : vgeometry.Min.y;
    }

    void Move(ImVec2 amount, int32_t direction)
    {
        if (direction & ToLeft) amount.x = -amount.x;
        if (direction & ToTop) amount.y = -amount.y;
        Context.nextpos += amount;
    }

    void Move(ImVec2 pos)
    {
        Context.nextpos = pos;
    }

    void PopSpan(int depth)
    {
        while (depth > 0 && Context.currSpanDepth > -1)
        {
            Context.spans[Context.currSpanDepth] = ElementSpan{};
            --Context.currSpanDepth;
            --depth;
        }
    }

    static void AlignLayoutAxisItems(LayoutDescriptor& layout)
    {
        switch (layout.type)
        {
        case Layout::Horizontal:
        {
            // If wrapping is enabled and the alignment is horizontally centered,
            // perform h-centering of the current row of widgets and move to next row
            // Otherwise, if output should be justified, move all widgets to specific
            // location after distributing the diff equally...
            if ((layout.alignment & TextAlignHCenter) && (layout.sizing & ExpandH))
            {
                auto totalspacing = 2.f * layout.spacing.x * (float)layout.currcol;
                auto hdiff = (layout.geometry.GetWidth() - layout.rows[layout.currow].x - totalspacing) * 0.5f;

                if (hdiff > 0.f)
                {
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];

                        if (widget.row == layout.currow)
                        {
                            widget.content.TranslateX(hdiff);
                            widget.padding.TranslateX(hdiff);
                            widget.border.TranslateX(hdiff);
                            widget.margin.TranslateX(hdiff);
                        }
                    }
                }
            }
            else if ((layout.alignment & TextAlignJustify) && (layout.sizing & ExpandH))
            {
                auto hdiff = (layout.geometry.GetWidth() - layout.rows[layout.currow].x) /
                    ((float)layout.currcol + 1.f);

                if (hdiff > 0.f)
                {
                    auto currposx = hdiff;
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];

                        if (widget.row == layout.currow)
                        {
                            auto translatex = currposx - widget.margin.Min.x;
                            widget.content.TranslateX(translatex);
                            widget.padding.TranslateX(translatex);
                            widget.border.TranslateX(translatex);
                            widget.margin.TranslateX(translatex);
                            currposx += widget.margin.GetWidth() + hdiff;
                        }
                    }
                }
            }
            break;
        }

        case Layout::Vertical:
        {
            // Similar logic as for horizontal alignment, implemented for vertical here
            if ((layout.alignment & TextAlignVCenter) && (layout.sizing & ExpandV))
            {
                auto totalspacing = 2.f * layout.spacing.x * (float)layout.currow;
                auto hdiff = (layout.geometry.GetHeight() - layout.cols[layout.currcol].x - totalspacing) * 0.5f;

                if (hdiff > 0.f)
                {
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];
                        if (widget.col == layout.currcol)
                        {
                            widget.content.TranslateY(hdiff);
                            widget.padding.TranslateY(hdiff);
                            widget.border.TranslateY(hdiff);
                            widget.margin.TranslateY(hdiff);
                        }
                    }
                }
            }
            else if ((layout.alignment & TextAlignJustify) && (layout.sizing & ExpandV))
            {
                auto hdiff = (layout.geometry.GetHeight() - layout.cols[layout.currcol].x) /
                    ((float)layout.currow + 1.f);

                if (hdiff > 0.f)
                {
                    auto currposx = hdiff;
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];
                        if (widget.col == layout.currcol)
                        {
                            auto translatex = currposx - widget.margin.Min.y;
                            widget.content.TranslateY(translatex);
                            widget.padding.TranslateY(translatex);
                            widget.border.TranslateY(translatex);
                            widget.margin.TranslateY(translatex);
                            currposx += widget.margin.GetHeight() + hdiff;
                        }
                    }
                }
            }
            break;
        }
        }
    }

    static void AlignCrossAxisItems(LayoutDescriptor& layout, int depth)
    {
        if ((layout.sizing & ExpandH) == 0)
            layout.geometry.Max.x = Context.layoutItems[layout.itemidx].margin.Max.x = layout.maxdim.x;
        if ((layout.sizing & ExpandV) == 0)
            layout.geometry.Max.y = Context.layoutItems[layout.itemidx].margin.Max.y = layout.maxdim.y;

        switch (layout.type)
        {
        case Layout::Horizontal:
        {
            if ((layout.alignment & TextAlignVCenter) && (layout.sizing & ExpandV))
            {
                auto totalspacing = (float)layout.currow * layout.spacing.y;
                auto cumulativey = layout.cumulative.y + layout.maxdim.y;
                auto vdiff = (layout.geometry.GetHeight() - totalspacing - cumulativey) * 0.5f;

                if (vdiff > 0.f)
                {
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];
                        widget.margin.TranslateY(vdiff);
                        widget.border.TranslateY(vdiff);
                        widget.padding.TranslateY(vdiff);
                        widget.content.TranslateY(vdiff);
                    }
                }
            }
            else if ((layout.alignment & TextAlignJustify) && (layout.sizing & ExpandV))
            {
                auto cumulativey = layout.cumulative.y + layout.maxdim.y;
                auto vdiff = (layout.geometry.GetHeight() - cumulativey) / ((float)layout.currow + 1.f);

                if (vdiff > 0.f)
                {
                    auto currposy = vdiff;
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];
                        auto translatey = currposy - widget.margin.Min.y;
                        widget.margin.TranslateY(translatey);
                        widget.border.TranslateY(translatey);
                        widget.padding.TranslateY(translatey);
                        widget.content.TranslateY(translatey);
                        currposy += vdiff + widget.margin.GetHeight();
                    }
                }
            }
            break;
        }

        case Layout::Vertical:
        {
            if ((layout.alignment & TextAlignHCenter) && (layout.sizing & ExpandH))
            {
                auto totalspacing = (float)layout.currcol * layout.spacing.x;
                auto cumulativex = layout.cumulative.x + layout.maxdim.x;
                auto hdiff = (layout.geometry.GetWidth() - totalspacing - cumulativex) * 0.5f;

                if (hdiff > 0.f)
                {
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];
                        widget.margin.TranslateX(hdiff);
                        widget.border.TranslateX(hdiff);
                        widget.padding.TranslateX(hdiff);
                        widget.content.TranslateX(hdiff);
                    }
                }
            }
            else if ((layout.alignment & TextAlignJustify) && (layout.sizing & ExpandH))
            {
                auto cumulativex = layout.cumulative.x + layout.maxdim.x;
                auto hdiff = (layout.geometry.GetWidth() - cumulativex) / ((float)layout.currcol + 1.f);

                if (hdiff > 0.f)
                {
                    auto currposy = hdiff;
                    for (auto idx = layout.from; idx <= layout.to; ++idx)
                    {
                        auto& widget = Context.layoutItems[idx];
                        auto translatex = currposy - widget.margin.Min.x;
                        widget.margin.TranslateX(translatex);
                        widget.border.TranslateX(translatex);
                        widget.padding.TranslateX(translatex);
                        widget.content.TranslateX(translatex);
                        currposy += hdiff + widget.margin.GetWidth();
                    }
                }
            }
            break;
        }
        }
    }

    static ImVec2 AddItemToLayout(LayoutDescriptor& layout, LayoutItemDescriptor& item)
    {
        ImVec2 offset = layout.nextpos;

        switch (layout.type)
        {
        case Layout::Horizontal:
        {
            auto width = item.margin.GetWidth();

            if ((layout.sizing & ExpandH) && (layout.hofmode == OverflowMode::Wrap))
            {
                if (layout.nextpos.x + width > layout.geometry.Max.x)
                {
                    layout.geometry.Max.y += layout.maxdim.y;
                    offset = ImVec2{ layout.geometry.Min.x, layout.geometry.Max.y - layout.maxdim.y };
                    layout.nextpos.x = offset.x + width;
                    layout.nextpos.y = offset.y;
                    AlignLayoutAxisItems(layout);

                    layout.cumulative.y += layout.maxdim.y;
                    layout.maxdim.y = 0.f;
                    layout.currcol = 0;
                    layout.currow++;
                    item.row = layout.currow;
                    item.col = 0;
                    layout.rows[layout.currow].x = 0.f;
                }
                else
                {
                    layout.maxdim.y = std::max(layout.maxdim.y, item.margin.GetHeight());
                    layout.nextpos.x += width;
                    layout.rows[layout.currow].x += width;
                    item.col = layout.currcol;
                    item.row = layout.currow;
                    layout.currcol++;
                }
            }
            else
            {
                if ((layout.sizing & ExpandH) == 0) layout.geometry.Max.x += width;
                layout.nextpos.x += width;
                layout.maxdim.y = std::max(layout.maxdim.y, item.margin.GetHeight());
                if ((layout.sizing & ExpandV) == 0) layout.geometry.Max.y = layout.geometry.Min.y + layout.maxdim.y;
                else layout.cumulative.y = layout.maxdim.y;
            }
            break;
        }

        case Layout::Vertical:
        {
            auto height = item.margin.GetHeight();

            if ((layout.sizing & ExpandV) && (layout.vofmode == OverflowMode::Wrap))
            {
                if (layout.nextpos.y + height > layout.geometry.Max.y)
                {
                    layout.geometry.Max.x += layout.maxdim.x;
                    offset = ImVec2{ layout.geometry.Max.x - layout.maxdim.x, layout.geometry.Min.y };
                    layout.nextpos.x = offset.x;
                    layout.nextpos.y = offset.y + height;
                    AlignLayoutAxisItems(layout);

                    layout.maxdim.x = 0.f;
                    layout.currow = 0;
                    layout.currcol++;
                    item.col = layout.currcol;
                    layout.cols[layout.currcol].y = 0.f;
                }
                else
                {
                    layout.maxdim.x = std::max(layout.maxdim.x, item.margin.GetWidth());
                    layout.nextpos.y += height;
                    layout.cols[layout.currcol].y += height;
                    item.col = layout.currcol;
                    item.row = layout.currow;
                    layout.currow++;
                }
            }
            else
            {
                if ((layout.sizing & ExpandV) == 0) layout.geometry.Max.y += height;
                layout.nextpos.y += height;
                layout.maxdim.x = std::max(layout.maxdim.x, item.margin.GetWidth());
                if ((layout.sizing & ExpandH) == 0) layout.geometry.Max.x = layout.geometry.Min.x + layout.maxdim.x;
            }
            break;
        }

        case Layout::Grid:
        {
            
            
            break;
        }
        default:
            break;
        }

        Context.layoutItems.push_back(item);
        if (layout.from == -1) layout.from = layout.to = (int16_t)(Context.layoutItems.size() - 1);
        else layout.to = (int16_t)(Context.layoutItems.size() - 1);
        layout.itemidx = layout.to;
        return offset;
    }

    static void ComputeInitialGeometry(LayoutDescriptor& layout)
    {
        if (Context.currLayoutDepth > 0)
        {
            auto& parent = Context.layouts[Context.currLayoutDepth - 1];
            const auto& sizing = Context.sizing[Context.currSizingDepth];

            // Ideally, for left or right alignment for parent, and expand for child
            // The child should be flexible and shrink to accommodate siblings?
            // TODO: Add a shrink flag to layouts?
            if ((layout.fill & FD_Horizontal) && (parent.sizing & ExpandH))
            {
                layout.geometry.Min.x = parent.nextpos.x + layout.spacing.x;
                layout.geometry.Max.x = parent.prevpos.x - parent.spacing.x;
            }
            else if (parent.alignment & TextAlignLeft)
            {
                layout.geometry.Min.x = parent.nextpos.x + layout.spacing.x;

                if (sizing.horizontal != FIT_SZ)
                {
                    auto hmeasure = sizing.relativeh ?
                        (parent.geometry.GetWidth() * sizing.horizontal) :
                        sizing.horizontal;
                    layout.geometry.Max.x = layout.geometry.Min.x + hmeasure;
                }
                else
                {
                    layout.sizing &= ~ExpandH;
                    layout.geometry.Max.x = layout.geometry.Min.x + layout.spacing.x;
                }

                parent.nextpos.x = layout.geometry.Max.x + parent.spacing.x;
            }
            else if (parent.alignment & TextAlignRight)
            {
                layout.geometry.Max.x = parent.prevpos.x - layout.spacing.x;

                if (layout.fill & FD_Horizontal) layout.geometry.Min.x = parent.nextpos.x + layout.spacing.x;
                else if (sizing.horizontal != FIT_SZ)
                {
                    auto hmeasure = sizing.relativeh ?
                        (parent.geometry.GetWidth() * sizing.horizontal) :
                        sizing.horizontal;
                    layout.geometry.Min.x = layout.geometry.Max.x - hmeasure;
                }

                parent.prevpos.x = layout.geometry.Min.x - parent.spacing.x;
            }

            if ((layout.fill & FD_Vertical) && (parent.sizing & ExpandV))
            {
                layout.geometry.Max.y = parent.prevpos.y - parent.spacing.y;
                layout.geometry.Min.y = parent.nextpos.y + layout.spacing.y;
            }
            else if (parent.alignment & TextAlignTop)
            {
                layout.geometry.Min.y = parent.nextpos.y + layout.spacing.y;

                if (sizing.vertical != FIT_SZ)
                {
                    auto vmeasure = sizing.relativev ?
                        (parent.geometry.GetHeight() * sizing.vertical) :
                        sizing.vertical;
                    layout.geometry.Max.y = layout.geometry.Min.y + vmeasure;
                }
                else
                {
                    layout.sizing &= ~ExpandV;
                    layout.geometry.Max.y = layout.geometry.Min.y + layout.spacing.y;
                }

                parent.nextpos.y = layout.geometry.Max.y + parent.spacing.y;
            }
            else if (parent.alignment & TextAlignBottom)
            {
                layout.geometry.Max.y = parent.prevpos.y - layout.spacing.y;

                if (layout.fill & FD_Vertical) layout.geometry.Min.y = parent.nextpos.y + layout.spacing.y;
                else if (sizing.vertical != FIT_SZ)
                {
                    auto vmeasure = sizing.relativev ?
                        (parent.geometry.GetHeight() * sizing.vertical) :
                        sizing.vertical;
                    layout.geometry.Min.y = layout.geometry.Max.y - vmeasure;
                }

                parent.prevpos.y = layout.geometry.Max.y - parent.spacing.y;
            }
        }
        else
        {
            layout.geometry.Max = ImGui::GetCurrentWindow()->Size;
        }

        layout.geometry.Min += ImVec2{ layout.border.left.thickness, layout.border.top.thickness };
        layout.geometry.Max -= ImVec2{ layout.border.right.thickness, layout.border.bottom.thickness };
        layout.nextpos = layout.geometry.Min + layout.spacing;

        if (layout.sizing & ExpandH) layout.prevpos.x = layout.geometry.Max.x - layout.spacing.x;
        if (layout.sizing & ExpandV) layout.prevpos.y = layout.geometry.Max.y - layout.spacing.y;
    }

    ImRect BeginLayout(Layout type, FillDirection fill, int32_t alignment, ImVec2 spacing, const NeighborWidgets& neighbors)
    {
        assert(Context.currSizingDepth > -1);
        Context.currLayoutDepth++;

        auto& layout = Context.layouts[Context.currLayoutDepth];
        layout.type = type;
        layout.alignment = alignment;
        layout.fill = fill;
        layout.spacing = spacing;

        ComputeInitialGeometry(layout);
        return layout.geometry;
    }

    ImRect BeginLayout(Layout type, std::string_view css, const NeighborWidgets& neighbors)
    {
        auto sidx = 0;
        CommonWidgetStyleDescriptor desc{};
        Context.currLayoutDepth++;

        auto& layout = Context.layouts[Context.currLayoutDepth];
        layout.type = type;
        auto pwidth = Context.currLayoutDepth > 0 ? Context.layouts[Context.currLayoutDepth - 1].geometry.GetWidth() : 1.f;
        auto pheight = Context.currLayoutDepth > 0 ? Context.layouts[Context.currLayoutDepth - 1].geometry.GetHeight() : 1.f;
        Sizing sizing;
        auto hasSizing = false;

        while (sidx < (int)css.size())
        {
            sidx = SkipSpace(css, sidx);
            auto stbegin = sidx;
            while ((sidx < (int)css.size()) && (css[sidx] != ':') &&
                !std::isspace(css[sidx])) sidx++;
            auto stylePropName = css.substr(stbegin, sidx - stbegin);

            sidx = SkipSpace(css, sidx);
            if (css[sidx] == ':') sidx++;
            sidx = SkipSpace(css, sidx);

            auto stylePropVal = GetQuotedString(css.data(), sidx, (int)css.size());
            if (!stylePropVal.has_value() || stylePropVal.value().empty())
            {
                stbegin = sidx;
                while ((sidx < (int)css.size()) && css[sidx] != ';') sidx++;
                stylePropVal = css.substr(stbegin, sidx - stbegin);

                if ((sidx < (int)css.size()) && css[sidx] == ';') sidx++;
            }

            if (stylePropVal.has_value())
            {
                if (AreSame(stylePropName, "width"))
                {
                    sizing.horizontal = ExtractFloatWithUnit(stylePropVal.value(), 0.f,
                        Config.defaultFontSz, pwidth, 1.f);
                    sizing.relativeh = stylePropVal.value().back() == '%';
                    hasSizing = true;
                }
                else if (AreSame(stylePropName, "height"))
                {
                    sizing.vertical = ExtractFloatWithUnit(stylePropVal.value(), 0.f,
                        Config.defaultFontSz, pheight, 1.f);
                    sizing.relativev = stylePropVal.value().back() == '%';
                    hasSizing = true;
                }
                else if (AreSame(stylePropName, "spacing-x")) layout.spacing.x =
                    ExtractFloatWithUnit(stylePropVal.value(), 0.f, Config.defaultFontSz, 1.f, 1.f);
                else if (AreSame(stylePropName, "spacing-y")) layout.spacing.x =
                    ExtractFloatWithUnit(stylePropVal.value(), 0.f, Config.defaultFontSz, 1.f, 1.f);
                else if (AreSame(stylePropName, "spacing")) layout.spacing.x = layout.spacing.y =
                    ExtractFloatWithUnit(stylePropVal.value(), 0.f, Config.defaultFontSz, 1.f, 1.f);
                else if (AreSame(stylePropName, "overflow-x")) layout.hofmode = AreSame(stylePropVal.value(), "clip") ?
                    OverflowMode::Clip : AreSame(stylePropVal.value(), "scroll") ? OverflowMode::Scroll : OverflowMode::Wrap;
                else if (AreSame(stylePropName, "overflow-y")) layout.vofmode = AreSame(stylePropVal.value(), "clip") ?
                    OverflowMode::Clip : AreSame(stylePropVal.value(), "scroll") ? OverflowMode::Scroll : OverflowMode::Wrap;
                else if (AreSame(stylePropName, "overflow")) layout.vofmode = layout.hofmode = AreSame(stylePropVal.value(), "clip") ?
                    OverflowMode::Clip : AreSame(stylePropVal.value(), "scroll") ? OverflowMode::Scroll : OverflowMode::Wrap;
                else if (AreSame(stylePropName, "halign") || AreSame(stylePropName, "horizontal-align"))
                    layout.alignment |= AreSame(stylePropVal.value(), "right") ? TextAlignRight :
                    AreSame(stylePropVal.value(), "center") ? TextAlignHCenter : TextAlignLeft;
                else if (AreSame(stylePropName, "valign") || AreSame(stylePropName, "vertical-align"))
                    layout.alignment |= AreSame(stylePropVal.value(), "bottom") ? TextAlignBottom :
                    AreSame(stylePropVal.value(), "center") ? TextAlignVCenter : TextAlignTop;
                else if (AreSame(stylePropName, "align") && AreSame(stylePropVal.value(), "center"))
                    layout.alignment = TextAlignCenter;
                else if (AreSame(stylePropName, "fill")) layout.fill = AreSame(stylePropVal.value(), "all") ?
                    FD_Horizontal | FD_Vertical : AreSame(stylePropVal.value(), "horizontal") ?
                    FD_Horizontal : AreSame(stylePropVal.value(), "vertical") ?
                    FD_Vertical : FD_None;
                else if (AreSame(stylePropName, "border"))
                {
                    layout.border.top = layout.border.bottom = layout.border.left = layout.border.right = ExtractBorder(stylePropVal.value(),
                        Config.defaultFontSz * Config.fontScaling, 1.f, GetColor, Config.userData);
                    layout.border.isUniform = true;
                }
                else if (AreSame(stylePropName, "border-top"))
                {
                    layout.border.top = ExtractBorder(stylePropVal.value(), Config.defaultFontSz * Config.fontScaling,
                        1.f, GetColor, Config.userData);
                    layout.border.isUniform = false;
                }
                else if (AreSame(stylePropName, "border-left"))
                {
                    layout.border.left = ExtractBorder(stylePropVal.value(), Config.defaultFontSz * Config.fontScaling,
                        1.f, GetColor, Config.userData);
                    layout.border.isUniform = false;
                }
                else if (AreSame(stylePropName, "border-right"))
                {
                    layout.border.right = ExtractBorder(stylePropVal.value(), Config.defaultFontSz * Config.fontScaling,
                        1.f, GetColor, Config.userData);
                    layout.border.isUniform = false;
                }
                else if (AreSame(stylePropName, "border-bottom"))
                {
                    layout.border.bottom = ExtractBorder(stylePropVal.value(), Config.defaultFontSz * Config.fontScaling,
                        1.f, GetColor, Config.userData);
                    layout.border.isUniform = false;
                }
            }
        }

        if (hasSizing)
        {
            PushSizing(sizing.horizontal, sizing.vertical, sizing.relativeh, sizing.relativev);
            layout.popSizingOnEnd = true;
        }

        ComputeInitialGeometry(layout);
        return layout.geometry;
    }

    void PushSizing(float width, float height, bool relativew, bool relativeh)
    {
        Context.currSizingDepth++;
        auto& sizing = Context.sizing[Context.currSizingDepth];
        sizing.horizontal = width;
        sizing.vertical = height;
        sizing.relativeh = relativew;
        sizing.relativev = relativeh;
    }

    void PopSizing(int depth)
    {
        while (depth > 0 && Context.currSizingDepth > 0)
        {
            Context.sizing[Context.currSizingDepth] = Sizing{};
            Context.currSizingDepth--;
            depth--;
        }
    }

    // Declarations of widget drawing impls:
    WidgetDrawResult ButtonImpl(int32_t id, const ImRect& margin, const ImRect& border, const ImRect& padding, const ImRect& content, const ImRect& textpos, IRenderer& renderer);
    WidgetDrawResult LabelImpl(int32_t id, const ImRect& margin, const ImRect& border, const ImRect& padding, const ImRect& content, const ImRect& textpos, IRenderer& renderer);

    ImRect EndLayout(int depth)
    {
        ImRect res;

        while (depth > 0 && Context.currLayoutDepth > -1)
        {
            // Align items in last row/column and align overall items
            // in cross-axis for flex-row/column layouts
            auto& layout = Context.layouts[Context.currLayoutDepth];
            AlignLayoutAxisItems(layout);
            AlignCrossAxisItems(layout, depth);
            //Context.layoutItems[layout.itemidx].viewport = layout.nextpos;

            // Update parent's nextpos as this layout is complete. This step is
            // necessary as sublayout may need to perform actual layout before
            // knowing its own dimension
            if (Context.currLayoutDepth > 0)
                Context.layouts[Context.currLayoutDepth - 1].nextpos +=
                    ImVec2{ layout.geometry.GetWidth(), layout.geometry.GetHeight() };

            DrawBorderRect(layout.geometry.Min, layout.geometry.Max, layout.border, Config.bgcolor,
                *Config.renderer);

            if (layout.popSizingOnEnd) PopSizing();

            if (Context.currLayoutDepth > 0)
            {
                auto& parent = Context.layouts[Context.currLayoutDepth];
                LayoutItemDescriptor desc;
                desc.wtype = WT_Sublayout;
                desc.margin = layout.geometry;
                desc.id = Context.currLayoutDepth;
                AddItemToLayout(parent, desc);
            }

            if (Context.currLayoutDepth == 0)
            {
                /*ImVec2 initpos{0.f, 0.f};
                //int layoutidxs[128];
                //auto totalLayouts = 0;
                //std::fill(std::begin(layoutidxs), std::end(layoutidxs), -1);

                //// Extract all sublayouts
                //for (auto idx = 0; idx < (int)Context.layoutItems.size(); ++idx)
                //{
                //    if (Context.layoutItems[idx].wtype == WT_Sublayout)
                //    {
                //        layoutidxs[totalLayouts++] = idx;
                //        totalLayouts++;
                //    }
                //}

                //// Sort by depth
                //std::sort(std::begin(layoutidxs), std::begin(layoutidxs) + totalLayouts, [](int lhs, int rhs) {
                //        auto lhsdepth = Context.layoutItems[lhs].id;
                //        auto rhsdepth = Context.layoutItems[rhs].id;
                //        return lhsdepth == rhsdepth ? lhs < rhs : lhsdepth < rhsdepth;
                //    });

                /*auto currdepth = 0;
                for (auto layoutidx = 0; layoutidx < totalLayouts; layoutidx++)
                {
                    auto& layout = Context.layoutItems[layoutidx];

                    if (layout.id > 0)
                    {
                        auto width = layout.margin.GetWidth();
                        layout.margin.Min.x = initpos.x;
                        layout.margin.Max.x = layout.margin.Min.x + width;

                    }

                    currdepth = layout.id;
                }*/

                // Handle scroll panes...
                for (auto& widget : Context.layoutItems)
                {
                    switch (widget.wtype)
                    {
                    case WT_Label:
                    {
                        LabelImpl(widget.id, widget.margin, widget.border, widget.padding, widget.content, widget.text,
                            *Config.renderer);
                        break;
                    }
                    case WT_Button:
                    {
                        ButtonImpl(widget.id, widget.margin, widget.border, widget.padding, widget.content, widget.text,
                            *Config.renderer);
                        break;
                    }
                    default:
                        break;
                    }
                }
            }

            res = Context.layouts[Context.currLayoutDepth].geometry;
            Context.layouts[Context.currLayoutDepth] = LayoutDescriptor{};
            --Context.currLayoutDepth;
            --depth;
        }

        return res;
    }

#pragma endregion

    // =============================================================================================
    // WIDGET FUNCTIONS
    // =============================================================================================

#pragma region Widgets

    static bool IsValueBetween(float val, float from, float to)
    {
        return val > from && val < to;
    }

    /* Here is the box model that is followed here:

            +--------------------------------+
            |            margin              |
            |   +------------------------+   |
            |   |       border           |   |
            |   |   +--------------+     |   |
            |   |   |   padding    |     |   |
            |   |   |  +--------+  |     |   |
            |   |   |  |        |  |     |   |
            |   |   |  |content |  |     |   |
            |   |   |  |        |  |     |   |
            |   |   |  +--------+  |     |   |
            |   |   |              |     |   |
            |   |   +--------------+     |   |
            |   |                        |   |
            |   +------------------------+   |
            |                                |
            +--------------------------------+

    */

    static std::tuple<ImRect, ImRect, ImRect, ImRect, ImRect> GetBoxModelBounds(ImVec2 pos, const StyleDescriptor& style,
        std::string_view text, IRenderer& renderer, int32_t geometry, const NeighborWidgets& neighbors = NeighborWidgets{})
    {
        ImRect content, padding, border, margin;
        const auto& borderstyle = style.border;
        const auto& font = style.font;
        margin.Min = pos;

        if (geometry & ToLeft)
        {
            border.Min.x = pos.x - style.margin.right;
            padding.Min.x = border.Min.x - borderstyle.right.thickness;
            content.Min.x = padding.Min.x - style.padding.right;
        }
        else
        {
            border.Min.x = pos.x + style.margin.left;
            padding.Min.x = border.Min.x + borderstyle.left.thickness;
            content.Min.x = padding.Min.x + style.padding.left;
        }

        if (geometry & ToTop)
        {
            border.Min.y = pos.y - style.margin.bottom;
            padding.Min.y = border.Min.y - borderstyle.bottom.thickness;
            content.Min.y = padding.Min.y - style.padding.bottom;
        }
        else
        {
            border.Min.y = pos.y + style.margin.top;
            padding.Min.y = border.Min.y + borderstyle.top.thickness;
            content.Min.y = padding.Min.y + style.padding.top;
        }

        auto hastextw = (style.specified & StyleWidth) && !((style.font.flags & FontStyleOverflowEllipsis) || 
            (style.font.flags & FontStyleOverflowMarquee));
        ImVec2 textsz{ 0.f, 0.f };
        auto textMetricsComputed = false, hexpanded = false, vexpanded = false;

        auto setHFromContent = [&] {
            if (geometry & ToLeft)
            {
                content.Max.x = (style.specified & StyleWidth) ? style.dimension.x :
                    content.Min.x - clamp(textsz.x, style.mindim.x, style.maxdim.x);
                padding.Max.x = content.Max.x - style.padding.left;
                border.Max.x = padding.Max.x - borderstyle.left.thickness;
                margin.Max.x = border.Max.x - style.margin.left;
            }
            else
            {
                content.Max.x = (style.specified & StyleWidth) ? style.dimension.x :
                    content.Min.x + clamp(textsz.x, style.mindim.x, style.maxdim.x);
                padding.Max.x = content.Max.x + style.padding.right;
                border.Max.x = padding.Max.x + borderstyle.right.thickness;
                margin.Max.x = border.Max.x + style.margin.right;
            }
        };

        auto setVFromContent = [&] {
            if (geometry & ToTop)
            {
                content.Max.y = (style.specified & StyleHeight) ? style.dimension.y :
                    content.Min.y - clamp(textsz.y, style.mindim.y, style.maxdim.y);
                padding.Max.y = content.Max.y - style.padding.top;
                border.Max.y = padding.Max.y - borderstyle.top.thickness;
                margin.Max.y = border.Max.y - style.margin.top;
            }
            else
            {
                content.Max.y = (style.specified & StyleHeight) ? style.dimension.y :
                    content.Min.y + clamp(textsz.y, style.mindim.y, style.maxdim.y);
                padding.Max.y = content.Max.y + style.padding.bottom;
                border.Max.y = padding.Max.y + borderstyle.bottom.thickness;
                margin.Max.y = border.Max.y + style.margin.bottom;
            }
        };

        auto setHFromExpansion = [&](float max) {
            if (geometry & ToLeft)
            {
                margin.Max.x = std::max(max, margin.Min.x - style.maxdim.x);
                border.Max.x = margin.Max.x + style.margin.left;
                padding.Max.x = border.Max.x + borderstyle.left.thickness;
                content.Max.x = padding.Max.x + style.padding.left;
            }
            else
            {
                margin.Max.x = std::min(max, margin.Min.x + style.maxdim.x);
                border.Max.x = margin.Max.x - style.margin.right;
                padding.Max.x = border.Max.x - borderstyle.right.thickness;
                content.Max.x = padding.Max.x - style.padding.right;
            }
        };

        auto setVFromExpansion = [&](float max) {
            if (geometry & ToTop)
            {
                margin.Max.y = std::max(max, margin.Min.y - style.maxdim.y);
                border.Max.y = margin.Max.y + style.margin.top;
                padding.Max.y = border.Max.y + borderstyle.top.thickness;
                content.Max.y = padding.Max.y + style.padding.top;
            }
            else
            {
                margin.Max.y = std::min(max, margin.Min.y + style.maxdim.y);
                border.Max.y = margin.Max.y - style.margin.bottom;
                padding.Max.y = border.Max.y - borderstyle.bottom.thickness;
                content.Max.y = padding.Max.y - style.padding.bottom;
            }
        };

        if ((geometry & ExpandH) == 0)
        {
            textMetricsComputed = true;
            textsz = renderer.GetTextSize(text, font.font, font.size, hastextw ? style.dimension.x : -1.f);
            setHFromContent();
        }
        else
        {
            // If we are inside a layout, and layout has expand policy, then it has a geometry
            // Use said geometry or set from content dimensions
            if (Context.currLayoutDepth > -1)
            {
                auto isLayoutFit = (Context.layouts[Context.currLayoutDepth].sizing & ExpandH) == 0;
                
                if (!isLayoutFit)
                {
                    setHFromExpansion((geometry& ToLeft) ? Context.layouts[Context.currLayoutDepth].prevpos.x :
                        Context.layouts[Context.currLayoutDepth].nextpos.x);
                    hexpanded = true;
                }
                else
                {
                    textMetricsComputed = true;
                    textsz = renderer.GetTextSize(text, font.font, font.size, hastextw ? style.dimension.x : -1.f);
                    setHFromContent();
                }
            }
            else
            {
                // If widget has a expand size policy then expand upto neighbors or upto window size
                setHFromExpansion((geometry & ToLeft) ? neighbors.left == -1 ? 0.f : Context.GetGeometry(neighbors.left).Max.x 
                    : neighbors.right == -1 ? ImGui::GetCurrentWindow()->Size.x : Context.GetGeometry(neighbors.right).Min.x);
                hexpanded = true;
            }
        }

        if ((geometry & ExpandV) == 0)
        {
            // When settings height from content, if text metrics are not already computed,
            // computed them on the fixed width that was computed in previous step
            if (!textMetricsComputed)
            {
                textMetricsComputed = true;
                textsz = renderer.GetTextSize(text, font.font, font.size, content.GetWidth());
            }
                
            setVFromContent();
        }
        else
        {
            // If we are inside a layout, and layout has expand policy, then it has a geometry
            // Use said geometry or set from content dimensions
            if (Context.currLayoutDepth > -1)
            {
                auto isLayoutFit = (Context.layouts[Context.currLayoutDepth].sizing & ExpandV) == 0;

                if (!isLayoutFit)
                {
                    setVFromExpansion((geometry& ToTop) ? Context.layouts[Context.currLayoutDepth].prevpos.y :
                        Context.layouts[Context.currLayoutDepth].nextpos.y);
                    vexpanded = true;
                }
                else
                {
                    if (!textMetricsComputed)
                    {
                        textMetricsComputed = true;
                        textsz = renderer.GetTextSize(text, font.font, font.size, content.GetWidth());
                    }

                    setVFromContent();
                }
            }
            else
            {
                // If widget has a expand size policy then expand upto neighbors or upto window size
                setVFromExpansion((geometry & ToTop) ? neighbors.top == -1 ? 0.f : Context.GetGeometry(neighbors.top).Max.y
                    : neighbors.bottom == -1 ? ImGui::GetCurrentWindow()->Size.y : Context.GetGeometry(neighbors.bottom).Min.y);
                vexpanded = true;
            }
        }

        if (geometry & ToTop)
        {
            std::swap(margin.Min.y, margin.Max.y);
            std::swap(border.Min.y, border.Max.y);
            std::swap(padding.Min.y, padding.Max.y);
            std::swap(content.Min.y, content.Max.y);
        }

        if (geometry & ToLeft)
        {
            std::swap(margin.Min.x, margin.Max.x);
            std::swap(border.Min.x, border.Max.x);
            std::swap(padding.Min.x, padding.Max.x);
            std::swap(content.Min.x, content.Max.x);
        }

        ImVec2 textpos;

        if (hexpanded)
        {
            auto cw = content.GetWidth();

            if (style.alignment & TextAlignHCenter)
            {
                if (!textMetricsComputed)
                {
                    textMetricsComputed = true;
                    textsz = renderer.GetTextSize(text, font.font, font.size, cw);
                }

                if (textsz.x < cw)
                {
                    auto hdiff = (cw - textsz.x) * 0.5f;
                    textpos.x = content.Min.x + hdiff;
                }
            }
            else if (style.alignment & TextAlignRight)
            {
                if (!textMetricsComputed)
                {
                    textMetricsComputed = true;
                    textsz = renderer.GetTextSize(text, font.font, font.size, cw);
                }

                if (textsz.x < cw)
                {
                    auto hdiff = (cw - textsz.x);
                    textpos.x = content.Min.x + hdiff;
                }
            }
            else textpos.x = content.Min.x;
        }
        else textpos.x = content.Min.x;

        if (vexpanded)
        {
            auto cw = content.GetWidth();
            auto ch = content.GetHeight();

            if (style.alignment & TextAlignVCenter)
            {
                if (!textMetricsComputed)
                {
                    textMetricsComputed = true;
                    textsz = renderer.GetTextSize(text, font.font, font.size, cw);
                }

                if (textsz.y < ch)
                {
                    auto vdiff = (ch - textsz.y) * 0.5f;
                    textpos.y = content.Min.y + vdiff;
                }
            }
            else if (style.alignment & TextAlignBottom)
            {
                if (!textMetricsComputed)
                {
                    textMetricsComputed = true;
                    textsz = renderer.GetTextSize(text, font.font, font.size, cw);
                }

                if (textsz.y < ch)
                {
                    auto vdiff = (ch - textsz.y);
                    textpos.y = content.Min.y + vdiff;
                }
            }
            else textpos.y = content.Min.y;
        }
        else textpos.y = content.Min.y;

        return std::make_tuple(content, padding, border, margin, ImRect{ textpos, textpos + textsz });
    }

    void ShowTooltip(long long& hoverDuration, const ImRect& margin, ImVec2 pos, std::string_view tooltip, IRenderer& renderer)
    {
        auto currts = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock().now().time_since_epoch()).count();
        if (hoverDuration == 0) hoverDuration = currts;
        else if ((int32_t)(currts - hoverDuration) >= Config.tooltipDelay)
        {
            auto font = GetFont(Config.tooltipFontFamily, Config.tooltipFontSz, FT_Normal);
            auto textsz = renderer.GetTextSize(tooltip, font, Config.tooltipFontSz);
            auto offsetx = std::max(0.f, margin.GetWidth() - textsz.x) * 0.5f;
            auto tooltippos = pos + ImVec2{ offsetx, -(textsz.y + 2.f) };
            renderer.DrawTooltip(tooltippos, tooltip);
        }
    }
    
    WidgetDrawResult LabelImpl(int32_t id, const ImRect& margin, const ImRect& border, const ImRect& padding, 
        const ImRect& content, const ImRect& text, IRenderer& renderer)
    {
        assert((id & 0xffff) <= (int)Context.states[WT_Label].size());

        WidgetDrawResult result;
        auto& state = Context.GetState(id).state.label;
        auto& style = Context.GetStyle(state.state);
        auto ismouseover = padding.Contains(ImGui::GetIO().MousePos);

        DrawBoxShadow(border.Min, border.Max, style, renderer);
        DrawBackground(border.Min, border.Max, style, renderer);
        DrawBorderRect(border.Min, border.Max, style.border, style.bgcolor, renderer);
        DrawText(content.Min, content.Max, text, state.text, state.state & WS_Disabled, style, renderer);

        if (ismouseover && !state.tooltip.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            ShowTooltip(state._hoverDuration, margin, margin.Min, state.tooltip, renderer);
        else state._hoverDuration == 0;

        result.geometry = margin;
        return result;
    }

    WidgetDrawResult ButtonImpl(int32_t id, const ImRect& margin, const ImRect& border, const ImRect& padding, 
        const ImRect& content, const ImRect& text, IRenderer& renderer)
    {
        WidgetDrawResult result;
        auto& state = Context.GetState(id).state.button;
        auto& style = Context.GetStyle(state.state);
        auto ismouseover = padding.Contains(ImGui::GetIO().MousePos);
        state.state = !ismouseover ? WS_Default :
            ImGui::IsMouseDown(ImGuiMouseButton_Left) ? WS_Pressed | WS_Hovered : WS_Hovered;

        DrawBoxShadow(border.Min, border.Max, style, renderer);
        DrawBackground(border.Min, border.Max, style, renderer);
        DrawBorderRect(border.Min, border.Max, style.border, style.bgcolor, renderer);
        DrawText(content.Min, content.Max, text, state.text, state.state & WS_Disabled, style, renderer);

        if (ismouseover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            result.event = WidgetEvent::Clicked;
        else if (ismouseover && !state.tooltip.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
            ShowTooltip(state._hoverDuration, margin, margin.Min, state.tooltip, renderer);
        else state._hoverDuration == 0;

        result.geometry = margin;
        return result;
    }

    static std::pair<ImRect, ImVec2> ToggleButtonBounds(ToggleButtonState& state, const ImRect& extent, IRenderer& renderer)
    {
        auto& style = Context.GetStyle(state.state);
        auto& specificStyle = Context.toggleButtonStyles[log2((unsigned)state.state)].top();
        ImRect result;
        ImVec2 text;
        result.Min = result.Max = extent.Min;

        if (specificStyle.showText)
        {
            if (specificStyle.fontptr == nullptr) specificStyle.fontptr = GetFont(IM_RICHTEXT_DEFAULT_FONTFAMILY, 
                specificStyle.fontsz, FT_Bold);

            renderer.SetCurrentFont(specificStyle.fontptr, specificStyle.fontsz);
            text = renderer.GetTextSize("ONOFF", specificStyle.fontptr, specificStyle.fontsz);
            result.Max += text;
            renderer.ResetFont();

            auto extra = 2.f * (-specificStyle.thumbOffset + specificStyle.trackBorderThickness);
            result.Max.x += extra;
            result.Max.y += extra;
        }
        else
        {
            result.Max.x += ((extent.GetHeight()) * 2.f);
            result.Max.y += extent.GetHeight();
        }

        return { result, text };
    }

    WidgetDrawResult ToggleButtonImpl(int32_t id, ToggleButtonState& state, const ImRect& extent, ImVec2 textsz, IRenderer& renderer)
    {
        WidgetDrawResult result;

        auto& style = Context.GetStyle(state.state);
        auto& specificStyle = Context.toggleButtonStyles[log2((unsigned)state.state)].top();
        auto& toggle = Context.ToggleState(id);

        auto extra = (-specificStyle.thumbOffset + specificStyle.trackBorderThickness);
        auto radius = (extent.GetHeight() * 0.5f) - (2.f * extra);
        auto movement = extent.GetWidth() - (2.f * (radius + extra));
        auto moveAmount = toggle.animate ? (ImGui::GetIO().DeltaTime / specificStyle.animate) * movement * (state.checked ? 1.f : -1.f) : 0.f;
        toggle.progress += std::fabsf(moveAmount / movement);

        auto center = toggle.btnpos == -1.f ? state.checked ? extent.Max - ImVec2{ extra + radius, extra + radius }
            : extent.Min + ImVec2{ radius + extra, extra + radius }
            : ImVec2{ toggle.btnpos, extra + radius };
        center.x = ImClamp(center.x + moveAmount, extent.Min.x + (extra * 0.5f), extent.Max.x - extra);
        center.y = extent.Min.y + (extent.GetHeight() * 0.5f);
        toggle.animate = (center.x < (extent.Max.x - extra - radius)) && (center.x > (extent.Min.x + extra + radius));

        auto mousepos = ImGui::GetIO().MousePos;
        auto dist = std::sqrtf((mousepos.x - center.x) * (mousepos.x - center.x) + (mousepos.y - center.y) * (mousepos.y - center.y));
        auto rounded = extent.GetHeight() * 0.5f;
        auto tcol = specificStyle.trackColor;
        if (toggle.animate)
        {
            auto prevTCol = state.checked ? Context.toggleButtonStyles[WSI_Default].top().trackColor :
                Context.toggleButtonStyles[WSI_Checked].top().trackColor;
            auto [fr, fg, fb, fa] = DecomposeColor(prevTCol);
            auto [tr, tg, tb, ta] = DecomposeColor(tcol);
            tr = (int)((1.f - toggle.progress) * (float)fr + toggle.progress * (float)tr);
            tg = (int)((1.f - toggle.progress) * (float)fg + toggle.progress * (float)tg);
            tb = (int)((1.f - toggle.progress) * (float)fb + toggle.progress * (float)tb);
            ta = (int)((1.f - toggle.progress) * (float)fa + toggle.progress * (float)ta);
            tcol = ToRGBA(tr, tg, tb, ta);
        }

        auto [tr, tg, tb, ta] = DecomposeColor(tcol);
        renderer.DrawRoundedRect(extent.Min, extent.Max, tcol, true, rounded, rounded, rounded, rounded);
        renderer.DrawRoundedRect(extent.Min, extent.Max, specificStyle.trackBorderColor, false, rounded, rounded, rounded, rounded,
            specificStyle.trackBorderThickness);

        if (specificStyle.showText && !toggle.animate)
        {
            renderer.SetCurrentFont(specificStyle.fontptr, specificStyle.fontsz);
            auto texth = ((extent.GetHeight() - textsz.y) * 0.5f) - 2.f;
            state.checked ? renderer.DrawText("ON", extent.Min + ImVec2{ extra, texth }, specificStyle.indicatorTextColor) :
                renderer.DrawText("OFF", extent.Min + ImVec2{ (extent.GetWidth() * 0.5f) - 5.f, texth }, specificStyle.indicatorTextColor);
            renderer.ResetFont();
        }

        renderer.DrawCircle(center, radius + specificStyle.thumbExpand, style.fgcolor, true);
        auto mouseover = extent.Contains(mousepos);

        if (mouseover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            result.event = WidgetEvent::Clicked;
            state.checked = !state.checked;
            toggle.animate = true;
            toggle.progress = 0.f;
        }

        toggle.btnpos = toggle.animate ? center.x : -1.f;
        state.state = mouseover && ImGui::IsMouseDown(ImGuiMouseButton_Left) ? WS_Hovered | WS_Pressed :
            mouseover ? WS_Hovered : WS_Default;
        state.state = state.checked ? state.state | WS_Checked : state.state & ~WS_Checked;
        result.geometry = extent;
        return result;
    }

    static ImRect RadioButtonBounds(const ImRect& extent)
    {
        auto radius = std::min(extent.GetWidth(), extent.GetHeight());
        return ImRect{ extent.Min, extent.Min + ImVec2{ radius, radius } };
    }

    WidgetDrawResult RadioButtonImpl(int32_t id, RadioButtonState& state, const ImRect& extent, IRenderer& renderer)
    {
        WidgetDrawResult result;
        auto& style = Context.GetStyle(state.state);
        auto& specificStyle = Context.radioButtonStyles[log2((unsigned)state.state)].top();
        auto& radio = Context.RadioState(id);
        auto mousepos = ImGui::GetIO().MousePos;

        auto radius = extent.GetWidth() * 0.5f;
        auto center = extent.Min + ImVec2{ radius, radius };
        renderer.DrawCircle(center, radius, specificStyle.outlineColor, false, specificStyle.outlineThickness);
        auto maxrad = radius * specificStyle.checkedRadius;
        radio.radius = radio.radius == -1.f ? state.checked ? maxrad : 0.f : radio.radius;
        radius = radio.radius;

        if (radius > 0.f)
        {
            renderer.DrawCircle(center, radius, specificStyle.checkedColor, true);
        }
        
        auto ratio = radio.animate ? (ImGui::GetIO().DeltaTime / specificStyle.animate) : 0.f;
        radio.progress += ratio;
        radio.radius += ratio * maxrad * (state.checked ? 1.f : -1.f);
        radio.animate = radio.radius > 0.f && radio.radius < maxrad;
        auto mouseover = extent.Contains(mousepos);

        if (mouseover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            result.event = WidgetEvent::Clicked;
            state.checked = !state.checked;
            radio.animate = true;
            radio.progress = 0.f;
            radio.radius = state.checked ? 0.f : maxrad;
        }

        state.state = mouseover && ImGui::IsMouseDown(ImGuiMouseButton_Left) ? WS_Hovered | WS_Pressed :
            mouseover ? WS_Hovered : WS_Default;
        state.state = state.checked ? state.state | WS_Checked : state.state & ~WS_Checked;
        result.geometry = extent;
        return result;
    }

    static void UpdatePosition(const TextInputState& state, int index, InputTextInternalState& input, const StyleDescriptor& style, IRenderer& renderer)
    {
        for (auto idx = index; idx < (int)state.text.size(); ++idx)
        {
            auto sz = renderer.GetTextSize(std::string_view{ state.text.data() + idx, 1 }, style.font.font, style.font.size).x;
            input.pixelpos[idx] = sz + (idx > 0 ? input.pixelpos[idx - 1] : 0.f);
        }
    }

    static void RemoveCharAt(int position, TextInputState& state, InputTextInternalState& input)
    {
        auto diff = input.pixelpos[position] - input.pixelpos[position - 1];

        for (auto idx = position; idx < (int)state.text.size(); ++idx)
        {
            state.text[idx - 1] = state.text[idx];
            input.pixelpos[idx - 1] -= diff;
        }

        state.text.pop_back();
        input.pixelpos.pop_back();
    }

    static void DeleteSelectedText(TextInputState& state, InputTextInternalState& input, const StyleDescriptor& style, IRenderer& renderer)
    {
        auto from = std::max(state.selection.first, state.selection.second) + 1,
            to = std::min(state.selection.first, state.selection.second);
        float shift = input.pixelpos[from - 1] - input.pixelpos[to - 1];
        auto textsz = (int)state.text.size();

        for (; from < textsz; ++from, ++to)
        {
            state.text[to] = state.text[from];
            input.pixelpos[to] = input.pixelpos[from] - shift;
        }

        for (auto idx = to; idx < textsz; ++idx)
        {
            state.text.pop_back();
            input.pixelpos.pop_back();
        }

        input.caretpos = std::min(state.selection.first, state.selection.second);
        state.selection.first = state.selection.second = -1;
        input.selectionStart = -1.f;
    }

    WidgetDrawResult TextInputImpl(int32_t id, TextInputState& state, const ImRect& extent, const ImRect& content, IRenderer& renderer)
    {
        WidgetDrawResult result;
        const auto& style = Context.GetStyle(state.state);
        auto& input = Context.InputTextState(id);
        auto mousepos = ImGui::GetIO().MousePos;

        if (state.state & WS_Focused)
            renderer.DrawRect(extent.Min, extent.Max, Config.focuscolor, false, 2.f);
            
        DrawBackground(extent.Min, extent.Max, style, renderer);
        DrawBorderRect(extent.Min, extent.Max, style.border, style.bgcolor, renderer);
        renderer.SetCurrentFont(style.font.font, style.font.size);
        
        if (state.text.empty() && !(state.state & WS_Focused))
        {
            auto phstyle = style;
            auto [fr, fg, fb, fa] = DecomposeColor(phstyle.fgcolor);
            fa = 150;
            phstyle.fgcolor = ToRGBA(fr, fg, fb, fa);
            DrawText(content.Min, content.Max, { content.Min, content.Min }, state.placeholder, state.state & WS_Disabled, 
                phstyle, renderer, FontStyleOverflowMarquee);
        }
        else
        {
            if (state.selection.second != -1)
            {
                std::string_view text{ state.text.data(), state.text.size() };
                auto selection = state.selection;
                selection = { std::min(state.selection.first, state.selection.second), 
                    std::max(state.selection.first, state.selection.second) };
                std::string_view parts[3] = { text.substr(0, selection.first), 
                    text.substr(selection.first, selection.second - selection.first + 1), 
                    text.substr(selection.second + 1) };
                ImVec2 startpos{ content.Min.x - input.offset, content.Min.y }, textsz;
                
                if (!parts[0].empty())
                {
                    textsz = renderer.GetTextSize(parts[0], style.font.font, style.font.size);
                    renderer.DrawText(parts[0], startpos, style.fgcolor);
                    startpos.x += textsz.x;
                }

                const auto& selstyle = Context.GetStyle(WS_Selected);
                textsz = renderer.GetTextSize(parts[1], style.font.font, style.font.size);
                renderer.DrawRect(startpos, startpos + textsz, selstyle.bgcolor, true);
                renderer.DrawText(parts[1], startpos, selstyle.fgcolor);
                startpos.x += textsz.x;

                if (!parts[2].empty())
                {
                    textsz = renderer.GetTextSize(parts[2], style.font.font, style.font.size);
                    renderer.DrawText(parts[2], startpos, style.fgcolor);
                }
            }
            else
            {
                ImVec2 startpos{ content.Min.x - input.offset, content.Min.y };
                std::string_view text{ state.text.data(), state.text.size() };
                renderer.DrawText(text, startpos, style.fgcolor);
            }
        }

        renderer.ResetFont();

        auto mouseover = content.Contains(mousepos);
        auto ispressed = mouseover && ImGui::IsMouseDown(ImGuiMouseButton_Left);
        auto hasdblclick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
        auto hasclick = ImGui::IsMouseClicked(ImGuiMouseButton_Left) || hasdblclick;
        auto isclicked = (hasclick && mouseover) || (!hasclick && (state.state & WS_Focused));
        ImGui::SetMouseCursor(mouseover ? ImGuiMouseCursor_TextInput : ImGuiMouseCursor_Arrow);
        mouseover ? state.state |= WS_Hovered : state.state &= ~WS_Hovered;
        ispressed ? state.state |= WS_Pressed : state.state &= ~WS_Pressed;
        isclicked ? state.state |= WS_Focused : state.state &= ~WS_Focused;

        // When mouse is down inside the input, text selection is in progress
        // Hence, either text selection is starting, in which case record the start position.
        // If mouse gets released at the same position, it is a click and not a selection,
        // in which case, move the caret to the respective char position.
        // If mouse gets dragged, select the region of text
        if (state.state & WS_Pressed)
        {
            if (!state.text.empty())
            {
                auto posx = mousepos.x - content.Min.x;
                if (input.selectionStart == -1.f) input.selectionStart = posx;
                else if (input.selectionStart != posx)
                {
                    if (state.selection.first == -1)
                    {
                        auto it = std::lower_bound(input.pixelpos.begin(), input.pixelpos.end(), input.selectionStart);
                        if (it != input.pixelpos.end())
                        {
                            auto idx = it - input.pixelpos.begin();
                            if (idx > 0 && (*it - posx) > 0.f) --idx;
                            state.selection.first = it - input.pixelpos.begin();
                            input.isSelecting = true;
                            input.caretVisible = false;
                        }
                    }

                    auto it = std::lower_bound(input.pixelpos.begin(), input.pixelpos.end(), posx);

                    if (it != input.pixelpos.end())
                    {
                        auto idx = it - input.pixelpos.begin();
                        if (idx > 0 && (*it - posx) > 0.f) --idx;

                        state.selection.second = it - input.pixelpos.begin();
                        result.event = WidgetEvent::Selected;
                    }
                }
            }
        }
        else
        {
            if (!state.text.empty())
            {
                auto posx = mousepos.x - content.Min.x;
                
                // This means we have clicked, not selecting text
                if (input.selectionStart == posx)
                {
                    auto it = std::lower_bound(input.pixelpos.begin(), input.pixelpos.end(), posx);
                    auto idx = it - input.pixelpos.begin();

                    //idx = idx > 0 ? idx - 1 : idx;
                    input.caretVisible = true;
                    state.selection.first = state.selection.second = -1;
                    input.caretpos = idx;
                    input.isSelecting = false;
                    input.selectionStart = -1.f;
                }
                else if (input.selectionStart != -1.f)
                {
                    if (state.selection.first == -1)
                    {
                        auto it = std::lower_bound(input.pixelpos.begin(), input.pixelpos.end(), input.selectionStart);
                        if (it != input.pixelpos.end())
                        {
                            auto idx = it - input.pixelpos.begin();
                            if (idx > 0 && (*it - posx) > 0.f) --idx;
                            state.selection.first = it - input.pixelpos.begin();
                            input.isSelecting = true;
                            input.caretVisible = false;
                        }
                    }

                    auto it = std::lower_bound(input.pixelpos.begin(), input.pixelpos.end(), posx);

                    if (it != input.pixelpos.end())
                    {
                        auto idx = it - input.pixelpos.begin();
                        if (idx > 0 && (*it - posx) > 0.f) --idx;

                        state.selection.second = it - input.pixelpos.begin();
                        result.event = WidgetEvent::Selected;
                        input.caretVisible = false;
                        input.isSelecting = true;
                    }
                }

                input.selectionStart = -1.f;
            }

            if (state.state & WS_Focused)
            {
                if (input.caretVisible)
                {
                    auto isCaretAtEnd = input.caretpos == (int)state.text.size();
                    auto offset = isCaretAtEnd ? 1.f : 0.f;
                    auto cursorxpos = (!input.pixelpos.empty() ? input.pixelpos[input.caretpos - 1] : 0.f) + offset;
                    renderer.DrawLine(content.Min + ImVec2{ cursorxpos, 1.f }, content.Min + ImVec2{ cursorxpos, content.GetHeight() - 1.f }, style.fgcolor, 2.f);
                }

                if (input.lastCaretShowTime > 0.5f && state.selection.second == -1)
                {
                    input.caretVisible = !input.caretVisible;
                    input.lastCaretShowTime = 0.f;
                }
                else input.lastCaretShowTime += ImGui::GetIO().DeltaTime;

                if (hasdblclick)
                {
                    input.selectionStart = 0.f;
                    state.selection.first = 0;
                    state.selection.second = (int)state.text.size() - 1;
                }

                for (auto key = (int)ImGuiKey_NamedKey_BEGIN; key != ImGuiKey_NamedKey_END; ++key)
                {
                    if (key >= ImGuiKey_MouseLeft && key <= ImGuiKey_MouseWheelY) continue;

                    if (ImGui::IsKeyPressed((ImGuiKey)key))
                    {
                        input.lastCaretShowTime = 0.f;
                        input.caretVisible = true;

                        if (key == ImGuiKey_LeftArrow)
                        {
                            auto prevpos = input.caretpos;
                            
                            if (ImGui::GetIO().KeyMods & ImGuiMod_Shift)
                            {
                                if (state.selection.first == -1)
                                {
                                    input.selectionStart = input.pixelpos[input.caretpos];
                                    state.selection.first = input.caretpos;
                                    input.caretpos = std::max(input.caretpos - 1, 0);
                                    state.selection.second = input.caretpos;
                                }
                                else
                                    state.selection.second = std::max(state.selection.second - 1, 0);

                                input.caretVisible = false;
                            }
                            else input.caretpos = std::max(input.caretpos - 1, 0);

                            if (prevpos > input.caretpos && input.pixelpos[input.caretpos] < 0.f)
                            {
                                auto width = std::fabsf(input.pixelpos[input.caretpos] - (input.caretpos > 0 ? input.pixelpos[input.caretpos - 1] : 0.f));
                                for (auto idx = 0; idx < input.pixelpos.size(); ++idx)
                                    input.pixelpos[idx] += width;
                            }
                        }
                        else if (key == ImGuiKey_RightArrow)
                        {
                            auto prevpos = input.caretpos;

                            if (ImGui::GetIO().KeyMods & ImGuiMod_Shift)
                            {
                                if (state.selection.first == -1)
                                {
                                    input.selectionStart = input.pixelpos[input.caretpos];
                                    state.selection.first = input.caretpos;
                                    input.caretpos = std::min(input.caretpos + 1, (int)state.text.size());
                                    state.selection.second = input.caretpos;
                                }
                                else
                                    state.selection.second = std::min(state.selection.second + 1, (int)state.text.size() - 1);

                                input.caretVisible = false;
                            }
                            else input.caretpos = std::min(input.caretpos + 1, (int)state.text.size());

                            if (prevpos < input.caretpos && input.pixelpos[input.caretpos] > content.GetWidth())
                            {
                                auto width = std::fabsf(input.pixelpos[input.caretpos] - (input.caretpos > 0 ? input.pixelpos[input.caretpos - 1] : 0.f));
                                for (auto idx = 0; idx < input.pixelpos.size(); ++idx)
                                    input.pixelpos[idx] -= width;
                            }
                        }
                        else if (key == ImGuiKey_CapsLock) input.capslock = !input.capslock;
                        else if (key == ImGuiKey_Backspace)
                        {
                            if (state.selection.second == -1)
                            {
                                std::string_view text{ state.text.data(), state.text.size() };
                                auto caretAtEnd = input.caretpos == (int)text.size();
                                if (text.empty()) continue;

                                if (caretAtEnd)
                                {
                                    state.text.pop_back();
                                    input.pixelpos.pop_back();
                                }
                                else RemoveCharAt(input.caretpos, state, input);

                                input.caretpos--;
                            }
                            else DeleteSelectedText(state, input, style, renderer);

                            result.event = WidgetEvent::Edited;
                        }
                        else if (key == ImGuiKey_Delete)
                        {
                            if (state.selection.second == -1)
                            {
                                std::string_view text{ state.text.data(), state.text.size() };
                                auto caretAtEnd = input.caretpos == (int)text.size();
                                if (text.empty()) continue;

                                if (!caretAtEnd) RemoveCharAt(input.caretpos + 1, state, input);
                            }
                            else DeleteSelectedText(state, input, style, renderer);

                            result.event = WidgetEvent::Edited;
                        }
                        else if (key == ImGuiKey_Space || (key >= ImGuiKey_0 && key <= ImGuiKey_Z) ||
                            (key >= ImGuiKey_Apostrophe && key <= ImGuiKey_GraveAccent) ||
                            (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_KeypadEqual))
                        {
                            if (key == ImGuiKey_V && ImGui::GetIO().KeyMods & ImGuiMod_Ctrl)
                            {
                                auto content = ImGui::GetClipboardText();
                                auto length = std::strlen(content);
                                auto caretAtEnd = input.caretpos == (int)state.text.size();
                                //state.text.resize(state.text.size() + length);
                                input.pixelpos.expand(length, 0.f);

                                if (caretAtEnd)
                                {
                                    for (auto idx = 0; idx < length; ++idx)
                                    {
                                        state.text.push_back(content[idx]);
                                        auto sz = renderer.GetTextSize(std::string_view{ content + idx, 1 }, style.font.font, style.font.size).x;
                                        input.pixelpos.push_back(sz + (state.text.size() > 1u ? input.pixelpos.back() : 0.f));
                                    }
                                }
                                else
                                {
                                    for (auto idx = (int)state.text.size() - 1; idx >= input.caretpos; --idx)
                                        state.text[idx] = state.text[idx - length];
                                    for (auto idx = 0; idx < length; ++idx)
                                        state.text[idx + input.caretpos] = content[idx];

                                    UpdatePosition(state, input.caretpos, input, style, renderer);
                                }

                                input.caretpos += length;
                                result.event = WidgetEvent::Edited;
                            }
                            else if (key == ImGuiKey_C && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl) && state.selection.second != -1)
                            {
                                static char buffer[256] = { 0 };
                                auto dest = 0;
                                auto sz = state.selection.second - state.selection.first + 2;

                                if (sz > 254)
                                {
                                    char* hbuffer = (char*)malloc(sz);
                                    for (auto idx = state.selection.first; idx <= state.selection.second; ++idx, ++dest)
                                        hbuffer[dest] = state.text[idx];
                                    hbuffer[dest] = 0;
                                    ImGui::SetClipboardText(hbuffer);
                                    std::free(hbuffer);
                                }
                                else
                                {
                                    std::memset(buffer, 0, 256);
                                    for (auto idx = state.selection.first; idx <= state.selection.second; ++idx, ++dest)
                                        buffer[dest] = state.text[idx];
                                    buffer[dest] = 0;
                                    ImGui::SetClipboardText(buffer);
                                }
                            }
                            else if (key == ImGuiKey_A && (ImGui::GetIO().KeyMods & ImGuiMod_Ctrl))
                            {
                                state.selection.first = 0;
                                state.selection.second = (int)state.text.size() - 1;
                                input.selectionStart = -1.f;
                                input.caretVisible = false;
                            }
                            else
                            {
                                const auto& io = ImGui::GetIO();
                                std::string_view text{ state.text.data(), state.text.size() };
                                auto ch = io.KeyShift ? KeyMappings[key].second : KeyMappings[key].first;
                                ch = input.capslock ? std::toupper(ch) : std::tolower(ch);
                                auto caretAtEnd = input.caretpos == (int)text.size();

                                if (caretAtEnd)
                                {
                                    state.text.push_back(ch);
                                    std::string_view newtext{ state.text.data(), state.text.size() };
                                    auto lastpos = (int)state.text.size() - 1;
                                    input.pixelpos.push_back(renderer.GetTextSize(newtext.substr(lastpos, 1), style.font.font, style.font.size).x +
                                        (lastpos > 0 ? input.pixelpos[lastpos - 1] : 0.f));

                                    input.offset = std::max(0.f, input.pixelpos.back() - content.GetWidth());
                                    for (auto idx = 0; idx < input.pixelpos.size(); ++idx)
                                        input.pixelpos[idx] -= input.offset;
                                }
                                else
                                {
                                    state.text.push_back(0);
                                    input.pixelpos.push_back(0.f);
                                    for (auto from = (int)state.text.size() - 2; from >= input.caretpos; --from)
                                        state.text[from + 1] = state.text[from];    
                                    state.text[input.caretpos] = (char)ch;
                                    UpdatePosition(state, input.caretpos, input, style, renderer);

                                    // TODO: is offset change required?
                                }

                                input.caretpos++;
                                result.event = WidgetEvent::Edited;
                            }
                        }
                    }
                }
            }
        }

        return result;
    }

    static void AlignVCenter(float height, int level, ItemGridState& state)
    {
        auto& headers = state.config.headers;

        for (int16_t col = 0; col < (int16_t)headers[level].size(); ++col)
        {
            auto& hdr = headers[level][col];
            hdr.content.Max.y = height;

            auto vdiff = (height - hdr.textrect.GetHeight() - hdr.style.padding.v()) * 0.5f;
            if (vdiff > 0.f)
                hdr.textrect.TranslateY(vdiff);
        }
    }

    static bool IsBetween(float point, float min, float max, float tolerance = 0.f)
    {
        return (point < (max + tolerance)) && (point > (min - tolerance));
    }

    static bool operator!=(const ImRect& lhs, const ImRect& rhs)
    {
        return lhs.Min != rhs.Min || lhs.Max != rhs.Max;
    }

    static WidgetDrawResult TabBarImpl(int32_t id, const ImRect& margin, const ImRect& border, const ImRect& padding,
        const ImRect& content, const ImRect& text, IRenderer& renderer)
    {
        WidgetDrawResult result;
        auto& state = Context.GetState(id).state.tab;
        auto& map = Context.TabStates(id);

        if (state.horizontal)
        {
            for (auto vcol = 0; vcol < (int)state.tabs.size(); ++vcol)
            {
                if (map.map.vtol[vcol] == -1)
                {
                    map.map.vtol[vcol] = vcol;
                    map.map.ltov[vcol] = vcol;
                }

                auto col = map.map.vtol[vcol];
                auto& tab = state.tabs[col];
                auto& style = Context.GetStyle(tab.state.state);

                auto ismouseover = padding.Contains(ImGui::GetIO().MousePos);
                tab.state.state = !ismouseover ? WS_Default :
                    ImGui::IsMouseDown(ImGuiMouseButton_Left) ? WS_Pressed | WS_Hovered : WS_Hovered;

                DrawBoxShadow(border.Min, border.Max, style, renderer);
                DrawBackground(border.Min, border.Max, style, renderer);
                DrawBorderRect(border.Min, border.Max, style.border, style.bgcolor, renderer);
                DrawText(content.Min, content.Max, text, tab.state.text, tab.state.state & WS_Disabled, style, renderer);

                if (ismouseover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    result.event = WidgetEvent::Clicked;
                else if (ismouseover && !tab.state.tooltip.empty() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    ShowTooltip(tab.state._hoverDuration, margin, margin.Min, tab.state.tooltip, renderer);
                else tab.state._hoverDuration == 0;

                result.geometry = margin;
                return result;
            }
        }
    }

    static bool UpdateSubHeadersResize(std::vector<std::vector<ItemGridState::ColumnConfig>>& headers, ItemGridInternalState& gridstate, 
        const ImRect& rect, int parent, int chlevel, bool mouseDown)
    {
        if (chlevel >= (int)headers.size()) return true;

        auto cchcol = 0, chcount = 0, startch = -1;
        while (cchcol < (int)headers[chlevel].size() && headers[chlevel][cchcol].parent != parent)
            cchcol++;

        startch = cchcol;
        while (cchcol < (int)headers[chlevel].size() && headers[chlevel][cchcol].parent == parent)
        {
            cchcol++;
            if (headers[chlevel][cchcol].props & COL_Resizable) chcount++;
        }

        if (chcount > 0)
        {
            auto hdiff = rect.GetWidth() / (float)chcount;

            while (startch < (int)headers[chlevel].size())
            {
                auto& hdr = headers[chlevel][startch];
                if (hdr.parent == parent)
                {
                    auto& props = gridstate.cols[chlevel][startch];
                    props.modified += hdiff;
                    startch++;
                }
                else break;
            }
        }

        return chcount > 0;
    }

    static void HandleColumnResize(std::vector<std::vector<ItemGridState::ColumnConfig>>& headers, ItemGridState::ColumnConfig& hdr, 
        ItemGridInternalState& gridstate, ImVec2 mousepos, int level, int col)
    {
        if (gridstate.state != ItemGridCurrentState::Default && gridstate.state != ItemGridCurrentState::ResizingColumns) return;

        auto isMouseNearColDrag = IsBetween(mousepos.x, hdr.content.Min.x, hdr.content.Min.x, 5.f) &&
            IsBetween(mousepos.y, hdr.content.Min.y, hdr.content.Max.y);
        auto& evprop = gridstate.cols[level][col - 1];

        if (!evprop.mouseDown)
            ImGui::SetMouseCursor(isMouseNearColDrag ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            if (!evprop.mouseDown)
            {
                if (isMouseNearColDrag)
                {
                    evprop.mouseDown = true;
                    evprop.lastPos = mousepos;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    gridstate.state = ItemGridCurrentState::ResizingColumns;
                    LOG("Drag start at %f for column #%d\n", mousepos.x, col);
                }
            }
            else
            {
                ImRect extendRect{ evprop.lastPos, mousepos };
                evprop.modified += (mousepos.x - evprop.lastPos.x);
                evprop.lastPos = mousepos;
                UpdateSubHeadersResize(headers, gridstate, extendRect, col - 1, level + 1, true);
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                gridstate.state = ItemGridCurrentState::ResizingColumns;
            }
        }
        else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && evprop.mouseDown)
        {
            if (mousepos.x != -FLT_MAX && mousepos.y != -FLT_MAX)
            {
                ImRect extendRect{ evprop.lastPos, mousepos };
                evprop.modified += (mousepos.x - evprop.lastPos.x);
                evprop.lastPos = mousepos;
                UpdateSubHeadersResize(headers, gridstate, extendRect, col - 1, level + 1, false);
                LOG("Drag end at %f for column #%d\n", mousepos.x, col);
            }

            evprop.mouseDown = false;
            ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
            gridstate.state = ItemGridCurrentState::Default;
        }
    }

    static void HandleColumnReorder(std::vector<std::vector<ItemGridState::ColumnConfig>>& headers, ItemGridInternalState& gridstate, 
        ImVec2 mousepos, int level, int vcol)
    {
        if (gridstate.state != ItemGridCurrentState::Default && gridstate.state != ItemGridCurrentState::ReorderingColumns) return;

        auto col = gridstate.colmap[level].vtol[vcol];
        auto& hdr = headers[level][col];
        auto isMouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        ImRect moveTriggerRect{ hdr.content.Min + ImVec2{ 5.5f, 0.f }, hdr.content.Max - ImVec2{ 5.5f, 0.f } };

        if (isMouseDown && moveTriggerRect.Contains(mousepos) && !gridstate.drag.mouseDown)
        {
            auto movingcol = vcol, siblingCount = 0;
            auto parent = hdr.parent;

            // This is required to "go up" he header hierarchy in case the sibling count 
            // at current level is 1 i.e. only the current header is a child of parent
            // In this case, consider the parent to be moving as well.
            // NOTE: Keep going up the hierarchy till we are at root level
            while (level > 0)
            {
                // TODO: Use logical index instead
                siblingCount = 0;
                for (int16_t col = 0; col < (int16_t)headers[level].size(); ++col)
                    if (headers[level][col].parent == parent)
                    {
                        siblingCount++; movingcol = col;
                    }

                if (siblingCount > 1) break;
                else if (level > 0) { parent = headers[level-1][parent].parent; level--; }
            }

            movingcol = siblingCount == 1 ? parent : movingcol;
            level = siblingCount == 1 ? level - 1 : level;

            auto lcol = col;
            auto& mcol = headers[level][lcol];
            gridstate.drag.config = mcol;
            gridstate.drag.mouseDown = true;
            gridstate.drag.lastPos = mousepos;
            gridstate.drag.startPos = mousepos;
            gridstate.drag.column = movingcol;
            gridstate.drag.level = level;
            gridstate.state = ItemGridCurrentState::ReorderingColumns;
            ERROR("\nMarking column (v: %d, l: %d) as moving (%f -> %f)\n", vcol, lcol, mcol.content.Min.x, mcol.content.Max.x);
        }
        else if (isMouseDown && gridstate.drag.mouseDown && gridstate.drag.column == vcol && gridstate.drag.level == level)
        {
            // This implying we are dragging the column around
            auto diff = mousepos.x - gridstate.drag.lastPos.x;

            if (diff > 0.f && (int16_t)headers[level].size() > (vcol + 1))
            {
                auto ncol = gridstate.colmap[level].vtol[vcol + 1];
                auto& next = headers[level][ncol];

                if ((mousepos.x - gridstate.drag.startPos.x) >= next.content.GetWidth())
                    gridstate.swapColumns(vcol, vcol + 1, headers, level);
            }
            else if (diff < 0.f && (col - 1) >= 0)
            {
                auto& prev = headers[level][col -1];

                if ((gridstate.drag.startPos.x - mousepos.x) >= prev.content.GetWidth())
                    gridstate.swapColumns(col - 1, col, headers, level);
            }

            gridstate.drag.lastPos = mousepos;
        }
        else if (!isMouseDown)
        {
            gridstate.drag = ItemGridInternalState::HeaderCellDragState{};
            gridstate.state = ItemGridCurrentState::Default;
        }
    }

    static void HandleScrollBars(ScrollBarState& scroll, IRenderer& renderer, ImVec2 mousepos, const ImRect& content, ImVec2 sz)
    {
        auto btnsz = Config.scrollbarSz;
        auto hasHScroll = false;
        float gripExtent = 0.f;
        const float opacityRatio = (256.f / Config.scrollAppearAnimationDuration);

        if (sz.x > content.GetWidth())
        {
            if ((content.Contains(mousepos) && (mousepos.y <= content.Max.y) && mousepos.x >= (content.Max.y - (1.5 * btnsz))) 
                || scroll.mouseDownOnHGrip)
            {
                ImRect left{ { content.Min.x, content.Max.y - btnsz }, { content.Min.x + btnsz, content.Max.y } };
                ImRect right{ { content.Max.x - btnsz, content.Max.y }, content.Max };
                ImRect path{ { left.Max.x, left.Min.y }, { right.Min.x, right.Max.y } };

                auto sizeOfGrip = (content.GetWidth() / sz.x) * (content.GetWidth() - (2.f * btnsz));
                ImRect grip{ { left.Max.x + scroll.pos.x, content.Max.y - btnsz },
                    { left.Max.x + scroll.pos.x + sizeOfGrip, content.Max.y } };

                renderer.DrawRect(left.Min, left.Max, ToRGBA(175, 175, 175), true);
                renderer.DrawTriangle({ left.Min.x + (btnsz * 0.25f), left.Min.y + (0.5f * btnsz) },
                    { left.Max.x - (0.125f * btnsz), left.Min.y + (0.125f * btnsz) },
                    { left.Max.x - (0.125f * btnsz), left.Max.y - (0.125f * btnsz) }, ToRGBA(100, 100, 100), true);

                //renderer.DrawRect(path.Min, path.Max, ToRGBA(100, 200, 100), true);
                renderer.DrawRect(right.Min, right.Max, ToRGBA(175, 175, 175), true);
                renderer.DrawTriangle({ right.Min.x + (btnsz * 0.25f), right.Min.y + (0.125f * btnsz) },
                    { right.Max.x - (0.125f * btnsz), right.Min.y + (0.5f * btnsz) },
                    { right.Min.x + (btnsz * 0.25f), right.Max.y - (0.125f * btnsz) }, ToRGBA(100, 100, 100), true);

                if (grip.Contains(mousepos))
                {
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    {
                        if (!scroll.mouseDownOnHGrip)
                        {
                            scroll.mouseDownOnHGrip = true;
                            scroll.lastMousePos.y = mousepos.y;
                        }

                        auto step = mousepos.x - scroll.lastMousePos.x;
                        scroll.pos.x = ImClamp(scroll.pos.x + step, 0.f, path.GetWidth() - sizeOfGrip);
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(100, 100, 100), true);
                        scroll.lastMousePos.x = mousepos.x;
                    }
                    else
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(150, 150, 150), true);
                }
                else
                {
                    if (scroll.mouseDownOnHGrip)
                    {
                        auto step = mousepos.x - scroll.lastMousePos.x;
                        scroll.pos.x = ImClamp(scroll.pos.x + step, 0.f, path.GetWidth() - sizeOfGrip);
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(100, 100, 100), true);
                        scroll.lastMousePos.x = mousepos.x;
                    }
                    else
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(150, 150, 150), true);

                    if (left.Contains(mousepos) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                        scroll.pos.x = ImClamp(scroll.pos.x - 1.f, 0.f, path.GetWidth() - sizeOfGrip);
                    else if (right.Contains(mousepos) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                        scroll.pos.x = ImClamp(scroll.pos.x + 1.f, 0.f, path.GetWidth() - sizeOfGrip);
                }

                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && scroll.mouseDownOnHGrip)
                {
                    scroll.mouseDownOnHGrip = false;
                    renderer.DrawRect(grip.Min, grip.Max, ToRGBA(150, 150, 150), true);
                }
            }

            hasHScroll = true;
        }

        if (sz.y > content.GetHeight())
        {
            auto hasOpacity = scroll.opacity > 0.f;
            auto hasMouseInteraction = (content.Contains(mousepos) && (mousepos.x <= content.Max.x) && mousepos.x >= (content.Max.x - (1.5f * btnsz)) &&
                (!hasHScroll || mousepos.y < (content.Max.y - btnsz))) || scroll.mouseDownOnVGrip;

            if (hasMouseInteraction || hasOpacity)
            {
                if (hasMouseInteraction && scroll.opacity < 255.f)
                    scroll.opacity = std::min((opacityRatio * ImGui::GetCurrentContext()->IO.DeltaTime) + scroll.opacity, 255.f);
                else if (!hasMouseInteraction && scroll.opacity > 0.f)
                    scroll.opacity = std::max(scroll.opacity - (opacityRatio * ImGui::GetCurrentContext()->IO.DeltaTime), 0.f);

                auto extrah = hasHScroll ? btnsz : 0.f;
                ImRect top{ { content.Max.x - btnsz, content.Min.y }, { content.Max.x, content.Min.y + btnsz } };
                ImRect bottom{ { content.Max.x - btnsz, content.Max.y - btnsz - extrah }, content.Max };
                ImRect path{ { top.Min.x, top.Max.y }, { bottom.Max.x, bottom.Min.y } };

                auto sizeOfGrip = (content.GetHeight() / sz.y) * (content.GetHeight() - (2.f * btnsz) - extrah);
                ImRect grip{ { content.Max.x - btnsz, top.Max.y + scroll.pos.y },
                    { content.Max.x, sizeOfGrip + top.Max.y + scroll.pos.y } };

                renderer.DrawRect(top.Min, top.Max, ToRGBA(175, 175, 175, (int)scroll.opacity), true);
                renderer.DrawTriangle({ top.Min.x + (btnsz * 0.5f), top.Min.y + (0.25f * btnsz) },
                    { top.Max.x - (0.125f * btnsz), top.Min.y + (0.75f * btnsz) },
                    { top.Min.x + (0.125f * btnsz), top.Min.y + (0.75f * btnsz) }, ToRGBA(100, 100, 100, (int)scroll.opacity), true);

                //renderer.DrawRect(path.Min, path.Max, ToRGBA(100, 200, 100), true);
                renderer.DrawRect(bottom.Min, bottom.Max, ToRGBA(175, 175, 175, (int)scroll.opacity), true);
                renderer.DrawTriangle({ bottom.Min.x + (btnsz * 0.125f), bottom.Min.y + (0.25f * btnsz) },
                    { bottom.Max.x - (0.125f * btnsz), bottom.Min.y + (0.25f * btnsz) },
                    { bottom.Max.x - (0.5f * btnsz), bottom.Max.y - (0.25f * btnsz) }, ToRGBA(100, 100, 100, (int)scroll.opacity), true);

                if (grip.Contains(mousepos))
                {
                    if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    {
                        if (!scroll.mouseDownOnVGrip)
                        {
                            scroll.mouseDownOnVGrip = true;
                            scroll.lastMousePos.y = mousepos.y;
                        }

                        auto step = mousepos.y - scroll.lastMousePos.y;
                        scroll.pos.y = ImClamp(scroll.pos.y + step, 0.f, path.GetHeight() - sizeOfGrip - extrah);
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(100, 100, 100), true);
                        scroll.lastMousePos.y = mousepos.y;
                    }
                    else
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(150, 150, 150, (int)scroll.opacity), true);
                }
                else
                {
                    if (scroll.mouseDownOnVGrip)
                    {
                        auto step = mousepos.y - scroll.lastMousePos.y;
                        scroll.pos.y = ImClamp(scroll.pos.y + step, 0.f, path.GetHeight() - sizeOfGrip - extrah);
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(100, 100, 100), true);
                        scroll.lastMousePos.y = mousepos.y;
                    }
                    else
                        renderer.DrawRect(grip.Min, grip.Max, ToRGBA(150, 150, 150, (int)scroll.opacity), true);

                    if (top.Contains(mousepos) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                        scroll.pos.y = ImClamp(scroll.pos.y - 1.f, 0.f, path.GetHeight() - sizeOfGrip - extrah);
                    else if (bottom.Contains(mousepos) && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                        scroll.pos.y = ImClamp(scroll.pos.y + 1.f, 0.f, path.GetHeight() - sizeOfGrip - extrah);
                }

                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) && scroll.mouseDownOnVGrip)
                {
                    scroll.mouseDownOnVGrip = false;
                    renderer.DrawRect(grip.Min, grip.Max, ToRGBA(100, 100, 100), true);
                }

                gripExtent = path.GetHeight() - sizeOfGrip - extrah;
            }

            if (content.Contains(mousepos))
            {
                auto rotation = ImGui::GetCurrentContext()->IO.MouseWheel;
                scroll.pos.y = ImClamp(rotation + scroll.pos.y, 0.f, gripExtent);
            }
        }
    }

    static ImRect DrawCells(ItemGridState& state, std::vector<std::vector<ItemGridState::ColumnConfig>>& headers, int32_t row, int16_t col, 
        float miny, IRenderer& renderer)
    {
        auto& hdr = headers.back()[col];
        auto& model = state.cell(row, col, 0);
        auto itemcontent = hdr.content;
        itemcontent.Min.y = miny;

        switch (model.wtype)
        {
        case WT_Label:
        {
            auto& itemstyle = Context.GetStyle(model.state.label.state);
            if (itemstyle.font.font == nullptr) itemstyle.font.font = GetFont(itemstyle.font.family,
                itemstyle.font.size, FT_Normal);

            auto textsz = renderer.GetTextSize(model.state.label.text, itemstyle.font.font,
                itemstyle.font.size, -1.f);
            itemcontent.Max.y = itemcontent.Min.y + textsz.y + hdr.style.padding.v();

            ImVec2 textstart{ itemcontent.Min.x + hdr.style.padding.left,
                itemcontent.Min.y + hdr.style.padding.top };

            if (textsz.x < itemcontent.GetWidth())
            {
                auto hdiff = (itemcontent.GetWidth() - textsz.x) * 0.5f;
                textstart.x += hdiff;
            }

            auto textend = itemcontent.Max - ImVec2{ hdr.style.padding.right, hdr.style.padding.bottom };
            DrawBackground(itemcontent.Min, itemcontent.Max, itemstyle, renderer);
            renderer.DrawRect(itemcontent.Min, itemcontent.Max, ToRGBA(100, 100, 100), false);
            DrawText(textstart, textend, { textstart, textstart + textsz }, model.state.label.text,
                model.state.label.state & WS_Disabled, itemstyle, renderer);
            break;
        }
        case WT_Custom:
        {
            renderer.SetClipRect(itemcontent.Min, itemcontent.Max);
            auto sz = model.state.CustomWidget(itemcontent.Min, itemcontent.Max);
            renderer.ResetClipRect();

            itemcontent.Max.y = itemcontent.Min.y + hdr.style.padding.v() + sz.y;
            break;
        }
        default:
            break;
        }

        return itemcontent;
    }

    template <typename T>
    int Find(T* start, T* end, T value)
    {
        for (auto it = start; it != end; ++it)
            if (*it == value) return (int)(it - start);
        return -1;
    }

    WidgetDrawResult ItemGridImpl(int32_t id, const ImRect& margin, const ImRect& border, const ImRect& padding,
        const ImRect& content, const ImRect& text, IRenderer& renderer)
    {
        WidgetDrawResult result;
        auto& state = Context.GetState(id).state.grid;
        auto& style = Context.GetStyle(state.state);
        auto& gridstate = Context.GridState(id);
        auto mousepos = ImGui::GetIO().MousePos;
        auto ismouseover = padding.Contains(mousepos);
        state.state = !ismouseover ? WS_Default :
            ImGui::IsMouseDown(ImGuiMouseButton_Left) ? WS_Pressed | WS_Hovered : WS_Hovered;

        DrawBoxShadow(border.Min, border.Max, style, renderer);
        DrawBackground(border.Min, border.Max, style, renderer);
        DrawBorderRect(border.Min, border.Max, style.border, style.bgcolor, renderer);

        auto& headers = state.config.headers;

        // Determine header geometry
        for (auto level = (int)headers.size() - 1; level >= 0; --level)
        {
            // If this is last level headers, it represents leaves, the upper levels
            // represent grouping of the headers, whose width is determined by the 
            // lower levels, hence start with bottom most levels.
            // We compute the local x-coordinate of each "header cell" and local
            // "text position" for text content. 
            // The y-coordinate will be assigned while actual rendering, which will 
            // start from highest level i.e. root to leaves.
            if (level == ((int)headers.size() - 1))
            {
                auto sum = 0.f;
                auto totalEx = 0;
                auto height = 0.f;

                for (int16_t vcol = 0; vcol < (int16_t)headers[level].size(); ++vcol)
                {
                    if (gridstate.cols[level].empty())
                        gridstate.cols[level].fill(ItemGridInternalState::HeaderCellResizeState{});

                    if (gridstate.colmap[level].vtol[vcol] == -1)
                    {
                        gridstate.colmap[level].ltov[vcol] = vcol;
                        gridstate.colmap[level].vtol[vcol] = vcol;
                    }

                    auto col = gridstate.colmap[level].vtol[vcol];
                    auto& hdr = headers[level][col];
                    if (hdr.style.font.font == nullptr) hdr.style.font.font = GetFont(hdr.style.font.family,
                        hdr.style.font.size, FT_Normal);

                    static char buffer[256];
                    std::memset(buffer, ' ', 255);
                    buffer[255] = 0;

                    auto colwidth = hdr.props & COL_WidthAbsolute ? (float)hdr.width :
                        renderer.GetTextSize(std::string_view{ buffer, buffer + hdr.width },
                            hdr.style.font.font, hdr.style.font.size).x;
                    auto hasWidth = colwidth > 0.f;
                    colwidth += gridstate.cols[level][col].modified;

                    auto wrap = (hdr.props & COL_WrapHeader) && hasWidth ? colwidth : -1.f;
                    auto textsz = renderer.GetTextSize(hdr.name, hdr.style.font.font, hdr.style.font.size, wrap);
                    hdr.content.Min.x = sum;
                    hdr.textrect.Min.x = hdr.content.Min.x + hdr.style.padding.left;
                    hdr.textrect.Max.x = hdr.textrect.Min.x + textsz.x;
                    hdr.textrect.Min.y = hdr.style.padding.top;
                    hdr.textrect.Max.y = textsz.y;
                    hdr.content.Max.x = hdr.content.Min.x + (hasWidth ? colwidth : colwidth + 
                        textsz.x + hdr.style.padding.h());

                    if (hdr.content.GetWidth() - hdr.style.padding.h() > textsz.x)
                    {
                        auto hdiff = (hdr.content.GetWidth() - textsz.x - hdr.style.padding.h()) * 0.5f;
                        hdr.textrect.TranslateX(hdiff);
                    }

                    hdr.content.Max.y = textsz.y + hdr.style.padding.v();
                    sum += hdr.content.GetWidth();
                    height = std::max(height, hdr.content.GetHeight());

                    // If user has resized column, do not expand it
                    if ((hdr.props & COL_Expandable) && gridstate.cols[level][col].modified == 0.f) totalEx++;
                }

                if (totalEx > 0)
                {
                    auto extra = (content.GetWidth() - sum) / (float)totalEx;
                    sum = 0.f;

                    if (extra > 0.f)
                        for (int16_t col = 0; col < (int16_t)headers[level].size(); ++col)
                        {
                            auto& hdr = headers[level][col];
                            auto width = hdr.content.Min.x;
                            auto isColExpandable = ((hdr.props & COL_Expandable) && 
                                gridstate.cols[level][col].modified == 0.f);
                            hdr.content.Min.x = sum;
                            hdr.content.Max.x = hdr.content.Max.x +
                                isColExpandable ? extra : 0.f;
                            if (!(hdr.props & COL_WrapHeader) && hdr.textrect.GetWidth() <
                                (hdr.content.GetWidth() - hdr.style.padding.h()))
                            {
                                auto diff = (hdr.content.GetWidth() - hdr.style.padding.h() - hdr.textrect.GetWidth()) * 0.5f;
                                hdr.textrect.TranslateX(diff);
                            }
                            sum = hdr.content.Max.x;

                            hdr.content.Max.y = height;
                            auto vdiff = (height - hdr.textrect.GetHeight() - hdr.style.padding.v()) * 0.5f;
                            if (vdiff > 0.f)
                                hdr.textrect.TranslateY(vdiff);
                        }
                    else AlignVCenter(height, level, state);
                }
                else AlignVCenter(height, level, state);
            }
            else
            {
                float lastx = 0.f, height = 0.f;
                if (gridstate.cols[level].empty())
                    gridstate.cols[level].fill(ItemGridInternalState::HeaderCellResizeState{});

                for (int16_t vcol = 0; vcol < (int16_t)headers[level].size(); ++vcol)
                {
                    float sum = 0.f;
                    if (gridstate.colmap[level].vtol[vcol] == -1)
                    {
                        gridstate.colmap[level].ltov[vcol] = vcol;
                        gridstate.colmap[level].vtol[vcol] = vcol;
                    }

                    auto col = gridstate.colmap[level].vtol[vcol];
                    auto& hdr = headers[level][col];
                    hdr.content.Min.x = lastx;
                    if (hdr.style.font.font == nullptr) hdr.style.font.font = GetFont(hdr.style.font.family,
                        hdr.style.font.size, FT_Normal);

                    // Find sum of all descendant headers, which will be this headers width
                    for (int16_t chcol = 0; chcol < (int16_t)headers[level + 1].size(); ++chcol)
                        if (headers[level + 1][chcol].parent == col)
                            sum += headers[level + 1][chcol].content.GetWidth();

                    auto wrap = hdr.props & COL_WrapHeader ? sum : -1.f;
                    auto textsz = renderer.GetTextSize(hdr.name, hdr.style.font.font, hdr.style.font.size, wrap);

                    // The available horizontal size is fixed by sum of descendant widths
                    // The height of the current level of headers is decided by the max height across all
                    hdr.content.Max.x = hdr.content.Min.x + sum;
                    hdr.textrect.Min.x = hdr.content.Min.x + hdr.style.padding.left;
                    hdr.textrect.Min.y = hdr.content.Min.y + hdr.style.padding.top;
                    hdr.textrect.Max.x = hdr.textrect.Min.x + std::min(sum, textsz.x);

                    if (!(hdr.props & COL_WrapHeader) && textsz.x < (sum - hdr.style.padding.h()))
                    {
                        auto diff = (sum - hdr.style.padding.h() - textsz.x) * 0.5f;
                        hdr.textrect.TranslateX(diff);
                    }

                    hdr.textrect.Max.y = hdr.textrect.Min.y + textsz.y;
                    hdr.content.Max.y = hdr.content.Min.y + textsz.y + hdr.style.padding.v();
                    height = std::max(height, hdr.content.GetHeight());
                    lastx += sum;
                }

                AlignVCenter(height, level, state);
            }
        }

        // Draw Headers
        auto posy = content.Min.y;
        auto totalh = 0.f;
        auto width = headers.back().back().content.Max.x - headers.back().front().content.Min.x;
        auto hshift = (width / content.GetWidth()) * -gridstate.scroll.pos.x;
        std::pair<int16_t, int16_t> movingColRange = { INT16_MAX, -1 }, nextMovingRange = { INT16_MAX, -1 };

        for (auto level = 0; level < (int)headers.size(); ++level)
        {
            auto maxh = 0.f, posx = content.Min.x + hshift;

            for (int16_t vcol = 0; vcol < (int16_t)headers[level].size(); ++vcol)
            {
                auto col = gridstate.colmap[level].vtol[vcol];
                auto& hdr = headers[level][col];
                auto isBeingMoved = gridstate.drag.column == vcol && gridstate.drag.level == level;

                if (isBeingMoved)
                {
                    auto movex = mousepos.x - gridstate.drag.startPos.x;
                    gridstate.drag.config = hdr;
                    auto& cfg = gridstate.drag.config;

                    cfg.content.TranslateY(posy);
                    cfg.textrect.TranslateY(posy);
                    cfg.content.TranslateX(posx + movex);
                    cfg.textrect.TranslateX(posx + movex);
                    
                    maxh = std::max(maxh, hdr.content.GetHeight());
                    nextMovingRange = { col, col };
                }
                else if (hdr.parent >= movingColRange.first && hdr.parent <= movingColRange.second)
                {
                    // The parent column is being moved in this case, record the range of child columns mapped to that parent range
                    auto movex = mousepos.x - gridstate.drag.startPos.x;
                    auto& cfg = gridstate.drag.config;

                    hdr.content.TranslateY(posy);
                    hdr.textrect.TranslateY(posy);
                    hdr.content.TranslateX(posx + movex);
                    hdr.textrect.TranslateX(posx + movex);

                    maxh = std::max(maxh, hdr.content.GetHeight());
                    nextMovingRange.first = std::min(nextMovingRange.first, col);
                    nextMovingRange.second = std::max(nextMovingRange.second, col);
                }
                else
                {
                    hdr.content.TranslateY(posy);
                    hdr.textrect.TranslateY(posy);
                    hdr.content.TranslateX(posx);
                    hdr.textrect.TranslateX(posx);
                    renderer.DrawRect(hdr.content.Min, hdr.content.Max, ToRGBA(100, 100, 100), false);

                    ImVec2 textend{ hdr.content.Max - ImVec2{ hdr.style.padding.right, hdr.style.padding.bottom } };
                    DrawText(hdr.textrect.Min, textend, hdr.textrect, hdr.name, false, hdr.style, renderer);
                    maxh = std::max(maxh, hdr.content.GetHeight());
                }

                if (vcol > 0)
                {
                    auto prevcol = gridstate.colmap[level].vtol[vcol - 1];
                    if (headers[level][prevcol].props & COL_Resizable)
                        HandleColumnResize(headers, hdr, gridstate, mousepos, level, col);
                }

                if (hdr.props & COL_Moveable)
                    HandleColumnReorder(headers, gridstate, mousepos, level, vcol);
            }

            posy += maxh;
            totalh += maxh;
            movingColRange = nextMovingRange;
            nextMovingRange = { INT16_MAX, -1 };
        }

        float height = 0.f;

        if (!state.uniformRowHeights)
        {
            // As each cell's height may be different, capture cell heights first,
            // then determine row height as max cell height, align and draw contents
            ImVec2 cellGeometries[128];
            ItemGridState::CellData data[128];

            for (auto row = 0; row < state.config.rows; ++row)
            {
                auto height = 0.f;
                for (int16_t col = 0; col < (int16_t)headers.back().size(); ++col)
                {
                    auto& hdr = headers.back()[col];
                    data[col] = state.cell(row, col, 0);

                    switch (data[col].wtype)
                    {
                    case WT_Label:
                    {
                        auto& itemstyle = Context.GetStyle(data[col].state.label.state);
                        if (itemstyle.font.font == nullptr) itemstyle.font.font = GetFont(itemstyle.font.family,
                            itemstyle.font.size, FT_Normal);
                        auto wrap = itemstyle.font.flags & FontStyleNoWrap ? -1.f : hdr.content.GetWidth();
                        auto textsz = renderer.GetTextSize(data[col].state.label.text, itemstyle.font.font,
                            itemstyle.font.size, wrap);
                        cellGeometries[col] = textsz;
                        height = std::max(height, textsz.y + hdr.style.padding.v());
                        break;
                    }
                    default:
                        break;
                    }
                }

                for (int16_t col = 0; col < (int16_t)headers.back().size(); ++col)
                {
                    auto& hdr = headers.back()[col];
                    auto content = hdr.content;
                    content.Max.y = height;
                    ImVec2 textstart{ content.Min.x + hdr.style.padding.left,
                        content.Min.y + hdr.style.padding.top };

                    if (cellGeometries[col].y < height)
                    {
                        auto vdiff = (height - cellGeometries[col].y) * 0.5f;
                        textstart.y += vdiff;
                    }

                    if (cellGeometries[col].x < content.GetWidth())
                    {
                        auto hdiff = (content.GetWidth() - cellGeometries[col].x) * 0.5f;
                        textstart.x += hdiff;
                    }

                    auto textend = content.Max - ImVec2{ hdr.style.padding.right, hdr.style.padding.bottom };
                    renderer.DrawRect(content.Min, content.Max, ToRGBA(100, 100, 100), false);
                    
                    switch (data[col].wtype)
                    {
                    case WT_Label:
                    {
                        const auto& itemstyle = Context.GetStyle(data[col].state.label.state);
                        DrawText(textstart, textend, { textstart, textstart + cellGeometries[col] }, data[col].state.label.text,
                            data[col].state.label.state& WS_Disabled, itemstyle, renderer);
                        break;
                    }
                    }

                    if (content.Contains(mousepos))
                    {
                        result.col = col;
                        result.row = row;
                        // ... add depth
                    }
                }

                posy += height;
            }
        }
        else
        {
            // Populate column wise for uniform row heights
            ImVec2 startpos{};
            startpos.y = -gridstate.scroll.pos.y * (gridstate.totalsz.y / content.GetHeight());
            renderer.SetClipRect(ImVec2{ content.Min.x, posy }, content.Max);
            auto maxlevel = (int)headers.size() - 1;

            for (int16_t vcol = 0; vcol < (int16_t)headers.back().size(); ++vcol)
            {
                height = 0.f;
                auto col = gridstate.colmap[maxlevel].vtol[vcol];

                for (auto row = 0; row < state.config.rows; ++row)
                {
                    if (col < movingColRange.first || col > movingColRange.second)
                    {
                        auto starty = height + posy + startpos.y;
                        auto itemrect = DrawCells(state, headers, row, col, starty, renderer);
                        height += itemrect.GetHeight();

                        if (itemrect.Contains(mousepos))
                        {
                            result.col = col;
                            result.row = row;
                            // ... add depth
                        }

                        //if (height + posy >= content.Max.y) break; 
                    }
                }
            }
        
            renderer.ResetClipRect();
        }

        totalh += height;

        if (gridstate.drag.column != -1 && gridstate.drag.level != -1)
        {
            const auto& cfg = gridstate.drag.config;
            LOG("Rendering column (v: %d) as moving (%f -> %f)\n", gridstate.drag.column, cfg.content.Min.x, cfg.content.Max.x);

            renderer.DrawRectGradient(cfg.content.Min + ImVec2{ -10.f, 0.f }, { cfg.content.Min.x, content.Max.y }, 
                ToRGBA(0, 0, 0, 0), ToRGBA(100, 100, 100), Direction::DIR_Horizontal);
            renderer.DrawRectGradient({ cfg.content.Max.x, cfg.content.Min.y }, ImVec2{ cfg.content.Max.x + 10.f, content.Max.y }, 
                ToRGBA(100, 100, 100), ToRGBA(0, 0, 0, 0), Direction::DIR_Horizontal);

            renderer.DrawRect(cfg.content.Min, cfg.content.Max, ToRGBA(100, 100, 100), false);
            ImVec2 textend{ cfg.content.Max - ImVec2{ cfg.style.padding.right, cfg.style.padding.bottom } };
            DrawText(cfg.textrect.Min, textend, cfg.textrect, cfg.name, false, cfg.style, renderer);

            auto level = gridstate.drag.level + 1;

            if (level < (int16_t)headers.size())
            {
                auto movex = mousepos.x - gridstate.drag.lastPos.x;
                auto lcol = gridstate.colmap[level - 1].vtol[gridstate.drag.column];
                std::pair<int16_t, int16_t> movingColRange = { lcol, lcol }, nextMovingRange = { INT16_MAX, -1 };

                for (; level < (int16_t)headers.size(); ++level)
                {
                    for (int16_t col = 0; col < (int16_t)headers[level].size(); ++col)
                    {
                        auto& hdr = headers[level][col];

                        if (hdr.parent >= movingColRange.first && hdr.parent <= movingColRange.second)
                        {
                            hdr.content.TranslateX(movex);
                            hdr.textrect.TranslateX(movex);

                            renderer.DrawRect(hdr.content.Min, hdr.content.Max, ToRGBA(100, 100, 100), false);
                            ImVec2 textend{ hdr.content.Max - ImVec2{ hdr.style.padding.right, hdr.style.padding.bottom } };
                            DrawText(hdr.textrect.Min, textend, hdr.textrect, hdr.name, false, hdr.style, renderer);

                            nextMovingRange.first = std::min(nextMovingRange.first, col);
                            nextMovingRange.second = std::max(nextMovingRange.second, col);
                        }
                    }

                    movingColRange = nextMovingRange;
                    nextMovingRange = { INT16_MAX, -1 };
                }
            }

            ImVec2 startpos{};
            startpos.y = -gridstate.scroll.pos.y * (gridstate.totalsz.y / content.GetHeight());
            renderer.SetClipRect(ImVec2{ content.Min.x, posy }, content.Max);

            for (int16_t col = movingColRange.first; col <= movingColRange.second; ++col)
            {
                height = 0.f;

                for (auto row = 0; row < state.config.rows; ++row)
                {
                    if (col >= movingColRange.first && col <= movingColRange.second)
                    {
                        auto itemrect = DrawCells(state, headers, row, col, height + posy + startpos.y, renderer);
                        height += itemrect.GetHeight();

                        if (itemrect.Contains(mousepos))
                        {
                            result.col = col;
                            result.row = row;
                            // ... add depth
                        }
                    }
                }
            }
        }

        gridstate.totalsz.y = totalh;
        gridstate.totalsz.x = width;
        HandleScrollBars(gridstate.scroll, renderer, mousepos, content, gridstate.totalsz);

        // Reset header calculations for next frame
        for (auto level = 0; level < (int)headers.size(); ++level)
        {
            for (int16_t col = 0; col < (int16_t)headers[level].size(); ++col)
            {
                auto& hdr = headers[level][col];
                hdr.content = ImRect{};
                hdr.textrect = ImRect{};
            }
        }

        if (ismouseover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            result.event = WidgetEvent::Clicked;
        else if (ismouseover && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            result.event = WidgetEvent::DoubleClicked;
        else if (ismouseover && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            result.event = WidgetEvent::RightClicked;

        result.geometry = margin;
        return result;
    }

    WindowConfig& GetWindowConfig()
    {
        return Config;
    }

    void BeginFrame()
    {
        Context.InsideFrame = true;
        //ERROR("\n===============================Frame start===============================\n");
    }

    void EndFrame()
    {
        Context.InsideFrame = false;
        Context.lastItemId = -1;
        Context.nextpos = { 0.f, 0.f };
        Context.layoutItems.clear();

        for (auto idx = 0; idx < WSI_Total; ++idx)
        {
            Context.pushedStyles[idx].clear();
            Context.pushedStyles[idx].push();
        }
        
        for (auto idx = 0; idx < WT_TotalTypes; ++idx)
        {
            Context.itemGeometries[idx].reset(ImRect{ {0.f, 0.f}, {0.f, 0.f} });
        }

        assert(Context.currLayoutDepth == -1);
        assert(Context.currSizingDepth == 0);
        assert(Context.currSpanDepth == -1);
        //ERROR("===============================Frame end===============================\n");
    }

    int32_t GetNextId(WidgetType type)
    {
        int32_t id = GetNextCount(type);
        id = id | (type << 16);
        return id;
    }

    int16_t GetNextCount(WidgetType type)
    {
        return Context.maxids[type];
    }

    WidgetDrawResult Label(int32_t id, ImVec2 pos, int32_t geometry)
    {
        assert((id & 0xffff) <= (int)Context.states[WT_Label].size());

        //UsePushedStyle(id);
        auto& state = Context.GetState(id).state.label;
        auto& renderer = *Config.renderer;

        // TODO: Adjust everything...
        auto& style = Context.GetStyle(state.state);
        auto [content, padding, border, margin, textsz] = GetBoxModelBounds(pos, style, state.text, renderer, geometry);
        return LabelImpl(id, margin, border, padding, content, textsz, renderer);
    }

    WidgetDrawResult Button(int32_t id, ImVec2 pos, int32_t geometry)
    {
        assert((id & 0xffff) <= (int)Context.states[WT_Button].size());

        //UsePushedStyle(id);
        auto& state = Context.GetState(id).state.button;
        auto& renderer = *Config.renderer;
        CopyStyle(Context.GetStyle(WS_Default), Context.GetStyle(state.state));

        // TODO: Adjust everything...
        auto& style = Context.GetStyle(state.state);
        auto [content, padding, border, margin, textsz] = GetBoxModelBounds(pos, style, state.text, renderer, geometry);
        return ButtonImpl(id, margin, border, padding, content, textsz, renderer);
    }

    WidgetDrawResult RadioButton(int32_t id, ImVec2 pos, IRenderer& renderer, std::optional<ImVec2> geometry)
    {
        assert(id <= (int)Context.states[WT_RadioButton].size());

        auto& state = Context.GetState(id).state.radio;
        CopyStyle(Context.GetStyle(WS_Default), Context.GetStyle(state.state));

        WidgetEvent result = WidgetEvent::None;
        auto& style = Context.GetStyle(state.state);

        ImRect extent;
        if (geometry.has_value()) { extent.Min = pos; extent.Max = extent.Min + geometry.value(); }
        else
        {
            extent.Min = pos + ImVec2{ style.margin.top + style.padding.top, style.margin.left + style.padding.right };
            extent.Max = extent.Min + style.dimension;
        }

        auto mousepos = ImGui::GetIO().MousePos;
        auto mouseover = extent.Contains(mousepos);
        state.state = mouseover && ImGui::IsMouseDown(ImGuiMouseButton_Left) ? WS_Hovered | WS_Pressed :
            mouseover ? WS_Hovered : WS_Default;

        auto radius = extent.GetHeight() * 0.5f;
        auto center = ImVec2{ radius * 0.5f, radius * 0.5f };
        auto color = Context.GetStyle(state.state).fgcolor;
        renderer.DrawCircle(center, radius, color, false, 1.f);
        radius -= 1.f;
        renderer.DrawCircle(center, radius, color, true);

        if (mouseover && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        {
            result = WidgetEvent::Clicked;
            state.checked = !state.checked;
        }

        if (!geometry.has_value()) extent.Max += ImVec2{ style.margin.right + style.padding.right, style.margin.bottom + style.padding.right };
        return { id, result };
    }

    static void AddExtent(LayoutItemDescriptor& wdesc, int32_t id, const StyleDescriptor& style, const NeighborWidgets& neighbors)
    {
        wdesc.margin.Min = Context.nextpos;
        if (neighbors.bottom != -1) wdesc.margin.Max.y = Context.GetGeometry(neighbors.bottom).Min.y;
        else wdesc.margin.Max.y = ImGui::GetCurrentWindow()->InnerClipRect.Max.y;
        if (neighbors.right != -1) wdesc.margin.Max.x = Context.GetGeometry(neighbors.right).Min.x;
        else wdesc.margin.Max.x = ImGui::GetCurrentWindow()->InnerClipRect.Max.x;

        wdesc.border.Min = wdesc.margin.Min + ImVec2{ style.margin.left, style.margin.top };
        wdesc.border.Max = wdesc.margin.Max - ImVec2{ style.margin.right, style.margin.bottom };

        wdesc.padding.Min = wdesc.border.Min + ImVec2{ style.border.left.thickness, style.border.top.thickness };
        wdesc.padding.Max = wdesc.border.Max - ImVec2{ style.border.right.thickness, style.border.bottom.thickness };

        wdesc.content.Min = wdesc.padding.Min + ImVec2{ style.padding.left, style.padding.top };
        wdesc.content.Max = wdesc.padding.Max - ImVec2{ style.padding.right, style.padding.bottom };
    }

    void Widget(int32_t id, WidgetType type, int32_t geometry, const NeighborWidgets& neighbors)
    {
        assert((id & 0xffff) <= (int)Context.states[type].size());

        auto& renderer = *Config.renderer;
        LayoutItemDescriptor wdesc;
        wdesc.wtype = type;
        wdesc.id = id;
        wdesc.sizing = geometry;

        auto wid = (type << 16) | id;
        //UsePushedStyle(wid);

        switch (type)
        {
        case WT_Label: {
            auto& state = Context.GetState(wid).state.label;
            CopyStyle(Context.GetStyle(WS_Default), Context.GetStyle(state.state));
            auto& style = Context.GetStyle(state.state);

            if (Context.currLayoutDepth != -1)
            {
                auto& layout = Context.layouts[Context.currLayoutDepth];
                auto pos = layout.geometry.Min;
                std::tie(wdesc.content, wdesc.padding, wdesc.border, wdesc.margin, wdesc.text) = GetBoxModelBounds(pos,
                    style, state.text, renderer, geometry, neighbors);
                AddItemToLayout(layout, wdesc);
            }
            else
            {
                std::tie(wdesc.content, wdesc.padding, wdesc.border, wdesc.margin, wdesc.text) = GetBoxModelBounds(Context.nextpos,
                    style, state.text, renderer, geometry, neighbors);
                Context.AddItemGeometry(wid, wdesc.margin);
                LabelImpl(wid, wdesc.margin, wdesc.border, wdesc.padding, wdesc.content, wdesc.text, renderer);
            }
            break;
        }
        case WT_Button: {
            auto& state = Context.GetState(wid).state.button;
            CopyStyle(Context.GetStyle(WS_Default), Context.GetStyle(state.state));
            auto& style = Context.GetStyle(state.state);

            if (Context.currLayoutDepth != -1)
            {
                auto& layout = Context.layouts[Context.currLayoutDepth];
                auto pos = layout.geometry.Min;
                std::tie(wdesc.content, wdesc.padding, wdesc.border, wdesc.margin, wdesc.text) = GetBoxModelBounds(pos,
                    style, state.text, renderer, geometry, neighbors);
                AddItemToLayout(layout, wdesc);
            }
            else
            {
                std::tie(wdesc.content, wdesc.padding, wdesc.border, wdesc.margin, wdesc.text) = GetBoxModelBounds(Context.nextpos,
                    style, state.text, renderer, geometry, neighbors);
                Context.AddItemGeometry(wid, wdesc.margin);
                ButtonImpl(wid, wdesc.margin, wdesc.border, wdesc.padding, wdesc.content, wdesc.text, renderer);
            }
            break;
        }
        case WT_RadioButton: {
            auto& state = Context.GetState(wid).state.radio;
            auto& style = Context.GetStyle(state.state);
            CopyStyle(Context.GetStyle(WS_Default), style);
            AddExtent(wdesc, id, style, neighbors);
            auto bounds = RadioButtonBounds(wdesc.content);

            renderer.SetClipRect(wdesc.content.Min, wdesc.content.Max);
            RadioButtonImpl(wid, state, bounds, renderer);
            Context.AddItemGeometry(wid, bounds);
            renderer.ResetClipRect();
            break;
        }
        case WT_ToggleButton: {
            auto& state = Context.GetState(wid).state.toggle;
            auto& style = Context.GetStyle(state.state);
            CopyStyle(Context.GetStyle(WS_Default), style);
            AddExtent(wdesc, id, style, neighbors);
            auto [bounds, textsz] = ToggleButtonBounds(state, wdesc.content, renderer);

            renderer.SetClipRect(wdesc.content.Min, wdesc.content.Max);
            ToggleButtonImpl(wid, state, bounds, textsz, renderer);
            Context.AddItemGeometry(wid, bounds);
            renderer.ResetClipRect();
            break;
        }
        case WT_TextInput: {
            auto& state = Context.GetState(wid).state.input;
            auto& style = Context.GetStyle(state.state);
            CopyStyle(Context.GetStyle(WS_Default), style);
            AddExtent(wdesc, id, style, neighbors);

            auto w = style.specified & StyleWidth ? style.dimension.x : wdesc.content.Max.x;
            w = ImClamp(w, style.mindim.x, style.maxdim.x);
            wdesc.content.Max.x = wdesc.content.Min.x + w;
            wdesc.padding.Max.x = wdesc.content.Max.x + style.padding.right;
            wdesc.border.Max.x = wdesc.padding.Max.x + style.border.right.thickness;
            wdesc.margin.Max.x = wdesc.border.Max.x + style.margin.right;

            wdesc.content.Max.y = wdesc.content.Min.y + style.font.size + 2.f;
            wdesc.padding.Max.y = wdesc.content.Max.y + style.padding.bottom;
            wdesc.border.Max.y = wdesc.padding.Max.y + style.border.bottom.thickness;
            wdesc.margin.Max.y = wdesc.border.Max.y + style.margin.bottom;

            renderer.SetClipRect(wdesc.margin.Min, wdesc.margin.Max);
            TextInputImpl(wid, state, wdesc.border, wdesc.content, renderer);
            Context.AddItemGeometry(wid, wdesc.margin);
            renderer.ResetClipRect();
            break;
        }
        case WT_TabBar: {
            auto& state = Context.GetState(wid).state.grid;
            auto& style = Context.GetStyle(state.state);
            CopyStyle(Context.GetStyle(WS_Default), style);
            AddExtent(wdesc, id, style, neighbors);

            renderer.SetClipRect(wdesc.margin.Min, wdesc.margin.Max);

            renderer.ResetClipRect();
            break;
        }
        case WT_ItemGrid: {
            auto& state = Context.GetState(wid).state.grid;
            auto& style = Context.GetStyle(state.state);
            CopyStyle(Context.GetStyle(WS_Default), style);
            AddExtent(wdesc, id, style, neighbors);

            renderer.SetClipRect(wdesc.margin.Min, wdesc.margin.Max);
            ItemGridImpl(wid, wdesc.margin, wdesc.border, wdesc.padding, wdesc.content, wdesc.text, renderer);
            Context.AddItemGeometry(wid, wdesc.margin);
            renderer.ResetClipRect();
            break;
        }
        default: break;
        }

        Context.currStyleStates = 0;
        for (auto idx = 0; idx < WSI_Total; ++idx) Context.currStyle[idx] = StyleDescriptor{};
    }

    void Label(int32_t id, int32_t geometry, const NeighborWidgets& neighbors)
    {
        Widget(id, WT_Label, geometry, neighbors);
    }

    void Button(int32_t id, int32_t geometry, const NeighborWidgets& neighbors)
    {
        Widget(id, WT_Button, geometry, neighbors);
    }

    void ToggleButton(int32_t id, int32_t geometry, const NeighborWidgets& neighbors)
    {
        Widget(id, WT_ToggleButton, geometry, neighbors);
    }

    void RadioButton(int32_t id, int32_t geometry, const NeighborWidgets& neighbors)
    {
        Widget(id, WT_RadioButton, geometry, neighbors);
    }

    void TextInput(int32_t id, int32_t geometry, const NeighborWidgets& neighbors)
    {
        Widget(id, WT_TextInput, geometry, neighbors);
    }

    void TabBar(int32_t id, int32_t geometry, const NeighborWidgets& neighbors)
    {
        Widget(id, WT_TabBar, geometry, neighbors);
    }

    void ItemGrid(int32_t id, int32_t geometry, const NeighborWidgets& neighbors)
    {
        Widget(id, WT_ItemGrid, geometry, neighbors);
    }

    WidgetStateData& CreateWidget(WidgetType type, int16_t id)
    {
        int32_t wid = id;
        wid = wid | (type << 16);
        Context.maxids[type]++;
        if (Context.InsideFrame) 
            Context.tempids[type] = std::min(Context.tempids[type], Context.maxids[type]);

        auto& state = Context.GetState(wid);
        return state;
    }

    WidgetStateData& CreateWidget(int32_t id)
    {
        auto wtype = (WidgetType)(id >> 16);
        return CreateWidget(wtype, (int16_t)(id & 0xffff));
    }

    void ItemGridState::setCellPadding(float padding)
    {
        for (auto level = 0; level < (int)config.headers.size(); ++level)
            for (auto col = 0; col < (int)config.headers[level].size(); ++col)
            {
                auto& p = config.headers[level][col].style.padding;
                p.top = p.bottom = p.left = p.right = padding;
            }
    }

    void ItemGridState::setColumnResizable(int16_t col, bool resizable)
    {
        setColumnProps(col, COL_Resizable, resizable);
    }

    void ItemGridState::setColumnProps(int16_t col, ColumnProperty prop, bool set)
    {
        if (col >= 0)
        {
            auto lastLevel = (int)config.headers.size() - 1;
            set ? config.headers[lastLevel][col].props |= prop : config.headers[lastLevel][col].props &= ~prop;
            while (lastLevel > 0)
            {
                auto parent = config.headers[lastLevel][col].parent;
                lastLevel--;
                set ? config.headers[lastLevel][parent].props |= prop : config.headers[lastLevel][parent].props &= ~prop;
            }
        }
        else
        {
            for (auto level = 0; level < (int)config.headers.size(); ++level)
                for (auto lcol = 0; lcol < (int)config.headers[level].size(); ++lcol)
                    set ? config.headers[level][lcol].props |= prop : config.headers[level][lcol].props &= ~prop;
        }
    }

#pragma endregion
}
