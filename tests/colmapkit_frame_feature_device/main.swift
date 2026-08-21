import CryptoKit
import Darwin
import Foundation
import SQLite3
import UIKit
@preconcurrency import ColmapKit

private let expectedRelease = "0.3.0-rc.3+14eadc5c"
private let expectedEngineCommit = "14eadc5c"
private let admissionBudgetBytes: UInt64 = 320 * 1024 * 1024
private let maximumFixtureAdmissionEstimateBytes: UInt64 = 325_534_076

private struct FixtureManifest: Codable {
  struct Frame: Codable {
    var filename: String
    var sha256: String
    var bytes: Int
    var width: Int
    var height: Int
    var stableFrameID: UInt64
    var frameRevision: UInt64
    var imageName: String
    var seed: Int
  }

  var schemaVersion: Int
  var generator: String
  var encoder: String
  var cameraModel: String
  var cameraParameters: [Double]
  var frames: [Frame]
}

private struct ResourceSample: Codable {
  var elapsedSeconds: Double
  var residentBytes: UInt64
  var thermal: String
  var batteryLevel: Float
  var batteryState: String
  var lowPowerMode: Bool
}

private struct DeviceBoundary: Codable {
  var name: String
  var model: String
  var systemName: String
  var systemVersion: String
  var operatingSystemVersion: String
  var availableCapacityBytes: Int64
  var thermal: String
  var batteryLevel: Float
  var batteryState: String
  var lowPowerMode: Bool
}

private struct DatabaseSummary: Codable {
  var cameras: Int64
  var images: Int64
  var keypointRows: Int64
  var keypoints: Int64
  var descriptorRows: Int64
  var descriptors: Int64
  var matches: Int64
  var twoViewGeometries: Int64
  var imageIDs: [Int64]
  var cameraIDs: [Int64]
}

private struct ExtractionRequestReceipt: Codable {
  var requestedBackend: UInt32
  var workerCount: UInt32
  var maxEncodedImageBytes: UInt64
  var memoryAdmissionBudgetBytes: UInt64
  var maximumFixtureAdmissionEstimateBytes: UInt64
  var maxImageSize: UInt32
  var maxNumFeatures: UInt32
  var firstOctave: Int32
  var numOctaves: UInt32
  var octaveResolution: UInt32
  var maxNumOrientations: UInt32
  var upright: Bool
  var normalization: UInt32
  var peakThreshold: Double
  var edgeThreshold: Double
}

private struct HarnessPreflightReceipt: Codable {
  var schemaVersion: Int
  var releaseVersion: String
  var engineBuildIdentity: String
  var fixtureManifestSHA256: String
  var request: ExtractionRequestReceipt
  var device: DeviceBoundary
  var residentBytes: UInt64
  var profileSHA256: String?
  var profileAvailability: String
}

private struct HarnessFailureReceipt: Codable {
  var schemaVersion: Int
  var message: String
  var releaseVersion: String
  var engineBuildIdentity: String
  var device: DeviceBoundary
  var residentBytes: UInt64
  var request: ExtractionRequestReceipt
  var profileSHA256: String?
  var profileAvailability: String
}

private struct ExtractionReceipt: Codable {
  var label: String
  var status: UInt32
  var actualBackend: UInt32
  var noFallback: Bool
  var effectiveWorkers: UInt32
  var stableFrameID: UInt64
  var frameRevision: UInt64
  var featureCount: UInt64
  var descriptorBytes: UInt64
  var admittedMemoryBytes: UInt64
  var peakResidentMemoryBytes: UInt64
  var totalSeconds: Double
  var imageSHA256: String
  var metadataSHA256: String
  var profileSHA256: String
  var payloadSHA256: String
  var artifactSHA256: String
  var sourceIdentity: String
  var artifactPath: String
}

private struct ImportReceipt: Codable {
  var label: String
  var status: UInt32
  var noFallback: Bool
  var effectiveWorkers: UInt32
  var importedItems: UInt64
  var importedCameras: UInt64
  var importedKeypoints: UInt64
  var importedDescriptorBytes: UInt64
  var admittedMemoryBytes: UInt64
  var peakResidentMemoryBytes: UInt64
  var totalSeconds: Double
  var sealedSetSHA256: String
  var databaseSHA256: String
  var receiptSHA256: String
  var profileSHA256: String
  var sourceIdentity: String
  var outputPath: String
  var database: DatabaseSummary
}

private struct FailureReceipt: Codable {
  var label: String
  var status: UInt32
  var message: String
  var outputAbsent: Bool
  var foreignOutputPreserved: Bool
}

private struct DeviceResult: Codable {
  var version: String
  var abiVersion: UInt32
  var releaseVersion: String
  var engineBuildIdentity: String
  var fixtureManifestSHA256: String
  var fixtures: [FixtureManifest.Frame]
  var request: ExtractionRequestReceipt
  var deviceStart: DeviceBoundary
  var deviceEnd: DeviceBoundary
  var extractions: [ExtractionReceipt]
  var imports: [ImportReceipt]
  var failures: [FailureReceipt]
  var repeatArtifactBytesIdentical: Bool
  var repeatReceiptBytesIdentical: Bool
  var repeatDatabaseIdentityIdentical: Bool
  var cancellationStatus: UInt32
  var cancellationLatencySeconds: Double
  var cancellationClean: Bool
  var lifecycleExtractionCount: Int
  var lifecycleImportCount: Int
  var resourceSamples: [ResourceSample]
  var thermalStart: String
  var thermalEnd: String
  var residentStartBytes: UInt64
  var residentEndBytes: UInt64
  var peakEngineResidentBytes: UInt64
}

private enum HarnessError: Error, LocalizedError {
  case failed(String)

  var errorDescription: String? {
    switch self {
    case let .failed(message): message
    }
  }
}

private final class CStringPool {
  private var pointers: [UnsafeMutablePointer<CChar>] = []

  func add(_ value: String) throws -> UnsafePointer<CChar> {
    guard let pointer = strdup(value) else {
      throw HarnessError.failed("Could not allocate C string: \(value)")
    }
    pointers.append(pointer)
    return UnsafePointer(pointer)
  }

  deinit {
    for pointer in pointers { free(pointer) }
  }
}

private final class ExtractionProgressProbe: @unchecked Sendable {
  private let condition = NSCondition()
  private var latestStage: UInt32 = 0

  func record(stage: UInt32) {
    condition.lock()
    latestStage = max(latestStage, stage)
    condition.broadcast()
    condition.unlock()
  }

  func waitForSafeBoundary(timeout: TimeInterval) -> Bool {
    let deadline = Date().addingTimeInterval(timeout)
    condition.lock()
    defer { condition.unlock() }
    while latestStage < COLMAPKIT_FRAME_FEATURE_PROGRESS_V1_DECODING.rawValue {
      if !condition.wait(until: deadline) { return false }
    }
    return true
  }
}

private func extractionProgressCallback(
  _ event: UnsafePointer<ColmapKitFrameFeatureProgressEventV1>?,
  _ userData: UnsafeMutableRawPointer?
) {
  guard let event, let userData else { return }
  Unmanaged<ExtractionProgressProbe>.fromOpaque(userData)
    .takeUnretainedValue()
    .record(stage: event.pointee.stage)
}

