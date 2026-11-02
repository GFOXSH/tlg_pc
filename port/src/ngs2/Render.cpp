#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static constexpr double PHASE_ONE = 4294967296.0;
static constexpr std::uint64_t PHASE_FRACTION = 0xffffffff;

static const std::uint8_t* FrameAt(const Ngs2Voice& voice, const Ngs2Block& block, std::uint32_t frame) {
    const std::size_t frameBytes = voice.channels * sizeof(std::int16_t);
    if (block.data == nullptr) return nullptr;
    if (block.streaming) {
        for (const auto& piece : block.pieces) {
            if (frame >= piece.firstFrame && frame - piece.firstFrame < piece.frames) return piece.data + (frame - piece.firstFrame) * frameBytes;
        }
        return nullptr;
    }
    return block.data + (static_cast<std::size_t>(block.info.num_skip_samples) + frame) * frameBytes;
}

static void ReadPcm(const Ngs2Voice& voice, const std::uint8_t* frame, float* out) {
    if (frame == nullptr) {
        for (std::uint32_t channel = 0; channel < voice.channels; channel++) out[channel] = 0.0f;
        return;
    }
    for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
        std::int16_t sample;
        std::memcpy(&sample, frame + channel * sizeof(sample), sizeof(sample));
        out[channel] = sample / 32768.0f;
    }
}

static void ReadFrames(Ngs2Voice& voice, float* current, float* next) {
    auto& block = voice.blocks.front();
    ReadPcm(voice, FrameAt(voice, block, block.cursor), current);
    if (block.streaming && block.cursor + 1 >= block.availableFrames) std::memcpy(next, current, voice.channels * sizeof(float));
    else if (block.cursor + 1 < block.info.num_samples) ReadPcm(voice, FrameAt(voice, block, block.cursor + 1), next);
    else if (block.info.num_repeats != 0) ReadPcm(voice, FrameAt(voice, block, 0), next);
    else if (voice.blocks.size() > 1) ReadPcm(voice, FrameAt(voice, voice.blocks[1], 0), next);
    else std::memcpy(next, current, voice.channels * sizeof(float));
}

bool Ngs2FinishBlock(Ngs2Voice& voice) {
    auto& block = voice.blocks.front();
    const bool repeat = block.info.num_repeats != 0;
    if (repeat) {
        if (block.info.num_repeats != UINT32_MAX) block.info.num_repeats--;
        block.numRepeated++;
        block.cursor = 0;
        block.started = false;
        if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) {
            block.dataCursor = 0;
            Ngs2RestartAtrac9(voice);
        }
    }
    const Ngs2VoiceCallbackInfo info{
        voice.callbackData,
        reinterpret_cast<Ngs2Handle>(&voice),
        repeat ? SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_REPEAT : SCE_NGS2_VOICE_CALLBACK_FLAG_BLOCK_END,
        0,
        block.info.user_data,
        block.data,
        static_cast<std::uint32_t>(block.info.data_size),
        block.numRepeated,
        0,
        0,
    };
    if (!repeat) {
        voice.waveformEnd = voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9 ? block.data + block.dataCursor : block.streaming ? Ngs2StreamEnd(voice, block) : FrameAt(voice, block, block.info.num_samples);
        voice.blocks.pop_front();
    }
    const auto revision = voice.waveformRevision;
    if (voice.callback != nullptr && (voice.callbackFlags & info.flag) != 0) {
        const Ngs2Unlocked unlocked;
        voice.callback(&info);
    }
    return voice.waveformRevision == revision && voice.state == Ngs2PlayState::Playing;
}

static void Advance(Ngs2Voice& voice, std::uint64_t frames) {
    while (frames != 0 && voice.state == Ngs2PlayState::Playing && !voice.blocks.empty()) {
        auto& block = voice.blocks.front();
        const std::uint64_t limit = block.streaming ? std::min<std::uint64_t>(block.info.num_samples, block.availableFrames) : block.info.num_samples;
        if (block.cursor >= limit && block.cursor != block.info.num_samples && !(block.streaming && !voice.acceptsBlocks)) break;
        const auto step = std::min<std::uint64_t>(frames, limit - block.cursor);
        block.cursor += static_cast<std::uint32_t>(step);
        voice.decodedSamples += step;
        if (voice.waveformType != SCE_NGS2_WAVEFORM_TYPE_ATRAC9) voice.decodedBytes += step * voice.channels * sizeof(std::int16_t);
        frames -= step;
        if (block.streaming) {
            while (block.pieces.size() > 1 && block.cursor >= block.pieces.front().firstFrame + block.pieces.front().frames) block.pieces.erase(block.pieces.begin());
        }
        if (block.cursor == block.info.num_samples || (block.streaming && !voice.acceptsBlocks && block.cursor == block.availableFrames)) {
            if (!Ngs2FinishBlock(voice)) return;
        }
    }
}

