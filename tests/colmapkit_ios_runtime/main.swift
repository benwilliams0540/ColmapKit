import Darwin
import Foundation
import simd
import UIKit
@preconcurrency import ColmapKit

private struct RuntimeResult: Codable {
  var version: String
  var abiVersion: UInt32
  var releaseVersion: String
  var engineBuildIdentity: String
  var initializeStatus: UInt32
  var reconstructionStatus: UInt32
  var registeredImages: Int
  var sparsePoints: Int
  var observations: Int
  var meanReprojectionError: Double
  var pointFilteringStatus: UInt32
  var pointFilteringInputPoints: Int
  var pointFilteringOutputPoints: Int
  var pointFilteringOutputReadable: Bool
  var modelCroppingStatus: UInt32
  var modelCroppingInputPoints: Int
  var modelCroppingOutputPoints: Int
  var modelCroppingOutputReadable: Bool
  var modelConversionStatus: UInt32
  var modelConversionOutputPoints: Int
  var modelConversionFilesWritten: Int
  var modelConversionTextReadable: Bool
  var inputModelUnchanged: Bool
  var invalidInputStatus: UInt32
  var invalidInputMessage: String
  var cancellationStatus: UInt32
  var cancellationLatencySeconds: Double
  var fixtureImageCount: Int
  var trackedV2Status: UInt32
  var trackedV2RegisteredImages: Int
  var trackedV2SparsePoints: Int
  var trackedV2NoFallback: Bool
  var trackedV2CancellationStatus: UInt32
  var priorV2Status: UInt32
  var priorV2Variant: UInt32
  var priorV2SHDegree: UInt32
  var priorV2OutputGaussians: Int
  var priorV2DensificationRatio: Double
  var priorV2PoseUnchanged: Bool
  var priorV2PLYValidated: Bool
}

private struct V2RuntimeResult {
  var trackedStatus: ColmapKitStatus
  var registeredImages: Int
  var sparsePoints: Int
  var noFallback: Bool
  var cancellationStatus: ColmapKitStatus
  var priorStatus: ColmapKitStatus
  var variant: UInt32
  var shDegree: UInt32
  var outputGaussians: Int
  var densificationRatio: Double
  var poseUnchanged: Bool
  var plyValidated: Bool
}

private struct PostprocessingRuntimeResult {
  var pointFilteringStatus: ColmapKitStatus
  var pointFilteringInputPoints: Int
  var pointFilteringOutputPoints: Int
  var pointFilteringOutputReadable: Bool
  var modelCroppingStatus: ColmapKitStatus
  var modelCroppingInputPoints: Int
  var modelCroppingOutputPoints: Int
  var modelCroppingOutputReadable: Bool
  var modelConversionStatus: ColmapKitStatus
  var modelConversionOutputPoints: Int
  var modelConversionFilesWritten: Int
  var modelConversionTextReadable: Bool
  var inputModelUnchanged: Bool
  var invalidInputStatus: ColmapKitStatus
  var invalidInputMessage: String
}

private struct ModelPoint {
  var x: Double
  var y: Double
  var z: Double
}

private enum HarnessError: Error, LocalizedError {
  case failed(String)

  var errorDescription: String? {
    switch self {
    case let .failed(message): message
    }
  }
}

private final class HarnessAppDelegate: UIResponder, UIApplicationDelegate {
  func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    DispatchQueue.global(qos: .userInitiated).async {
      do {
        let result = try ColmapKitRuntimeHarness.run()
        let data = try JSONEncoder().encode(result)
        let json = String(decoding: data, as: UTF8.self)
        print("COLMAPKIT_RUNTIME_RESULT=\(json)")
        fflush(stdout)
        exit(EXIT_SUCCESS)
      } catch {
        print("COLMAPKIT_RUNTIME_ERROR=\(error.localizedDescription)")
        fflush(stdout)
        exit(EXIT_FAILURE)
      }
    }
    return true
  }
}

