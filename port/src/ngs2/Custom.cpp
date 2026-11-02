#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static constexpr std::uint32_t USER_FX2_FLAG_FIRST_PROCESS = 1;
static constexpr std::uint32_t USER_FX2_FLAG_PARAM_CHANGED = 2;

struct Ngs2CustomVoiceChorusParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t reserved;
    std::uint32_t phases;
    std::uint32_t mask;
    float gain, delay_ms, rate_hz, depth, feedback, wet, dry;
};
static_assert(sizeof(Ngs2CustomVoiceChorusParam) == 48);

struct Ngs2CustomVoiceDelayParam {
    Ngs2VoiceParamHeader header;
    float dry, wet, gain, feedback, lowpass_hz;
    std::uint32_t taps;
    float tap_level[8];
    float tap_ms[8];
    float input_gain[8];
    std::uint32_t mask, flags;
};
static_assert(sizeof(Ngs2CustomVoiceDelayParam) == 136);

static float ClampFinite(float value, float low, float high) {
    if (!std::isfinite(value)) APS5_INVALID_ARG_EX;
    return std::clamp(value, low, high);
}

void Ngs2CheckCustomRack(const Ngs2CustomRackOption& option) {
    if (option.num_buffers != 1) ngs2_stop("NGS2: custom racks with " + std::to_string(option.num_buffers) + " buffers are not implemented");
    if (option.num_modules > SCE_NGS2_CUSTOM_MAX_MODULES || option.rack_option.max_ports > SCE_NGS2_CUSTOM_MAX_PORTS) APS5_INVALID_ARG_EX;
    for (std::uint32_t m = 0; m < option.num_modules; m++) {
        const auto& info = option.module[m];
        if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2) {
            if (info.option == nullptr || info.option->size != sizeof(Ngs2CustomUserFx2ModuleOption)) APS5_INVALID_ARG_EX;
            if (info.source_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
            if (reinterpret_cast<const Ngs2CustomUserFx2ModuleOption*>(info.option)->process_handler == nullptr) APS5_INVALID_ARG_EX;
        } else if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_CHORUS) {
            if (info.option != nullptr) APS5_INVALID_ARG_EX;
            if (info.source_buffer_id != 0 || info.extra_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
        } else if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) {
            if (info.option != nullptr || info.state_size != 0 || info.state_offset != 0) APS5_INVALID_ARG_EX;
            if (info.source_buffer_id != 0 || info.extra_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
        } else if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
            if (info.source_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
            if (info.option != nullptr && info.option->size < sizeof(Ngs2CustomModuleOption)) APS5_INVALID_ARG_EX;
        } else {
            ngs2_stop("NGS2: custom module id " + Ngs2Hex(info.module_id) + " is not implemented");
        }
    }
    for (std::uint32_t p = 0; p < option.rack_option.max_ports; p++) {
        if (option.port[p].source_buffer_id != 0) APS5_INVALID_ARG_EX;
    }
}

static Ngs2UserFx2SetupContext UserFxContext(Ngs2Rack& rack, std::size_t module, std::uint32_t voiceIndex) {
    auto& fx = rack.voices[voiceIndex].userFx[module];
    return {rack.userFxCommon[module].data(), fx.param.data(), fx.work.data(), rack.userFx[module].user_data,
            static_cast<std::uint32_t>(rack.voices.size()), voiceIndex, {}};
}

void Ngs2SetupUserFx(Ngs2Rack& rack, const Ngs2CustomRackOption& option) {
    rack.customModuleIds.resize(option.num_modules);
    rack.userFx.resize(option.num_modules);
    rack.userFxCommon.resize(option.num_modules);
    for (auto& voice : rack.voices) {
        voice.userFx.resize(option.num_modules);
        voice.customUserFx.resize(option.num_modules);
    }
    for (std::uint32_t m = 0; m < option.num_modules; m++) {
        rack.customModuleIds[m] = option.module[m].module_id;
        if (option.module[m].module_id == SCE_NGS2_CUSTOM_MODULE_ID_CHORUS ||
            option.module[m].module_id == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) continue;
        if (option.module[m].module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2) {
            const auto& module = *reinterpret_cast<const Ngs2CustomUserFx2ModuleOption*>(option.module[m].option);
            rack.userFx[m] = module;
            rack.userFxCommon[m].resize(module.common_size);
            for (std::uint32_t i = 0; i < rack.voices.size(); i++) {
                auto& fx = rack.voices[i].userFx[m];
                fx.param.resize(module.param_size);
                fx.work.resize(module.work_size);
                fx.state.resize(option.module[m].state_size);
                fx.flags = USER_FX2_FLAG_FIRST_PROCESS;
                if (module.setup_handler == nullptr) continue;
                auto context = UserFxContext(rack, m, i);
                const int result = module.setup_handler(&context);
                if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx2 setup handler failed with " + std::to_string(result));
            }
        } else if (option.module[m].module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
            for (std::uint32_t i = 0; i < rack.voices.size(); i++) {
                rack.voices[i].customUserFx[m].flags = 1;
            }
        }
    }
}

