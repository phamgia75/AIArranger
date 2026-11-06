// P2.6 iPadOS host tests: HostEngine + CoreMidiPorts over the real C ABI.
// Runs natively on macOS and inside the iOS simulator (`simctl spawn`):
//   ipados-host-tests <path to fixtures/synthetic/SYNTH_FULL_SFF2.uasf>
import CAIArranger
import Foundation

setenv("AIARR_MIDI_HW_TX", "0", 1)   // never reach a real device
var passes = 0, failures = 0
func TEST(_ name: String, _ ok: Bool) {
    if ok { passes += 1; print("  PASS: \(name)") } else { failures += 1; print("  FAIL: \(name)") }
}
func throwsCode(_ code: AiArrError, _ f: () throws -> Void) -> Bool {
    do { try f(); return false } catch let e as EngineError { return e.code == code } catch { return false }
}
func ok(_ f: () throws -> Void) -> Bool { do { try f(); return true } catch { print("    \(error)"); return false } }

/// Records engine output and tracks sounding notes (CC 123 clears a channel).
final class Recorder: MidiSink {
    var messages: [[UInt8]] = []
    var sounding = Set<Int>()
    func send(_ b: [UInt8]) {
        messages.append(b)
        guard let st = b.first else { return }
        let ch = Int(st & 0x0F)
        switch st & 0xF0 {
        case 0x90 where b.count == 3 && b[2] > 0: sounding.insert(ch << 8 | Int(b[1]))
        case 0x80, 0x90: if b.count >= 2 { sounding.remove(ch << 8 | Int(b[1])) }
        case 0xB0 where b.count == 3 && (b[1] == 123 || b[1] == 120): sounding = sounding.filter { $0 >> 8 != ch }
        default: break
        }
    }
    var noteOns: [[UInt8]] { messages.filter { $0.count == 3 && $0[0] & 0xF0 == 0x90 && $0[2] > 0 } }
}

print("Test: iPadOS host (HostEngine over C ABI \(String(aiarr_contract_version(), radix: 16)))")
guard CommandLine.arguments.count > 1, let style = FileManager.default.contents(atPath: CommandLine.arguments[1]) else {
    print("usage: ipados-host-tests <SYNTH_FULL_SFF2.uasf>"); exit(2)
}

// ── Contract gate ──
TEST("contract: library is 1.1.0-compatible", HostEngine.isCompatible(aiarr_contract_version()))
TEST("contract: 1.0.x and 2.x are refused, 1.2.0 accepted",
     !HostEngine.isCompatible(0x01_00_02) && !HostEngine.isCompatible(0x02_00_00) && HostEngine.isCompatible(0x01_02_00))

// ── Init / destroy ──
do {
    let rec = Recorder()
    var e: HostEngine? = try HostEngine(sink: rec)
    e!.advance(seconds: 0.01)   // the snapshot is published on a tick
    TEST("init: engine running, host ports live", e!.lifecycle == AIARR_LIFECYCLE_RUNNING &&
         { let s = e!.snapshot(); return s.midiInLive == 1 && s.midiOutLive == 1 }())
    try e!.loadStyle(uasf: style)
    try e!.play()
    e!.advance(seconds: 1)
    TEST("destroy while playing: notes were sounding", !rec.noteOns.isEmpty)
    e = nil
    TEST("destroy while playing: silenced, no stuck notes", rec.sounding.isEmpty &&
         rec.messages.contains { $0.count == 3 && $0[0] & 0xF0 == 0xB0 && $0[1] == 123 })
    var cycles = true
    for _ in 0..<20 {
        let r = Recorder()
        do { let x = try HostEngine(sink: r); try x.loadStyle(uasf: style); try x.play(); x.advance(seconds: 0.2) }
        cycles = cycles && r.sounding.isEmpty
    }
    TEST("init/play/destroy x20: no stuck notes", cycles)
} catch { TEST("init: \(error)", false) }

