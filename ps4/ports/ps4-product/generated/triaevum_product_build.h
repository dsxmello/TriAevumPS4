#pragma once

// PS4 product build (ports/ps4-product): the title AOT is linked statically
// into the eboot, there is no NRI, no experimental GI and no developer tools.
inline constexpr const char* kTriAevumSourceCommit = "4aca4d7b6a8cb61905bc3480b30398202456425e";
inline constexpr bool kTriAevumProductPlugin = false;
inline constexpr bool kTriAevumProductNri = false;
inline constexpr bool kTriAevumProductExperimentalGi = false;
inline constexpr bool kTriAevumProductDeveloperTools = false;
