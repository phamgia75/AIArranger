// CoreMIDI adapter for the iPad host: engine output -> one destination (the
// Pa5X over USB), every source -> engine input. MIDI 1.0 UMP words both ways.
// HARDWARE VALIDATION PENDING: never run against a real iPad + Pa5X yet.
import CoreMIDI
import Foundation

public final class CoreMidiPorts: MidiSink {
    private var client = MIDIClientRef()
    private var outPort = MIDIPortRef()
    private var inPort = MIDIPortRef()
    private let lock = NSLock()
    private var dest = MIDIEndpointRef()
    /// Same guard as CoreMidiOut (CMakeLists "Hardware-transmission guard"):
    /// AIARR_MIDI_HW_TX=0 blocks every send, so tests never drive a real device.
    public let hardwareTxBlocked = ProcessInfo.processInfo.environment["AIARR_MIDI_HW_TX"] == "0"
    private var inputHandler: (([UInt8]) -> Void)?
    /// Raw bytes from any connected source; called on a CoreMIDI thread.
    public var onInput: (([UInt8]) -> Void)? {
        get { lock.lock(); defer { lock.unlock() }; return inputHandler }
        set { lock.lock(); inputHandler = newValue; lock.unlock() }
    }
    public private(set) var ready = false

    public init() {
        // A device plugged in later (the Pa5X over USB) changes the setup: reconnect inputs.
        guard MIDIClientCreateWithBlock("AIArranger" as CFString, &client, { [weak self] note in
                  if note.pointee.messageID == .msgSetupChanged { self?.connectAllSources() }
              }) == noErr,
              MIDIOutputPortCreate(client, "AIArranger Out" as CFString, &outPort) == noErr,
              MIDIInputPortCreateWithProtocol(client, "AIArranger In" as CFString, ._1_0, &inPort,
                                              { [weak self] list, _ in self?.receive(list) }) == noErr
        else { return }
        ready = true
        connectAllSources()
    }

    /// Every current source -> input. Disconnect first so a source is never connected twice.
    public func connectAllSources() {
        guard ready else { return }
        for i in 0..<MIDIGetNumberOfSources() {
            let src = MIDIGetSource(i)
            MIDIPortDisconnectSource(inPort, src)
            MIDIPortConnectSource(inPort, src, nil)
        }
    }

    deinit { if client != 0 { MIDIClientDispose(client) } }

    public static func destinationNames() -> [String] {
        (0..<MIDIGetNumberOfDestinations()).map { i in
            var name: Unmanaged<CFString>?
            MIDIObjectGetStringProperty(MIDIGetDestination(i), kMIDIPropertyDisplayName, &name)
            return (name?.takeRetainedValue() as String?) ?? "Destination \(i + 1)"
        }
    }

    /// -1 (or out of range) = no destination: output is dropped.
    public func selectDestination(_ index: Int) {
        let d = index >= 0 && index < MIDIGetNumberOfDestinations() ? MIDIGetDestination(index) : 0
        lock.lock(); dest = d; lock.unlock()
    }

    public func send(_ bytes: [UInt8]) {
        lock.lock(); let d = dest; lock.unlock()
        guard d != 0, !hardwareTxBlocked, ready, let word = Self.umpWord(bytes) else { return }
        var list = MIDIEventList()
        var w = word
        let pkt = MIDIEventListInit(&list, ._1_0)
        _ = MIDIEventListAdd(&list, MemoryLayout<MIDIEventList>.size, pkt, 0, 1, &w)   // one word always fits
        MIDISendEventList(outPort, d, &list)
    }

    /// One 1-3 byte MIDI 1.0 message -> a 32-bit UMP word, group 0:
    /// type 2 (channel voice) or type 1 (system common / real time).
    public static func umpWord(_ b: [UInt8]) -> UInt32? {
        guard let st = b.first, st & 0x80 != 0 else { return nil }
        let type: UInt32 = st >= 0xF0 ? 0x1 : 0x2
        let d1 = UInt32(b.count > 1 ? b[1] & 0x7F : 0), d2 = UInt32(b.count > 2 ? b[2] & 0x7F : 0)
        return type << 28 | UInt32(st) << 16 | d1 << 8 | d2
    }

    /// Inverse of umpWord for types 1 and 2; other UMP types (SysEx, MIDI 2.0) -> nil.
    public static func bytes(fromUMP w: UInt32) -> [UInt8]? {
        let type = w >> 28, st = UInt8(w >> 16 & 0xFF), d1 = UInt8(w >> 8 & 0x7F), d2 = UInt8(w & 0x7F)
        guard (type == 1 || type == 2), st & 0x80 != 0 else { return nil }
        switch st & 0xF0 {
        case 0xC0, 0xD0: return [st, d1]
        case 0xF0:
            switch st {
            case 0xF1, 0xF3: return [st, d1]
            case 0xF2: return [st, d1, d2]
            default: return [st]
            }
        default: return [st, d1, d2]
        }
    }

    private func receive(_ list: UnsafePointer<MIDIEventList>) {
        var out: [UInt8] = []
        for packet in list.unsafeSequence() {
            let n = Int(packet.pointee.wordCount)
            withUnsafeBytes(of: packet.pointee.words) { raw in
                for w in raw.bindMemory(to: UInt32.self).prefix(n) {
                    if let b = Self.bytes(fromUMP: w) { out += b }
                }
            }
        }
        if !out.isEmpty { onInput?(out) }
    }
}
