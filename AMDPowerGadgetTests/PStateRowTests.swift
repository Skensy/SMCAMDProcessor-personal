import XCTest
@testable import AMD_Power_Gadget

final class PStateRowTests: XCTestCase {
    func testZen3RoundTrip() {
        let raw: UInt64 = (1 << 63) | (0x48 << 14) | (0x08 << 8) | 0xA0
        let row = PStateRow.from(raw: raw, index: 0, cpuFamily: 0x19)
        XCTAssertEqual(row.enabled, 1)
        XCTAssertEqual(row.cpuFid, 0xA0)
        XCTAssertEqual(row.cpuDfsId, 0x08)
        XCTAssertEqual(row.cpuVid, 0x48)
        XCTAssertEqual(row.computedSpeedMHz, 4000.0, accuracy: 0.1)
        XCTAssertEqual(row.rawValue, raw)
    }

    func testZen5RoundTrip() {
        let raw: UInt64 = (1 << 63) | (0x50 << 14) | 0x3A6
        let row = PStateRow.from(raw: raw, index: 0, cpuFamily: 0x1A)
        XCTAssertTrue(row.isZen5)
        XCTAssertEqual(row.cpuFid, 0x3A6)
        XCTAssertEqual(row.computedSpeedMHz, 4670.0, accuracy: 0.1)
        XCTAssertEqual(row.rawValue, raw)
    }

    func testZeroDfsIdDoesNotCrash() {
        let raw: UInt64 = (1 << 63) | 0xA0
        let row = PStateRow.from(raw: raw, index: 1, cpuFamily: 0x19)
        XCTAssertEqual(row.computedSpeedMHz, 0.0)
    }

    func testSparseRenoirStatesRetainHardwareIDs() {
        let raw: [UInt64] = [0x80000000478D4878, 0x44580C90, 0x8000000043998E62]
        XCTAssertEqual(LegacySpeedStep.choices(definitions: raw, family: 0x17),
                       [.automatic, .hardware(id: 0, mhz: 3000), .hardware(id: 2, mhz: 1400)])
    }

    func testSingleP0OffersOnlyDistinctControls() {
        XCTAssertEqual(LegacySpeedStep.choices(definitions: [0x80000000478D4878, 0], family: 0x17),
                       [.boost, .base, .lowPower])
        XCTAssertTrue(LegacySpeedStep.choices(definitions: [0, 0], family: 0x17).isEmpty)
    }

    func testLegacyDividerUsesAllSixBits() {
        let raw: UInt64 = (1 << 63) | (0x28 << 8) | 200
        let row = PStateRow.from(raw: raw, index: 2, cpuFamily: 0x17)
        XCTAssertEqual(row.rawValue, raw)
        XCTAssertEqual(row.computedSpeedMHz, 1000)
    }

    func testGPUStatsPreserveZeroAndFillMissingKeysFromNextDevice() {
        var stats = GPUStatistics()
        stats.merge(["Temperature(C)": 51, "Device Utilization %": 0])
        XCTAssertFalse(stats.complete)
        stats.merge(["Temperature(C)": 60, "Device Utilization %": 20, "Total Power(W)": 7,
                     "inUseVidMemoryBytes": 1024, "Fan Speed(RPM)": 1200])
        XCTAssertTrue(stats.complete)
        XCTAssertEqual(stats.temperature, 51)
        XCTAssertEqual(stats.utilization, 0)
        XCTAssertEqual(stats.power, 7)
        XCTAssertEqual(stats.vram, 1024)
        XCTAssertEqual(stats.fan, 1200)
    }

    func testCoolGPUDoesNotNeedAFanCounter() {
        var stats = GPUStatistics()
        stats.merge(["Temperature(C)": 42, "Device Utilization %": 1,
                     "Total Power(W)": 5, "inUseVidMemoryBytes": 1024])
        XCTAssertTrue(stats.complete)
        XCTAssertEqual(stats.fan, 0)
    }
}