private enum ColmapKitRuntimeHarness {
  static func run() throws -> RuntimeResult {
    let version = String(cString: ColmapKitVersion())
    guard !version.isEmpty else {
      throw HarnessError.failed("ColmapKitVersion returned an empty string.")
    }
    let abiVersion = ColmapKitGetABIVersionV2()
    let releaseVersion = String(cString: ColmapKitGetReleaseVersionV2())
    let engineBuildIdentity = String(cString: ColmapKitGetEngineBuildIdentityV2())
    guard abiVersion == 2, !releaseVersion.isEmpty, !engineBuildIdentity.isEmpty else {
      throw HarnessError.failed("ColmapKit V2 identity exports are invalid.")
    }

    let initializeStatus = "ColmapKitRuntimeHarness".withCString(ColmapKitInitialize)
    guard isSuccess(initializeStatus) else {
      throw HarnessError.failed("ColmapKitInitialize failed with \(initializeStatus.rawValue).")
    }

    guard let fixtureURL = Bundle.main.url(forResource: "Fixture", withExtension: nil) else {
      throw HarnessError.failed("The deterministic fixture is missing from the app bundle.")
    }
    let fixtureImageCount = try FileManager.default.contentsOfDirectory(atPath: fixtureURL.path).count
    guard fixtureImageCount >= 3 else {
      throw HarnessError.failed("The deterministic fixture contains fewer than three images.")
    }

    let runRoot = FileManager.default.temporaryDirectory
      .appendingPathComponent("colmapkit-runtime-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: runRoot, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: runRoot) }

    let completeRoot = runRoot.appendingPathComponent("complete", isDirectory: true)
    let reconstruction = try runReconstruction(
      fixtureURL: fixtureURL,
      rootURL: completeRoot,
      cancelImmediately: false
    )
    guard isSuccess(reconstruction.waitStatus), isSuccess(reconstruction.result.status) else {
      throw HarnessError.failed(
        "Deterministic reconstruction failed: \(message(from: reconstruction.result))"
      )
    }
    guard reconstruction.result.registered_images >= 3,
          reconstruction.result.sparse_points > 0
    else {
      throw HarnessError.failed(
        "Deterministic reconstruction produced only \(reconstruction.result.registered_images) registered images and \(reconstruction.result.sparse_points) points."
      )
    }

    let inputModelURL = completeRoot
      .appendingPathComponent("sparse", isDirectory: true)
      .appendingPathComponent(
        String(reconstruction.result.largest_model_index),
        isDirectory: true
      )
    let postprocessing = try runPostprocessing(
      inputModelURL: inputModelURL,
      rootURL: runRoot.appendingPathComponent("postprocessing", isDirectory: true)
    )

    let cancellation = try runReconstruction(
      fixtureURL: fixtureURL,
      rootURL: runRoot.appendingPathComponent("cancelled", isDirectory: true),
      cancelImmediately: true
    )
    guard isCancelled(cancellation.waitStatus) || isCancelled(cancellation.result.status) else {
      throw HarnessError.failed(
        "Cancellation returned wait=\(cancellation.waitStatus.rawValue), result=\(cancellation.result.status.rawValue)."
      )
    }

    let v2 = try runV2(
      fixtureURL: fixtureURL,
      rootURL: runRoot.appendingPathComponent("v2", isDirectory: true)
    )

    return RuntimeResult(
      version: version,
      abiVersion: abiVersion,
      releaseVersion: releaseVersion,
      engineBuildIdentity: engineBuildIdentity,
      initializeStatus: initializeStatus.rawValue,
      reconstructionStatus: reconstruction.result.status.rawValue,
      registeredImages: Int(reconstruction.result.registered_images),
      sparsePoints: Int(reconstruction.result.sparse_points),
      observations: Int(reconstruction.result.observations),
      meanReprojectionError: reconstruction.result.mean_reprojection_error,
      pointFilteringStatus: postprocessing.pointFilteringStatus.rawValue,
      pointFilteringInputPoints: postprocessing.pointFilteringInputPoints,
      pointFilteringOutputPoints: postprocessing.pointFilteringOutputPoints,
      pointFilteringOutputReadable: postprocessing.pointFilteringOutputReadable,
      modelCroppingStatus: postprocessing.modelCroppingStatus.rawValue,
      modelCroppingInputPoints: postprocessing.modelCroppingInputPoints,
      modelCroppingOutputPoints: postprocessing.modelCroppingOutputPoints,
      modelCroppingOutputReadable: postprocessing.modelCroppingOutputReadable,
      modelConversionStatus: postprocessing.modelConversionStatus.rawValue,
      modelConversionOutputPoints: postprocessing.modelConversionOutputPoints,
      modelConversionFilesWritten: postprocessing.modelConversionFilesWritten,
      modelConversionTextReadable: postprocessing.modelConversionTextReadable,
      inputModelUnchanged: postprocessing.inputModelUnchanged,
      invalidInputStatus: postprocessing.invalidInputStatus.rawValue,
      invalidInputMessage: postprocessing.invalidInputMessage,
      cancellationStatus: cancellation.result.status.rawValue,
      cancellationLatencySeconds: cancellation.cancellationLatencySeconds,
      fixtureImageCount: fixtureImageCount,
      trackedV2Status: v2.trackedStatus.rawValue,
      trackedV2RegisteredImages: v2.registeredImages,
      trackedV2SparsePoints: v2.sparsePoints,
      trackedV2NoFallback: v2.noFallback,
      trackedV2CancellationStatus: v2.cancellationStatus.rawValue,
      priorV2Status: v2.priorStatus.rawValue,
      priorV2Variant: v2.variant,
      priorV2SHDegree: v2.shDegree,
      priorV2OutputGaussians: v2.outputGaussians,
      priorV2DensificationRatio: v2.densificationRatio,
      priorV2PoseUnchanged: v2.poseUnchanged,
      priorV2PLYValidated: v2.plyValidated
    )
  }

  private static func runV2(fixtureURL: URL, rootURL: URL) throws -> V2RuntimeResult {
    let fileManager = FileManager.default
    try fileManager.createDirectory(at: rootURL, withIntermediateDirectories: true)
    let imageURLs = try fileManager.contentsOfDirectory(
      at: fixtureURL,
      includingPropertiesForKeys: nil
    ).filter { $0.pathExtension.lowercased() == "pgm" }.sorted {
      $0.lastPathComponent < $1.lastPathComponent
    }
    guard imageURLs.count == 8 else {
      throw HarnessError.failed("V2 Simulator fixture must contain exactly eight PGM images.")
    }

    var allocatedStrings: [UnsafeMutablePointer<CChar>] = []
    func cString(_ value: String) throws -> UnsafePointer<CChar> {
      guard let pointer = strdup(value) else {
        throw HarnessError.failed("Could not allocate C string for \(value).")
      }
      allocatedStrings.append(pointer)
      return UnsafePointer(pointer)
    }
    defer { allocatedStrings.forEach { free($0) } }

    var images: [ColmapKitTrackedImageV2] = []
    for (index, imageURL) in imageURLs.enumerated() {
      var image = ColmapKitTrackedImageV2()
      image.struct_size = UInt32(MemoryLayout<ColmapKitTrackedImageV2>.size)
      image.camera_model = COLMAPKIT_CAMERA_MODEL_V2_SIMPLE_PINHOLE.rawValue
      image.stable_id = UInt64(1_000 + index)
      image.order_index = UInt32(index)
      image.encoded_width = 1_024
      image.encoded_height = 768
      image.num_camera_params = 3
      withUnsafeMutableBytes(of: &image.camera_params) { bytes in
        let values = bytes.bindMemory(to: Double.self)
        values[0] = 900
        values[1] = 512
        values[2] = 384
      }
      let transform = arkitWorldFromCamera(index: index, count: imageURLs.count)
      withUnsafeMutableBytes(of: &image.world_from_camera) { bytes in
        let values = bytes.bindMemory(to: Double.self)
        for offset in transform.indices { values[offset] = transform[offset] }
      }
      image.tracking_state = COLMAPKIT_TRACKING_STATE_V2_NORMAL.rawValue
      image.inclusion_flags = UInt32(COLMAPKIT_TRACKED_IMAGE_FLAG_V2_INCLUDED)
      image.translation_weight = 1
      image.rotation_weight = 1
      image.image_path = try cString(imageURL.path)
      images.append(image)
    }

    let databaseURL = rootURL.appendingPathComponent("database.db")
    let modelURL = rootURL.appendingPathComponent("sparse", isDirectory: true)
    let poseURL = rootURL.appendingPathComponent("refined-poses.json")
    let trackedEvidenceURL = rootURL.appendingPathComponent("tracked-evidence.json")
    var trackedConfig = ColmapKitTrackedPoseConfigV2()
    trackedConfig.struct_size = UInt32(MemoryLayout<ColmapKitTrackedPoseConfigV2>.size)
    trackedConfig.num_images = UInt32(images.count)
    trackedConfig.max_features_per_image = 4_096
    trackedConfig.temporal_neighbor_count = 3
    trackedConfig.max_revisit_neighbors_per_image = 2
    trackedConfig.max_image_pairs = 28
    trackedConfig.max_triangulation_passes = 2
    trackedConfig.max_bundle_adjustment_iterations = 40
    trackedConfig.random_seed = 7
    trackedConfig.num_threads = 1
    trackedConfig.revisit_min_translation_meters = 0.1
    trackedConfig.revisit_max_translation_meters = 2
    trackedConfig.revisit_max_rotation_degrees = 45
    trackedConfig.min_triangulation_angle_degrees = 0.1
    trackedConfig.max_reprojection_error_pixels = 4
    trackedConfig.max_allowed_scale_drift_ratio = 1e-9
    trackedConfig.database_path = try cString(databaseURL.path)
    trackedConfig.output_model_path = try cString(modelURL.path)
    trackedConfig.refined_pose_path = try cString(poseURL.path)
    trackedConfig.evidence_path = try cString(trackedEvidenceURL.path)

    var trackedJob: OpaquePointer?
    let trackedStart = images.withUnsafeBufferPointer { buffer in
      trackedConfig.images = buffer.baseAddress
      return ColmapKitStartTrackedPoseReconstructionV2(&trackedConfig, &trackedJob)
    }
    guard isSuccess(trackedStart), let trackedJob else {
      throw HarnessError.failed("V2 tracked start failed with \(trackedStart.rawValue).")
    }
    defer { ColmapKitReleaseTrackedPoseReconstructionJobV2(trackedJob) }
    var trackedResult = ColmapKitTrackedPoseResultV2()
    trackedResult.struct_size = UInt32(MemoryLayout<ColmapKitTrackedPoseResultV2>.size)
    let trackedWait = ColmapKitWaitTrackedPoseReconstructionV2(trackedJob, &trackedResult)
    guard isSuccess(trackedWait),
          trackedResult.status == COLMAPKIT_STATUS_OK.rawValue,
          Int(trackedResult.registered_images) == images.count,
          trackedResult.sparse_points > 0
    else {
      throw HarnessError.failed("V2 tracked reconstruction failed: \(message(from: trackedResult))")
    }
    let trackedEvidence = try jsonDictionary(at: trackedEvidenceURL)
    let noFallback = trackedEvidence["route"] as? String == "tracked_pose_bounded_v2"
      && trackedEvidence["fallback_used"] as? Bool == false
      && trackedEvidence["arkit_world_frame_preserved"] as? Bool == true
      && trackedEvidence["arkit_metric_scale_preserved"] as? Bool == true
    guard noFallback else {
      throw HarnessError.failed("V2 tracked evidence did not prove the bounded no-fallback route.")
    }

    let cancellationRoot = rootURL.appendingPathComponent("cancelled", isDirectory: true)
    try fileManager.createDirectory(at: cancellationRoot, withIntermediateDirectories: true)
    var cancelConfig = trackedConfig
    cancelConfig.database_path = try cString(cancellationRoot.appendingPathComponent("database.db").path)
    cancelConfig.output_model_path = try cString(cancellationRoot.appendingPathComponent("sparse").path)
    cancelConfig.refined_pose_path = try cString(cancellationRoot.appendingPathComponent("poses.json").path)
    cancelConfig.evidence_path = try cString(cancellationRoot.appendingPathComponent("evidence.json").path)
    var cancelJob: OpaquePointer?
    let cancelStart = images.withUnsafeBufferPointer { buffer in
      cancelConfig.images = buffer.baseAddress
      return ColmapKitStartTrackedPoseReconstructionV2(&cancelConfig, &cancelJob)
    }
    guard isSuccess(cancelStart), let cancelJob else {
      throw HarnessError.failed("V2 cancellation job failed to start.")
    }
    defer { ColmapKitReleaseTrackedPoseReconstructionJobV2(cancelJob) }
    guard isSuccess(ColmapKitCancelTrackedPoseReconstructionV2(cancelJob)) else {
      throw HarnessError.failed("V2 cancellation request failed.")
    }
    var cancelResult = ColmapKitTrackedPoseResultV2()
    cancelResult.struct_size = UInt32(MemoryLayout<ColmapKitTrackedPoseResultV2>.size)
    let cancelWait = ColmapKitWaitTrackedPoseReconstructionV2(cancelJob, &cancelResult)
    guard isCancelled(cancelWait), cancelResult.status == COLMAPKIT_STATUS_CANCELLED.rawValue else {
      throw HarnessError.failed("V2 cancellation did not return CANCELLED.")
    }

    let poseBefore = try Data(contentsOf: poseURL)
    let poseSHA = fixedCString(&trackedResult.refined_pose_sha256)
    let plyURL = rootURL.appendingPathComponent("init.ply")
    let priorEvidenceURL = rootURL.appendingPathComponent("prior-evidence.json")
    var priorConfig = ColmapKitRGBPriorConfigV2()
    priorConfig.struct_size = UInt32(MemoryLayout<ColmapKitRGBPriorConfigV2>.size)
    priorConfig.num_images = UInt32(images.count)
    priorConfig.normal_neighbor_count = 12
    priorConfig.max_points_per_spatial_cell = 96
    priorConfig.max_output_gaussians = 12_000
    priorConfig.minimum_densification_percent = 10
    priorConfig.random_seed = 7
    priorConfig.spatial_cell_size_meters = 0.08
    priorConfig.min_spacing_meters = 0.001
    priorConfig.max_spacing_meters = 0.25
    priorConfig.tangent_scale_multiplier = 0.8
    priorConfig.normal_scale_multiplier = 0.2
    priorConfig.initial_opacity = 0.1
    priorConfig.database_path = try cString(databaseURL.path)
    priorConfig.refined_model_path = try cString(modelURL.path)
    priorConfig.refined_pose_path = try cString(poseURL.path)
    priorConfig.expected_refined_pose_sha256 = try cString(poseSHA)
    priorConfig.output_ply_path = try cString(plyURL.path)
    priorConfig.evidence_path = try cString(priorEvidenceURL.path)
    var priorResult = ColmapKitRGBPriorResultV2()
    priorResult.struct_size = UInt32(MemoryLayout<ColmapKitRGBPriorResultV2>.size)
    let priorStatus = images.withUnsafeBufferPointer { buffer in
      priorConfig.images = buffer.baseAddress
      return ColmapKitRunRGBGaussianPriorV2(&priorConfig, &priorResult)
    }
    guard isSuccess(priorStatus),
          priorResult.status == COLMAPKIT_STATUS_OK.rawValue,
          priorResult.variant == UInt32(COLMAPKIT_RGB_PRIOR_VARIANT_V2_D),
          priorResult.sh_degree == 0,
          priorResult.densification_ratio >= 1.1,
          priorResult.output_gaussians > trackedResult.sparse_points
    else {
      throw HarnessError.failed("V2 prior failed: \(message(from: priorResult))")
    }
    let poseUnchanged = poseBefore == (try Data(contentsOf: poseURL))
    guard poseUnchanged else {
      throw HarnessError.failed("V2 prior modified the frozen pose artifact.")
    }
    let priorEvidence = try jsonDictionary(at: priorEvidenceURL)
    guard priorEvidence["variant"] as? String == "D",
          priorEvidence["depth_used"] as? Bool == false,
          priorEvidence["poses_frozen"] as? Bool == true,
          priorEvidence["input_pose_sha256"] as? String == poseSHA,
          priorEvidence["output_pose_sha256"] as? String == poseSHA
    else {
      throw HarnessError.failed("V2 prior evidence is incomplete.")
    }
    let plyValidated = try validateGaussianPLY(
      at: plyURL,
      expectedVertices: Int(priorResult.output_gaussians)
    )
    guard plyValidated else {
      throw HarnessError.failed("V2 Gaussian PLY validation failed.")
    }

    return V2RuntimeResult(
      trackedStatus: trackedWait,
      registeredImages: Int(trackedResult.registered_images),
      sparsePoints: Int(trackedResult.sparse_points),
      noFallback: noFallback,
      cancellationStatus: cancelWait,
      priorStatus: priorStatus,
      variant: priorResult.variant,
      shDegree: priorResult.sh_degree,
      outputGaussians: Int(priorResult.output_gaussians),
      densificationRatio: priorResult.densification_ratio,
      poseUnchanged: poseUnchanged,
      plyValidated: plyValidated
    )
  }

  private static func runPostprocessing(
    inputModelURL: URL,
    rootURL: URL
  ) throws -> PostprocessingRuntimeResult {
    let fileManager = FileManager.default
    try fileManager.createDirectory(at: rootURL, withIntermediateDirectories: true)
    let inputSnapshot = try snapshotModel(at: inputModelURL)

    let conversionURL = rootURL.appendingPathComponent("converted-text", isDirectory: true)
    try fileManager.createDirectory(at: conversionURL, withIntermediateDirectories: true)
    let conversion = runModelConversion(
      inputURL: inputModelURL,
      outputURL: conversionURL,
      outputType: COLMAPKIT_MODEL_OUTPUT_TYPE_TXT
    )
    guard isSuccess(conversion.status), isSuccess(conversion.result.status) else {
      throw HarnessError.failed(
        "Model conversion failed: \(message(from: conversion.result))"
      )
    }
    let convertedPoints = try readTextPoints(from: conversionURL)
    guard conversion.result.output_points == convertedPoints.count,
          conversion.result.files_written >= 3
    else {
      throw HarnessError.failed(
        "Model conversion output counters do not match the readable text model."
      )
    }

    let filteringURL = rootURL.appendingPathComponent("filtered", isDirectory: true)
    try fileManager.createDirectory(at: filteringURL, withIntermediateDirectories: true)
    let filtering = runPointFiltering(inputURL: inputModelURL, outputURL: filteringURL)
    guard isSuccess(filtering.status),
          isSuccess(filtering.result.status),
          filtering.result.input_points == convertedPoints.count,
          filtering.result.output_points > 0
    else {
      throw HarnessError.failed(
        "Point filtering failed: \(message(from: filtering.result))"
      )
    }
    let filteringReadableURL = rootURL
      .appendingPathComponent("filtered-readable", isDirectory: true)
    try fileManager.createDirectory(
      at: filteringReadableURL,
      withIntermediateDirectories: true
    )
    let filteringReadback = runModelConversion(
      inputURL: filteringURL,
      outputURL: filteringReadableURL,
      outputType: COLMAPKIT_MODEL_OUTPUT_TYPE_TXT
    )
    let filteredPoints = try readTextPoints(from: filteringReadableURL)
    let filteringOutputReadable =
      isSuccess(filteringReadback.status)
      && isSuccess(filteringReadback.result.status)
      && filteredPoints.count == filtering.result.output_points
    guard filteringOutputReadable else {
      throw HarnessError.failed(
        "Point filtering output could not be read back through model conversion."
      )
    }

    let sortedX = convertedPoints.map(\.x).sorted()
    guard let minimumX = sortedX.first,
          let maximumX = sortedX.last,
          minimumX < maximumX
    else {
      throw HarnessError.failed("Converted fixture does not have a usable X extent.")
    }
    let minimumY = convertedPoints.map(\.y).min()!
    let maximumY = convertedPoints.map(\.y).max()!
    let minimumZ = convertedPoints.map(\.z).min()!
    let maximumZ = convertedPoints.map(\.z).max()!
    let xPadding = max((maximumX - minimumX) * 0.01, 1e-9)
    let yPadding = max((maximumY - minimumY) * 0.01, 1e-9)
    let zPadding = max((maximumZ - minimumZ) * 0.01, 1e-9)

    let croppingURL = rootURL.appendingPathComponent("cropped", isDirectory: true)
    try fileManager.createDirectory(at: croppingURL, withIntermediateDirectories: true)
    let cropping = runModelCropping(
      inputURL: inputModelURL,
      outputURL: croppingURL,
      minX: minimumX - xPadding,
      minY: minimumY - yPadding,
      minZ: minimumZ - zPadding,
      maxX: sortedX[sortedX.count / 2],
      maxY: maximumY + yPadding,
      maxZ: maximumZ + zPadding
    )
    guard isSuccess(cropping.status),
          isSuccess(cropping.result.status),
          cropping.result.input_points == convertedPoints.count,
          cropping.result.output_points > 0,
          cropping.result.output_points < cropping.result.input_points
    else {
      throw HarnessError.failed(
        "Model cropping failed to produce a non-empty bounded subset: \(message(from: cropping.result))"
      )
    }
    let croppingReadableURL = rootURL
      .appendingPathComponent("cropped-readable", isDirectory: true)
    try fileManager.createDirectory(
      at: croppingReadableURL,
      withIntermediateDirectories: true
    )
    let croppingReadback = runModelConversion(
      inputURL: croppingURL,
      outputURL: croppingReadableURL,
      outputType: COLMAPKIT_MODEL_OUTPUT_TYPE_TXT
    )
    let croppedPoints = try readTextPoints(from: croppingReadableURL)
    let croppingOutputReadable =
      isSuccess(croppingReadback.status)
      && isSuccess(croppingReadback.result.status)
      && croppedPoints.count == cropping.result.output_points
    guard croppingOutputReadable else {
      throw HarnessError.failed(
        "Model cropping output could not be read back through model conversion."
      )
    }

    let invalidOutputURL = rootURL
      .appendingPathComponent("invalid-output", isDirectory: true)
    try fileManager.createDirectory(at: invalidOutputURL, withIntermediateDirectories: true)
    let invalidInputURL = rootURL.appendingPathComponent("missing-model", isDirectory: true)
    let invalid = runPointFiltering(inputURL: invalidInputURL, outputURL: invalidOutputURL)
    let invalidInputMessage = message(from: invalid.result)
    guard invalid.status.rawValue == COLMAPKIT_STATUS_INVALID_ARGUMENT.rawValue,
          invalid.result.status.rawValue == COLMAPKIT_STATUS_INVALID_ARGUMENT.rawValue,
          invalidInputMessage.contains("input_path")
    else {
      throw HarnessError.failed(
        "Invalid point-filtering input did not return an authoritative error."
      )
    }

    let inputModelUnchanged = inputSnapshot == (try snapshotModel(at: inputModelURL))
    guard inputModelUnchanged else {
      throw HarnessError.failed("A post-processing operation modified the input model.")
    }

    return PostprocessingRuntimeResult(
      pointFilteringStatus: filtering.status,
      pointFilteringInputPoints: Int(filtering.result.input_points),
      pointFilteringOutputPoints: Int(filtering.result.output_points),
      pointFilteringOutputReadable: filteringOutputReadable,
      modelCroppingStatus: cropping.status,
      modelCroppingInputPoints: Int(cropping.result.input_points),
      modelCroppingOutputPoints: Int(cropping.result.output_points),
      modelCroppingOutputReadable: croppingOutputReadable,
      modelConversionStatus: conversion.status,
      modelConversionOutputPoints: Int(conversion.result.output_points),
      modelConversionFilesWritten: Int(conversion.result.files_written),
      modelConversionTextReadable: !convertedPoints.isEmpty,
      inputModelUnchanged: inputModelUnchanged,
      invalidInputStatus: invalid.status,
      invalidInputMessage: invalidInputMessage
    )
  }

  private static func runPointFiltering(
    inputURL: URL,
    outputURL: URL
  ) -> (status: ColmapKitStatus, result: ColmapKitPointFilteringResult) {
    var result = ColmapKitPointFilteringResult()
    result.struct_size = MemoryLayout<ColmapKitPointFilteringResult>.size
    let status = inputURL.path.withCString { inputPath in
      outputURL.path.withCString { outputPath in
        var config = ColmapKitPointFilteringConfig()
        config.struct_size = MemoryLayout<ColmapKitPointFilteringConfig>.size
        config.input_path = inputPath
        config.output_path = outputPath
        config.min_track_len = 0
        config.max_reproj_error = 1_000_000
        config.min_tri_angle = 0
        return ColmapKitRunPointFiltering(&config, &result)
      }
    }
    return (status, result)
  }

  private static func runModelCropping(
    inputURL: URL,
    outputURL: URL,
    minX: Double,
    minY: Double,
    minZ: Double,
    maxX: Double,
    maxY: Double,
    maxZ: Double
  ) -> (status: ColmapKitStatus, result: ColmapKitModelCroppingResult) {
    var result = ColmapKitModelCroppingResult()
    result.struct_size = MemoryLayout<ColmapKitModelCroppingResult>.size
    let status = inputURL.path.withCString { inputPath in
      outputURL.path.withCString { outputPath in
        var config = ColmapKitModelCroppingConfig()
        config.struct_size = MemoryLayout<ColmapKitModelCroppingConfig>.size
        config.input_path = inputPath
        config.output_path = outputPath
        config.min_x = minX
        config.min_y = minY
        config.min_z = minZ
        config.max_x = maxX
        config.max_y = maxY
        config.max_z = maxZ
        return ColmapKitRunModelCropping(&config, &result)
      }
    }
    return (status, result)
  }

  private static func runModelConversion(
    inputURL: URL,
    outputURL: URL,
    outputType: ColmapKitModelOutputType
  ) -> (status: ColmapKitStatus, result: ColmapKitModelConversionResult) {
    var result = ColmapKitModelConversionResult()
    result.struct_size = MemoryLayout<ColmapKitModelConversionResult>.size
    let status = inputURL.path.withCString { inputPath in
      outputURL.path.withCString { outputPath in
        var config = ColmapKitModelConversionConfig()
        config.struct_size = MemoryLayout<ColmapKitModelConversionConfig>.size
        config.input_path = inputPath
        config.output_path = outputPath
        config.output_type = outputType
        return ColmapKitRunModelConversion(&config, &result)
      }
    }
    return (status, result)
  }

  private static func readTextPoints(from modelURL: URL) throws -> [ModelPoint] {
    let camerasURL = modelURL.appendingPathComponent("cameras.txt")
    let imagesURL = modelURL.appendingPathComponent("images.txt")
    let pointsURL = modelURL.appendingPathComponent("points3D.txt")
    for requiredURL in [camerasURL, imagesURL, pointsURL] {
      guard FileManager.default.fileExists(atPath: requiredURL.path) else {
        throw HarnessError.failed("Text model is missing \(requiredURL.lastPathComponent).")
      }
    }

    let contents = try String(contentsOf: pointsURL, encoding: .utf8)
    var points: [ModelPoint] = []
    for line in contents.split(whereSeparator: \.isNewline) {
      let fields = line.split(whereSeparator: \.isWhitespace)
      if fields.isEmpty || fields[0].hasPrefix("#") {
        continue
      }
      guard fields.count >= 4,
            let x = Double(fields[1]),
            let y = Double(fields[2]),
            let z = Double(fields[3])
      else {
        throw HarnessError.failed("Text model contains an unreadable points3D row.")
      }
      points.append(ModelPoint(x: x, y: y, z: z))
    }
    guard !points.isEmpty else {
      throw HarnessError.failed("Text model contains no readable 3D points.")
    }
    return points
  }

  private static func snapshotModel(at modelURL: URL) throws -> [String: Data] {
    let fileURLs = try FileManager.default.contentsOfDirectory(
      at: modelURL,
      includingPropertiesForKeys: [.isRegularFileKey]
    )
    var snapshot: [String: Data] = [:]
    for fileURL in fileURLs {
      let values = try fileURL.resourceValues(forKeys: [.isRegularFileKey])
      if values.isRegularFile == true {
        snapshot[fileURL.lastPathComponent] = try Data(contentsOf: fileURL)
      }
    }
    for requiredName in ["cameras.bin", "images.bin", "points3D.bin"] {
      guard snapshot[requiredName] != nil else {
        throw HarnessError.failed("Binary model is missing \(requiredName).")
      }
    }
    return snapshot
  }

  private static func arkitWorldFromCamera(index: Int, count: Int) -> [Double] {
    let fraction = count == 1 ? 0 : Double(index) / Double(count - 1)
    let center = SIMD3<Double>(
      -0.65 + 1.3 * fraction,
      0.04 * sin(Double(index)),
      0
    )
    let target = SIMD3<Double>(0, 0, 4.6)
    let z = simd_normalize(target - center)
    let x = simd_normalize(simd_cross(z, SIMD3<Double>(0, 1, 0)))
    let y = simd_cross(z, x)
    // world_from_arkit_camera = world_from_colmap_camera * diag(1,-1,-1,1)
    return [
      x.x, x.y, x.z, 0,
      -y.x, -y.y, -y.z, 0,
      -z.x, -z.y, -z.z, 0,
      center.x, center.y, center.z, 1,
    ]
  }

  private static func jsonDictionary(at url: URL) throws -> [String: Any] {
    let object = try JSONSerialization.jsonObject(with: Data(contentsOf: url))
    guard let dictionary = object as? [String: Any] else {
      throw HarnessError.failed("Expected JSON object at \(url.lastPathComponent).")
    }
    return dictionary
  }

  private static func fixedCString<T>(_ value: inout T) -> String {
    withUnsafePointer(to: &value) { pointer in
      pointer.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) {
        String(cString: $0)
      }
    }
  }