do {
    let rec = Recorder()
    let e = try HostEngine(sink: rec)

    // ── Style loading ──
    TEST("style: garbage -> UNSUPPORTED", throwsCode(AIARR_ERR_UNSUPPORTED) { try e.loadStyle(uasf: Data([1, 2, 3, 4, 5, 6, 7, 8])) })
    TEST("style: empty -> UNSUPPORTED", throwsCode(AIARR_ERR_UNSUPPORTED) { try e.loadStyle(uasf: Data()) })
    TEST("style: SYNTH_FULL_SFF2.uasf loads", ok { try e.loadStyle(uasf: style) })
    e.advance(seconds: 0.05)
    TEST("style: tempo 92 from the style", e.snapshot().tempoBpm == 92)

    // ── Chord injection (Fingered: F A C below the split, running status) ──
    TEST("chord: inject F A C", ok { try e.inject(midi: [0x90, 41, 100, 45, 100, 48, 100]) })
    e.advance(seconds: 0.05)
    var s = e.snapshot()
    TEST("chord: detected F major", s.chordRoot % 12 == 5 && s.chordType == 0)

    // ── Playback ──
    TEST("play", ok { try e.play() })
    e.advance(seconds: 2)
    s = e.snapshot()
    TEST("play: playing, position moving", s.playing == 1 && s.positionTicks > 0)
    TEST("play: notes reach the sink", !rec.noteOns.isEmpty && e.sentMessages == rec.messages.count)
    TEST("play: soundingNotes matches the recorder", e.soundingNotes == rec.sounding.count)
    TEST("play: bass follows the held F (ch 11)",
         rec.noteOns.contains { $0[0] & 0x0F == 10 && $0[1] % 12 == 5 })
    TEST("load while playing -> INVALID_STATE", throwsCode(AIARR_ERR_INVALID_STATE) { try e.loadStyle(uasf: style) })

    // ── MIDI output handling ──
    TEST("output: every message 1-3 bytes with a status byte",
         rec.messages.allSatisfy { (1...3).contains($0.count) && $0[0] & 0x80 != 0 })
    TEST("output: every message survives the CoreMIDI UMP round trip",
         rec.messages.allSatisfy { CoreMidiPorts.umpWord($0).flatMap(CoreMidiPorts.bytes(fromUMP:)) == $0 })

    // ── Tempo ──
    TEST("tempo 150", ok { try e.setTempo(150) })
    TEST("tempo 10 -> UNSUPPORTED", throwsCode(AIARR_ERR_UNSUPPORTED) { try e.setTempo(10) })
    e.advance(seconds: 0.05)
    TEST("tempo: snapshot 150", e.snapshot().tempoBpm == 150)

    // ── Section selection (Main A-D) ──
    // Start plays Intro A (section 0) first, then hands over to Main A.
    e.advance(seconds: 6)
    let mainA = e.snapshot().section
    TEST("section: Intro A has handed over to Main A", mainA > 0)
    TEST("section: variation 4 -> UNSUPPORTED", throwsCode(AIARR_ERR_UNSUPPORTED) { try e.selectVariation(4) })
    var seen = Set<Int32>([mainA])
    for v: Int32 in [1, 2, 3] {
        _ = ok { try e.selectVariation(v) }
        e.advance(seconds: 4)   // past the next bar line at 150 BPM
        seen.insert(e.snapshot().section)
    }
    TEST("section: Main B, C, D are three different sections", seen.count == 4)
    _ = ok { try e.selectVariation(0) }
    e.advance(seconds: 4)
    TEST("section: back to Main A", e.snapshot().section == mainA)
    TEST("section: still playing", e.snapshot().playing == 1)

    // ── Chord mode ──
    TEST("chord mode Single Finger", ok { try e.setChordMode(AIARR_CHORD_MODE_SINGLE_FINGER) })
    _ = ok { try e.inject(midi: [0x80, 41, 0, 45, 0, 48, 0, 0x90, 43, 100]) }
    e.advance(seconds: 0.05)
    s = e.snapshot()
    TEST("chord: Single Finger G = G major", s.chordRoot % 12 == 7 && s.chordType == 0)
    TEST("inject 300 bytes: split into <=256-byte calls, accepted",
         ok { try e.inject(midi: Array(repeating: [0xB0, 1, 0], count: 100).flatMap { $0 }) })

    // ── Stop and panic ──
    TEST("stop", ok { try e.stopPlayback() })
    e.advance(seconds: 0.05)
    TEST("stop: not playing, no stuck notes", e.snapshot().playing == 0 && rec.sounding.isEmpty)
    let before = rec.messages.count
    TEST("panic", ok { try e.panic() })
    let cc = rec.messages[before...].filter { $0.count == 3 && $0[0] & 0xF0 == 0xB0 }
    TEST("panic: CC 120 + 123 on all 16 channels",
         Set(cc.filter { $0[1] == 123 }.map { $0[0] & 0x0F }).count == 16 &&
         Set(cc.filter { $0[1] == 120 }.map { $0[0] & 0x0F }).count == 16)

    // ── App lifecycle ──
    _ = ok { try e.play() }
    e.handle(phase: .active)
    TEST("lifecycle active: wall clock running", e.isClockRunning)
    Thread.sleep(forTimeInterval: 0.1)   // a new Start restarts the position
    let p0 = e.snapshot().positionTicks
    Thread.sleep(forTimeInterval: 0.5)
    TEST("lifecycle active: position advances in real time", e.snapshot().positionTicks > p0)
    e.handle(phase: .inactive)
    TEST("lifecycle inactive: keeps playing", e.isClockRunning && e.snapshot().playing == 1)
    e.handle(phase: .background)
    s = e.snapshot()
    TEST("lifecycle background: stopped, clock off, no stuck notes",
         s.playing == 0 && !e.isClockRunning && rec.sounding.isEmpty)
    TEST("soundingNotes agrees with the recorder (0)", e.soundingNotes == 0)
    let p1 = e.snapshot().positionTicks
    Thread.sleep(forTimeInterval: 0.2)
    TEST("lifecycle background: engine time frozen", e.snapshot().positionTicks == p1)
    e.handle(phase: .active)
    TEST("lifecycle foreground: clock back, playback does not resume by itself",
         e.isClockRunning && e.snapshot().playing == 0)
    e.stopClock()
} catch { TEST("engine: \(error)", false) }

