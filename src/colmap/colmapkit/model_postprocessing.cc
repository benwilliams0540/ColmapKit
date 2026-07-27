// Copyright (c), ETH Zurich and UNC Chapel Hill.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
//     * Neither the name of ETH Zurich and UNC Chapel Hill nor the names of
//       its contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "colmap/colmapkit/colmapkit.h"
#include "colmap/scene/reconstruction.h"
#include "colmap/sfm/observation_manager.h"
#include "colmap/util/file.h"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace {

std::string CStringOrEmpty(const char* value) {
  return value == nullptr ? std::string() : std::string(value);
}

std::filesystem::path PathFromCString(const char* value) {
  return std::filesystem::path(CStringOrEmpty(value));
}

template <typename Struct>
bool HasSupportedStructSize(const Struct& value) {
  return value.struct_size == 0 || value.struct_size >= sizeof(Struct);
}

template <typename Result>
bool ResetResult(Result* result) {
  if (result == nullptr) {
    return false;
  }
  if (!HasSupportedStructSize(*result)) {
    return false;
  }
  std::memset(result, 0, sizeof(*result));
  result->struct_size = sizeof(*result);
  return true;
}

template <typename Result>
void CopyMessage(const std::string& message, Result* result) {
  std::strncpy(result->message, message.c_str(), COLMAPKIT_MESSAGE_CAPACITY);
  result->message[COLMAPKIT_MESSAGE_CAPACITY - 1] = '\0';
}

template <typename Result>
ColmapKitStatus SetFailure(const ColmapKitStatus status,
                           const std::string& message,
                           Result* result) {
  result->status = status;
  CopyMessage(message, result);
  return status;
}

bool HasBinaryModelFiles(const std::filesystem::path& path) {
  return colmap::ExistsFile(path / "cameras.bin") &&
         colmap::ExistsFile(path / "images.bin") &&
         colmap::ExistsFile(path / "points3D.bin");
}

bool HasTextModelFiles(const std::filesystem::path& path) {
  return colmap::ExistsFile(path / "cameras.txt") &&
         colmap::ExistsFile(path / "images.txt") &&
         colmap::ExistsFile(path / "points3D.txt");
}

void ValidateModelPaths(const char* input_value, const char* output_value) {
  const auto input_path = PathFromCString(input_value);
  const auto output_path = PathFromCString(output_value);
  if (input_path.empty()) {
    throw std::invalid_argument("input_path is required.");
  }
  if (output_path.empty()) {
    throw std::invalid_argument("output_path is required.");
  }
  if (!colmap::ExistsDir(input_path)) {
    throw std::invalid_argument("input_path must be an existing directory.");
  }
  if (!HasBinaryModelFiles(input_path) && !HasTextModelFiles(input_path)) {
    throw std::invalid_argument(
        "input_path must contain a complete COLMAP binary or text sparse "
        "model.");
  }
  if (!colmap::ExistsDir(output_path)) {
    throw std::invalid_argument("output_path must be an existing directory.");
  }
}

void WriteBoundingBox(const std::filesystem::path& reconstruction_path,
                      const Eigen::AlignedBox3d& bbox) {
  const Eigen::Vector3d extent = bbox.diagonal();
  {
    const auto path = reconstruction_path / "bbox_aligned.txt";
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open bounding-box output: " +
                               path.string());
    }
    file.imbue(std::locale::classic());
    file.precision(17);
    file << bbox.min().transpose() << '\n';
    file << bbox.max().transpose() << '\n';
  }
  {
    const auto path = reconstruction_path / "bbox_oriented.txt";
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) {
      throw std::runtime_error("Failed to open bounding-box output: " +
                               path.string());
    }
    file.imbue(std::locale::classic());
    file.precision(17);
    const Eigen::Vector3d center = (bbox.min() + bbox.max()) * 0.5;
    file << center.transpose() << "\n\n";
    file << "1 0 0\n0 1 0\n0 0 1\n\n";
    file << extent.transpose() << '\n';
  }
}

template <typename Result>
ColmapKitStatus HandleFailure(const std::invalid_argument& error,
                              Result* result) {
  return SetFailure(COLMAPKIT_STATUS_INVALID_ARGUMENT, error.what(), result);
}