void Ngs2CleanupUserFx(Ngs2Rack& rack) {
    for (std::uint32_t i = 0; i < rack.voices.size(); i++) {
        for (std::size_t m = 0; m < rack.userFx.size(); m++) {
            if (rack.customModuleIds[m] != SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2) continue;
            if (rack.userFx[m].cleanup_handler == nullptr) continue;
            auto context = UserFxContext(rack, m, i);
            const int result = rack.userFx[m].cleanup_handler(&context);
            if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx2 cleanup handler failed with " + std::to_string(result));
        }
    }
}

static void SetupCustomSubmixer(Ngs2Voice& voice, const Ngs2CustomSubmixerVoiceSetupParam& setup) {
    if (setup.flags != 0) ngs2_stop("NGS2: custom submixer setup flags are not implemented");
    if (setup.num_input_channels != setup.num_output_channels) ngs2_stop("NGS2: custom submixer channel conversion is not implemented");
    if (setup.num_input_channels == 0 || setup.num_input_channels > voice.rack->maxChannels) APS5_INVALID_ARG_EX;
    voice.ResetSetup();
    voice.channels = setup.num_input_channels;
}

static void SetUserFxParam(Ngs2Voice& voice, std::uint32_t index, const Ngs2CustomVoiceUserFx2Param& param) {
    if (index >= voice.userFx.size()) APS5_INVALID_ARG_EX;
    const auto& module = voice.rack->userFx[index];
    if (module.control_handler != nullptr) ngs2_stop("NGS2: the UserFx2 control handler is not implemented");
    auto& fx = voice.userFx[index];
    if (param.data_size != fx.param.size() || (param.data == nullptr && param.data_size != 0)) APS5_INVALID_ARG_EX;
    if (param.data_size != 0) std::memcpy(fx.param.data(), param.data, param.data_size);
    fx.flags |= USER_FX2_FLAG_PARAM_CHANGED;
}

static void SetCustomUserFxParam(Ngs2Voice& voice, std::uint32_t index, const Ngs2CustomVoiceUserFxParam& param) {
    if (index >= voice.customUserFx.size()) APS5_INVALID_ARG_EX;
    if (index >= voice.rack->customModuleIds.size() || voice.rack->customModuleIds[index] != SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
        APS5_INVALID_ARG_EX;
    }
    auto& fx = voice.customUserFx[index];
    fx.handler = param.handler;
    fx.data = {param.user_data0, param.user_data1, param.user_data2};
    fx.flags = 1;
}


static void SetChorusParam(Ngs2Voice& voice, const Ngs2CustomVoiceChorusParam& param) {
    if (voice.rack->rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER ||
        std::find(voice.rack->customModuleIds.begin(), voice.rack->customModuleIds.end(),
                  SCE_NGS2_CUSTOM_MODULE_ID_CHORUS) == voice.rack->customModuleIds.end()) APS5_INVALID_ARG_EX;
    auto& chorus = voice.chorus;
    chorus.mask = param.mask & 0xffu;
    chorus.phases = std::clamp(param.phases, 1u, 4u);
    chorus.gain = ClampFinite(param.gain, 0.0f, 1.0f);
    chorus.delayMs = ClampFinite(param.delay_ms, 0.1f, 40.0f);
    chorus.rateHz = ClampFinite(param.rate_hz, 0.001f, 20.0f);
    chorus.depth = ClampFinite(param.depth, 0.0f, 1.0f);
    chorus.feedback = ClampFinite(param.feedback, -1.0f, 1.0f);
    chorus.wet = ClampFinite(param.wet, -1.0f, 1.0f);
    chorus.dry = ClampFinite(param.dry, 0.0f, 1.0f);
    constexpr std::size_t lineSize = static_cast<std::size_t>(NGS2_MAX_CHANNELS) * 4 * 4096;
    if (chorus.line.empty()) chorus.line.assign(lineSize, 0.0f);
    chorus.configured = true;
}

