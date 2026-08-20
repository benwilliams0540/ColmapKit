// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.

#pragma once

#include <string>
#include <string_view>

namespace colmap::internal {

std::string FrameFeatureSHA256(std::string_view input);

}  // namespace colmap::internal
