#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"

static constexpr std::uint32_t USER_FX2_FLAG_FIRST_PROCESS = 1;
static constexpr std::uint32_t USER_FX2_FLAG_PARAM_CHANGED = 2;

void Ngs2CheckCustomRack(std::uint32_t rackId, const Ngs2CustomRackOption& option) {
    if (option.num_buffers != 1) ngs2_stop("NGS2: custom racks with " + std::to_string(option.num_buffers) + " buffers are not implemented");
    if (option.num_modules > SCE_NGS2_CUSTOM_MAX_MODULES || option.rack_option.max_ports > SCE_NGS2_CUSTOM_MAX_PORTS) APS5_INVALID_ARG_EX;
    for (std::uint32_t m = 0; m < option.num_modules; m++) {
        const auto& info = option.module[m];
        // Module state is a slice of the rack-owned state block. Check the offset
        // first and subtract to avoid overflow in state_offset + state_size.
        if (info.state_offset > option.state_size ||
            info.state_size > option.state_size - info.state_offset) {
            APS5_INVALID_ARG_EX;
        }
        if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2) {
            if (info.option == nullptr || info.option->size != sizeof(Ngs2CustomUserFx2ModuleOption)) APS5_INVALID_ARG_EX;
            if (info.source_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
            const auto& module = *reinterpret_cast<const Ngs2CustomUserFx2ModuleOption*>(info.option);
            if (module.control_handler != nullptr) ngs2_stop("NGS2: UserFx2 control handler is not implemented");
            if (module.process_handler == nullptr) APS5_INVALID_ARG_EX;
        } else if (info.module_id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_CHORUS) {
            if (info.option == nullptr || info.option->size != sizeof(Ngs2CustomChorusModuleOption)) APS5_INVALID_ARG_EX;
            if (info.source_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
            const auto& module = *reinterpret_cast<const Ngs2CustomChorusModuleOption*>(info.option);
            if (module.max_phases > 4) APS5_INVALID_ARG_EX;
        } else if (info.module_id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_DELAY) {
            if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER || info.state_size != 0 || info.state_offset != 0 ||
                info.option == nullptr || info.option->size != sizeof(Ngs2CustomDelayModuleOption)) APS5_INVALID_ARG_EX;
            if (info.source_buffer_id != 0 || info.dest_buffer_id != 0) APS5_INVALID_ARG_EX;
            Ngs2CustomDelayModuleOption module{};
            std::memcpy(&module, info.option, sizeof(module));
            if (module.type > 1 || module.max_taps == 0 || module.max_taps > 8 ||
                !std::isfinite(module.max_length) || module.max_length < 1.0f || module.max_length > 20000.0f ||
                module.reserved != 0) APS5_INVALID_ARG_EX;
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
    rack.customStateSize = option.state_size;
    rack.customNumBuffers = option.num_buffers;
    rack.customModules.resize(option.num_modules);
    rack.customPorts.assign(option.port, option.port + option.rack_option.max_ports);
    rack.customModuleIds.resize(option.num_modules);
    rack.userFx.resize(option.num_modules);
    rack.userFxCommon.resize(option.num_modules);
    rack.customModuleOptions.resize(option.num_modules);
    for (auto& voice : rack.voices) {
        voice.userFx.resize(option.num_modules);
        voice.customUserFx.resize(option.num_modules);
        voice.customModuleRuntime.resize(option.num_modules);
    }
    for (std::uint32_t m = 0; m < option.num_modules; m++) {
        const auto& info = option.module[m];
        rack.customModuleIds[m] = info.module_id;
        rack.customModules[m] = {info.module_id, info.source_buffer_id, info.extra_buffer_id,
                                 info.dest_buffer_id, info.state_offset, info.state_size};
        if (info.module_id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_CHORUS ||
            info.module_id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_DELAY) {
            auto& ownedOption = rack.customModuleOptions[m];
            ownedOption.resize(info.option->size);
            std::memcpy(ownedOption.data(), info.option, ownedOption.size());
            const std::size_t paramSize = info.module_id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_CHORUS
                                              ? sizeof(Ngs2CustomVoiceChorusParam)
                                              : sizeof(Ngs2CustomVoiceDelayParam);
            for (auto& voice : rack.voices) {
                auto& runtime = voice.customModuleRuntime[m];
                runtime.param.resize(paramSize);
                if (info.module_id == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_DELAY) {
                    Ngs2CustomDelayModuleOption delayOption{};
                    std::memcpy(&delayOption, ownedOption.data(), sizeof(delayOption));
                    runtime.type = delayOption.type;
                    runtime.maxLengthMs = delayOption.max_length;
                }
            }
            continue;
        }
        if (info.module_id == SCE_NGS2_CUSTOM_MODULE_ID_FILTER) continue;
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
                if (module.setup_handler != nullptr) {
                    auto context = UserFxContext(rack, m, i);
                    const int result = module.setup_handler(&context);
                    if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx2 setup handler failed with " + std::to_string(result));
                }
                // Preserve the module's post-setup parameter defaults for later voice SETUP calls.
                fx.initialParam = fx.param;
            }
        } else if (option.module[m].module_id == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
            for (std::uint32_t i = 0; i < rack.voices.size(); i++) {
                rack.voices[i].customUserFx[m].flags = 1;
            }
        }
    }
}