static void SetDelayParam(Ngs2Voice& voice, const Ngs2CustomVoiceDelayParam& param) {
    if (voice.rack->rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER ||
        std::find(voice.rack->customModuleIds.begin(), voice.rack->customModuleIds.end(),
                  SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) == voice.rack->customModuleIds.end()) APS5_INVALID_ARG_EX;
    if (voice.channels == 0 || voice.channels > NGS2_MAX_CHANNELS) APS5_INVALID_ARG_EX;
    const auto sampleRate = voice.rack->system->option.sample_rate;
    const auto grain = voice.rack->system->option.num_grain_samples;
    if (sampleRate == 0 || grain == 0) APS5_INVALID_ARG_EX;

    auto& delay = voice.delay;
    delay.dry = ClampFinite(param.dry, -1.0f, 1.0f);
    delay.wet = ClampFinite(param.wet, -1.0f, 1.0f);
    delay.gain = ClampFinite(param.gain, -1.0f, 1.0f);
    delay.feedback = ClampFinite(param.feedback, -1.0f, 1.0f);
    delay.lowpassHz = ClampFinite(param.lowpass_hz, 0.0f, std::min(24000.0f, sampleRate * 0.5f));
    delay.taps = std::clamp(param.taps, 1u, 8u);
    delay.mask = param.mask;
    delay.flags = param.flags;

    std::uint32_t required = grain + 1;
    for (std::uint32_t tap = 0; tap < 8; tap++) {
        delay.tapLevel[tap] = tap < delay.taps ? ClampFinite(param.tap_level[tap], -1.0f, 1.0f) : 0.0f;
        delay.tapMs[tap] = tap < delay.taps ? ClampFinite(param.tap_ms[tap], 0.0f, 20000.0f) : 0.0f;
        const auto samples = static_cast<std::uint64_t>(
            static_cast<double>(sampleRate) * delay.tapMs[tap] * 0.001 + 0.5);
        const auto bounded = std::max<std::uint64_t>(samples, grain);
        if (bounded >= std::numeric_limits<std::uint32_t>::max()) APS5_INVALID_ARG_EX;
        delay.tapDelay[tap] = tap < delay.taps ? static_cast<std::uint32_t>(bounded) : 0;
        required = std::max(required, delay.tapDelay[tap] + 1);
    }
    for (std::uint32_t channel = 0; channel < NGS2_MAX_CHANNELS; channel++) {
        delay.inputGain[channel] = (delay.flags & 2u) != 0
            ? ClampFinite(param.input_gain[channel], -1.0f, 1.0f) : 1.0f;
    }

    if (required != delay.length || voice.channels != delay.channels || delay.line.empty()) {
        const std::size_t samples = static_cast<std::size_t>(required) * voice.channels;
        delay.line.assign(samples, 0.0f);
        delay.lowpassState.fill(0.0f);
        delay.write = 0;
        delay.length = required;
        delay.channels = voice.channels;
    }
    delay.work.resize(grain);
    const float cosine = std::cos(delay.lowpassHz * 6.2831853071795864769f / sampleRate);
    const float k = 2.0f - cosine;
    delay.lowpassCoefficient = k - std::sqrt(std::max(k * k - 1.0f, 0.0f));
    delay.configured = true;
}

static void ProcessChorus(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t sampleRate) {
    auto& chorus = voice.chorus;
    if (!chorus.configured || chorus.line.empty() || sampleRate == 0) return;
    constexpr std::uint32_t lineMask = 4095;
    const float base = static_cast<float>(static_cast<std::uint32_t>(chorus.delayMs * 0.001f * sampleRate));
    const float depth = base * chorus.depth;
    const float step = chorus.rateHz / static_cast<float>(sampleRate);
    const float dry = chorus.gain * chorus.dry;
    const float wet = chorus.gain * chorus.wet;
    const float phase0 = chorus.phase;
    const std::uint32_t write0 = chorus.write;
    for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
        if (((chorus.mask >> channel) & 1u) != 0) continue;
        float phase = phase0;
        std::uint32_t write = write0;
        float* channelLine = chorus.line.data() + static_cast<std::size_t>(channel) * 4 * 4096;
        float* samples = voice.samples.data() + static_cast<std::size_t>(channel) * grain;
        for (std::uint32_t i = 0; i < grain; i++) {
            const float original = samples[i];
            float input = original;
            float delayed = 0.0f;
            for (std::uint32_t phaseIndex = 0; phaseIndex < chorus.phases; phaseIndex++) {
                float lfo = phase + static_cast<float>(phaseIndex) / static_cast<float>(chorus.phases);
                lfo -= std::floor(lfo);
                const float delaySamples = base + 1.0f + depth * std::sin(6.2831853071795864769f * lfo);
                const auto integerDelay = static_cast<std::uint32_t>(delaySamples);
                const float fraction = delaySamples - static_cast<float>(integerDelay);
                float* line = channelLine + static_cast<std::size_t>(phaseIndex) * 4096;
                delayed = fraction * line[(write + integerDelay + 1) & lineMask] +
                          (1.0f - fraction) * line[(write + integerDelay) & lineMask];
                line[write] = input + chorus.feedback * delayed;
                input = delayed;
            }
            samples[i] = dry * original + wet * delayed;
            phase += step;
            write = (write - 1) & lineMask;
        }
    }
    const float nextPhase = phase0 + step * static_cast<float>(grain);
    chorus.phase = nextPhase - std::floor(nextPhase);
    chorus.write = (write0 - grain) & lineMask;
}