static void ConsumeSamples(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t systemRate) {
    const auto revision = voice.waveformRevision;
    const auto step = static_cast<std::uint64_t>(std::llround(voice.sampleRate * static_cast<double>(voice.pitch) / systemRate * PHASE_ONE));
    std::uint32_t written = 0;
    float current[NGS2_MAX_CHANNELS];
    float next[NGS2_MAX_CHANNELS];
    for (; written < grain && voice.state == Ngs2PlayState::Playing && !voice.blocks.empty() && voice.waveformRevision == revision; written++) {
        ReadFrames(voice, current, next);
        const auto fraction = static_cast<float>(static_cast<double>(voice.phase) / PHASE_ONE);
        for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
            voice.samples[channel * grain + written] = std::lerp(current[channel], next[channel], fraction);
        }
        voice.phase += step;
        Advance(voice, voice.phase >> 32);
        voice.phase &= PHASE_FRACTION;
    }
    voice.hasSamples = written != 0;
}

static bool HasHistory(const Ngs2Filter& filter) {
    if (!filter.enabled) return false;
    for (std::uint32_t channel = 0; channel < filter.history.size(); channel++) {
        const auto& h = filter.history[channel];
        if (channel < 64 && (filter.bypassMask & (std::uint64_t{1} << channel)) != 0) continue;
        if (h.x1 != 0.0 || h.x2 != 0.0 || h.y1 != 0.0 || h.y2 != 0.0) return true;
    }
    return false;
}
static bool HasFiltersAt(const Ngs2Voice& voice, std::uint32_t location) {
    return std::any_of(voice.filters.begin(), voice.filters.end(), [location](const Ngs2Filter& f) { return f.enabled && f.location == location; });
}
static bool HasFilterHistoryAt(const Ngs2Voice& voice, std::uint32_t location) {
    return std::any_of(voice.filters.begin(), voice.filters.end(), [location](const Ngs2Filter& f) { return f.location == location && HasHistory(f); });
}
static bool HasAnyFilterHistory(const Ngs2Voice& voice) {
    return std::any_of(voice.filters.begin(), voice.filters.end(), HasHistory);
}
static void ApplyFilter(Ngs2Filter& filter, std::vector<float>& samples, std::uint32_t channels, std::uint32_t grain) {
    for (std::uint32_t channel = 0; channel < channels; channel++) {
        if (channel < 64 && (filter.bypassMask & (std::uint64_t{1} << channel)) != 0) continue;
        auto& h = filter.history[channel];
        for (std::uint32_t i = 0; i < grain; i++) {
            auto& sample = samples[channel * grain + i];
            const double x = sample;
            double y = filter.b0 * x + filter.b1 * h.x1 + filter.b2 * h.x2 - filter.a1 * h.y1 - filter.a2 * h.y2;
            if (std::abs(y) < 1e-20) y = 0.0;
            h = {x, h.x1, y, h.y1};
            sample = static_cast<float>(y);
        }
    }
}
static void ApplyFiltersAt(Ngs2Voice& voice, std::uint32_t location, std::vector<float>& samples, std::uint32_t grain) {
    for (auto& filter : voice.filters) {
        if (filter.enabled && filter.location == location) ApplyFilter(filter, samples, voice.channels, grain);
    }
}
static float MeasurePeak(const std::vector<float>& samples, std::uint32_t channels, std::uint32_t grain) {
    float peak = 0.0f;
    for (std::uint32_t channel = 0; channel < channels; channel++)
        for (std::uint32_t i = 0; i < grain; i++) peak = std::max(peak, std::abs(samples[channel * grain + i]));
    return peak;
}
static bool HasSignal(const std::vector<float>& samples) {
    return std::any_of(samples.begin(), samples.end(), [](float sample) { return sample != 0.0f; });
}