private final class ResourceSampler: @unchecked Sendable {
  private let lock = NSLock()
  private let started = ContinuousClock.now
  private var timer: DispatchSourceTimer?
  private var values: [ResourceSample] = []

  func start() {
    DispatchQueue.main.sync {
      MainActor.assumeIsolated {
        UIDevice.current.isBatteryMonitoringEnabled = true
      }
    }
    sample()
    let timer = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
    timer.schedule(deadline: .now() + 5, repeating: 5)
    timer.setEventHandler { [weak self] in self?.sample() }
    timer.resume()
    self.timer = timer
  }

  func stop() -> [ResourceSample] {
    timer?.cancel()
    timer = nil
    sample()
    lock.lock()
    defer { lock.unlock() }
    return values
  }

  private func sample() {
    let duration = started.duration(to: .now)
    let elapsed = Double(duration.components.seconds)
      + Double(duration.components.attoseconds) / 1_000_000_000_000_000_000
    let battery = DispatchQueue.main.sync {
      MainActor.assumeIsolated {
        (UIDevice.current.batteryLevel, batteryStateName(UIDevice.current.batteryState))
      }
    }
    let value = ResourceSample(
      elapsedSeconds: elapsed,
      residentBytes: currentResidentBytes(),
      thermal: thermalStateName(ProcessInfo.processInfo.thermalState),
      batteryLevel: battery.0,
      batteryState: battery.1,
      lowPowerMode: ProcessInfo.processInfo.isLowPowerModeEnabled
    )
    lock.lock()
    values.append(value)
    lock.unlock()
  }
}

private final class HarnessAppDelegate: UIResponder, UIApplicationDelegate {
  func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    DispatchQueue.global(qos: .userInitiated).async {
      do {
        let result = try FrameFeatureDeviceHarness.run()
        let data = try JSONEncoder().encode(result)
        print("COLMAPKIT_FRAME_FEATURE_DEVICE_RESULT=\(String(decoding: data, as: UTF8.self))")
        fflush(stdout)
        exit(EXIT_SUCCESS)
      } catch {
        let failure = HarnessFailureReceipt(
          schemaVersion: 1,
          message: error.localizedDescription,
          releaseVersion: String(cString: ColmapKitGetReleaseVersionV2()),
          engineBuildIdentity: String(cString: ColmapKitGetEngineBuildIdentityV2()),
          device: currentDeviceBoundary(),
          residentBytes: currentResidentBytes(),
          request: extractionRequestReceipt(FrameFeatureDeviceHarness.extractorConfig()),
          profileSHA256: nil,
          profileAvailability: "Unavailable until a feature result reaches a profile-bearing terminal boundary."
        )
        if let data = try? JSONEncoder().encode(failure) {
          print(
            "COLMAPKIT_FRAME_FEATURE_DEVICE_FAILURE=\(String(decoding: data, as: UTF8.self))"
          )
        }
        print("COLMAPKIT_FRAME_FEATURE_DEVICE_ERROR=\(error.localizedDescription)")
        fflush(stdout)
        exit(EXIT_FAILURE)
      }
    }
    return true
  }
}

private enum FrameFeatureDeviceHarness {
  private struct ExtractedFrame {
    var frame: FixtureManifest.Frame
    var imageURL: URL
    var artifactURL: URL
    var result: ColmapKitFrameFeatureResultV1
  }