// ── CoreMIDI adapter (no device; HW TX blocked) ──
do {
    let ports = CoreMidiPorts()
    TEST("coremidi: AIARR_MIDI_HW_TX=0 blocks hardware sends", ports.hardwareTxBlocked)
    ports.selectDestination(-1)
    ports.send([0x90, 60, 100])   // no destination: dropped, must not crash
    ports.selectDestination(9999)
    ports.send([0x80, 60, 0])
    print("  info: CoreMIDI client ready=\(ports.ready), destinations=\(CoreMidiPorts.destinationNames().count)")
    TEST("ump: Note On ch 3, group 0 = 0x2092_3C64", CoreMidiPorts.umpWord([0x92, 60, 100]) == 0x2092_3C64)
    TEST("ump: clock F8 is a type-1 word", CoreMidiPorts.umpWord([0xF8]) == 0x10F8_0000 &&
         CoreMidiPorts.bytes(fromUMP: 0x10F8_0000) == [0xF8])
    TEST("ump: program change keeps 2 bytes", CoreMidiPorts.bytes(fromUMP: CoreMidiPorts.umpWord([0xC1, 5])!) == [0xC1, 5])
    TEST("ump: data bytes are masked, non-status refused",
         CoreMidiPorts.umpWord([0x90, 0xFF, 0x80]) == 0x2090_7F00 && CoreMidiPorts.umpWord([0x40]) == nil)
    TEST("ump: SysEx / MIDI 2.0 words ignored", CoreMidiPorts.bytes(fromUMP: 0x3000_0000) == nil &&
         CoreMidiPorts.bytes(fromUMP: 0x4090_0000) == nil)
}

print("\n\(passes) passed, \(failures) failed")
exit(failures == 0 ? 0 : 1)