// Envelope curves and Dynamics processing follow the PS4 Runtime parameter layouts.
static float EnvelopeCurve(std::uint32_t curve, float x, bool rising) {
    switch (curve) {
        case 0: return x;
        case 1: return (std::pow(10.0f, x) - 1.0f) / 9.0f;
        case 2: return std::log10(1.0f + 9.0f * x);
        case 3: return 0.5f - 0.5f * std::cos(std::numbers::pi_v<float> * x);
        case 4: return 0.5f + std::asin(2.0f * x - 1.0f) / std::numbers::pi_v<float>;
        case 5: return rising ? std::sin(std::numbers::pi_v<float> * 0.5f * x) : 1.0f - std::cos(std::numbers::pi_v<float> * 0.5f * x);
        default: return 0.0f;
    }
}
static bool EnvelopeStep(Ngs2Envelope& e, float& gain) {
    if (e.forward.empty() && !e.releasing) { e.gain = 1.0f; gain = e.gain; return true; }
    const auto& points = e.releasing ? e.release : e.forward;
    while (e.segment < points.size() && (points[e.segment].duration == 0 || e.position >= points[e.segment].duration)) {
        e.start = points[e.segment].height; e.gain = e.start; e.segment++; e.position = 0;
    }
    if (e.segment >= points.size()) {
        if (e.releasing) { gain = e.gain; return false; }
        if (!e.forward.empty() && e.forward.back().height == 0.0f) { e.gain = 0.0f; gain = 0.0f; return false; }
        gain = e.gain; return true;
    }
    const auto& point = points[e.segment];
    const float t = static_cast<float>(e.position) / static_cast<float>(point.duration);
    const float curve = EnvelopeCurve(point.curve, t, point.height > e.start);
    e.gain = point.curve > 5 ? e.start : e.start + (point.height - e.start) * curve;
    e.position++; gain = e.gain; return true;
}
static void ApplyEnvelope(Ngs2Voice& voice, std::uint32_t grain) {
    auto& e = voice.envelope;
    if (e.forward.empty() && (!e.releasing || e.release.empty())) return;
    for (std::uint32_t i = 0; i < grain; i++) {
        float gain = 1.0f;
        if (!EnvelopeStep(e, gain)) {
            for (std::uint32_t c = 0; c < voice.channels; c++)
                std::fill(voice.samples.begin() + c * grain + i, voice.samples.begin() + (c + 1) * grain, 0.0f);
            voice.state = Ngs2PlayState::Stopped; voice.hasSamples = false; return;
        }
        for (std::uint32_t c = 0; c < voice.channels; c++) voice.samples[c * grain + i] *= gain;
    }
}
static void ResetDynamics(Ngs2Dynamics& d) {
    d.gain.fill(1.0f); d.attackCoefficient = 0.0f; d.releaseCoefficient = 0.0f;
}
static void ApplyDynamics(Ngs2Voice& voice, Ngs2Dynamics& d, std::uint32_t grain, std::uint32_t rate) {
    if (!d.enabled) { ResetDynamics(d); return; }
    const auto coefficient = [rate](float seconds) {
        const auto samples = static_cast<std::uint32_t>(static_cast<float>(rate) * seconds);
        return samples != 0 ? std::exp(-1.0f / static_cast<float>(samples)) : 0.0f;
    };
    d.attackCoefficient = coefficient(d.attack); d.releaseCoefficient = coefficient(d.release);
    for (std::uint32_t start = 0; start < grain; start += 32) {
        const std::uint32_t length = std::min(32u, grain - start);
        std::array<float, NGS2_MAX_CHANNELS> levels{};
        for (std::uint32_t c = 0; c < voice.channels; c++) {
            const float* input = voice.samples.data() + c * grain + start;
            float level = 0.0f;
            if (d.rms) {
                for (std::uint32_t i = 0; i < length; i++) level += input[i] * input[i];
                level = std::sqrt(level / static_cast<float>(length));
            } else for (std::uint32_t i = 0; i < length; i++) level = std::max(level, std::abs(input[i]));
            levels[c] = level;
        }
        // Bit 3 keeps LFE independent; otherwise the detector links all channels.
        float linked = 0.0f;
        for (std::uint32_t c = 0; c < voice.channels; c++)
            if (!d.linkAllButLfe || c != 3) linked = std::max(linked, levels[c]);
        for (std::uint32_t c = 0; c < voice.channels; c++)
            if (!d.linkAllButLfe || c != 3) levels[c] = linked;
        for (std::uint32_t c = 0; c < voice.channels; c++) {
            const float detector = levels[c], threshold = d.threshold;
            const float kneeStart = threshold * (1.0f - d.knee * 0.5f), over = detector - kneeStart;
            float target = 1.0f;
            if (d.knee > 0.0f && over > 0.0f && over < threshold - kneeStart)
                target = (kneeStart + over * ((over / (threshold - kneeStart)) * 0.5f * (d.ratio - 1.0f) + 1.0f)) / detector;
            else if (threshold < detector) target = (threshold + (detector - threshold) * d.ratio) / detector;
            float gain = d.gain[c];
            const float smoothing = gain <= target ? d.releaseCoefficient : d.attackCoefficient;
            float* output = voice.samples.data() + c * grain + start;
            for (std::uint32_t i = 0; i < length; i++) { gain = target + smoothing * (gain - target); output[i] *= d.level * gain; }
            d.gain[c] = gain;
        }
    }
}
static void ProcessChorus(Ngs2Voice& voice, Ngs2CustomModuleRuntime& runtime, std::uint32_t grain, std::uint32_t rate) {
    if (!runtime.active || runtime.param.size() != sizeof(Ngs2CustomVoiceChorusParam)) return;
    constexpr std::uint32_t mask = 4095;
    const auto& p = *reinterpret_cast<const Ngs2CustomVoiceChorusParam*>(runtime.param.data());
    const auto phases = std::clamp(p.num_phases, 1u, 4u);
    const float base = static_cast<float>(static_cast<int>(p.delay_time * 0.001f * rate));
    const float depth = base * p.modulation_depth, step = p.modulation_ratio / static_cast<float>(rate);
    const float dry = p.input_level * p.dry_level, wet = p.input_level * p.wet_level;
    const float phase0 = runtime.phase; const std::uint32_t write0 = runtime.write;
    for (std::uint32_t ch = 0; ch < voice.channels; ch++) {
        if ((p.channel_mask & (1u << ch)) != 0) continue;
        float phase = phase0; std::uint32_t write = write0;
        float* lines = runtime.line.data() + static_cast<std::size_t>(ch) * 4u * 4096u;
        for (std::uint32_t i = 0; i < grain; i++) {
            const float original = voice.samples[ch * grain + i]; float input = original, delayed = 0.0f;
            for (std::uint32_t k = 0; k < phases; k++) {
                float ph = phase + static_cast<float>(k) / static_cast<float>(phases); ph -= std::floor(ph);
                const float delay = base + 1.0f + depth * std::sin(2.0f * std::numbers::pi_v<float> * ph);
                const auto di = static_cast<std::uint32_t>(delay); const float frac = delay - static_cast<float>(di);
                float* line = lines + static_cast<std::size_t>(k) * 4096u;
                delayed = frac * line[(write + di + 1u) & mask] + (1.0f - frac) * line[(write + di) & mask];
                line[write] = input + p.feedback_level * delayed; input = delayed;
            }
            voice.samples[ch * grain + i] = dry * original + wet * delayed;
            phase += step; write = (write - 1u) & mask;
        }
    }
    const float phase = phase0 + step * static_cast<float>(grain);
    runtime.phase = phase - std::floor(phase); runtime.write = (write0 - grain) & mask;
}
static float DelayInputGain(const Ngs2Voice& voice, const Ngs2CustomVoiceDelayParam& p, std::uint32_t ch, std::uint32_t type) {
    if ((p.flags & 2u) != 0) return p.input_mix_level[ch];
    if (type == 1) return 1.0f;
    if (voice.channels >= 8) return 0.267f;
    if (voice.channels >= 6) return 0.31623f;
    if (voice.channels >= 2) return 0.70711f;
    return 1.0f;
}
static void ProcessDelay(Ngs2Voice& voice, Ngs2CustomModuleRuntime& r, std::uint32_t grain) {
    if (!r.active || r.param.size() != sizeof(Ngs2CustomVoiceDelayParam) || r.lineLength == 0) return;
    const auto& p = *reinterpret_cast<const Ngs2CustomVoiceDelayParam*>(r.param.data());
    const std::uint32_t length = r.lineLength; float tapMagnitude = 0.0f;
    for (std::uint32_t t = 0; t < p.num_taps; t++) tapMagnitude += std::abs(p.tap[t].tap_level);
    const float feedback = p.feedback_level / std::max(tapMagnitude, 1.0f);
    const float dry = p.input_level * p.dry_level, wet = p.input_level * p.wet_level;
    const bool lowpass = (p.flags & 1u) != 0, mono = r.type == 0;
    for (std::uint32_t i = 0; i < grain; i++) {
        if (mono) {
            float input = 0.0f;
            for (std::uint32_t ch = 0; ch < voice.channels; ch++) if (ch != 3) input += voice.samples[ch * grain + i] * DelayInputGain(voice,p,ch,r.type);
            float delayed = 0.0f;
            for (std::uint32_t t = 0; t < p.num_taps; t++) {
                const auto read = (r.write + length - r.tapDelay[t]) % length;
                delayed += p.tap[t].tap_level * r.line[read];
            }
            if (lowpass) { float& state = r.lowpassState[0]; state = (1.0f-r.lowpassCoefficient)*delayed + r.lowpassCoefficient*state; delayed=state; }
            r.line[r.write] = input + feedback * delayed;
            for (std::uint32_t ch = 0; ch < voice.channels; ch++)
                if (ch != 3 && (p.channel_mask & (1u << ch)) == 0) voice.samples[ch * grain + i] = dry * input + wet * delayed;
        } else {
            for (std::uint32_t ch = 0; ch < voice.channels; ch++) {
                if (ch == 3 || (p.channel_mask & (1u << ch)) != 0) continue;
                const std::size_t off = static_cast<std::size_t>(ch) * length; float delayed = 0.0f;
                for (std::uint32_t t = 0; t < p.num_taps; t++) {
                    const auto read = (r.write + length - r.tapDelay[t]) % length;
                    delayed += p.tap[t].tap_level * r.line[off + read];
                }
                if (lowpass) { float& state = r.lowpassState[ch]; state=(1.0f-r.lowpassCoefficient)*delayed+r.lowpassCoefficient*state; delayed=state; }
                const float original=voice.samples[ch*grain+i];
                r.line[off+r.write]=DelayInputGain(voice,p,ch,r.type)*original+feedback*delayed;
                voice.samples[ch*grain+i]=dry*original+wet*delayed;
            }
        }
        if (++r.write == length) r.write = 0;
    }
}
static void ProcessCustomModules(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t rate) {
    for (std::uint32_t m = 0; m < voice.rack->customModuleIds.size(); m++) {
        const auto id = voice.rack->customModuleIds[m];
        if (id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_CHORUS) ProcessChorus(voice, voice.customModuleRuntime[m], grain, rate);
        else if (id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_DELAY) ProcessDelay(voice, voice.customModuleRuntime[m], grain);
        else if (id == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) {
            if (m < voice.filters.size() && voice.filters[m].enabled) ApplyFilter(voice.filters[m], voice.samples, voice.channels, grain);
        } else Ngs2ProcessUserFxModule(voice, m, grain, rate);
        voice.hasSamples = HasSignal(voice.samples);
    }
}