  static func run() throws -> DeviceResult {
    let version = String(cString: ColmapKitVersion())
    let abiVersion = ColmapKitGetABIVersionV2()
    let releaseVersion = String(cString: ColmapKitGetReleaseVersionV2())
    let engineBuildIdentity = String(cString: ColmapKitGetEngineBuildIdentityV2())
    guard abiVersion == 2,
          releaseVersion == expectedRelease,
          engineBuildIdentity.contains(expectedEngineCommit)
    else {
      throw HarnessError.failed(
        "Unexpected package identity ABI=\(abiVersion) release=\(releaseVersion) engine=\(engineBuildIdentity)"
      )
    }
    let initializeStatus = "ColmapKitFrameFeatureDeviceHarness".withCString {
      ColmapKitInitialize($0)
    }
    guard initializeStatus == COLMAPKIT_STATUS_OK else {
      throw HarnessError.failed("ColmapKitInitialize failed.")
    }

    let deviceStart = currentDeviceBoundary()
    guard deviceStart.thermal == "nominal" || deviceStart.thermal == "fair" else {
      throw HarnessError.failed("Refusing to start at thermal state \(deviceStart.thermal).")
    }
    guard deviceStart.batteryLevel >= 0.20 else {
      throw HarnessError.failed("Refusing to start below 20% battery or with unknown battery state.")
    }
    guard deviceStart.availableCapacityBytes >= 2 * 1024 * 1024 * 1024 else {
      throw HarnessError.failed("Refusing to start with less than 2 GiB available storage.")
    }
    guard let fixtureRoot = Bundle.main.url(forResource: "Fixture", withExtension: nil) else {
      throw HarnessError.failed("Fixture directory is absent from the app bundle.")
    }
    let manifestURL = fixtureRoot.appendingPathComponent("fixture-manifest.json")
    let manifestData = try Data(contentsOf: manifestURL)
    let manifest = try JSONDecoder().decode(FixtureManifest.self, from: manifestData)
    guard manifest.schemaVersion == 1,
          manifest.frames.count == 3,
          manifest.cameraModel == "PINHOLE",
          manifest.cameraParameters.count == 4
    else { throw HarnessError.failed("Fixture manifest is invalid.") }
    for frame in manifest.frames {
      let bytes = try Data(contentsOf: fixtureRoot.appendingPathComponent(frame.filename))
      guard bytes.count == frame.bytes, sha256(bytes) == frame.sha256 else {
        throw HarnessError.failed("Fixture identity mismatch: \(frame.filename)")
      }
    }
    let config = extractorConfig()
    let computedMaximumAdmission = manifest.frames.map {
      estimatedAdmissionBytes(frame: $0, config: config)
    }.max()
    guard computedMaximumAdmission == maximumFixtureAdmissionEstimateBytes else {
      throw HarnessError.failed("Fixture admission estimate contract mismatch.")
    }
    let request = extractionRequestReceipt(config)
    let residentStart = currentResidentBytes()
    let preflight = HarnessPreflightReceipt(
      schemaVersion: 1,
      releaseVersion: releaseVersion,
      engineBuildIdentity: engineBuildIdentity,
      fixtureManifestSHA256: sha256(manifestData),
      request: request,
      device: deviceStart,
      residentBytes: residentStart,
      profileSHA256: nil,
      profileAvailability: "Available from successful extraction/import result receipts."
    )
    let preflightData = try JSONEncoder().encode(preflight)
    print(
      "COLMAPKIT_FRAME_FEATURE_DEVICE_PREFLIGHT=\(String(decoding: preflightData, as: UTF8.self))"
    )
    fflush(stdout)
    let sampler = ResourceSampler()
    sampler.start()

    let runRoot = FileManager.default.temporaryDirectory
      .appendingPathComponent("colmapkit-frame-feature-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: runRoot, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: runRoot) }

    var extractedFrames: [ExtractedFrame] = []
    var extractionReceipts: [ExtractionReceipt] = []
    for frame in manifest.frames {
      let imageURL = fixtureRoot.appendingPathComponent(frame.filename)
      let artifactURL = runRoot.appendingPathComponent("primary-\(frame.stableFrameID).ckfeatures")
      let extracted = try extract(
        label: "primary-\(frame.stableFrameID)",
        frame: frame,
        imageURL: imageURL,
        artifactURL: artifactURL,
        cameraParameters: manifest.cameraParameters
      )
      extractedFrames.append(extracted)
      extractionReceipts.append(receipt(label: "primary-\(frame.stableFrameID)", frame: extracted))
      try validate(frame: extracted)
    }
    guard extractedFrames.first?.result.feature_count == 4096 else {
      throw HarnessError.failed(
        "Representative high-texture frame did not reach the repaired 4096-row terminal bound."
      )
    }

    let repeatURL = runRoot.appendingPathComponent("repeat.ckfeatures")
    let repeated = try extract(
      label: "repeat",
      frame: manifest.frames[0],
      imageURL: fixtureRoot.appendingPathComponent(manifest.frames[0].filename),
      artifactURL: repeatURL,
      cameraParameters: manifest.cameraParameters
    )
    try validate(frame: repeated)
    extractionReceipts.append(receipt(label: "repeat", frame: repeated))
    let repeatArtifactBytesIdentical =
      try Data(contentsOf: extractedFrames[0].artifactURL) == Data(contentsOf: repeatURL)
    guard repeatArtifactBytesIdentical else {
      throw HarnessError.failed("Same-device repeat extraction bytes differ.")
    }

    let cancellation = try cancelExtraction(
      frame: manifest.frames[0],
      imageURL: fixtureRoot.appendingPathComponent(manifest.frames[0].filename),
      artifactURL: runRoot.appendingPathComponent("cancelled.ckfeatures"),
      cameraParameters: manifest.cameraParameters
    )
    guard cancellation.status == COLMAPKIT_STATUS_CANCELLED.rawValue,
          cancellation.clean,
          cancellation.latency <= 5
    else {
      throw HarnessError.failed(
        "Cancellation gate failed status=\(cancellation.status) clean=\(cancellation.clean) latency=\(cancellation.latency)"
      )
    }

    var failures = try validationFailureMatrix(
      valid: extractedFrames[0],
      root: runRoot,
      cameraParameters: manifest.cameraParameters
    )
    failures.append(try metalRejection())
    failures.append(
      try preexistingExtractionRejection(
        frame: manifest.frames[0],
        imageURL: fixtureRoot.appendingPathComponent(manifest.frames[0].filename),
        root: runRoot,
        cameraParameters: manifest.cameraParameters
      )
    )

    let firstImportURL = runRoot.appendingPathComponent("first.ckseal", isDirectory: true)
    let firstImport = try runImport(label: "first", frames: extractedFrames, outputURL: firstImportURL)
    let secondImportURL = runRoot.appendingPathComponent("second.ckseal", isDirectory: true)
    let secondImport = try runImport(label: "second", frames: extractedFrames, outputURL: secondImportURL)
    let firstReceiptData = try Data(
      contentsOf: firstImportURL.appendingPathComponent("import-receipt.json")
    )
    let secondReceiptData = try Data(
      contentsOf: secondImportURL.appendingPathComponent("import-receipt.json")
    )
    let repeatReceiptBytesIdentical = firstReceiptData == secondReceiptData
    let repeatDatabaseIdentityIdentical =
      firstImport.databaseSHA256 == secondImport.databaseSHA256
        && firstImport.sealedSetSHA256 == secondImport.sealedSetSHA256
    guard repeatReceiptBytesIdentical, repeatDatabaseIdentityIdentical else {
      throw HarnessError.failed("Repeat import identities differ.")
    }
    failures.append(try preexistingImportRejection(frames: extractedFrames, root: runRoot))
    failures.append(try corruptImportRejection(frames: extractedFrames, root: runRoot))
    failures.append(try duplicateImportRejection(frames: extractedFrames, root: runRoot))
    failures.append(try partialImportRejection(frames: extractedFrames, root: runRoot))
    failures.append(
      try mixedProfileImportRejection(
        frames: extractedFrames,
        fixtureRoot: fixtureRoot,
        cameraParameters: manifest.cameraParameters,
        root: runRoot
      )
    )

    var lifecycleExtractionCount = 0
    var lifecyclePeakResidentBytes: UInt64 = 0
    for index in 0..<10 {
      let frame = manifest.frames[index % manifest.frames.count]
      let lifecycleURL = runRoot.appendingPathComponent("lifecycle-\(index).ckfeatures")
      let extracted = try extract(
        label: "lifecycle-\(index)",
        frame: frame,
        imageURL: fixtureRoot.appendingPathComponent(frame.filename),
        artifactURL: lifecycleURL,
        cameraParameters: manifest.cameraParameters
      )
      try validate(frame: extracted)
      lifecyclePeakResidentBytes = max(
        lifecyclePeakResidentBytes, extracted.result.peak_resident_memory_bytes
      )
      lifecycleExtractionCount += 1
    }

    let resourceSamples = sampler.stop()
    let residentEnd = currentResidentBytes()
    let peakEngine = max(
      extractionReceipts.map(\.peakResidentMemoryBytes).max() ?? 0,
      max(
        lifecyclePeakResidentBytes,
        max(firstImport.peakResidentMemoryBytes, secondImport.peakResidentMemoryBytes)
      )
    )
    let terminalGrowth = residentEnd > residentStart ? residentEnd - residentStart : 0
    guard peakEngine < 768 * 1024 * 1024,
          terminalGrowth <= 128 * 1024 * 1024,
          !resourceSamples.isEmpty,
          resourceSamples.allSatisfy({ $0.thermal != "critical" })
    else {
      throw HarnessError.failed(
        "Resource gate failed peak=\(peakEngine) growth=\(terminalGrowth) samples=\(resourceSamples.count)"
      )
    }

    return DeviceResult(
      version: version,
      abiVersion: abiVersion,
      releaseVersion: releaseVersion,
      engineBuildIdentity: engineBuildIdentity,
      fixtureManifestSHA256: sha256(manifestData),
      fixtures: manifest.frames,
      request: request,
      deviceStart: deviceStart,
      deviceEnd: currentDeviceBoundary(),
      extractions: extractionReceipts,
      imports: [firstImport, secondImport],
      failures: failures,
      repeatArtifactBytesIdentical: repeatArtifactBytesIdentical,
      repeatReceiptBytesIdentical: repeatReceiptBytesIdentical,
      repeatDatabaseIdentityIdentical: repeatDatabaseIdentityIdentical,
      cancellationStatus: cancellation.status,
      cancellationLatencySeconds: cancellation.latency,
      cancellationClean: cancellation.clean,
      lifecycleExtractionCount: lifecycleExtractionCount,
      lifecycleImportCount: 2,
      resourceSamples: resourceSamples,
      thermalStart: deviceStart.thermal,
      thermalEnd: thermalStateName(ProcessInfo.processInfo.thermalState),
      residentStartBytes: residentStart,
      residentEndBytes: residentEnd,
      peakEngineResidentBytes: peakEngine
    )
  }

  fileprivate static func extractorConfig(
    backend: ColmapKitFrameFeatureBackendV1 = COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU
  ) -> ColmapKitFrameFeatureExtractorConfigV1 {
    var config = ColmapKitFrameFeatureExtractorConfigV1()
    config.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureExtractorConfigV1>.size)
    config.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    config.requested_backend = backend.rawValue
    config.worker_count = 1
    config.max_encoded_image_bytes = 16 * 1024 * 1024
    config.memory_admission_budget_bytes = admissionBudgetBytes
    config.max_image_size = 1024
    config.max_num_features = 4096
    config.first_octave = -1
    config.num_octaves = 4
    config.octave_resolution = 3
    config.max_num_orientations = 2
    config.upright = 0
    config.normalization = COLMAPKIT_SIFT_NORMALIZATION_V1_L1_ROOT.rawValue
    config.peak_threshold = 0.006666666666666667
    config.edge_threshold = 10
    return config
  }

