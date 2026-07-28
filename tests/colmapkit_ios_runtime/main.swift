import Darwin
import Foundation
import UIKit
@preconcurrency import ColmapKit

private struct RuntimeResult: Codable {
  var version: String
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

    return RuntimeResult(
      version: version,
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
      fixtureImageCount: fixtureImageCount
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