  private static func validateGaussianPLY(
    at url: URL,
    expectedVertices: Int
  ) throws -> Bool {
    let data = try Data(contentsOf: url)
    let terminator = Data("end_header\n".utf8)
    guard let terminatorRange = data.range(of: terminator) else { return false }
    let headerEnd = terminatorRange.upperBound
    guard let header = String(data: data[..<headerEnd], encoding: .ascii) else {
      return false
    }
    let expectedProperties = [
      "x", "y", "z", "scale_0", "scale_1", "scale_2", "opacity",
      "rot_0", "rot_1", "rot_2", "rot_3", "f_dc_0", "f_dc_1", "f_dc_2",
    ]
    let properties = header.split(whereSeparator: \.isNewline).compactMap { line -> String? in
      let fields = line.split(whereSeparator: \.isWhitespace)
      guard fields.count == 3, fields[0] == "property", fields[1] == "float" else {
        return nil
      }
      return String(fields[2])
    }
    guard header.hasPrefix("ply\nformat binary_little_endian 1.0\n"),
          header.contains("comment sh_degree 0\n"),
          header.contains("element vertex \(expectedVertices)\n"),
          properties == expectedProperties,
          data.count - headerEnd == expectedVertices * 14 * MemoryLayout<Float>.size
    else { return false }

    func float(at byteOffset: Int) -> Float {
      let bits = data.withUnsafeBytes { bytes in
        bytes.loadUnaligned(fromByteOffset: byteOffset, as: UInt32.self)
      }
      return Float(bitPattern: UInt32(littleEndian: bits))
    }
    let shC0 = 0.28209479177387814
    for vertex in 0..<expectedVertices {
      let base = headerEnd + vertex * 14 * MemoryLayout<Float>.size
      let fields = (0..<14).map { Double(float(at: base + $0 * 4)) }
      guard fields.allSatisfy(\.isFinite),
            exp(fields[3]) > exp(fields[5]),
            exp(fields[4]) > exp(fields[5])
      else { return false }
      let quaternionNorm = sqrt(fields[7...10].reduce(0) { $0 + $1 * $1 })
      guard abs(quaternionNorm - 1) < 1e-4 else { return false }
      let alpha = 1 / (1 + exp(-fields[6]))
      guard alpha > 0, alpha < 1 else { return false }
      let rgb = fields[11...13].map { $0 * shC0 + 0.5 }
      guard rgb.allSatisfy({ $0 >= -1e-4 && $0 <= 1.0001 }) else { return false }
    }
    return true
  }

