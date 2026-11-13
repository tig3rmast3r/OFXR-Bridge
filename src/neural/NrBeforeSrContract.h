#pragma once

#include <cstdint>

namespace DlssNr
{
struct RenderExtent
{
    unsigned int width;
    unsigned int height;
};

// A pair is authoritative only when both components are present and nonzero.
inline RenderExtent BeforeSrExtent(RenderExtent subrect, bool hasSubrect, RenderExtent generic,
                                   RenderExtent allocation)
{
    if (hasSubrect && subrect.width && subrect.height)
        return subrect;
    if (generic.width && generic.height)
        return generic;
    return allocation;
}

inline bool FitsBeforeSr(RenderExtent active, uint64_t allocationWidth, unsigned int allocationHeight)
{
    return active.width && active.height && active.width <= allocationWidth && active.height <= allocationHeight;
}

// Display-sized motion vectors cover the declared DLSS output view, not its
// padded allocation. Keep invalid complete declarations visible to bounds
// checks; never silently clip a view to make unrelated guides appear usable.
inline RenderExtent DeclaredOutputExtent(RenderExtent declared, RenderExtent allocation)
{
    return declared.width && declared.height ? declared : allocation;
}

}