  private static func makeError() -> ColmapKitFrameFeatureErrorV1 {
    var error = ColmapKitFrameFeatureErrorV1()
    error.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureErrorV1>.size)
    error.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    return error
  }

  private static func makeFeatureResult() -> ColmapKitFrameFeatureResultV1 {
    var result = ColmapKitFrameFeatureResultV1()
    result.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureResultV1>.size)
    result.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    return result
  }

  private static func metadata(
    frame: FixtureManifest.Frame,
    cameraParameters: [Double]
  ) -> ColmapKitFrameMetadataV1 {
    var metadata = ColmapKitFrameMetadataV1()
    metadata.struct_size = UInt32(MemoryLayout<ColmapKitFrameMetadataV1>.size)
    metadata.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    metadata.image_format = COLMAPKIT_FRAME_IMAGE_FORMAT_V1_JPEG.rawValue
    metadata.encoded_width = UInt32(frame.width)
    metadata.encoded_height = UInt32(frame.height)
    metadata.orientation = COLMAPKIT_FRAME_ORIENTATION_V1_UP.rawValue
    metadata.camera_model = COLMAPKIT_CAMERA_MODEL_V2_PINHOLE.rawValue
    metadata.num_camera_params = UInt32(cameraParameters.count)
    withUnsafeMutableBytes(of: &metadata.camera_params) { raw in
      let values = raw.bindMemory(to: Double.self)
      for (index, value) in cameraParameters.enumerated() { values[index] = value }
    }
    return metadata
  }

  private static func createExtractor(
    config: inout ColmapKitFrameFeatureExtractorConfigV1
  ) throws -> OpaquePointer {
    var extractor: OpaquePointer?
    var error = makeError()
    let status = ColmapKitCreateFrameFeatureExtractorV1(&config, &extractor, &error)
    guard status == COLMAPKIT_STATUS_OK, let extractor else {
      throw HarnessError.failed("Create extractor failed: \(fixedCString(&error.message))")
    }
    return extractor
  }

  private static func extract(
    label: String,
    frame: FixtureManifest.Frame,
    imageURL: URL,
    artifactURL: URL,
    cameraParameters: [Double],
    configOverride: ColmapKitFrameFeatureExtractorConfigV1? = nil
  ) throws -> ExtractedFrame {
    var config = configOverride ?? extractorConfig()
    let extractor = try createExtractor(config: &config)
    defer { ColmapKitReleaseFrameFeatureExtractorV1(extractor) }
    let encoded = try Data(contentsOf: imageURL)
    let pool = CStringPool()
    let imageHash = try pool.add(frame.sha256)
    let outputPath = try pool.add(artifactURL.path)
    var input = ColmapKitFrameFeatureInputV1()
    input.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureInputV1>.size)
    input.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    input.stable_frame_id = frame.stableFrameID
    input.frame_revision = frame.frameRevision
    input.encoded_image_size = UInt64(encoded.count)
    input.expected_image_sha256 = imageHash
    input.metadata = metadata(frame: frame, cameraParameters: cameraParameters)
    input.output_artifact_path = outputPath
    var job: OpaquePointer?
    var error = makeError()
    let start = encoded.withUnsafeBytes { raw -> ColmapKitStatus in
      input.encoded_image_bytes = raw.bindMemory(to: UInt8.self).baseAddress
      return ColmapKitStartFrameFeatureExtractionV1(extractor, &input, &job, &error)
    }
    guard start == COLMAPKIT_STATUS_OK, let job else {
      throw HarnessError.failed("\(label) start failed: \(fixedCString(&error.message))")
    }
    defer { ColmapKitReleaseFrameFeatureJobV1(job) }
    var result = makeFeatureResult()
    let wait = ColmapKitWaitFrameFeatureExtractionV1(job, &result)
    guard wait == COLMAPKIT_STATUS_OK,
          result.status == COLMAPKIT_STATUS_OK.rawValue,
          result.actual_backend == COLMAPKIT_FRAME_FEATURE_BACKEND_V1_CPU.rawValue,
          result.no_fallback_satisfied == 1,
          result.effective_worker_count == 1,
          result.feature_count > 0,
          result.feature_count <= 4096,
          result.descriptor_bytes == result.feature_count * 128,
          FileManager.default.fileExists(atPath: artifactURL.path)
    else { throw HarnessError.failed("\(label) extraction failed: \(fixedCString(&result.message))") }
    return ExtractedFrame(frame: frame, imageURL: imageURL, artifactURL: artifactURL, result: result)
  }

  private static func validate(frame: ExtractedFrame) throws {
    var config = extractorConfig()
    let extractor = try createExtractor(config: &config)
    defer { ColmapKitReleaseFrameFeatureExtractorV1(extractor) }
    var source = frame.result
    let pool = CStringPool()
    var expectation = ColmapKitFrameFeatureArtifactExpectationV1()
    expectation.struct_size = UInt32(
      MemoryLayout<ColmapKitFrameFeatureArtifactExpectationV1>.size
    )
    expectation.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    expectation.stable_frame_id = frame.frame.stableFrameID
    expectation.frame_revision = frame.frame.frameRevision
    expectation.expected_image_sha256 = try pool.add(frame.frame.sha256)
    expectation.expected_metadata_sha256 = try pool.add(fixedCString(&source.metadata_sha256))
    expectation.artifact_path = try pool.add(frame.artifactURL.path)
    var result = makeFeatureResult()
    let status = ColmapKitValidateFrameFeatureArtifactV1(extractor, &expectation, &result)
    guard status == COLMAPKIT_STATUS_OK,
          result.status == COLMAPKIT_STATUS_OK.rawValue,
          fixedCString(&result.artifact_sha256) == fixedCString(&source.artifact_sha256)
    else { throw HarnessError.failed("Artifact validation failed: \(fixedCString(&result.message))") }
  }

  private static func receipt(label: String, frame: ExtractedFrame) -> ExtractionReceipt {
    var result = frame.result
    return ExtractionReceipt(
      label: label,
      status: result.status,
      actualBackend: result.actual_backend,
      noFallback: result.no_fallback_satisfied == 1,
      effectiveWorkers: result.effective_worker_count,
      stableFrameID: result.stable_frame_id,
      frameRevision: result.frame_revision,
      featureCount: result.feature_count,
      descriptorBytes: result.descriptor_bytes,
      admittedMemoryBytes: result.admitted_memory_bytes,
      peakResidentMemoryBytes: result.peak_resident_memory_bytes,
      totalSeconds: result.total_seconds,
      imageSHA256: fixedCString(&result.image_sha256),
      metadataSHA256: fixedCString(&result.metadata_sha256),
      profileSHA256: fixedCString(&result.profile_sha256),
      payloadSHA256: fixedCString(&result.payload_sha256),
      artifactSHA256: fixedCString(&result.artifact_sha256),
      sourceIdentity: fixedCString(&result.source_identity),
      artifactPath: frame.artifactURL.path
    )
  }

  private static func cancelExtraction(
    frame: FixtureManifest.Frame,
    imageURL: URL,
    artifactURL: URL,
    cameraParameters: [Double]
  ) throws -> (status: UInt32, latency: Double, clean: Bool) {
    var config = extractorConfig()
    let extractor = try createExtractor(config: &config)
    defer { ColmapKitReleaseFrameFeatureExtractorV1(extractor) }
    let encoded = try Data(contentsOf: imageURL)
    let pool = CStringPool()
    let probe = ExtractionProgressProbe()
    let retainedProbe = Unmanaged.passRetained(probe)
    defer { retainedProbe.release() }
    var input = ColmapKitFrameFeatureInputV1()
    input.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureInputV1>.size)
    input.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    input.stable_frame_id = frame.stableFrameID
    input.frame_revision = frame.frameRevision
    input.encoded_image_size = UInt64(encoded.count)
    input.expected_image_sha256 = try pool.add(frame.sha256)
    input.metadata = metadata(frame: frame, cameraParameters: cameraParameters)
    input.output_artifact_path = try pool.add(artifactURL.path)
    input.progress_callback = extractionProgressCallback
    input.progress_user_data = retainedProbe.toOpaque()
    var job: OpaquePointer?
    var error = makeError()
    let start = encoded.withUnsafeBytes { raw -> ColmapKitStatus in
      input.encoded_image_bytes = raw.bindMemory(to: UInt8.self).baseAddress
      return ColmapKitStartFrameFeatureExtractionV1(extractor, &input, &job, &error)
    }
    guard start == COLMAPKIT_STATUS_OK, let job else {
      throw HarnessError.failed("Cancellation start failed: \(fixedCString(&error.message))")
    }
    defer { ColmapKitReleaseFrameFeatureJobV1(job) }
    guard probe.waitForSafeBoundary(timeout: 2) else {
      throw HarnessError.failed("Cancellation progress boundary was not observed.")
    }
    let cancellationStart = ContinuousClock.now
    let cancel = ColmapKitCancelFrameFeatureExtractionV1(job, &error)
    guard cancel == COLMAPKIT_STATUS_OK else {
      throw HarnessError.failed("Cancellation request failed: \(fixedCString(&error.message))")
    }
    var result = makeFeatureResult()
    _ = ColmapKitWaitFrameFeatureExtractionV1(job, &result)
    let duration = cancellationStart.duration(to: .now)
    let latency = Double(duration.components.seconds)
      + Double(duration.components.attoseconds) / 1_000_000_000_000_000_000
    let siblings = try FileManager.default.contentsOfDirectory(
      at: artifactURL.deletingLastPathComponent(),
      includingPropertiesForKeys: nil
    )
    let clean = !FileManager.default.fileExists(atPath: artifactURL.path)
      && !siblings.contains(where: { $0.lastPathComponent.hasPrefix(artifactURL.lastPathComponent + ".tmp.") })
    return (result.status, latency, clean)
  }

  private static func runImport(
    label: String,
    frames: [ExtractedFrame],
    outputURL: URL
  ) throws -> ImportReceipt {
    let pool = CStringPool()
    var items: [ColmapKitFrameFeatureImportItemV1] = []
    for frame in frames {
      var result = frame.result
      var item = ColmapKitFrameFeatureImportItemV1()
      item.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureImportItemV1>.size)
      item.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1)
      item.stable_frame_id = frame.frame.stableFrameID
      item.frame_revision = frame.frame.frameRevision
      item.image_name = try pool.add(frame.frame.imageName)
      item.image_path = try pool.add(frame.imageURL.path)
      item.artifact_path = try pool.add(frame.artifactURL.path)
      item.expected_image_sha256 = try pool.add(frame.frame.sha256)
      item.expected_metadata_sha256 = try pool.add(fixedCString(&result.metadata_sha256))
      item.expected_artifact_sha256 = try pool.add(fixedCString(&result.artifact_sha256))
      items.append(item)
    }
    var config = ColmapKitFrameFeatureImportConfigV1()
    config.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureImportConfigV1>.size)
    config.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1)
    config.mode = COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_CREATE_NEW.rawValue
    config.worker_count = 1
    config.num_items = UInt64(items.count)
    config.extractor_config = extractorConfig()
    config.max_total_artifact_bytes = 64 * 1024 * 1024
    config.max_total_image_bytes = 64 * 1024 * 1024
    config.max_total_features = 100_000
    config.max_base_database_bytes = 64 * 1024 * 1024
    config.output_bundle_path = try pool.add(outputURL.path)
    var job: OpaquePointer?
    var error = makeError()
    let start = items.withUnsafeBufferPointer { buffer -> ColmapKitStatus in
      config.items = buffer.baseAddress
      return ColmapKitStartFrameFeatureImportV1(&config, &job, &error)
    }
    guard start == COLMAPKIT_STATUS_OK, let job else {
      throw HarnessError.failed("\(label) import start failed: \(fixedCString(&error.message))")
    }
    defer { ColmapKitReleaseFrameFeatureImportJobV1(job) }
    var result = ColmapKitFrameFeatureImportResultV1()
    result.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureImportResultV1>.size)
    result.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1)
    let wait = ColmapKitWaitFrameFeatureImportV1(job, &result)
    let receiptURL = outputURL.appendingPathComponent("import-receipt.json")
    let databaseURL = outputURL.appendingPathComponent("database.db")
    guard wait == COLMAPKIT_STATUS_OK,
          result.status == COLMAPKIT_STATUS_OK.rawValue,
          result.no_fallback_satisfied == 1,
          result.effective_worker_count == 1,
          result.imported_items == UInt64(frames.count),
          result.imported_keypoints == frames.reduce(0, { $0 + $1.result.feature_count }),
          FileManager.default.fileExists(atPath: receiptURL.path),
          FileManager.default.fileExists(atPath: databaseURL.path)
    else { throw HarnessError.failed("\(label) import failed: \(fixedCString(&result.message))") }
    let database = try inspectDatabase(databaseURL)
    guard database.cameras == 1,
          database.images == Int64(frames.count),
          database.keypointRows == Int64(frames.count),
          database.keypoints == Int64(result.imported_keypoints),
          database.descriptorRows == Int64(frames.count),
          database.descriptors == Int64(result.imported_keypoints),
          database.matches == 0,
          database.twoViewGeometries == 0,
          database.imageIDs == Array(1...Int64(frames.count)),
          database.cameraIDs == Array(repeating: 1, count: frames.count)
    else { throw HarnessError.failed("\(label) database structure is invalid: \(database)") }
    return ImportReceipt(
      label: label,
      status: result.status,
      noFallback: result.no_fallback_satisfied == 1,
      effectiveWorkers: result.effective_worker_count,
      importedItems: result.imported_items,
      importedCameras: result.imported_cameras,
      importedKeypoints: result.imported_keypoints,
      importedDescriptorBytes: result.imported_descriptor_bytes,
      admittedMemoryBytes: result.admitted_memory_bytes,
      peakResidentMemoryBytes: result.peak_resident_memory_bytes,
      totalSeconds: result.total_seconds,
      sealedSetSHA256: fixedCString(&result.sealed_set_sha256),
      databaseSHA256: fixedCString(&result.database_sha256),
      receiptSHA256: fixedCString(&result.receipt_sha256),
      profileSHA256: fixedCString(&result.profile_sha256),
      sourceIdentity: fixedCString(&result.source_identity),
      outputPath: outputURL.path,
      database: database
    )
  }

  private static func validationFailureMatrix(
    valid: ExtractedFrame,
    root: URL,
    cameraParameters: [Double]
  ) throws -> [FailureReceipt] {
    let original = try Data(contentsOf: valid.artifactURL)
    var receipts: [FailureReceipt] = []
    for (label, data) in [
      ("corrupt-artifact", mutated(original)),
      ("truncated-artifact", original.prefix(max(1, original.count / 2)).data),
    ] {
      let url = root.appendingPathComponent("\(label).ckfeatures")
      try data.write(to: url, options: .atomic)
      let status = try validationStatus(
        frame: valid,
        artifactURL: url,
        expectedImageSHA256: valid.frame.sha256
      )
      guard status.status != COLMAPKIT_STATUS_OK.rawValue else {
        throw HarnessError.failed("\(label) unexpectedly validated.")
      }
      receipts.append(
        FailureReceipt(
          label: label,
          status: status.status,
          message: status.message,
          outputAbsent: false,
          foreignOutputPreserved: true
        )
      )
    }
    let stale = try validationStatus(
      frame: valid,
      artifactURL: valid.artifactURL,
      expectedImageSHA256: String(repeating: "0", count: 64)
    )
    guard stale.status != COLMAPKIT_STATUS_OK.rawValue else {
      throw HarnessError.failed("Stale image expectation unexpectedly validated.")
    }
    receipts.append(
      FailureReceipt(
        label: "stale-image",
        status: stale.status,
        message: stale.message,
        outputAbsent: false,
        foreignOutputPreserved: true
      )
    )
    return receipts
  }

  private static func validationStatus(
    frame: ExtractedFrame,
    artifactURL: URL,
    expectedImageSHA256: String
  ) throws -> (status: UInt32, message: String) {
    var config = extractorConfig()
    let extractor = try createExtractor(config: &config)
    defer { ColmapKitReleaseFrameFeatureExtractorV1(extractor) }
    var source = frame.result
    let pool = CStringPool()
    var expectation = ColmapKitFrameFeatureArtifactExpectationV1()
    expectation.struct_size = UInt32(
      MemoryLayout<ColmapKitFrameFeatureArtifactExpectationV1>.size
    )
    expectation.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    expectation.stable_frame_id = frame.frame.stableFrameID
    expectation.frame_revision = frame.frame.frameRevision
    expectation.expected_image_sha256 = try pool.add(expectedImageSHA256)
    expectation.expected_metadata_sha256 = try pool.add(fixedCString(&source.metadata_sha256))
    expectation.artifact_path = try pool.add(artifactURL.path)
    var result = makeFeatureResult()
    let status = ColmapKitValidateFrameFeatureArtifactV1(extractor, &expectation, &result)
    return (status.rawValue, fixedCString(&result.message))
  }

  private static func metalRejection() throws -> FailureReceipt {
    var config = extractorConfig(backend: COLMAPKIT_FRAME_FEATURE_BACKEND_V1_METAL)
    var extractor: OpaquePointer?
    var error = makeError()
    let status = ColmapKitCreateFrameFeatureExtractorV1(&config, &extractor, &error)
    if let extractor { ColmapKitReleaseFrameFeatureExtractorV1(extractor) }
    guard status != COLMAPKIT_STATUS_OK, extractor == nil else {
      throw HarnessError.failed("Metal backend request did not fail closed.")
    }
    return FailureReceipt(
      label: "metal-backend",
      status: status.rawValue,
      message: fixedCString(&error.message),
      outputAbsent: true,
      foreignOutputPreserved: true
    )
  }

  private static func preexistingExtractionRejection(
    frame: FixtureManifest.Frame,
    imageURL: URL,
    root: URL,
    cameraParameters: [Double]
  ) throws -> FailureReceipt {
    let output = root.appendingPathComponent("preexisting.ckfeatures")
    let sentinel = Data("foreign-output".utf8)
    try sentinel.write(to: output)
    var config = extractorConfig()
    let extractor = try createExtractor(config: &config)
    defer { ColmapKitReleaseFrameFeatureExtractorV1(extractor) }
    let encoded = try Data(contentsOf: imageURL)
    let pool = CStringPool()
    var input = ColmapKitFrameFeatureInputV1()
    input.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureInputV1>.size)
    input.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_ABI_VERSION_V1)
    input.stable_frame_id = frame.stableFrameID
    input.frame_revision = frame.frameRevision
    input.encoded_image_size = UInt64(encoded.count)
    input.expected_image_sha256 = try pool.add(frame.sha256)
    input.metadata = metadata(frame: frame, cameraParameters: cameraParameters)
    input.output_artifact_path = try pool.add(output.path)
    var job: OpaquePointer?
    var error = makeError()
    let start = encoded.withUnsafeBytes { raw -> ColmapKitStatus in
      input.encoded_image_bytes = raw.bindMemory(to: UInt8.self).baseAddress
      return ColmapKitStartFrameFeatureExtractionV1(extractor, &input, &job, &error)
    }
    if let job { ColmapKitReleaseFrameFeatureJobV1(job) }
    let preserved = try Data(contentsOf: output) == sentinel
    guard start != COLMAPKIT_STATUS_OK, preserved else {
      throw HarnessError.failed("Preexisting extraction output was not preserved.")
    }
    return FailureReceipt(
      label: "preexisting-extraction",
      status: start.rawValue,
      message: fixedCString(&error.message),
      outputAbsent: false,
      foreignOutputPreserved: preserved
    )
  }

  private static func preexistingImportRejection(
    frames: [ExtractedFrame],
    root: URL
  ) throws -> FailureReceipt {
    let output = root.appendingPathComponent("preexisting.ckseal", isDirectory: true)
    try FileManager.default.createDirectory(at: output, withIntermediateDirectories: true)
    let marker = output.appendingPathComponent("foreign.txt")
    let sentinel = Data("foreign-output".utf8)
    try sentinel.write(to: marker)
    let failure = try importFailure(frames: frames, outputURL: output)
    let preserved = try Data(contentsOf: marker) == sentinel
    guard failure.status != COLMAPKIT_STATUS_OK.rawValue, preserved else {
      throw HarnessError.failed("Preexisting import output was not preserved.")
    }
    return FailureReceipt(
      label: "preexisting-import",
      status: failure.status,
      message: failure.message,
      outputAbsent: false,
      foreignOutputPreserved: preserved
    )
  }

  private static func corruptImportRejection(
    frames: [ExtractedFrame],
    root: URL
  ) throws -> FailureReceipt {
    var corrupted = frames
    let corruptURL = root.appendingPathComponent("import-corrupt.ckfeatures")
    let source = try Data(contentsOf: frames[0].artifactURL)
    try mutated(source).write(to: corruptURL, options: .atomic)
    corrupted[0].artifactURL = corruptURL
    let output = root.appendingPathComponent("corrupt-import.ckseal", isDirectory: true)
    let failure = try importFailure(frames: corrupted, outputURL: output)
    let absent = !FileManager.default.fileExists(atPath: output.path)
    guard failure.status != COLMAPKIT_STATUS_OK.rawValue, absent else {
      throw HarnessError.failed("Corrupt import did not fail without visible output.")
    }
    return FailureReceipt(
      label: "corrupt-import",
      status: failure.status,
      message: failure.message,
      outputAbsent: absent,
      foreignOutputPreserved: true
    )
  }

  private static func duplicateImportRejection(
    frames: [ExtractedFrame],
    root: URL
  ) throws -> FailureReceipt {
    var duplicated = frames
    duplicated[1].frame.frameRevision = duplicated[0].frame.frameRevision
    duplicated[1].frame.stableFrameID = duplicated[0].frame.stableFrameID
    duplicated[1].frame.imageName = duplicated[0].frame.imageName
    let output = root.appendingPathComponent("duplicate-import.ckseal", isDirectory: true)
    let failure = try importFailure(frames: duplicated, outputURL: output)
    let absent = !FileManager.default.fileExists(atPath: output.path)
    guard failure.status != COLMAPKIT_STATUS_OK.rawValue, absent else {
      throw HarnessError.failed("Duplicate import did not fail without visible output.")
    }
    return FailureReceipt(
      label: "duplicate-import",
      status: failure.status,
      message: failure.message,
      outputAbsent: absent,
      foreignOutputPreserved: true
    )
  }

  private static func partialImportRejection(
    frames: [ExtractedFrame],
    root: URL
  ) throws -> FailureReceipt {
    var partial = frames
    partial[2].artifactURL = root.appendingPathComponent("missing.ckfeatures")
    let output = root.appendingPathComponent("partial-import.ckseal", isDirectory: true)
    let failure = try importFailure(frames: partial, outputURL: output)
    let absent = !FileManager.default.fileExists(atPath: output.path)
    guard failure.status != COLMAPKIT_STATUS_OK.rawValue, absent else {
      throw HarnessError.failed("Partial import did not fail without visible output.")
    }
    return FailureReceipt(
      label: "partial-import",
      status: failure.status,
      message: failure.message,
      outputAbsent: absent,
      foreignOutputPreserved: true
    )
  }

  private static func mixedProfileImportRejection(
    frames: [ExtractedFrame],
    fixtureRoot: URL,
    cameraParameters: [Double],
    root: URL
  ) throws -> FailureReceipt {
    var differentProfile = extractorConfig()
    differentProfile.max_num_features = 2048
    let mixed = try extract(
      label: "mixed-profile-source",
      frame: frames[0].frame,
      imageURL: fixtureRoot.appendingPathComponent(frames[0].frame.filename),
      artifactURL: root.appendingPathComponent("mixed-profile.ckfeatures"),
      cameraParameters: cameraParameters,
      configOverride: differentProfile
    )
    var mixedFrames = frames
    mixedFrames[0] = mixed
    let output = root.appendingPathComponent("mixed-profile-import.ckseal", isDirectory: true)
    let failure = try importFailure(frames: mixedFrames, outputURL: output)
    let absent = !FileManager.default.fileExists(atPath: output.path)
    guard failure.status != COLMAPKIT_STATUS_OK.rawValue, absent else {
      throw HarnessError.failed("Mixed-profile import did not fail without visible output.")
    }
    return FailureReceipt(
      label: "mixed-profile-import",
      status: failure.status,
      message: failure.message,
      outputAbsent: absent,
      foreignOutputPreserved: true
    )
  }

  private static func inspectDatabase(_ url: URL) throws -> DatabaseSummary {
    var database: OpaquePointer?
    guard sqlite3_open_v2(url.path, &database, SQLITE_OPEN_READONLY, nil) == SQLITE_OK,
          let database
    else { throw HarnessError.failed("Could not open imported database read-only.") }
    defer { sqlite3_close(database) }
    return DatabaseSummary(
      cameras: try scalar(database, "SELECT COUNT(*) FROM cameras"),
      images: try scalar(database, "SELECT COUNT(*) FROM images"),
      keypointRows: try scalar(database, "SELECT COUNT(*) FROM keypoints"),
      keypoints: try scalar(database, "SELECT COALESCE(SUM(rows), 0) FROM keypoints"),
      descriptorRows: try scalar(database, "SELECT COUNT(*) FROM descriptors"),
      descriptors: try scalar(database, "SELECT COALESCE(SUM(rows), 0) FROM descriptors"),
      matches: try scalar(database, "SELECT COUNT(*) FROM matches"),
      twoViewGeometries: try scalar(database, "SELECT COUNT(*) FROM two_view_geometries"),
      imageIDs: try column(database, "SELECT image_id FROM images ORDER BY image_id"),
      cameraIDs: try column(database, "SELECT camera_id FROM images ORDER BY image_id")
    )
  }

  private static func scalar(_ database: OpaquePointer, _ sql: String) throws -> Int64 {
    let values = try column(database, sql)
    guard values.count == 1 else { throw HarnessError.failed("Unexpected SQLite scalar: \(sql)") }
    return values[0]
  }

  private static func column(_ database: OpaquePointer, _ sql: String) throws -> [Int64] {
    var statement: OpaquePointer?
    guard sqlite3_prepare_v2(database, sql, -1, &statement, nil) == SQLITE_OK,
          let statement
    else { throw HarnessError.failed("SQLite prepare failed: \(sql)") }
    defer { sqlite3_finalize(statement) }
    var values: [Int64] = []
    while true {
      let status = sqlite3_step(statement)
      if status == SQLITE_DONE { return values }
      guard status == SQLITE_ROW else {
        throw HarnessError.failed("SQLite step failed: \(sql)")
      }
      values.append(sqlite3_column_int64(statement, 0))
    }
  }

  private static func importFailure(
    frames: [ExtractedFrame],
    outputURL: URL
  ) throws -> (status: UInt32, message: String) {
    let pool = CStringPool()
    var items: [ColmapKitFrameFeatureImportItemV1] = []
    for frame in frames {
      var result = frame.result
      var item = ColmapKitFrameFeatureImportItemV1()
      item.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureImportItemV1>.size)
      item.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1)
      item.stable_frame_id = frame.frame.stableFrameID
      item.frame_revision = frame.frame.frameRevision
      item.image_name = try pool.add(frame.frame.imageName)
      item.image_path = try pool.add(frame.imageURL.path)
      item.artifact_path = try pool.add(frame.artifactURL.path)
      item.expected_image_sha256 = try pool.add(frame.frame.sha256)
      item.expected_metadata_sha256 = try pool.add(fixedCString(&result.metadata_sha256))
      item.expected_artifact_sha256 = try pool.add(fixedCString(&result.artifact_sha256))
      items.append(item)
    }
    var config = ColmapKitFrameFeatureImportConfigV1()
    config.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureImportConfigV1>.size)
    config.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1)
    config.mode = COLMAPKIT_FRAME_FEATURE_IMPORT_MODE_V1_CREATE_NEW.rawValue
    config.worker_count = 1
    config.num_items = UInt64(items.count)
    config.extractor_config = extractorConfig()
    config.max_total_artifact_bytes = 64 * 1024 * 1024
    config.max_total_image_bytes = 64 * 1024 * 1024
    config.max_total_features = 100_000
    config.max_base_database_bytes = 64 * 1024 * 1024
    config.output_bundle_path = try pool.add(outputURL.path)
    var job: OpaquePointer?
    var error = makeError()
    let start = items.withUnsafeBufferPointer { buffer -> ColmapKitStatus in
      config.items = buffer.baseAddress
      return ColmapKitStartFrameFeatureImportV1(&config, &job, &error)
    }
    guard start == COLMAPKIT_STATUS_OK, let job else {
      return (start.rawValue, fixedCString(&error.message))
    }
    defer { ColmapKitReleaseFrameFeatureImportJobV1(job) }
    var result = ColmapKitFrameFeatureImportResultV1()
    result.struct_size = UInt32(MemoryLayout<ColmapKitFrameFeatureImportResultV1>.size)
    result.abi_version = UInt32(COLMAPKIT_FRAME_FEATURE_IMPORT_ABI_VERSION_V1)
    let wait = ColmapKitWaitFrameFeatureImportV1(job, &result)
    return (wait.rawValue, fixedCString(&result.message))
  }

  private static func mutated(_ data: Data) -> Data {
    var copy = data
    if !copy.isEmpty { copy[copy.count / 2] ^= 0x5A }
    return copy
  }

  private static func sha256(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
  }
}

