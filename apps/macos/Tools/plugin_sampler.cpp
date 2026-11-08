// aiarranger-plugin-sampler (docs/SOUND_CONVERSION_ROADMAP.md S7): render notes from an AU or VST3
// instrument the user is licensed for, offline, into a Pa5X package (media/sound/package_writer.h).
// Opens no audio device and no MIDI port: the plugin is driven by processBlock with an in-memory
// MidiBuffer. The user is responsible for the plugin licence permitting sampled use.
//
//   aiarranger-plugin-sampler list
//   aiarranger-plugin-sampler render --plugin NAME --name "Sound Name" --out DIR
//        [--program N] [--low 36] [--high 96] [--step 3] [--vel 100 | --vel 40,80,120] [--splits 60,100]
//        [--hold 2.0] [--release 1.0] [--loop on] [--stereo on]
// --vel: one velocity layer per rendered velocity (up to 24); --splits: lowest velocity of layers
// 2..N (default: halfway). --loop on: loop the held part. --stereo on: keep L/R (default: mono sum).
// Exit 0 = done, 1 = usage/plugin/package error.

#include "media/sound/package_writer.h"
#include "media/sound/sound_recipe.h"
#include "media/sound/rendered_source.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>
#include <map>

using namespace ai_arranger::media::sound;

