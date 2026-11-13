#pragma once

// Sample Lab waveform view (P1, no audio playback): the whole sample as min/max columns with the loop shaded, and the
// loop seam frame by frame: the last 64 looped frames followed by the first 64 from the loop start, as playback hears
// them. load() reads the WAV and runs on a worker thread; the view only paints what it is given.

#include <juce_gui_basics/juce_gui_basics.h>
#include "media/samplelab/sample_lab_io.h"

namespace ai_arranger::app {

class WaveView : public juce::Component {
public:
    struct Data {
        std::vector<media::samplelab::Peak> all, seam;   // seam: 2 * kSeam frames, the loop end then the loop start
        size_t frames = 0;
        bool looped = false;
        uint32_t loopStart = 0, loopEnd = 0;
    };
    static constexpr uint32_t kSeam = 64;

    // Worker thread only (reads the file). nullptr with *err set when the WAV cannot be read.
    static std::shared_ptr<const Data> load(const std::filesystem::path& wav, std::string* err, size_t columns = 600) {
        media::samplelab::Sample s;
        if (!media::samplelab::readWavFile(wav, &s, err)) return nullptr;
        auto d = std::make_shared<Data>();
        d->frames = s.frames();
        d->all = media::samplelab::overview(s, 0, s.frames(), columns);
        d->looped = s.looped && s.loopEnd <= s.frames() && s.loopEnd - s.loopStart >= kSeam;
        d->loopStart = s.loopStart;
        d->loopEnd = s.loopEnd;
        if (d->looped) {
            d->seam = media::samplelab::overview(s, s.loopEnd - kSeam, s.loopEnd, kSeam);
            const auto start = media::samplelab::overview(s, s.loopStart, s.loopStart + kSeam, kSeam);
            d->seam.insert(d->seam.end(), start.begin(), start.end());
        }
        return d;
    }
    void setData(std::shared_ptr<const Data> d) { data_ = std::move(d); repaint(); }
    const Data* data() const { return data_.get(); }

    void paint(juce::Graphics& g) override {
        g.fillAll(juce::Colour(0xff1c1c22));
        auto r = getLocalBounds().toFloat().reduced(4);
        g.setFont(juce::FontOptions(11.0f));
        if (!data_ || data_->all.empty()) {
            g.setColour(juce::Colour(0xff9090a0));
            g.drawText("Analyze a WAV to see its waveform and loop seam (no playback).", r, juce::Justification::centred);
            return;
        }
        auto top = r.removeFromLeft(r.getWidth() * 0.7f), seam = r.withTrimmedLeft(6);
        const auto& d = *data_;
        if (d.looped) {   // loop region
            const float a = top.getX() + top.getWidth() * float(d.loopStart) / float(d.frames), b = top.getX() + top.getWidth() * float(d.loopEnd) / float(d.frames);
            g.setColour(juce::Colour(0x3340a0f0));
            g.fillRect(a, top.getY(), std::max(1.0f, b - a), top.getHeight());
        }
        trace(g, d.all, top, juce::Colour(0xff6fb0f0));
        g.setColour(juce::Colour(0xff9090a0));
        g.drawText(juce::String(int64_t(d.frames)) + " frames" + (d.looped ? "  loop " + juce::String(d.loopStart) + "-" + juce::String(d.loopEnd) : "  no loop"),
                   top.removeFromTop(14), juce::Justification::left);
        if (d.seam.empty()) { g.drawText("no loop: no seam", seam, juce::Justification::centred); return; }
        trace(g, d.seam, seam, juce::Colour(0xfff0c040));
        g.setColour(juce::Colour(0xffe05050));   // the seam itself
        g.drawVerticalLine(int(seam.getCentreX()), seam.getY(), seam.getBottom());
        g.setColour(juce::Colour(0xff9090a0));
        g.drawText("seam: end | start", seam.removeFromTop(14), juce::Justification::centred);
    }

private:
    static void trace(juce::Graphics& g, const std::vector<media::samplelab::Peak>& p, juce::Rectangle<float> r, juce::Colour c) {
        g.setColour(c);
        const float mid = r.getCentreY(), h = r.getHeight() / 2, w = r.getWidth() / float(p.size());
        for (size_t i = 0; i < p.size(); ++i) {
            const float y0 = mid - h * float(p[i].hi) / 32768.0f, y1 = mid - h * float(p[i].lo) / 32768.0f;
            g.fillRect(r.getX() + float(i) * w, y0, std::max(1.0f, w - 0.5f), std::max(1.0f, y1 - y0));
        }
    }
    std::shared_ptr<const Data> data_;
};

}  // namespace ai_arranger::app
