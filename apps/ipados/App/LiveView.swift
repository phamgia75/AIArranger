// The single live screen: transport, Main A-D, tempo, chord mode, MIDI output.
import CAIArranger
import SwiftUI
import UniformTypeIdentifiers

struct LiveView: View {
    @ObservedObject var model: LiveModel
    private let names = ["Main A", "Main B", "Main C", "Main D"]

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("AIArranger").font(.largeTitle.bold())
            Text(model.status).font(.callout).foregroundStyle(.secondary)
            HStack {
                Text(model.snap.playing == 1 ? "Playing" : "Stopped").font(.title2.bold())
                Spacer()
                Text("Section \(model.snap.section)  ·  Chord \(chordName)  ·  \(model.snap.tempoBpm) BPM")
                    .monospacedDigit()
            }
            HStack(spacing: 12) {
                Button("Start") { model.act { try $0.play() } }.buttonStyle(.borderedProminent)
                Button("Stop") { model.act { try $0.stopPlayback() } }.buttonStyle(.bordered)
                Button("PANIC", role: .destructive) { model.act { try $0.panic() } }.buttonStyle(.bordered)
            }
            HStack(spacing: 12) {
                ForEach(0..<4, id: \.self) { i in
                    Button(names[i]) { model.act { try $0.selectVariation(Int32(i)) } }.buttonStyle(.bordered)
                }
            }
            Stepper("Tempo \(model.snap.tempoBpm)",
                    onIncrement: { model.act { try $0.setTempo(model.snap.tempoBpm + 1) } },
                    onDecrement: { model.act { try $0.setTempo(model.snap.tempoBpm - 1) } })
            Picker("Chord mode", selection: $model.chordMode) {
                Text("Fingered").tag(0); Text("Fingered on Bass").tag(1); Text("Single Finger").tag(2)
            }.pickerStyle(.segmented)
            HStack {
                Picker("MIDI out", selection: $model.destination) {
                    Text("None").tag(-1)
                    ForEach(Array(model.destinations.enumerated()), id: \.offset) { i, n in Text(n).tag(i) }
                }
                Button("Refresh") { model.refreshDestinations() }
                Spacer()
                Button("Load Style (.uasf)…") { model.importing = true }
            }
            Spacer()
        }
        .padding(24)
        .fileImporter(isPresented: $model.importing, allowedContentTypes: [UTType(filenameExtension: "uasf") ?? .data]) {
            if case .success(let url) = $0 { model.loadStyle(from: url) }
        }
    }

    private var chordName: String {
        let roots = ["C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"]
        return model.snap.chordType == 11 /* ChordType::NoChord */ ? "–" : "\(roots[Int(model.snap.chordRoot % 12)]) (\(model.snap.chordType))"
    }
}