static void RenderSampler(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t systemRate) {
    const auto channels = voice.channels;
    if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) Ngs2ConsumeAtrac9(voice, grain, systemRate);
    else ConsumeSamples(voice, grain, systemRate);
    if (voice.channels != channels) {
        voice.samples.assign(static_cast<std::size_t>(grain) * voice.channels, 0.0f);
        voice.hasSamples = false; return;
    }
    const bool decodedFrames = voice.hasSamples;
    if (decodedFrames || HasFilterHistoryAt(voice, 0)) {
        ApplyFiltersAt(voice, 0, voice.samples, grain);
        voice.hasSamples = decodedFrames || HasSignal(voice.samples);
    }
    ApplyEnvelope(voice, grain);
    const bool buffered = voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9 &&
                          voice.atrac9.windowCursor * voice.channels < voice.atrac9.window.size();
    if (voice.state == Ngs2PlayState::Playing && voice.blocks.empty() && !buffered &&
        !voice.acceptsBlocks && !HasAnyFilterHistory(voice)) voice.state = Ngs2PlayState::Empty;
}

static void RenderVoice(Ngs2Voice& voice, const std::vector<Ngs2Voice*>& voices, std::uint32_t grain, std::uint32_t systemRate);

struct Ngs2DefaultRow {
    float mono;
    float stereo[2];
    float surround51[6];
    float surround71[8];
};

