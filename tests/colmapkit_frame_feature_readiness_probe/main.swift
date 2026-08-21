import Darwin
import Foundation
import UIKit

private let minimumBatteryLevel: Float = 0.20
private let minimumFreeStorageBytes: Int64 = 2 * 1024 * 1024 * 1024
private let expectedMachine = "iPad13,6"

private struct ReadinessGates: Codable {
  var exactMachine: Bool
  var batteryAtLeast20Percent: Bool
  var batteryStateKnown: Bool
  var lowPowerModeOff: Bool
  var thermalNominalOrFair: Bool
  var freeStorageAtLeast2GiB: Bool
}

private struct ReadinessReceipt: Codable {
  var schemaVersion: Int
  var probeVersion: String
  var measuredAtUTC: String
  var deviceName: String
  var deviceModel: String
  var machine: String
  var systemName: String
  var systemVersion: String
  var operatingSystemVersion: String
  var batteryLevel: Float
  var batteryPercent: Int
  var batteryState: String
  var lowPowerMode: Bool
  var thermalState: String
  var freeStorageBytes: Int64
  var processResidentBytes: UInt64
  var gates: ReadinessGates
  var eligible: Bool
  var ineligibilityReasons: [String]
  var engineApiCallCount: Int
  var acceptanceMatrixLaunchAttempted: Bool
  var terminatesAfterReadiness: Bool
}

@MainActor
private final class ReadinessProbeAppDelegate: UIResponder, UIApplicationDelegate {
  func application(
    _ application: UIApplication,
    didFinishLaunchingWithOptions launchOptions: [UIApplication.LaunchOptionsKey: Any]? = nil
  ) -> Bool {
    UIDevice.current.isBatteryMonitoringEnabled = true
    let receipt = collectReadiness()
    let encoder = JSONEncoder()
    encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
    if let data = try? encoder.encode(receipt) {
      print("COLMAPKIT_FRAME_FEATURE_READINESS_RESULT=\(String(decoding: data, as: UTF8.self))")
    } else {
      print("COLMAPKIT_FRAME_FEATURE_READINESS_ERROR={\"schemaVersion\":1,\"message\":\"receipt encoding failed\"}")
    }
    fflush(stdout)
    exit(0)
  }

  private func collectReadiness() -> ReadinessReceipt {
    let batteryLevel = UIDevice.current.batteryLevel
    let batteryState = batteryStateName(UIDevice.current.batteryState)
    let thermalState = thermalStateName(ProcessInfo.processInfo.thermalState)
    let lowPowerMode = ProcessInfo.processInfo.isLowPowerModeEnabled
    let machine = machineIdentifier()
    let freeStorageBytes = (try? FileManager.default.temporaryDirectory.resourceValues(
      forKeys: [.volumeAvailableCapacityForImportantUsageKey]
    ).volumeAvailableCapacityForImportantUsage) ?? -1
    let gates = ReadinessGates(
      exactMachine: machine == expectedMachine,
      batteryAtLeast20Percent: batteryLevel >= minimumBatteryLevel,
      batteryStateKnown: batteryState != "unknown",
      lowPowerModeOff: !lowPowerMode,
      thermalNominalOrFair: thermalState == "nominal" || thermalState == "fair",
      freeStorageAtLeast2GiB: freeStorageBytes >= minimumFreeStorageBytes
    )
    var reasons: [String] = []
    if !gates.exactMachine { reasons.append("machine_identity_mismatch") }
    if !gates.batteryAtLeast20Percent { reasons.append("battery_below_20_percent") }
    if !gates.batteryStateKnown { reasons.append("battery_state_unknown") }
    if !gates.lowPowerModeOff { reasons.append("low_power_mode_enabled") }
    if !gates.thermalNominalOrFair { reasons.append("thermal_not_nominal_or_fair") }
    if !gates.freeStorageAtLeast2GiB { reasons.append("free_storage_below_2_gib") }

    let formatter = ISO8601DateFormatter()
    formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
    return ReadinessReceipt(
      schemaVersion: 1,
      probeVersion: "physical-readiness-v1",
      measuredAtUTC: formatter.string(from: Date()),
      deviceName: UIDevice.current.name,
      deviceModel: UIDevice.current.model,
      machine: machine,
      systemName: UIDevice.current.systemName,
      systemVersion: UIDevice.current.systemVersion,
      operatingSystemVersion: ProcessInfo.processInfo.operatingSystemVersionString,
      batteryLevel: batteryLevel,
      batteryPercent: batteryLevel < 0 ? -1 : Int((batteryLevel * 100).rounded()),
      batteryState: batteryState,
      lowPowerMode: lowPowerMode,
      thermalState: thermalState,
      freeStorageBytes: freeStorageBytes,
      processResidentBytes: currentResidentBytes(),
      gates: gates,
      eligible: reasons.isEmpty,
      ineligibilityReasons: reasons,
      engineApiCallCount: 0,
      acceptanceMatrixLaunchAttempted: false,
      terminatesAfterReadiness: true
    )
  }
}

private func machineIdentifier() -> String {
  var size = 0
  guard sysctlbyname("hw.machine", nil, &size, nil, 0) == 0, size > 0 else {
    return "unknown"
  }
  var buffer = [CChar](repeating: 0, count: size)
  guard sysctlbyname("hw.machine", &buffer, &size, nil, 0) == 0 else {
    return "unknown"
  }
  return String(cString: buffer)
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
  NSStringFromClass(ReadinessProbeAppDelegate.self)
)
