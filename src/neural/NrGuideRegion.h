#pragma once
#include <cstdint>
namespace DlssNr {
struct GuideRegion {unsigned x{},y{},width{},height{};};
constexpr bool GuideFits(GuideRegion r,std::uint64_t w,std::uint64_t h) noexcept {
 return r.width&&r.height&&r.x<=w&&r.y<=h&&r.width<=w-r.x&&r.height<=h-r.y;
}
constexpr float GuideScale(float source_units,unsigned supplied_extent,unsigned reference_extent) noexcept {
 return reference_extent?source_units*float(supplied_extent)/float(reference_extent):0.f;
}
}
