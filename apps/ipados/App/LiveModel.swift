// UI state for the live screen. Owns the engine and the CoreMIDI ports.
import CAIArranger
import Foundation
import SwiftUI

@MainActor
final class LiveModel: ObservableObject {
    @Published private(set) var snap = AiArrSnapshot()
    @Published private(set) var status = ""
    @Published private(set) var destinations: [String] = []
    @Published var importing = false   // file importer shown
    @Published var destination = -1 { didSet { ports.selectDestination(destination) } }
    @Published var chordMode = 0 {
        didSet { act { try $0.setChordMode(AiArrChordMode(rawValue: UInt32(chordMode))) } }
    }

    let ports = CoreMidiPorts()
    private(set) var engine: HostEngine?
    private var refresh: Timer?

    init() {
        do {
            let e = try HostEngine(sink: ports)
            engine = e
            ports.onInput = { [weak e] bytes in try? e?.inject(midi: bytes) }
            if let url = Bundle.main.url(forResource: "SYNTH_FULL_SFF2", withExtension: "uasf") {
                try e.loadStyle(uasf: Data(contentsOf: url))
                status = "Style: SYNTH_FULL_SFF2"
            }
            e.startClock()
        } catch { status = "Engine unavailable: \(error)" }
        refreshDestinations()
        refresh = Timer.scheduledTimer(withTimeInterval: 0.1, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.snap = self?.engine?.snapshot() ?? AiArrSnapshot() }
        }
        if ProcessInfo.processInfo.environment["AIARR_SELFTEST"] == "1" { selfTest() }
    }

    @discardableResult
    func act(_ f: (HostEngine) throws -> Void) -> Bool {
        guard let engine else { return false }
        do { try f(engine); return true } catch { status = "\(error)"; return false }
    }

    func refreshDestinations() {
        destinations = CoreMidiPorts.destinationNames()
        if destination >= destinations.count { destination = -1 }
        ports.selectDestination(destination)   // endpoint refs change when a device is replugged
        ports.connectAllSources()
    }

    func loadStyle(from url: URL) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            if act({ try $0.loadStyle(uasf: data) }) {
                status = "Style: \(url.deletingPathExtension().lastPathComponent)"
            }
        } catch { status = "Cannot read \(url.lastPathComponent): \(error.localizedDescription)" }
    }

    func phase(_ p: ScenePhase) {
        let hp: HostPhase = p == .background ? .background : p == .active ? .active : .inactive
        engine?.handle(phase: hp)
        if hp == .background, let s = engine?.snapshot() {
            log("LIFECYCLE background playing=\(s.playing) stuck=\(engine?.soundingNotes ?? -1) sent=\(engine?.sentMessages ?? 0)")
        }
    }

    /// AIARR_SELFTEST=1 (simulator CI): drive the real model like a user, then
    /// stay up so the harness can send the app to the background.
    private func selfTest() {
        Task { @MainActor in
            guard let e = engine else { log("SELFTEST FAIL no engine"); return }
            var fails: [String] = []
            func check(_ name: String, _ ok: Bool) { if !ok { fails.append(name) } }
            act { try $0.inject(midi: [0x90, 41, 100, 45, 100, 48, 100]) }   // F major
            act { try $0.play() }
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            var s = e.snapshot()
            check("playing", s.playing == 1)
            check("chord F", s.chordRoot % 12 == 5 && s.chordType == 0)
            check("output sent", e.sentMessages > 0)
            act { try $0.setTempo(140) }
            try? await Task.sleep(nanoseconds: 300_000_000)
            check("tempo 140", e.snapshot().tempoBpm == 140)
            act { try $0.selectVariation(1) }
            try? await Task.sleep(nanoseconds: 300_000_000)
            s = e.snapshot()
            check("still playing", s.playing == 1)
            log(fails.isEmpty ? "SELFTEST PASS sent=\(e.sentMessages)" : "SELFTEST FAIL \(fails)")
        }
    }

    private func log(_ line: String) { FileHandle.standardError.write(Data((line + "\n").utf8)) }
}