static void ProcessDelay(Ngs2Voice& voice, std::uint32_t grain) {
    auto& delay = voice.delay;
    if (!delay.configured || delay.line.empty() || delay.length <= grain) return;
    float levelSum = 0.0f;
    for (std::uint32_t tap = 0; tap < delay.taps; tap++) levelSum += std::abs(delay.tapLevel[tap]);
    const float feedback = delay.feedback / std::max(levelSum, 1.0f);
    const float dry = delay.gain * delay.dry;
    const float wet = delay.gain * delay.wet;
    const auto length = delay.length;
    for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
        if (channel == 3 || ((delay.mask >> channel) & 1u) != 0) continue;
        float* line = delay.line.data() + static_cast<std::size_t>(channel) * length;
        float* samples = voice.samples.data() + static_cast<std::size_t>(channel) * grain;
        std::fill(delay.work.begin(), delay.work.begin() + grain, 0.0f);
        for (std::uint32_t tap = 0; tap < delay.taps; tap++) {
            if (delay.tapLevel[tap] == 0.0f) continue;
            std::uint32_t read = (delay.write + length - delay.tapDelay[tap]) % length;
            for (std::uint32_t i = 0; i < grain; i++) {
                delay.work[i] += delay.tapLevel[tap] * line[read];
                if (++read == length) read = 0;
            }
        }
        if ((delay.flags & 1u) != 0) {
            const float coefficient = delay.lowpassCoefficient;
            float state = delay.lowpassState[channel];
            for (std::uint32_t i = 0; i < grain; i++) {
                state = (1.0f - coefficient) * delay.work[i] + coefficient * state;
                delay.work[i] = state;
            }
            delay.lowpassState[channel] = std::abs(state) < 1e-20f ? 0.0f : state;
        }
        std::uint32_t write = delay.write;
        for (std::uint32_t i = 0; i < grain; i++) {
            line[write] = delay.inputGain[channel] * samples[i] + feedback * delay.work[i];
            samples[i] = dry * samples[i] + wet * delay.work[i];
            if (++write == length) write = 0;
        }
    }
    delay.write = (delay.write + grain) % length;
}

static bool HasSamples(const Ngs2Voice& voice) {
    return std::any_of(voice.samples.begin(), voice.samples.end(), [](float sample) { return sample != 0.0f; });
}