  private static func runReconstruction(
    fixtureURL: URL,
    rootURL: URL,
    cancelImmediately: Bool
  ) throws -> (
    waitStatus: ColmapKitStatus,
    result: ColmapKitSparseReconstructionResult,
    cancellationLatencySeconds: Double
  ) {
    let databaseURL = rootURL.appendingPathComponent("database.db")
    let sparseURL = rootURL.appendingPathComponent("sparse", isDirectory: true)
    let sparseTextURL = rootURL.appendingPathComponent("sparse-text", isDirectory: true)
    try FileManager.default.createDirectory(at: rootURL, withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: sparseURL, withIntermediateDirectories: true)
    try FileManager.default.createDirectory(at: sparseTextURL, withIntermediateDirectories: true)

    var allocatedStrings: [UnsafeMutablePointer<CChar>] = []
    func cString(_ value: String) throws -> UnsafePointer<CChar> {
      guard let pointer = strdup(value) else {
        throw HarnessError.failed("Could not allocate C string for \(value).")
      }
      allocatedStrings.append(pointer)
      return UnsafePointer(pointer)
    }
    defer { allocatedStrings.forEach { free($0) } }

    var config = ColmapKitSparseReconstructionConfig(
      struct_size: MemoryLayout<ColmapKitSparseReconstructionConfig>.size,
      database_path: try cString(databaseURL.path),
      image_path: try cString(fixtureURL.path),
      output_path: try cString(sparseURL.path),
      sparse_text_output_path: try cString(sparseTextURL.path),
      image_list_path: nil,
      camera_model: try cString("SIMPLE_PINHOLE"),
      camera_params: try cString("900,512,384"),
      single_camera: 1,
      max_image_size: 1024,
      num_threads: 1,
      use_gpu: 0,
      use_metal_sift: 0,
      use_metal_matching: 0,
      estimate_affine_shape: 0,
      domain_size_pooling: 0,
      matcher: COLMAPKIT_MATCHER_SEQUENTIAL,
      sequential_overlap: 4,
      mapper_min_num_matches: 15,
      mapper_min_model_size: 3,
      mapper_random_seed: 0,
      write_sparse_text: 1,
      progress_callback: nil,
      progress_user_data: nil
    )
    var jobPointer: OpaquePointer?
    let startStatus = ColmapKitStartSparseReconstruction(&config, &jobPointer)
    guard isSuccess(startStatus), let jobPointer else {
      throw HarnessError.failed("ColmapKitStartSparseReconstruction failed with \(startStatus.rawValue).")
    }
    defer { ColmapKitReleaseSparseReconstructionJob(jobPointer) }

    var cancellationStart: ContinuousClock.Instant?
    if cancelImmediately {
      cancellationStart = .now
      let cancelStatus = ColmapKitCancelSparseReconstruction(jobPointer)
      guard isSuccess(cancelStatus) else {
        throw HarnessError.failed("ColmapKitCancelSparseReconstruction failed with \(cancelStatus.rawValue).")
      }
    }

    var result = ColmapKitSparseReconstructionResult()
    result.struct_size = MemoryLayout<ColmapKitSparseReconstructionResult>.size
    let waitStatus = ColmapKitWaitSparseReconstruction(jobPointer, &result)
    let cancellationLatencySeconds: Double
    if let cancellationStart {
      let duration = cancellationStart.duration(to: .now)
      cancellationLatencySeconds = Double(duration.components.seconds)
        + Double(duration.components.attoseconds) / 1_000_000_000_000_000_000
    } else {
      cancellationLatencySeconds = 0
    }
    return (waitStatus, result, cancellationLatencySeconds)
  }

