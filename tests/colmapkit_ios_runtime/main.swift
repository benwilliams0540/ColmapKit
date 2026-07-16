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
  var cancellationStatus: UInt32
  var cancellationLatencySeconds: Double
  var fixtureImageCount: Int
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

    let reconstruction = try runReconstruction(
      fixtureURL: fixtureURL,
      rootURL: runRoot.appendingPathComponent("complete", isDirectory: true),
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
      cancellationStatus: cancellation.result.status.rawValue,
      cancellationLatencySeconds: cancellation.cancellationLatencySeconds,
      fixtureImageCount: fixtureImageCount
    )
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