void Ngs2ResetUserFx2(Ngs2Voice& voice) {
    for (auto& fx : voice.userFx) {
        if (fx.initialParam.size() != fx.param.size()) APS5_INVALID_ARG_EX;
        std::copy(fx.initialParam.begin(), fx.initialParam.end(), fx.param.begin());
        // Keep work/state (module runtime memory) intact, but make the next process call initialize.
        fx.flags = USER_FX2_FLAG_FIRST_PROCESS;
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
    // Custom module setup is rack-scoped. Reset its per-voice UserFx2 parameters,
    // but retain the custom voice's routing, filters and module work/state buffers.
    Ngs2ResetUserFx2(voice);
    voice.channels = setup.num_input_channels;
    // Like the PS4 runtime, a free custom-submixer voice becomes READY; re-SETUP
    // of a voice that is already active must not silently stop it.
    if (voice.state == Ngs2PlayState::Empty) {
        voice.state = Ngs2PlayState::Ready;
        voice.stateFlags = static_cast<std::uint32_t>(voice.state);
    }
}

static void SetUserFxParam(Ngs2Voice& voice, std::uint32_t index, const Ngs2CustomVoiceUserFx2Param& param) {
    if (index >= voice.userFx.size() || index >= voice.rack->userFx.size() ||
        index >= voice.rack->customModuleIds.size() ||
        voice.rack->customModuleIds[index] != SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2) {
        APS5_INVALID_ARG_EX;
    }
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

void Ngs2ApplyCustomParam(Ngs2Voice& voice, const Ngs2VoiceParamHeader& param) {
    const auto rackId = voice.rack->rackId;
    const std::uint32_t moduleParamId = param.id & ~0x1fu;
    if (moduleParamId == SCE_NGS2_PS4_CUSTOM_VOICE_PARAM_CHORUS ||
        moduleParamId == SCE_NGS2_PS4_CUSTOM_VOICE_PARAM_DELAY) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) {
            ngs2_stop("NGS2: custom module parameter belongs to another rack");
        }
        const auto index = param.id & 0x1fu;
        if (index >= voice.rack->customModuleIds.size()) APS5_INVALID_ARG_EX;

        const auto moduleId = voice.rack->customModuleIds[index];
        if (moduleParamId == SCE_NGS2_PS4_CUSTOM_VOICE_PARAM_CHORUS) {
            if (moduleId != NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_CHORUS ||
                index >= voice.customModuleRuntime.size()) {
                APS5_INVALID_ARG_EX;
            }
            auto& runtime = voice.customModuleRuntime[index];
            if (param.size != sizeof(Ngs2CustomVoiceChorusParam) ||
                runtime.param.size() != sizeof(Ngs2CustomVoiceChorusParam)) {
                APS5_INVALID_ARG_EX;
            }
            auto chorus = ParamAs<Ngs2CustomVoiceChorusParam>(param);
            Ngs2CustomChorusModuleOption option{};
            std::memcpy(&option, voice.rack->customModuleOptions[index].data(), sizeof(option));
            const std::uint32_t maxPhases = std::clamp(option.max_phases, 1u, 4u);
            chorus.num_phases = std::clamp(chorus.num_phases, 1u, maxPhases);
            if (!std::isfinite(chorus.input_level) || !std::isfinite(chorus.delay_time) ||
                !std::isfinite(chorus.modulation_ratio) || !std::isfinite(chorus.modulation_depth) ||
                !std::isfinite(chorus.feedback_level) || !std::isfinite(chorus.wet_level) ||
                !std::isfinite(chorus.dry_level)) APS5_INVALID_ARG_EX;
            chorus.input_level = std::clamp(chorus.input_level, 0.0f, 1.0f);
            chorus.delay_time = std::clamp(chorus.delay_time, 0.1f, 40.0f);
            chorus.modulation_ratio = std::clamp(chorus.modulation_ratio, 0.001f, 20.0f);
            chorus.modulation_depth = std::clamp(chorus.modulation_depth, 0.0f, 1.0f);
            chorus.feedback_level = std::clamp(chorus.feedback_level, -1.0f, 1.0f);
            chorus.wet_level = std::clamp(chorus.wet_level, -1.0f, 1.0f);
            chorus.dry_level = std::clamp(chorus.dry_level, 0.0f, 1.0f);
            constexpr std::size_t chorusSamples = NGS2_MAX_CHANNELS * 4u * 4096u;
            if (runtime.line.size() != chorusSamples) {
                runtime.line.assign(chorusSamples, 0.0f);
                runtime.write = 0;
                runtime.phase = 0.0f;
            }
            runtime.active = true;
            std::memcpy(runtime.param.data(), &chorus, sizeof(chorus));
            return;
        }

        // 0x40001d00 is PS4 Delay and also the retained PS5 User-FX parameter ID.
        // Resolve it by configured module instead of shadowing User-FX unconditionally.
        if (moduleId == NGS2_INTERNAL_CUSTOM_MODULE_ID_PS4_DELAY) {
            if (index >= voice.customModuleRuntime.size()) APS5_INVALID_ARG_EX;
            auto& runtime = voice.customModuleRuntime[index];
            if (param.size != sizeof(Ngs2CustomVoiceDelayParam) ||
                runtime.param.size() != sizeof(Ngs2CustomVoiceDelayParam)) {
                APS5_INVALID_ARG_EX;
            }
            auto delay = ParamAs<Ngs2CustomVoiceDelayParam>(param);
            if (runtime.type > 1) APS5_INVALID_ARG_EX;
            if (!std::isfinite(delay.dry_level) || !std::isfinite(delay.wet_level) ||
                !std::isfinite(delay.input_level) || !std::isfinite(delay.feedback_level) ||
                !std::isfinite(delay.lowpass_fc)) APS5_INVALID_ARG_EX;
            delay.dry_level = std::clamp(delay.dry_level, -1.0f, 1.0f);
            delay.wet_level = std::clamp(delay.wet_level, -1.0f, 1.0f);
            delay.input_level = std::clamp(delay.input_level, -1.0f, 1.0f);
            delay.feedback_level = std::clamp(delay.feedback_level, -1.0f, 1.0f);
            delay.lowpass_fc = std::clamp(delay.lowpass_fc, 0.0f, 24000.0f);
            delay.num_taps = std::clamp(delay.num_taps, 1u, 8u);
            const float maxLengthMs = runtime.maxLengthMs;
            Ngs2CustomDelayModuleOption delayOption{};
            std::memcpy(&delayOption, voice.rack->customModuleOptions[index].data(), sizeof(delayOption));
            delay.num_taps = std::clamp(delay.num_taps, 1u, delayOption.max_taps);
            std::uint32_t maxDelay = voice.rack->system->option.num_grain_samples;
            for (std::uint32_t tap = 0; tap < 8; tap++) {
                if (!std::isfinite(delay.tap[tap].tap_level) || !std::isfinite(delay.tap[tap].delay_time)) APS5_INVALID_ARG_EX;
                delay.tap[tap].tap_level = tap < delay.num_taps ? std::clamp(delay.tap[tap].tap_level, -1.0f, 1.0f) : 0.0f;
                delay.tap[tap].delay_time = tap < delay.num_taps ? std::clamp(delay.tap[tap].delay_time, 0.0f, maxLengthMs) : 0.0f;
                if (tap < delay.num_taps) {
                    const auto samples = static_cast<std::uint32_t>(voice.rack->system->option.sample_rate * delay.tap[tap].delay_time * 0.001f + 0.5f);
                    runtime.tapDelay[tap] = std::max(samples, voice.rack->system->option.num_grain_samples);
                    maxDelay = std::max(maxDelay, runtime.tapDelay[tap]);
                } else runtime.tapDelay[tap] = 0;
            }
            if ((delay.flags & 2u) != 0) {
                for (float& level : delay.input_mix_level) {
                    if (!std::isfinite(level)) APS5_INVALID_ARG_EX;
                    level = std::clamp(level, -1.0f, 1.0f);
                }
            }
            const std::uint32_t newLength = maxDelay + 1;
            const std::size_t lineChannels = runtime.type == 0 ? 1u : NGS2_MAX_CHANNELS;
            const std::size_t lineSize = lineChannels * newLength;
            if (runtime.lineLength < newLength ||
                runtime.line.size() != lineChannels * static_cast<std::size_t>(runtime.lineLength)) {
                runtime.line.assign(lineSize, 0.0f);
                runtime.write = 0;
                runtime.lowpassState = {};
                runtime.lineLength = newLength;
            }
            const float cosine = std::cos(delay.lowpass_fc * 2.0f * 3.14159265358979323846f /
                                          static_cast<float>(voice.rack->system->option.sample_rate));
            const float k = 2.0f - cosine;
            runtime.lowpassCoefficient = k - std::sqrt(std::max(k * k - 1.0f, 0.0f));
            runtime.active = true;
            std::memcpy(runtime.param.data(), &delay, sizeof(delay));
            return;
        }
        if (moduleId == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
            SetCustomUserFxParam(voice, index, ParamAs<Ngs2CustomVoiceUserFxParam>(param));
            return;
        }
        APS5_INVALID_ARG_EX;
    }
    if (param.id == SCE_NGS2_CUSTOM_SUBMIXER_VOICE_PARAM_SETUP) {
        if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
        SetupCustomSubmixer(voice, ParamAs<Ngs2CustomSubmixerVoiceSetupParam>(param));
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
            // Keep the resolved module index: disabling a nonzero custom-filter
            // module must not accidentally clear filter slot 0.
        } else if (custom.type == 1) {
            filter.frequency = custom.param.fcq.frequency;
            filter.q = custom.param.fcq.q;
            filter.level = custom.param.fcq.level;
        } else if (custom.type == 0x20) {
            filter.frequency = custom.param.direct.i0;
            filter.q = custom.param.direct.i1;
            filter.level = custom.param.direct.i2;
            std::memcpy(&filter.reserved[0], &custom.param.direct.o1, sizeof(float));
            std::memcpy(&filter.reserved[1], &custom.param.direct.o2, sizeof(float));
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
    if (rackId != SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
    ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " is not implemented");
}

void Ngs2ProcessUserFxModule(Ngs2Voice& voice, std::uint32_t moduleIndex,
                               std::uint32_t grain, std::uint32_t sampleRate) {
    if (moduleIndex >= voice.rack->customModuleIds.size()) return;
    const auto moduleId = voice.rack->customModuleIds[moduleIndex];
    if (moduleId == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX2) {
        if (!voice.hasSamples || moduleIndex >= voice.userFx.size() || moduleIndex >= voice.rack->userFx.size()) return;
        auto& fx = voice.userFx[moduleIndex];
        const auto& module = voice.rack->userFx[moduleIndex];
        if (module.process_handler == nullptr) return;
        std::vector<float*> channels(voice.channels);
        for (std::uint32_t c = 0; c < voice.channels; c++) channels[c] = voice.samples.data() + static_cast<std::size_t>(c) * grain;
        Ngs2UserFx2ProcessContext context{channels.data(), voice.rack->userFxCommon[moduleIndex].data(),
                                          fx.param.data(), fx.work.data(), fx.state.data(), module.user_data,
                                          fx.flags, voice.channels, voice.channels, grain, sampleRate, 0, {}};
        const int result = module.process_handler(&context);
        if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx2 process handler failed with " + std::to_string(result));
        fx.flags = 0;
        voice.hasSamples = std::any_of(voice.samples.begin(), voice.samples.end(), [](float sample) { return sample != 0.0f; });
    } else if (moduleId == SCE_NGS2_CUSTOM_MODULE_ID_USER_FX) {
        if (!voice.hasSamples || moduleIndex >= voice.customUserFx.size()) return;
        auto& fx = voice.customUserFx[moduleIndex];
        if (fx.handler == nullptr) return;
        std::vector<float*> channels(voice.channels);
        for (std::uint32_t c = 0; c < voice.channels; c++) channels[c] = voice.samples.data() + static_cast<std::size_t>(c) * grain;
        Ngs2UserFxProcessContext context{channels.data(), fx.data[0], fx.data[1], fx.data[2],
                                         fx.flags, voice.channels, grain, sampleRate};
        const int result = fx.handler(&context);
        if (result != SCE_NGS2_OK) ngs2_stop("NGS2: the UserFx process handler failed with " + std::to_string(result));
        fx.flags = 0;
        voice.hasSamples = std::any_of(voice.samples.begin(), voice.samples.end(), [](float sample) { return sample != 0.0f; });
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