private extension Data.SubSequence {
  var data: Data { Data(self) }
}

private func estimatedAdmissionBytes(
  frame: FixtureManifest.Frame,
  config: ColmapKitFrameFeatureExtractorConfigV1
) -> UInt64 {
  let scale = min(
    1.0,
    Double(config.max_image_size) / Double(max(frame.width, frame.height))
  )
  let width = max(1, UInt64(ceil(Double(frame.width) * scale)))
  let height = max(1, UInt64(ceil(Double(frame.height) * scale)))
  let firstOctaveFactor: UInt64 = config.first_octave < 0 ? 4 : 1
  return UInt64(frame.bytes) * 2
    + width * height * firstOctaveFactor * 96
    + UInt64(config.max_num_features) * 256
    + 16 * 1024 * 1024
}

private func extractionRequestReceipt(
  _ config: ColmapKitFrameFeatureExtractorConfigV1
) -> ExtractionRequestReceipt {
  ExtractionRequestReceipt(
    requestedBackend: config.requested_backend,
    workerCount: config.worker_count,
    maxEncodedImageBytes: config.max_encoded_image_bytes,
    memoryAdmissionBudgetBytes: config.memory_admission_budget_bytes,
    maximumFixtureAdmissionEstimateBytes: maximumFixtureAdmissionEstimateBytes,
    maxImageSize: config.max_image_size,
    maxNumFeatures: config.max_num_features,
    firstOctave: config.first_octave,
    numOctaves: config.num_octaves,
    octaveResolution: config.octave_resolution,
    maxNumOrientations: config.max_num_orientations,
    upright: config.upright == 1,
    normalization: config.normalization,
    peakThreshold: config.peak_threshold,
    edgeThreshold: config.edge_threshold
  )
}