template <typename Result>
ColmapKitStatus HandleFailure(const std::exception& error, Result* result) {
  return SetFailure(COLMAPKIT_STATUS_RUNTIME_ERROR, error.what(), result);
}

template <typename Result>
ColmapKitStatus HandleUnknownFailure(const std::string& operation,
                                     Result* result) {
  return SetFailure(COLMAPKIT_STATUS_RUNTIME_ERROR,
                    "Unknown ColmapKit " + operation + " failure.",
                    result);
}

size_t CountModelFiles(const std::filesystem::path& output_path,
                       const ColmapKitModelOutputType output_type) {
  const char* extension =
      output_type == COLMAPKIT_MODEL_OUTPUT_TYPE_BIN ? ".bin" : ".txt";
  size_t count = 0;
  for (const char* stem : {"rigs", "cameras", "frames", "images", "points3D"}) {
    if (colmap::ExistsFile(output_path / (std::string(stem) + extension))) {
      ++count;
    }
  }
  return count;
}

}  // namespace

ColmapKitStatus ColmapKitRunPointFiltering(
    const ColmapKitPointFilteringConfig* config,
    ColmapKitPointFilteringResult* result) {
  if (!ResetResult(result)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (config == nullptr) {
    return SetFailure(
        COLMAPKIT_STATUS_INVALID_ARGUMENT, "config is required.", result);
  }

  try {
    if (!HasSupportedStructSize(*config)) {
      throw std::invalid_argument(
          "Unsupported ColmapKit point-filtering config struct size.");
    }
    ValidateModelPaths(config->input_path, config->output_path);
    if (config->min_track_len < 0) {
      throw std::invalid_argument("min_track_len must be non-negative.");
    }
    if (!std::isfinite(config->max_reproj_error) ||
        config->max_reproj_error < 0.0) {
      throw std::invalid_argument(
          "max_reproj_error must be finite and non-negative.");
    }
    if (!std::isfinite(config->min_tri_angle) || config->min_tri_angle < 0.0) {
      throw std::invalid_argument(
          "min_tri_angle must be finite and non-negative.");
    }

    colmap::Reconstruction reconstruction;
    reconstruction.Read(PathFromCString(config->input_path));
    result->input_points = reconstruction.NumPoints3D();
    result->input_registered_images = reconstruction.NumRegImages();

    colmap::ObservationManager observation_manager(reconstruction);
    result->filtered_observations = observation_manager.FilterAllPoints3D(
        config->max_reproj_error, config->min_tri_angle);
    result->filtered_observations +=
        observation_manager.FilterPoints3DWithShortTracks(
            static_cast<size_t>(config->min_track_len));

    result->output_points = reconstruction.NumPoints3D();
    result->output_registered_images = reconstruction.NumRegImages();
    result->filtered_points = result->input_points - result->output_points;
    reconstruction.Write(PathFromCString(config->output_path));

    result->status = COLMAPKIT_STATUS_OK;
    std::ostringstream message;
    message << "Point filtering complete: input_points=" << result->input_points
            << ", output_points=" << result->output_points
            << ", registered_images=" << result->output_registered_images
            << ", filtered_points=" << result->filtered_points
            << ", filtered_observations=" << result->filtered_observations;
    CopyMessage(message.str(), result);
    return result->status;
  } catch (const std::invalid_argument& error) {
    return HandleFailure(error, result);
  } catch (const std::exception& error) {
    return HandleFailure(error, result);
  } catch (...) {
    return HandleUnknownFailure("point-filtering", result);
  }
}

ColmapKitStatus ColmapKitRunModelCropping(
    const ColmapKitModelCroppingConfig* config,
    ColmapKitModelCroppingResult* result) {
  if (!ResetResult(result)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (config == nullptr) {
    return SetFailure(
        COLMAPKIT_STATUS_INVALID_ARGUMENT, "config is required.", result);
  }

  try {
    if (!HasSupportedStructSize(*config)) {
      throw std::invalid_argument(
          "Unsupported ColmapKit model-cropping config struct size.");
    }
    ValidateModelPaths(config->input_path, config->output_path);
    const double bounds[] = {config->min_x,
                             config->min_y,
                             config->min_z,
                             config->max_x,
                             config->max_y,
                             config->max_z};
    for (const double value : bounds) {
      if (!std::isfinite(value)) {
        throw std::invalid_argument(
            "Model-cropping bounds must contain only finite values.");
      }
    }
    if (config->min_x > config->max_x || config->min_y > config->max_y ||
        config->min_z > config->max_z) {
      throw std::invalid_argument(
          "Model-cropping minimum bounds must not exceed maximum bounds.");
    }

    colmap::Reconstruction reconstruction;
    reconstruction.Read(PathFromCString(config->input_path));
    result->input_points = reconstruction.NumPoints3D();
    result->input_registered_images = reconstruction.NumRegImages();

    const Eigen::AlignedBox3d bounding_box(
        Eigen::Vector3d(config->min_x, config->min_y, config->min_z),
        Eigen::Vector3d(config->max_x, config->max_y, config->max_z));
    colmap::Reconstruction cropped = reconstruction.Crop(bounding_box);
    result->output_points = cropped.NumPoints3D();
    result->output_registered_images = cropped.NumRegImages();
    result->removed_points = result->input_points - result->output_points;
    result->removed_registered_images =
        result->input_registered_images - result->output_registered_images;

    const auto output_path = PathFromCString(config->output_path);
    cropped.Write(output_path);
    WriteBoundingBox(output_path, bounding_box);

    result->status = COLMAPKIT_STATUS_OK;
    std::ostringstream message;
    message << "Model cropping complete: input_points=" << result->input_points
            << ", output_points=" << result->output_points
            << ", input_registered_images=" << result->input_registered_images
            << ", output_registered_images=" << result->output_registered_images
            << ", removed_points=" << result->removed_points;
    CopyMessage(message.str(), result);
    return result->status;
  } catch (const std::invalid_argument& error) {
    return HandleFailure(error, result);
  } catch (const std::exception& error) {
    return HandleFailure(error, result);
  } catch (...) {
    return HandleUnknownFailure("model-cropping", result);
  }
}

ColmapKitStatus ColmapKitRunModelConversion(
    const ColmapKitModelConversionConfig* config,
    ColmapKitModelConversionResult* result) {
  if (!ResetResult(result)) {
    return COLMAPKIT_STATUS_INVALID_ARGUMENT;
  }
  if (config == nullptr) {
    return SetFailure(
        COLMAPKIT_STATUS_INVALID_ARGUMENT, "config is required.", result);
  }

  try {
    if (!HasSupportedStructSize(*config)) {
      throw std::invalid_argument(
          "Unsupported ColmapKit model-conversion config struct size.");
    }
    ValidateModelPaths(config->input_path, config->output_path);
    if (config->output_type != COLMAPKIT_MODEL_OUTPUT_TYPE_BIN &&
        config->output_type != COLMAPKIT_MODEL_OUTPUT_TYPE_TXT) {
      return SetFailure(COLMAPKIT_STATUS_UNSUPPORTED,
                        "output_type must be BIN or TXT.",
                        result);
    }

    colmap::Reconstruction reconstruction;
    reconstruction.Read(PathFromCString(config->input_path));
    result->input_points = reconstruction.NumPoints3D();
    result->input_registered_images = reconstruction.NumRegImages();
    result->output_type = config->output_type;

    const auto output_path = PathFromCString(config->output_path);
    if (config->output_type == COLMAPKIT_MODEL_OUTPUT_TYPE_BIN) {
      reconstruction.WriteBinary(output_path);
    } else {
      reconstruction.WriteText(output_path);
    }

    result->output_points = reconstruction.NumPoints3D();
    result->output_registered_images = reconstruction.NumRegImages();
    result->files_written = CountModelFiles(output_path, config->output_type);
    result->status = COLMAPKIT_STATUS_OK;
    std::ostringstream message;
    message << "Model conversion complete: output_type="
            << (result->output_type == COLMAPKIT_MODEL_OUTPUT_TYPE_BIN ? "BIN"
                                                                       : "TXT")
            << ", points=" << result->output_points
            << ", registered_images=" << result->output_registered_images
            << ", files_written=" << result->files_written;
    CopyMessage(message.str(), result);
    return result->status;
  } catch (const std::invalid_argument& error) {
    return HandleFailure(error, result);
  } catch (const std::exception& error) {
    return HandleFailure(error, result);
  } catch (...) {
    return HandleUnknownFailure("model-conversion", result);
  }
}