static constexpr float HALF_POWER = std::numbers::sqrt2_v<float> / 2.0f;
static constexpr float QUARTER_POWER = std::numbers::sqrt2_v<float> / 4.0f;

static constexpr Ngs2DefaultRow MONO_CENTER{1.0f, {HALF_POWER, HALF_POWER}, {0, 0, 1, 0, 0, 0}, {0, 0, 1, 0, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow FRONT_LEFT{HALF_POWER, {1, 0}, {1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow FRONT_RIGHT{HALF_POWER, {0, 1}, {0, 1, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow LFE{0.0f, {0, 0}, {0, 0, 0, 1, 0, 0}, {0, 0, 0, 1, 0, 0, 0, 0}};
static constexpr Ngs2DefaultRow QUAD_LEFT{0.5f, {HALF_POWER, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0}};
static constexpr Ngs2DefaultRow QUAD_RIGHT{0.5f, {0, HALF_POWER}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 0, 0}};
static constexpr Ngs2DefaultRow FIVE_SURROUND_LEFT{0.5f, {0, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0}};
static constexpr Ngs2DefaultRow FIVE_SURROUND_RIGHT{0.5f, {0, 0}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 0, 0}};
static constexpr Ngs2DefaultRow SURROUND_LEFT{0.5f, {0.5f, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 1, 0, 0, 0}};
static constexpr Ngs2DefaultRow SURROUND_RIGHT{0.5f, {0, 0.5f}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 1, 0, 0}};
static constexpr Ngs2DefaultRow BACK_CENTER{QUARTER_POWER, {QUARTER_POWER, QUARTER_POWER}, {0, 0, 0, 0, HALF_POWER, HALF_POWER}, {0, 0, 0, 0, 0, 0, HALF_POWER, HALF_POWER}};
static constexpr Ngs2DefaultRow BACK_LEFT{0.5f, {0.5f, 0}, {0, 0, 0, 0, 1, 0}, {0, 0, 0, 0, 0, 0, 1, 0}};
static constexpr Ngs2DefaultRow BACK_RIGHT{0.5f, {0, 0.5f}, {0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 0, 1}};

static constexpr Ngs2DefaultRow DEFAULT_MAP[NGS2_MAX_CHANNELS][NGS2_MAX_CHANNELS] = {
    {MONO_CENTER},
    {FRONT_LEFT, FRONT_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, LFE},
    {FRONT_LEFT, FRONT_RIGHT, QUAD_LEFT, QUAD_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, FIVE_SURROUND_LEFT, FIVE_SURROUND_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, LFE, SURROUND_LEFT, SURROUND_RIGHT},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, LFE, SURROUND_LEFT, SURROUND_RIGHT, BACK_CENTER},
    {FRONT_LEFT, FRONT_RIGHT, MONO_CENTER, LFE, SURROUND_LEFT, SURROUND_RIGHT, BACK_LEFT, BACK_RIGHT},
};

float Ngs2DefaultLevel(std::uint32_t sourceChannels, std::uint32_t source, std::uint32_t destChannels, std::uint32_t dest) {
    const auto& row = DEFAULT_MAP[sourceChannels - 1][source];
    switch (destChannels) {
        case 1: return row.mono;
        case 2: return row.stereo[dest];
        case 6: return row.surround51[dest];
        case 8: return row.surround71[dest];
        default: ngs2_stop("NGS2: the default channel map into " + std::to_string(destChannels) + " channels is not implemented");
    }
}

static void MixPort(Ngs2Voice& voice, const Ngs2Voice& source, const Ngs2Port& port,
                     const std::vector<float>& sourceSamples, std::uint32_t grain) {
    const auto* matrix = port.matrix < 0 || source.matrices[port.matrix].empty() ? nullptr : &source.matrices[port.matrix];
    const std::size_t outputs = matrix == nullptr ? voice.channels : std::min<std::size_t>(voice.channels, matrix->size() / source.channels);
    for (std::size_t dst = 0; dst < outputs; dst++) {
        for (std::uint32_t src = 0; src < source.channels; src++) {
            const float level = port.volume * (matrix == nullptr ? Ngs2DefaultLevel(source.channels, src, voice.channels, static_cast<std::uint32_t>(dst)) : (*matrix)[dst * source.channels + src]);
            if (level == 0.0f) continue;
            for (std::uint32_t i = 0; i < grain; i++) voice.samples[dst * grain + i] += sourceSamples[src * grain + i] * level;
        }
    }
}

static void MixInputs(Ngs2Voice& voice, const std::vector<Ngs2Voice*>& voices, std::uint32_t grain, std::uint32_t systemRate) {
    std::vector<float> portSamples;
    for (auto* source : voices) {
        for (std::size_t portIndex = 0; portIndex < source->ports.size(); portIndex++) {
            const auto& port = source->ports[portIndex];
            if (port.dest != &voice || port.volume == 0.0f) continue;
            RenderVoice(*source, voices, grain, systemRate);
            const auto location = static_cast<std::uint32_t>(2 + portIndex);
            if (!source->hasSamples && !HasFilterHistoryAt(*source, location)) continue;
            if (HasFiltersAt(*source, location) || HasFilterHistoryAt(*source, location)) {
                portSamples = source->samples;
                ApplyFiltersAt(*source, location, portSamples, grain);
                if (source->hasSamples || HasSignal(portSamples)) {
                    MixPort(voice, *source, port, portSamples, grain);
                    voice.hasSamples = true;
                }
            } else {
                MixPort(voice, *source, port, source->samples, grain);
                voice.hasSamples = true;
            }
        }
    }
}

static void RenderVoice(Ngs2Voice& voice, const std::vector<Ngs2Voice*>& voices,
                         std::uint32_t grain, std::uint32_t systemRate) {
    if (voice.rendered) return;
    if (voice.rendering) ngs2_stop("NGS2: the voice patches form a cycle");
    voice.rendering = true;
    struct RenderingScope {
        bool& active;
        ~RenderingScope() { active = false; }
    } renderingScope{voice.rendering};
    voice.samples.assign(static_cast<std::size_t>(grain) * voice.channels, 0.0f);
    voice.hasSamples = false;
    voice.peak = 0.0f;
    if (voice.state == Ngs2PlayState::Playing && voice.channels != 0) {
        const auto rackId = voice.rack->rackId;
        const bool sampler = rackId == SCE_NGS2_RACK_ID_SAMPLER || rackId == SCE_NGS2_RACK_ID_CUSTOM_SAMPLER;
        if (sampler) {
            RenderSampler(voice, grain, systemRate);
            if (rackId == SCE_NGS2_RACK_ID_CUSTOM_SAMPLER) {
                ProcessCustomModules(voice, grain, systemRate);
            } else {
                Ngs2ProcessLegacyUserFx(voice, grain, systemRate);
                const bool processedFrames = voice.hasSamples;
                if (processedFrames || HasFilterHistoryAt(voice, 1)) {
                    ApplyFiltersAt(voice, 1, voice.samples, grain);
                    voice.hasSamples = processedFrames || HasSignal(voice.samples);
                }
            }
        } else {
            MixInputs(voice, voices, grain, systemRate);
            if (rackId == SCE_NGS2_RACK_ID_SUBMIXER) {
                const bool mixedFrames = voice.hasSamples;
                if (mixedFrames || HasFilterHistoryAt(voice, 0)) {
                    ApplyFiltersAt(voice, 0, voice.samples, grain);
                    voice.hasSamples = mixedFrames || HasSignal(voice.samples);
                }
                if (voice.compressor.enabled) ApplyDynamics(voice, voice.compressor, grain, systemRate);
                Ngs2ProcessLegacyUserFx(voice, grain, systemRate);
                const bool processedFrames = voice.hasSamples;
                if (processedFrames || HasFilterHistoryAt(voice, 1)) {
                    ApplyFiltersAt(voice, 1, voice.samples, grain);
                    voice.hasSamples = processedFrames || HasSignal(voice.samples);
                }
            } else if (rackId == SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) {
                ProcessCustomModules(voice, grain, systemRate);
            } else if (rackId == SCE_NGS2_RACK_ID_REVERB && voice.reverb) {
                voice.hasSamples = Ngs2ProcessReverb(voice, grain, systemRate);
            } else if (rackId == SCE_NGS2_RACK_ID_MASTERING && voice.limiter.enabled) {
                ApplyDynamics(voice, voice.limiter, grain, systemRate);
            }
        }
        if (voice.hasSamples) voice.peak = MeasurePeak(voice.samples, voice.channels, grain);
    }
    voice.rendered = true;
}

static std::size_t SampleBytes(std::uint32_t waveformType) {
    if (waveformType == SCE_NGS2_WAVEFORM_TYPE_PCM_I16L) return sizeof(std::int16_t);
    if (waveformType == SCE_NGS2_WAVEFORM_TYPE_PCM_F32L) return sizeof(float);
    ngs2_stop("NGS2: render waveform type " + Ngs2Hex(waveformType) + " is not implemented");
}

static std::vector<float>& OutputMix(std::vector<std::vector<float>>& mixes, const Ngs2RenderBufferInfo* bufferInfo, std::uint32_t numBufferInfo,
                                     const Ngs2Voice& voice, std::uint32_t grain) {
    if (voice.outputId >= numBufferInfo) ngs2_stop("NGS2: mastering output " + std::to_string(voice.outputId) + " has no render buffer");
    const auto& output = bufferInfo[voice.outputId];
    const auto sampleBytes = SampleBytes(output.waveform_type);
    // knack_pc: other channel counts go through the default channel map (any into 1, 2, 6 or 8; Knack
    // renders its 8-channel mastering voice into the 1-channel pad speaker buffer).
    if (output.num_channels != voice.channels && output.num_channels != 1 && output.num_channels != 2 &&
        output.num_channels != 6 && output.num_channels != 8) {
        ngs2_stop("NGS2: mixing " + std::to_string(voice.channels) + " mastering channels into " + std::to_string(output.num_channels) + " is not implemented");
    }
    if (output.buffer == nullptr || output.buffer_size < static_cast<std::size_t>(grain) * output.num_channels * sampleBytes) {
        ngs2_stop("NGS2: render buffer " + std::to_string(voice.outputId) + " is missing or too small");
    }
    auto& mix = mixes[voice.outputId];
    if (mix.empty()) mix.resize(static_cast<std::size_t>(grain) * output.num_channels);
    return mix;
}

static void WriteOutput(const Ngs2RenderBufferInfo& output, const std::vector<float>& mix) {
    if (output.waveform_type == SCE_NGS2_WAVEFORM_TYPE_PCM_F32L) {
        std::memcpy(output.buffer, mix.data(), mix.size() * sizeof(float));
        return;
    }
    auto* pcm = static_cast<std::int16_t*>(output.buffer);
    for (std::size_t i = 0; i < mix.size(); i++) {
        pcm[i] = static_cast<std::int16_t>(std::min(std::lrint(std::clamp(mix[i], -1.0f, 1.0f) * 32768.0f), 32767L));
    }
}

static std::vector<std::size_t> OutputBytes(const Ngs2System& system, const Ngs2RenderBufferInfo* bufferInfo, std::uint32_t numBufferInfo) {
    const auto grain = system.option.num_grain_samples;
    std::vector<std::size_t> outputBytes(numBufferInfo);
    for (std::uint32_t i = 0; i < numBufferInfo; i++) {
        const auto& output = bufferInfo[i];
        if (output.buffer == nullptr) continue;
        if (output.num_channels == 0 || output.num_channels > NGS2_MAX_CHANNELS) {
            ngs2_stop("NGS2: render channel count is invalid");
        }
        outputBytes[i] = static_cast<std::size_t>(grain) * output.num_channels * SampleBytes(output.waveform_type);
        if (output.buffer_size < outputBytes[i]) ngs2_stop("NGS2: render buffer " + std::to_string(i) + " is too small");
    }
    return outputBytes;
}

void Ngs2RenderSilence(const Ngs2System& system, const Ngs2RenderBufferInfo* bufferInfo, std::uint32_t numBufferInfo) {
    const auto outputBytes = OutputBytes(system, bufferInfo, numBufferInfo);
    for (std::uint32_t i = 0; i < numBufferInfo; i++) {
        if (outputBytes[i] != 0) std::memset(bufferInfo[i].buffer, 0, outputBytes[i]);
    }
}

void Ngs2RenderSystem(Ngs2System& system, const Ngs2RenderBufferInfo* bufferInfo, std::uint32_t numBufferInfo) {
    const auto grain = system.option.num_grain_samples;
    const auto outputBytes = OutputBytes(system, bufferInfo, numBufferInfo);
    std::vector<Ngs2Voice*> voices;
    for (auto* rack : system.racks) {
        for (auto& voice : rack->voices) {
            voice.rendered = false;
            voices.push_back(&voice);
        }
    }
    std::vector<std::vector<float>> mixes(numBufferInfo);
    for (auto* voice : voices) {
        RenderVoice(*voice, voices, grain, system.option.sample_rate);
        if (voice->rack->rackId != SCE_NGS2_RACK_ID_MASTERING || !voice->hasSamples) continue;
        auto& mix = OutputMix(mixes, bufferInfo, numBufferInfo, *voice, grain);
        const std::uint32_t outputChannels = bufferInfo[voice->outputId].num_channels;
        float outputPeak = 0.0f;
        for (std::uint32_t channel = 0; channel < voice->channels; channel++) {
            const bool lfe = channel == 3 && (voice->channels == 6 || voice->channels == 8);
            const float level = lfe ? voice->lfeLevel : voice->fbwLevel;
            for (std::uint32_t i = 0; i < grain; i++) {
                outputPeak = std::max(outputPeak, std::abs(voice->samples[channel * grain + i] * level));
            }
            for (std::uint32_t out = 0; out < outputChannels; out++) {
                const float gain = voice->channels == outputChannels ? (out == channel ? level : 0.0f)
                                 : level * Ngs2DefaultLevel(voice->channels, channel, outputChannels, out);
                if (gain == 0.0f) continue;
                for (std::uint32_t i = 0; i < grain; i++) mix[i * outputChannels + out] += voice->samples[channel * grain + i] * gain;
            }
        }
        voice->peak = outputPeak;
    }
    for (std::uint32_t i = 0; i < numBufferInfo; i++) {
        if (!mixes[i].empty()) WriteOutput(bufferInfo[i], mixes[i]);
        else if (outputBytes[i] != 0) std::memset(bufferInfo[i].buffer, 0, outputBytes[i]);
    }
    for (auto* voice : voices) {
        if (voice->state == Ngs2PlayState::Stopped) voice->state = Ngs2PlayState::Empty;
        voice->stateFlags = static_cast<std::uint32_t>(voice->state);
        if (voice->state == Ngs2PlayState::Playing && voice->rack->rackId == SCE_NGS2_RACK_ID_SAMPLER && voice->blocks.empty() && voice->acceptsBlocks) voice->stateFlags |= SCE_NGS2_VOICE_STATE_FLAG_EMPTY;
    }
    system.renderCount++;
}