private func fixedCString<T>(_ value: inout T) -> String {
  withUnsafePointer(to: &value) { pointer in
    pointer.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) {
      String(cString: $0)
    }
  }
}

private func currentResidentBytes() -> UInt64 {
  var info = mach_task_basic_info_data_t()
  var count = mach_msg_type_number_t(
    MemoryLayout<mach_task_basic_info_data_t>.size / MemoryLayout<natural_t>.size
  )
  let status = withUnsafeMutablePointer(to: &info) { pointer in
    pointer.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
      task_info(mach_task_self_, task_flavor_t(MACH_TASK_BASIC_INFO), $0, &count)
    }
  }
  return status == KERN_SUCCESS ? UInt64(info.resident_size) : 0
}

private func currentDeviceBoundary() -> DeviceBoundary {
  let device = DispatchQueue.main.sync {
    MainActor.assumeIsolated {
      UIDevice.current.isBatteryMonitoringEnabled = true
      return (
        UIDevice.current.name,
        UIDevice.current.model,
        UIDevice.current.systemName,
        UIDevice.current.systemVersion,
        UIDevice.current.batteryLevel,
        batteryStateName(UIDevice.current.batteryState)
      )
    }
  }
  let capacity = (try? FileManager.default.temporaryDirectory.resourceValues(
    forKeys: [.volumeAvailableCapacityForImportantUsageKey]
  ).volumeAvailableCapacityForImportantUsage) ?? -1
  return DeviceBoundary(
    name: device.0,
    model: device.1,
    systemName: device.2,
    systemVersion: device.3,
    operatingSystemVersion: ProcessInfo.processInfo.operatingSystemVersionString,
    availableCapacityBytes: capacity,
    thermal: thermalStateName(ProcessInfo.processInfo.thermalState),
    batteryLevel: device.4,
    batteryState: device.5,
    lowPowerMode: ProcessInfo.processInfo.isLowPowerModeEnabled
  )
}

private func thermalStateName(_ state: ProcessInfo.ThermalState) -> String {
  switch state {
  case .nominal: "nominal"
  case .fair: "fair"
  case .serious: "serious"
  case .critical: "critical"
  @unknown default: "unknown"
  }
}

private func batteryStateName(_ state: UIDevice.BatteryState) -> String {
  switch state {
  case .unknown: "unknown"
  case .unplugged: "unplugged"
  case .charging: "charging"
  case .full: "full"
  @unknown default: "unknown"
  }
}

UIApplicationMain(
  CommandLine.argc,
  CommandLine.unsafeArgv,
  nil,
  NSStringFromClass(HarnessAppDelegate.self)
)