static juce::OwnedArray<juce::PluginDescription> instruments(juce::AudioPluginFormatManager& fm) {
    juce::OwnedArray<juce::PluginDescription> out;
    for (auto* fmt : fm.getFormats())
        for (const auto& id : fmt->searchPathsForPlugins(fmt->getDefaultLocationsToSearch(), true, false)) {
            juce::OwnedArray<juce::PluginDescription> d;
            fmt->findAllTypesForFile(d, id);
            for (auto* p : d)
                if (p->isInstrument) out.add(new juce::PluginDescription(*p));
        }
    return out;
}

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI juceInit;
    juce::AudioPluginFormatManager fm;
    fm.addDefaultFormats();
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "list") {
        for (auto* p : instruments(fm))
            std::printf("%-6s  %-32s  %s\n", p->pluginFormatName.toRawUTF8(), p->name.toRawUTF8(), p->manufacturerName.toRawUTF8());
        return 0;
    }
    std::map<std::string, std::string> a;
    for (int i = 2; i + 1 < argc; i += 2) a[argv[i]] = argv[i + 1];
    if (cmd != "render" || !a.count("--plugin") || !a.count("--name") || !a.count("--out") || (argc % 2) != 0) {
        std::fprintf(stderr, "usage: aiarranger-plugin-sampler list\n"
                             "       aiarranger-plugin-sampler render --plugin NAME --name \"Sound Name\" --out DIR\n"
                             "            [--program N] [--low 36] [--high 96] [--step 3] [--vel 100|40,80,120] [--splits 60,100]\n"
                             "            [--hold 2.0] [--release 1.0] [--loop on] [--stereo on]\n");
        return 1;
    }
    auto num = [&](const char* k, double d) { return a.count(k) ? std::atof(a[k].c_str()) : d; };
    RenderSpec spec;
    spec.name = a["--name"];
    spec.lowRoot = static_cast<int>(num("--low", 36));
    spec.highRoot = static_cast<int>(num("--high", 96));
    spec.step = static_cast<int>(num("--step", 3));
    if (a.count("--splits"))
        for (const auto& v : juce::StringArray::fromTokens(a["--splits"], ",", ""))
            spec.splits.push_back(static_cast<uint8_t>(juce::jlimit(1, 127, v.getIntValue())));
    const bool stereo = a.count("--stereo") && a["--stereo"] == "on";
    spec.velocities.clear();
    for (const auto& v : juce::StringArray::fromTokens(a.count("--vel") ? a["--vel"] : "100", ",", ""))
        spec.velocities.push_back(static_cast<uint8_t>(juce::jlimit(1, 127, v.getIntValue())));
    const double hold = num("--hold", 2.0), release = num("--release", 1.0);
    if (a.count("--loop") && a["--loop"] == "on") spec.loopHoldSec = hold;
    if (hold <= 0 || release < 0 || hold + release > 21) { std::fprintf(stderr, "hold + release must be 0-21 s\n"); return 1; }

    std::unique_ptr<juce::PluginDescription> desc;
    for (auto* p : instruments(fm))
        if (p->name.equalsIgnoreCase(juce::String(a["--plugin"])) || p->fileOrIdentifier == juce::String(a["--plugin"])) {
            desc = std::make_unique<juce::PluginDescription>(*p);
            break;
        }
    if (!desc) { std::fprintf(stderr, "no instrument plugin named '%s' (see: list)\n", a["--plugin"].c_str()); return 1; }

    const double rate = spec.rate;
    const int block = 512;
    juce::String err;
    auto plugin = fm.createPluginInstance(*desc, rate, block, err);
    if (!plugin) { std::fprintf(stderr, "cannot load %s: %s\n", desc->name.toRawUTF8(), err.toRawUTF8()); return 1; }
    plugin->enableAllBuses();
    plugin->setNonRealtime(true);
    plugin->prepareToPlay(rate, block);
    if (a.count("--program")) plugin->setCurrentProgram(static_cast<int>(num("--program", 0)));
    const int chans = juce::jmax(plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
    const int outs = plugin->getTotalNumOutputChannels();
    if (outs < 1) { std::fprintf(stderr, "%s has no audio output\n", desc->name.toRawUTF8()); return 1; }
    spec.channels = stereo && outs >= 2 ? 2 : 1;
    if (stereo && outs < 2) std::fprintf(stderr, "warning: %s has %d output channel(s); writing mono\n", desc->name.toRawUTF8(), outs);

    const RenderFn render = [&](uint8_t note, uint8_t vel) {
        plugin->reset();
        juce::AudioBuffer<float> buf(chans, block);
        juce::MidiBuffer none;
        for (int i = 0; i < static_cast<int>(0.1 * rate) / block; ++i) { buf.clear(); plugin->processBlock(buf, none); }   // settle
        const int64_t offAt = static_cast<int64_t>(hold * rate), total = static_cast<int64_t>((hold + release) * rate);
        std::vector<float> x;
        x.reserve(static_cast<size_t>(total + block) * spec.channels);
        for (int64_t pos = 0; pos < total; pos += block) {
            buf.clear();
            juce::MidiBuffer m;
            if (pos == 0) m.addEvent(juce::MidiMessage::noteOn(1, note, vel), 0);
            if (offAt >= pos && offAt < pos + block) m.addEvent(juce::MidiMessage::noteOff(1, note), static_cast<int>(offAt - pos));
            plugin->processBlock(buf, m);
            for (int i = 0; i < block; ++i) {
                if (spec.channels == 2) { x.push_back(buf.getSample(0, i)); x.push_back(buf.getSample(1, i)); continue; }
                float s = 0;
                for (int c = 0; c < outs; ++c) s += buf.getSample(c, i);
                x.push_back(s / static_cast<float>(outs));   // mono: sum of all outputs
            }
        }
        juce::MidiBuffer off;
        off.addEvent(juce::MidiMessage::allNotesOff(1), 0);
        buf.clear();
        plugin->processBlock(buf, off);
        return x;
    };

    // Raw plugin parameters for the Sound Recipe (EXTRACTED, unmapped).
    // shortcut: first 64 parameters only; large synths expose thousands, add a filter if owners need more.
    std::vector<std::pair<std::string, std::string>> params;
    const auto& all = plugin->getParameters();
    for (int i = 0; i < all.size() && i < 64; ++i)
        params.push_back({all[i]->getName(40).toStdString(), all[i]->getCurrentValueAsText().toStdString()});
    RenderReport rep;
    const Instrument in = buildFromRenders(spec, render, &rep);
    plugin->releaseResources();
    for (const auto& w : rep.warnings) std::fprintf(stderr, "warning: %s\n", w.c_str());
    if (in.samples.empty()) return 1;
    std::string notes = "Source: " + desc->name.toStdString() + " (" + desc->manufacturerName.toStdString() + ", " +
                        desc->pluginFormatName.toStdString() + ") rendered offline at 48 kHz, " + (spec.channels == 2 ? "stereo" : "mono sum") + ", " +
                        (spec.loopHoldSec > 0 ? "held part looped" : "one-shot samples") + ".\n"
                        "Rights: the user must hold a licence that permits sampling this instrument.\n"
                        "Measured pitch per sample (cents from equal temperament), loop seam match (1 = identical),\n"
                        "release (note-off to -60 dB, from the render):\n";
    std::vector<double> rel;
    for (const auto& r : rep.rows) {
        char l[128];
        std::snprintf(l, sizeof l, "  note %3u vel %3u  %+7.2f  %7zu frames  loop %s  release %s\n", r.note, r.velocity, r.cents, r.frames,
                      std::isnan(r.loopMatch) ? "-" : std::to_string(r.loopMatch).substr(0, 5).c_str(),
                      std::isnan(r.releaseSec) ? "-" : (std::to_string(r.releaseSec).substr(0, 4) + " s").c_str());
        notes += l;
        if (!std::isnan(r.releaseSec)) rel.push_back(r.releaseSec);
    }
    if (!rel.empty()) {
        std::sort(rel.begin(), rel.end());
        char l[320];
        std::snprintf(l, sizeof l, "Release: median %.2f s (range %.2f-%.2f s). The loop cuts the recorded release and the Pa5X does\n"
                      "not import envelopes (TX16Wx guide p.85): set Sound Edit > Amp > Envelope Generator release by hand\n"
                      "(manual p.1130) to approximate it.\n", rel[rel.size() / 2], rel.front(), rel.back());
        notes += l;
    } else if (spec.loopHoldSec > 0) {
        notes += "Release: not measured (render ended before -60 dB; raise --release).\n";
    }
    for (const auto& w : rep.warnings) notes += "WARNING: " + w + "\n";
    const SoundRecipe recipe = recipeFromRender(spec, rep, params);
    if (!validate(recipe).empty()) { std::fprintf(stderr, "internal: recipe breaks its provenance rules\n"); return 1; }
    notes += "\n" + formatRecipe(recipe);
    if (all.size() > 64) notes += "      (" + std::to_string(all.size() - 64) + " more plugin parameters not listed)\n";
    std::string perr;
    const std::string out = writePackage(in, a["--out"], notes, &perr);
    if (out.empty()) { std::fprintf(stderr, "%s\n", perr.c_str()); return 1; }
    std::fputs(out.c_str(), stdout);
    return 0;
}
