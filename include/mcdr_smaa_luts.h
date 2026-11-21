#pragma once

namespace d3d12_mc::smaa_luts {

inline constexpr unsigned kAreaTexWidth  = 160;
inline constexpr unsigned kAreaTexHeight = 560;
inline constexpr unsigned kAreaTexBytesPerPixel = 2;
extern const unsigned char kAreaTex[];

inline constexpr unsigned kSearchTexWidth  = 64;
inline constexpr unsigned kSearchTexHeight = 16;
inline constexpr unsigned kSearchTexBytesPerPixel = 1;
extern const unsigned char kSearchTex[];

}
