#pragma once

// PS4 product build (ports/ps4-product): the title AOT is linked statically
// into the eboot, there is no NRI, no experimental GI and no developer tools.
inline constexpr const char* kTriAevumSourceCommit = "b20ff67e19716319c518665b31931556f4711653";
inline constexpr bool kTriAevumProductPlugin = false;
inline constexpr bool kTriAevumProductNri = false;
inline constexpr bool kTriAevumProductExperimentalGi = false;
inline constexpr bool kTriAevumProductDeveloperTools = false;