void Ngs2ApplyCustomParam(Ngs2Voice& voice, const Ngs2VoiceParamHeader& param) {
    const auto rackId = voice.rack->rackId;
    if (param.id == SCE_NGS2_CUSTOM_SUBMIXER_VOICE_PARAM_SETUP) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
        SetupCustomSubmixer(voice, ParamAs<Ngs2CustomSubmixerVoiceSetupParam>(param));
        return;
    }
    if (param.id == SCE_NGS2_CUSTOM_VOICE_PARAM_CHORUS) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER || param.size < sizeof(Ngs2CustomVoiceChorusParam)) APS5_INVALID_ARG_EX;
        SetChorusParam(voice, ParamAs<Ngs2CustomVoiceChorusParam>(param));
        return;
    }
    if (param.id == SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX && param.size >= sizeof(Ngs2CustomVoiceDelayParam)) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) APS5_INVALID_ARG_EX;
        SetDelayParam(voice, ParamAs<Ngs2CustomVoiceDelayParam>(param));
        return;
    }
    if ((param.id & ~0x1fu) == SCE_NGS2_CUSTOM_VOICE_PARAM_FILTER) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SAMPLER && rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) {
            ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
        }
        const auto index = param.id & 0x1f;
        if (index >= voice.rack->customModuleIds.size() || voice.rack->customModuleIds[index] != SCE_NGS2_CUSTOM_MODULE_ID_FILTER) APS5_INVALID_ARG_EX;
        const auto& custom = ParamAs<Ngs2CustomVoiceFilterParam>(param);
        Ngs2SamplerVoiceFilterParam filter{};
        filter.header = custom.header;
        filter.index = index;
        filter.location = 1;
        filter.type = custom.type;
        filter.channel_mask = custom.channel_mask;
        if (custom.type == 0) {
            filter = {};
        } else if (custom.type == 1) {
            filter.frequency = custom.param.fcq.frequency;
            filter.q = custom.param.fcq.q;
            filter.level = custom.param.fcq.level;
        } else {
            ngs2_stop("NGS2: custom filter type " + std::to_string(custom.type) + " is not implemented");
        }
        Ngs2SetFilter(voice, filter);
        return;
    }
    if ((param.id & ~0x1fu) == SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX2) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SAMPLER && rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) {
            ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
        }
        SetUserFxParam(voice, param.id & 0x1f, ParamAs<Ngs2CustomVoiceUserFx2Param>(param));
        return;
    }
    if ((param.id & ~0x1fu) == SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
        SetCustomUserFxParam(voice, param.id & 0x1f, ParamAs<Ngs2CustomVoiceUserFxParam>(param));
        return;
    }
    if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
    ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " is not implemented");
}

void Ngs2ProcessUserFx(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t sampleRate) {
    if (voice.userFx.empty() && voice.customUserFx.empty()) return;
    std::vector<float*> channels(voice.channels);
    for (std::uint32_t c = 0; c < voice.channels; c++) channels[c] = voice.samples.data() + static_cast<std::size_t>(c) * grain;
    for (std::size_t m = 0; m < voice.rack->customModuleIds.size(); m++) {
        if (voice.rack->customModuleIds[m] == SCE_NGS2_CUSTOM_MODULE_ID_CHORUS) {
            ProcessChorus(voice, grain, sampleRate);
            voice.hasSamples = HasSamples(voice);
        } else if (voice.rack->customModuleIds[m] == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2) {
            if (!voice.hasSamples) continue;
            auto& fx = voice.userFx[m];
            const auto& module = voice.rack->userFx[m];
            if (module.process_handler == nullptr) continue;
            Ngs2UserFx2ProcessContext context{channels.data(), voice.rack->userFxCommon[m].data(), fx.param.data(), fx.work.data(), fx.state.data(),
                                              module.user_data, fx.flags, voice.channels, voice.channels, grain, sampleRate, 0, {}};
            const int result = module.process_handler(&context);
            if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx2 process handler failed with " + std::to_string(result));
            fx.flags = 0;
        } else if (voice.rack->customModuleIds[m] == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
            if (voice.delay.configured) {
                ProcessDelay(voice, grain);
                voice.hasSamples = HasSamples(voice);
            } else if (m < voice.customUserFx.size() && voice.customUserFx[m].handler != nullptr) {
                auto& fx = voice.customUserFx[m];
                Ngs2UserFxProcessContext context{channels.data(), fx.data[0], fx.data[1], fx.data[2],
                                                 fx.flags, voice.channels, grain, sampleRate};
                const int result = fx.handler(&context);
                if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx process handler failed with " + std::to_string(result));
                fx.flags = 0;
                voice.hasSamples = std::any_of(voice.samples.begin(), voice.samples.end(), [](float sample) { return sample != 0.0f; });
            }
        }
    }
}

void Ngs2ProcessLegacyUserFx(Ngs2Voice& voice, std::uint32_t grain, std::uint32_t sampleRate) {
    if (voice.userFxHandler == nullptr) return;
    std::array<float*, NGS2_MAX_CHANNELS> channels{};
    for (std::uint32_t c = 0; c < voice.channels; ++c) channels[c] = voice.samples.data() + static_cast<std::size_t>(c) * grain;
    Ngs2UserFxProcessContext context{channels.data(), voice.userFxData[0], voice.userFxData[1], voice.userFxData[2],
                                    voice.userFxFlags, voice.channels, grain, sampleRate};
    const int result = voice.userFxHandler(&context);
    if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx process handler failed with " + std::to_string(result));
    voice.userFxFlags = 0;
    voice.hasSamples = std::any_of(voice.samples.begin(), voice.samples.end(), [](float sample) { return sample != 0.0f; });
}
