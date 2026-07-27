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
#include "colmap/exe/model.h"
#include "colmap/exe/sfm.h"
#include "colmap/math/random.h"
#include "colmap/scene/reconstruction.h"
#include "colmap/scene/synthetic.h"
#include "colmap/util/file.h"
#include "colmap/util/testing.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

#include <Eigen/Core>
#include <gtest/gtest.h>

namespace colmap {
namespace {

using CommandFunction = int (*)(int, char**);

int RunCommand(CommandFunction command, std::vector<std::string> arguments) {
  std::vector<char*> argv;
  argv.reserve(arguments.size());
  for (std::string& argument : arguments) {
    argv.push_back(argument.data());
  }
  return command(static_cast<int>(argv.size()), argv.data());
}

std::string ReadFileBytes(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

void ExpectSamePoints(const Reconstruction& left, const Reconstruction& right) {
  ASSERT_EQ(left.NumPoints3D(), right.NumPoints3D());
  const auto left_id_set = left.Point3DIds();
  const auto right_id_set = right.Point3DIds();
  std::vector<point3D_t> left_ids(left_id_set.begin(), left_id_set.end());
  std::vector<point3D_t> right_ids(right_id_set.begin(), right_id_set.end());
  std::sort(left_ids.begin(), left_ids.end());
  std::sort(right_ids.begin(), right_ids.end());
  ASSERT_EQ(left_ids, right_ids);
  for (const point3D_t point3D_id : left_ids) {
    EXPECT_EQ(left.Point3D(point3D_id).xyz, right.Point3D(point3D_id).xyz);
    EXPECT_EQ(left.Point3D(point3D_id).track.Length(),
              right.Point3D(point3D_id).track.Length());
  }
}

Reconstruction CreateFixture(const std::filesystem::path& input_path) {
  SetPRNGSeed(0);
  SyntheticDatasetOptions options;
  options.num_rigs = 1;
  options.num_cameras_per_rig = 1;
  options.num_frames_per_rig = 4;
  options.num_points3D = 12;
  options.num_points2D_without_point3D = 0;

  Reconstruction reconstruction;
  SynthesizeDataset(options, &reconstruction);

  const auto point3D_id_set = reconstruction.Point3DIds();
  std::vector<point3D_t> point3D_ids(point3D_id_set.begin(),
                                     point3D_id_set.end());
  std::sort(point3D_ids.begin(), point3D_ids.end());
  EXPECT_EQ(point3D_ids.size(), 12);

  const TrackElement short_track_element =
      reconstruction.Point3D(point3D_ids[0]).track.Elements().front();
  reconstruction.DeleteObservation(short_track_element.image_id,
                                   short_track_element.point2D_idx);

  const TrackElement high_error_element =
      reconstruction.Point3D(point3D_ids[1]).track.Elements().front();
  reconstruction.Image(high_error_element.image_id)
      .Point2D(high_error_element.point2D_idx)
      .xy += Eigen::Vector2d(100.0, 100.0);

  CreateDirIfNotExists(input_path);
  reconstruction.WriteBinary(input_path);
  return reconstruction;
}

std::string BoundaryString(const Eigen::AlignedBox3d& bounds) {
  std::ostringstream stream;
  stream.imbue(std::locale::classic());
  stream << std::setprecision(17) << bounds.min().x() << ',' << bounds.min().y()
         << ',' << bounds.min().z() << ',' << bounds.max().x() << ','
         << bounds.max().y() << ',' << bounds.max().z();
  return stream.str();
}

TEST(ColmapKitModelPostprocessing, PointFilteringMatchesCliRetainedPoints) {
  const auto test_dir = CreateTestDir();
  const auto input_path = test_dir / "input";
  const auto cli_output_path = test_dir / "cli";
  const auto colmapkit_output_path = test_dir / "colmapkit";
  const Reconstruction input = CreateFixture(input_path);
  CreateDirIfNotExists(cli_output_path);
  CreateDirIfNotExists(colmapkit_output_path);

  ASSERT_EQ(RunCommand(RunPointFiltering,
                       {"colmap",
                        "--input_path",
                        input_path.string(),
                        "--output_path",
                        cli_output_path.string(),
                        "--min_track_len",
                        "4",
                        "--max_reproj_error",
                        "4",
                        "--min_tri_angle",
                        "0"}),
            EXIT_SUCCESS);

  ColmapKitPointFilteringConfig config = {};
  config.struct_size = sizeof(config);
  config.input_path = input_path.c_str();
  config.output_path = colmapkit_output_path.c_str();
  config.min_track_len = 4;
  config.max_reproj_error = 4.0;
  config.min_tri_angle = 0.0;
  ColmapKitPointFilteringResult result = {};
  result.struct_size = sizeof(result);
  ASSERT_EQ(ColmapKitRunPointFiltering(&config, &result), COLMAPKIT_STATUS_OK)
      << result.message;

  Reconstruction cli_output;
  cli_output.Read(cli_output_path);
  Reconstruction colmapkit_output;
  colmapkit_output.Read(colmapkit_output_path);
  ExpectSamePoints(cli_output, colmapkit_output);
  EXPECT_LT(result.output_points, result.input_points);
  EXPECT_EQ(result.input_points, input.NumPoints3D());
  EXPECT_EQ(result.output_points, cli_output.NumPoints3D());
  EXPECT_EQ(result.output_registered_images, cli_output.NumRegImages());
  EXPECT_EQ(result.filtered_points, result.input_points - result.output_points);
  EXPECT_GT(result.filtered_observations, 0);
}

TEST(ColmapKitModelPostprocessing, ModelCroppingMatchesCliBoundsBehavior) {
  const auto test_dir = CreateTestDir();
  const auto input_path = test_dir / "input";
  const auto cli_output_path = test_dir / "cli";
  const auto colmapkit_output_path = test_dir / "colmapkit";
  const Reconstruction input = CreateFixture(input_path);
  CreateDirIfNotExists(cli_output_path);
  CreateDirIfNotExists(colmapkit_output_path);

  std::vector<double> point_x;
  point_x.reserve(input.NumPoints3D());
  for (const auto& [_, point3D] : input.Points3D()) {
    point_x.push_back(point3D.xyz.x());
  }
  std::sort(point_x.begin(), point_x.end());
  Eigen::AlignedBox3d bounds = input.ComputeBoundingBox(0.0, 1.0);
  bounds.max().x() = point_x[point_x.size() / 2];

  ASSERT_EQ(RunCommand(RunModelCropper,
                       {"colmap",
                        "--input_path",
                        input_path.string(),
                        "--output_path",
                        cli_output_path.string(),
                        "--boundary",
                        BoundaryString(bounds)}),
            EXIT_SUCCESS);

  ColmapKitModelCroppingConfig config = {};
  config.struct_size = sizeof(config);
  config.input_path = input_path.c_str();
  config.output_path = colmapkit_output_path.c_str();
  config.min_x = bounds.min().x();
  config.min_y = bounds.min().y();
  config.min_z = bounds.min().z();
  config.max_x = bounds.max().x();
  config.max_y = bounds.max().y();
  config.max_z = bounds.max().z();
  ColmapKitModelCroppingResult result = {};
  result.struct_size = sizeof(result);
  ASSERT_EQ(ColmapKitRunModelCropping(&config, &result), COLMAPKIT_STATUS_OK)
      << result.message;

  Reconstruction cli_output;
  cli_output.Read(cli_output_path);
  Reconstruction colmapkit_output;
  colmapkit_output.Read(colmapkit_output_path);
  ExpectSamePoints(cli_output, colmapkit_output);
  EXPECT_EQ(ReadFileBytes(cli_output_path / "bbox_aligned.txt"),
            ReadFileBytes(colmapkit_output_path / "bbox_aligned.txt"));
  EXPECT_EQ(ReadFileBytes(cli_output_path / "bbox_oriented.txt"),
            ReadFileBytes(colmapkit_output_path / "bbox_oriented.txt"));
  EXPECT_GT(result.output_points, 0);
  EXPECT_LT(result.output_points, result.input_points);
  EXPECT_EQ(result.output_points, cli_output.NumPoints3D());
  EXPECT_EQ(result.output_registered_images, cli_output.NumRegImages());
  EXPECT_EQ(result.removed_points, result.input_points - result.output_points);
}

TEST(ColmapKitModelPostprocessing, TextConversionMatchesCliBytes) {
  const auto test_dir = CreateTestDir();
  const auto input_path = test_dir / "input";
  const auto cli_output_path = test_dir / "cli";
  const auto colmapkit_output_path = test_dir / "colmapkit";
  const Reconstruction input = CreateFixture(input_path);
  CreateDirIfNotExists(cli_output_path);
  CreateDirIfNotExists(colmapkit_output_path);

  ASSERT_EQ(RunCommand(RunModelConverter,
                       {"colmap",
                        "--input_path",
                        input_path.string(),
                        "--output_path",
                        cli_output_path.string(),
                        "--output_type",
                        "TXT"}),
            EXIT_SUCCESS);

  ColmapKitModelConversionConfig config = {};
  config.struct_size = sizeof(config);
  config.input_path = input_path.c_str();
  config.output_path = colmapkit_output_path.c_str();
  config.output_type = COLMAPKIT_MODEL_OUTPUT_TYPE_TXT;
  ColmapKitModelConversionResult result = {};
  result.struct_size = sizeof(result);
  ASSERT_EQ(ColmapKitRunModelConversion(&config, &result), COLMAPKIT_STATUS_OK)
      << result.message;

  for (const char* filename : {"rigs.txt",
                               "cameras.txt",
                               "frames.txt",
                               "images.txt",
                               "points3D.txt"}) {
    EXPECT_EQ(ReadFileBytes(cli_output_path / filename),
              ReadFileBytes(colmapkit_output_path / filename))
        << filename;
  }
  EXPECT_EQ(result.input_points, input.NumPoints3D());
  EXPECT_EQ(result.output_points, input.NumPoints3D());
  EXPECT_EQ(result.output_registered_images, input.NumRegImages());
  EXPECT_EQ(result.files_written, 5);
}

TEST(ColmapKitModelPostprocessing, BinaryConversionMatchesCliBytes) {
  const auto test_dir = CreateTestDir();
  const auto binary_input_path = test_dir / "binary-input";
  const auto text_input_path = test_dir / "text-input";
  const auto cli_output_path = test_dir / "cli";
  const auto colmapkit_output_path = test_dir / "colmapkit";
  const Reconstruction input = CreateFixture(binary_input_path);
  CreateDirIfNotExists(text_input_path);
  CreateDirIfNotExists(cli_output_path);
  CreateDirIfNotExists(colmapkit_output_path);
  input.WriteText(text_input_path);

  ASSERT_EQ(RunCommand(RunModelConverter,
                       {"colmap",
                        "--input_path",
                        text_input_path.string(),
                        "--output_path",
                        cli_output_path.string(),
                        "--output_type",
                        "BIN"}),
            EXIT_SUCCESS);

  ColmapKitModelConversionConfig config = {};
  config.struct_size = sizeof(config);
  config.input_path = text_input_path.c_str();
  config.output_path = colmapkit_output_path.c_str();
  config.output_type = COLMAPKIT_MODEL_OUTPUT_TYPE_BIN;
  ColmapKitModelConversionResult result = {};
  result.struct_size = sizeof(result);
  ASSERT_EQ(ColmapKitRunModelConversion(&config, &result), COLMAPKIT_STATUS_OK)
      << result.message;

  for (const char* filename : {"rigs.bin",
                               "cameras.bin",
                               "frames.bin",
                               "images.bin",
                               "points3D.bin"}) {
    EXPECT_EQ(ReadFileBytes(cli_output_path / filename),
              ReadFileBytes(colmapkit_output_path / filename))
        << filename;
  }
  EXPECT_EQ(result.files_written, 5);
}

TEST(ColmapKitModelPostprocessing, InvalidInputsReturnMessages) {
  const auto test_dir = CreateTestDir();
  const auto output_path = test_dir / "output";
  CreateDirIfNotExists(output_path);
  const std::string missing_path = (test_dir / "missing").string();

  ColmapKitPointFilteringConfig filtering_config = {};
  filtering_config.struct_size = sizeof(filtering_config);
  filtering_config.input_path = missing_path.c_str();
  filtering_config.output_path = output_path.c_str();
  filtering_config.min_track_len = 2;
  filtering_config.max_reproj_error = 4.0;
  filtering_config.min_tri_angle = 1.5;
  ColmapKitPointFilteringResult filtering_result = {};
  filtering_result.struct_size = sizeof(filtering_result);
  EXPECT_EQ(ColmapKitRunPointFiltering(&filtering_config, &filtering_result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(std::string(filtering_result.message).find("input_path"),
            std::string::npos);

  const auto input_path = test_dir / "input";
  CreateFixture(input_path);
  ColmapKitModelCroppingConfig cropping_config = {};
  cropping_config.struct_size = sizeof(cropping_config);
  cropping_config.input_path = input_path.c_str();
  cropping_config.output_path = output_path.c_str();
  cropping_config.min_x = 1.0;
  cropping_config.max_x = -1.0;
  ColmapKitModelCroppingResult cropping_result = {};
  cropping_result.struct_size = sizeof(cropping_result);
  EXPECT_EQ(ColmapKitRunModelCropping(&cropping_config, &cropping_result),
            COLMAPKIT_STATUS_INVALID_ARGUMENT);
  EXPECT_NE(std::string(cropping_result.message).find("minimum bounds"),
            std::string::npos);

  ColmapKitModelConversionConfig conversion_config = {};
  conversion_config.struct_size = sizeof(conversion_config);
  conversion_config.input_path = input_path.c_str();
  conversion_config.output_path = output_path.c_str();
  conversion_config.output_type = static_cast<ColmapKitModelOutputType>(100);
  ColmapKitModelConversionResult conversion_result = {};
  conversion_result.struct_size = sizeof(conversion_result);
  EXPECT_EQ(ColmapKitRunModelConversion(&conversion_config, &conversion_result),
            COLMAPKIT_STATUS_UNSUPPORTED);
  EXPECT_NE(std::string(conversion_result.message).find("BIN or TXT"),
            std::string::npos);
}

}  // namespace
}  // namespace colmap