  private static func message(from result: ColmapKitSparseReconstructionResult) -> String {
    var result = result
    return withUnsafePointer(to: &result.message) { pointer in
      pointer.withMemoryRebound(to: CChar.self, capacity: Int(COLMAPKIT_MESSAGE_CAPACITY)) {
        String(cString: $0)
      }
    }
  }

  private static func message(from result: ColmapKitPointFilteringResult) -> String {
    var result = result
    return withUnsafePointer(to: &result.message) { pointer in
      pointer.withMemoryRebound(to: CChar.self, capacity: Int(COLMAPKIT_MESSAGE_CAPACITY)) {
        String(cString: $0)
      }
    }
  }

  private static func message(from result: ColmapKitModelCroppingResult) -> String {
    var result = result
    return withUnsafePointer(to: &result.message) { pointer in
      pointer.withMemoryRebound(to: CChar.self, capacity: Int(COLMAPKIT_MESSAGE_CAPACITY)) {
        String(cString: $0)
      }
    }
  }

  private static func message(from result: ColmapKitModelConversionResult) -> String {
    var result = result
    return withUnsafePointer(to: &result.message) { pointer in
      pointer.withMemoryRebound(to: CChar.self, capacity: Int(COLMAPKIT_MESSAGE_CAPACITY)) {
        String(cString: $0)
      }
    }
  }

  private static func message(from result: ColmapKitTrackedPoseResultV2) -> String {
    var result = result
    return fixedCString(&result.message)
  }

  private static func message(from result: ColmapKitRGBPriorResultV2) -> String {
    var result = result
    return fixedCString(&result.message)
  }

  private static func isSuccess(_ status: ColmapKitStatus) -> Bool {
    status.rawValue == COLMAPKIT_STATUS_OK.rawValue
  }

  private static func isCancelled(_ status: ColmapKitStatus) -> Bool {
    status.rawValue == COLMAPKIT_STATUS_CANCELLED.rawValue
  }
}

UIApplicationMain(
  CommandLine.argc,
  CommandLine.unsafeArgv,
  nil,
  NSStringFromClass(HarnessAppDelegate.self)
)
