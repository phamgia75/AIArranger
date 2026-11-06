// P2.6 iPadOS host: the Swift owner of one engine, over the C ABI only.
//
// Threading (header "Threading"): every engine call runs on `queue`, the one
// owner thread. Public methods hop onto it synchronously, so callers on any
// thread (main, CoreMIDI) are safe. The clock timer ticks on the same queue.
import CAIArranger
import Foundation

/// Where engine MIDI output goes (CoreMIDI on device, a recorder in tests).
/// Called on the engine queue with one 1-3 byte short message; must not call
/// back into HostEngine (its methods sync onto that queue).
public protocol MidiSink: AnyObject {
    func send(_ bytes: [UInt8])
}

public struct EngineError: Error, CustomStringConvertible {
    public let code: AiArrError
    public var description: String { "AiArrError(\(code.rawValue))" }
}

/// App lifecycle as the host sees it (SwiftUI ScenePhase maps onto this).
public enum HostPhase { case active, inactive, background }

public final class HostEngine {
    public static let sampleRate: UInt64 = 48_000   // engine_session.cpp clock
    public static let requiredContract: UInt32 = 0x01_01_00

    private let handle: OpaquePointer
    private let queue = DispatchQueue(label: "aiarranger.engine", qos: .userInteractive)
    private static let onQueue = DispatchSpecificKey<Bool>()
    private weak var sink: MidiSink?
    private var timer: DispatchSourceTimer?
    private var clockStart: UInt64 = 0, samplesTicked: UInt64 = 0
    private var outBuf = [AiArrMidiMessage](repeating: AiArrMidiMessage(), count: 64)
    private var sent = 0
    private var sounding = Set<UInt16>()            // channel << 8 | note
    /// Messages handed to the sink so far.
    public var sentMessages: Int { queue.sync { sent } }
    /// Notes sent On and not yet Off (CC 120/123 clear a channel): 0 = nothing stuck.
    public var soundingNotes: Int { queue.sync { sounding.count } }

    /// Same major, at least our minor: the functions this file calls exist.
    public static func isCompatible(_ v: UInt32) -> Bool {
        v >> 16 == requiredContract >> 16 && v >= requiredContract
    }

    public init(sink: MidiSink?) throws {
        guard Self.isCompatible(aiarr_contract_version()) else { throw EngineError(code: AIARR_ERR_UNSUPPORTED) }
        guard let h = aiarr_engine_create() else { throw EngineError(code: AIARR_ERR_INVALID_STATE) }
        // Set up before `self` is complete: a throw after that would run deinit too.
        var r = aiarr_engine_start(h)
        if r == AIARR_OK { r = aiarr_engine_select_midi_input(h, 0) }
        if r == AIARR_OK { r = aiarr_engine_select_midi_output(h, 0) }
        guard r == AIARR_OK else { aiarr_engine_destroy(h); throw EngineError(code: r) }
        handle = h
        self.sink = sink
        queue.setSpecific(key: Self.onQueue, value: true)
    }

    deinit {
        timer?.cancel()
        let teardown = {
            // Silence before teardown so nothing is left sounding on the Pa5X.
            _ = aiarr_engine_transport_stop(self.handle)
            _ = aiarr_engine_panic(self.handle)
            self.pump(0)
            _ = aiarr_engine_stop(self.handle)
            aiarr_engine_destroy(self.handle)
        }
        // The last reference can drop inside a timer fire, i.e. on the queue itself.
        if DispatchQueue.getSpecific(key: Self.onQueue) == true { teardown() } else { queue.sync(execute: teardown) }
    }

    // ── Commands ──
    public func loadStyle(uasf data: Data) throws {
        guard data.count <= Int(UInt32.max) else { throw EngineError(code: AIARR_ERR_UNSUPPORTED) }
        try queue.sync {
            try data.withUnsafeBytes { raw in
                try check(aiarr_engine_load_style_uasf(handle, raw.bindMemory(to: UInt8.self).baseAddress,
                                                       UInt32(data.count)))
            }
        }
    }
    public func play() throws { try queue.sync { try check(aiarr_engine_transport_start(handle)) } }
    public func stopPlayback() throws { try queue.sync { try check(aiarr_engine_transport_stop(handle)) } }
    public func setTempo(_ bpm: UInt32) throws { try queue.sync { try check(aiarr_engine_set_tempo(handle, bpm)) } }
    /// Main A-D = 0-3.
    public func selectVariation(_ index: Int32) throws { try queue.sync { try check(aiarr_engine_set_variation(handle, index)) } }
    public func setChordMode(_ mode: AiArrChordMode) throws {
        try queue.sync { try check(aiarr_engine_set_chord_mode(handle, Int32(mode.rawValue))) }
    }
    public func panic() throws { try queue.sync { try check(aiarr_engine_panic(handle)); pump(0) } }

    /// Raw MIDI from the keyboard (chords below the split). Split into <=256-byte
    /// calls; a message crossing a split point may be dropped by the engine.
    public func inject(midi bytes: [UInt8]) throws {
        try queue.sync {
            var i = 0
            while i < bytes.count {
                let n = min(256, bytes.count - i)
                try bytes[i..<i + n].withUnsafeBufferPointer {
                    try check(aiarr_engine_send_midi_in(handle, $0.baseAddress, UInt32(n)))
                }
                i += n
            }
        }
    }

    public func snapshot() -> AiArrSnapshot {
        queue.sync {
            var s = AiArrSnapshot()
            _ = aiarr_engine_snapshot(handle, &s)
            return s
        }
    }
    public var lifecycle: AiArrLifecycleState { queue.sync { aiarr_engine_lifecycle_state(handle) } }
    public var isClockRunning: Bool { queue.sync { timer != nil } }

    // ── Time ──
    /// Deterministic advance for tests: tick in 10 ms steps, draining output.
    public func advance(seconds: Double) {
        queue.sync {
            var left = UInt64(seconds * Double(Self.sampleRate))
            while left > 0 { let n = min(left, 480); pump(UInt32(n)); left -= n }
        }
    }

    /// Wall-clock ticking on the engine queue (1 ms timer). Ticks the samples
    /// that really elapsed, so a late timer fire does not slow the tempo.
    public func startClock() {
        queue.sync {
            guard timer == nil else { return }
            clockStart = DispatchTime.now().uptimeNanoseconds
            samplesTicked = 0
            let t = DispatchSource.makeTimerSource(flags: .strict, queue: queue)
            t.schedule(deadline: .now(), repeating: .milliseconds(1), leeway: .microseconds(100))
            t.setEventHandler { [weak self] in   // nil once deinit has begun
                guard let self else { return }
                let due = (DispatchTime.now().uptimeNanoseconds - self.clockStart) * Self.sampleRate / 1_000_000_000
                let n = min(due - self.samplesTicked, UInt64(UInt32.max))
                self.samplesTicked += n
                self.pump(UInt32(n))
            }
            timer = t
            t.resume()
        }
    }
    public func stopClock() {
        queue.sync { timer?.cancel(); timer = nil }
    }

    /// iPadOS suspends a backgrounded MIDI-only app, so going to the background
    /// stops playback and silences every channel; nothing resumes by itself.
    /// `inactive` (Control Center, a split-view resize) keeps playing.
    public func handle(phase: HostPhase) {
        switch phase {
        case .background:
            queue.sync {
                _ = aiarr_engine_transport_stop(handle)
                _ = aiarr_engine_panic(handle)
                pump(0)
                pump(0)   // the transport stop is applied on a tick; flush what it sent
            }
            stopClock()
        case .active:
            startClock()
        case .inactive:
            break
        }
    }

    // ── Engine queue only ──
    private func pump(_ samples: UInt32) {
        _ = aiarr_engine_tick(handle, samples)
        var count: UInt32 = 0
        repeat {
            count = 0
            let cap = UInt32(outBuf.count)
            guard aiarr_engine_read_midi_out(handle, &outBuf, cap, &count) == AIARR_OK else { return }
            for m in outBuf.prefix(Int(count)) {
                let bytes = [m.status, m.data1, m.data2].prefix(Int(m.length))
                sink?.send(Array(bytes))
                sent += 1
                track(m)
            }
        } while count == UInt32(outBuf.count)
    }

    private func track(_ m: AiArrMidiMessage) {
        guard m.length == 3 else { return }
        let ch = UInt16(m.status & 0x0F)
        switch m.status & 0xF0 {
        case 0x90 where m.data2 > 0: sounding.insert(ch << 8 | UInt16(m.data1))
        case 0x80, 0x90: sounding.remove(ch << 8 | UInt16(m.data1))
        case 0xB0 where m.data1 == 120 || m.data1 == 123: sounding = sounding.filter { $0 >> 8 != ch }
        default: break
        }
    }

    private func check(_ r: AiArrError) throws {
        if r != AIARR_OK { throw EngineError(code: r) }
    }
}
