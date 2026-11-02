#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <string>

#include "ngs2_compat.hpp"
#include "Ngs2Internal.hpp"
#include "hevag_coefs.h"

static constexpr std::uint32_t COMMAND_EVENT = 2;
static constexpr std::uint32_t COMMAND_MATRIX_LEVELS = 5;
static constexpr std::uint32_t COMMAND_PORT_VOLUME = 6;
static constexpr std::uint32_t COMMAND_PORT_MATRIX = 7;
static constexpr std::uint8_t COMMAND_TYPE_FLOAT = 1;
static constexpr std::uint8_t COMMAND_TYPE_INT = 3;
static constexpr std::uint8_t COMMAND_TYPE_UINT = 4;
static constexpr std::uint8_t COMMAND_TYPE_FLOAT_ARRAY = 0x11;
static constexpr float MAX_MATRIX_LEVEL = 4.0f;

void Ngs2Voice::SetEvent(std::uint32_t eventId) {
    const auto previousState = state;
    switch (eventId) {
        case SCE_NGS2_VOICE_EVENT_PLAY:
            if (state == Ngs2PlayState::Empty || state == Ngs2PlayState::Ready || state == Ngs2PlayState::Stopped) {
                state = Ngs2PlayState::Playing;
                envelope.releasing = false;
                envelope.segment = 0;
                envelope.position = 0;
                envelope.start = 0.0f;
                envelope.gain = envelope.forward.empty() ? 1.0f : 0.0f;
                if (userFxHandler) userFxFlags = 1;
            }
            break;
        case SCE_NGS2_VOICE_EVENT_STOP:
            // knack_pc: with release points the voice plays on until they end (Render.cpp).
            if (state == Ngs2PlayState::Playing && !envelope.release.empty()) {
                if (!envelope.releasing) {
                    envelope.releasing = true;
                    envelope.segment = 0;
                    envelope.position = 0;
                    envelope.start = envelope.gain;
                }
                break;
            }
            if (state == Ngs2PlayState::Playing || state == Ngs2PlayState::Paused) state = Ngs2PlayState::Stopped;
            break;
        case SCE_NGS2_VOICE_EVENT_STOP_IMM:
        case SCE_NGS2_VOICE_EVENT_KILL:
            state = Ngs2PlayState::Empty;
            stateFlags = 0;
            Ngs2ClearReverb(*this);
            peak = 0.0f;
            break;
        case SCE_NGS2_VOICE_EVENT_PAUSE:
            if (state == Ngs2PlayState::Playing) state = Ngs2PlayState::Paused;
            break;
        case SCE_NGS2_VOICE_EVENT_RESUME:
            if (state == Ngs2PlayState::Paused) state = Ngs2PlayState::Playing;
            break;
        default: ngs2_stop("NGS2: unknown voice event " + Ngs2Hex(eventId));
    }
    // GetState/GetStateFlags expose the last committed flags. Keep them in step
    // with actual state transitions, while preserving render-derived bits on no-op events.
    if (state != previousState) stateFlags = static_cast<std::uint32_t>(state);
}

void Ngs2Voice::ResetSetup() {
    Ngs2ResetUserFx2(*this);
    ++waveformRevision;
    SetEvent(SCE_NGS2_VOICE_EVENT_STOP_IMM);
    std::fill(ports.begin(), ports.end(), Ngs2Port{});
    for (auto& matrix : matrices) matrix.clear();
    filters.clear();
    envelope = {};
    compressor = {};
    limiter = {};
    if (rack && rack->rackId == SCE_NGS2_RACK_ID_MASTERING) {
        // Console mastering racks start with an enabled 0 dB limiter (1 ms attack / 10 ms release).
        limiter.enabled = true;
        limiter.threshold = 1.0f;
        limiter.ratio = 0.0f;
        limiter.knee = 0.0f;
        limiter.attack = 0.001f;
        limiter.release = 0.01f;
        limiter.level = 1.0f;
        limiter.gain.fill(1.0f);
    }
    peak = 0.0f;
    vagHistory = {};
    for (auto& module : customModuleRuntime) {
        std::fill(module.param.begin(), module.param.end(), 0);
        module.line.clear();
        module.lowpassState = {};
        module.tapDelay = {};
        module.lineLength = 0;
        module.write = 0;
        module.phase = 0.0f;
        module.lowpassCoefficient = 0.0f;
        module.active = false;
    }
    fbwLevel = 1.0f;
    lfeLevel = 1.0f;
    channels = 0;
    sampleRate = 0;
    waveformType = 0;
    vag = false;
    atrac9 = {};
    pitch = 1.0f;
    phase = 0;
    blocks.clear();
    acceptsBlocks = true;
    decodedSamples = 0;
    decodedBytes = 0;
    waveformEnd = nullptr;
    userFxHandler = nullptr;
    userFxData = {};
    userFxFlags = 0;
    callback = nullptr;
    callbackData = 0;
    callbackFlags = 0;
}

const std::uint8_t* Ngs2StreamEnd(const Ngs2Voice& voice, const Ngs2Block& block) {
    const auto& last = block.pieces.back();
    return last.data + last.frames * voice.channels * sizeof(std::int16_t);
}

const std::uint8_t* Ngs2Voice::WaveformData() const {
    if (blocks.empty()) return waveformEnd;
    const auto& block = blocks.front();
    if (waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) return block.data + block.dataCursor;
    if (block.streaming) {
        for (const auto& piece : block.pieces) {
            if (block.cursor >= piece.firstFrame && block.cursor - piece.firstFrame < piece.frames) return piece.data + (block.cursor - piece.firstFrame) * channels * sizeof(std::int16_t);
        }
        return Ngs2StreamEnd(*this, block);
    }
    return block.data + (static_cast<std::size_t>(block.info.num_skip_samples) + block.cursor) * channels * sizeof(std::int16_t);
}

static Ngs2Port& PortAt(Ngs2Voice& voice, std::uint32_t port) {
    if (port >= voice.ports.size()) ngs2_stop("NGS2: voice port " + std::to_string(port) + " is out of range");
    return voice.ports[port];
}

static void SetMatrixLevels(Ngs2Voice& voice, std::uint32_t matrixId, const float* levels, std::uint32_t numLevels) {
    if (numLevels == 0 && voice.channels == 0 && matrixId < voice.matrices.size()) {
        voice.matrices[matrixId].clear();
        return;
    }
    if (matrixId >= voice.matrices.size() || numLevels == 0 || numLevels > NGS2_MAX_CHANNELS * NGS2_MAX_CHANNELS || levels == nullptr) {
        APS5_INVALID_ARG_EX;
    }
    auto& matrix = voice.matrices[matrixId];
    matrix.resize(numLevels);
    for (std::uint32_t i = 0; i < numLevels; i++) matrix[i] = std::clamp(levels[i], -MAX_MATRIX_LEVEL, MAX_MATRIX_LEVEL);
}

static void SetPortMatrix(Ngs2Voice& voice, std::uint32_t port, std::int32_t matrixId) {
    if (matrixId < -1 || matrixId >= static_cast<std::int32_t>(voice.matrices.size())) APS5_INVALID_ARG_EX;
    PortAt(voice, port).matrix = matrixId;
}

static void Patch(Ngs2Voice& voice, const Ngs2VoicePatchParam& patch) {
    auto& port = PortAt(voice, patch.port);
    if (patch.dest_input_id != 0) ngs2_stop("NGS2: voice input " + std::to_string(patch.dest_input_id) + " is not implemented");
    if (patch.dest_handle == 0) {
        port.dest = nullptr;
        return;
    }
    auto* dest = Ngs2FindVoice(patch.dest_handle);
    if (dest == nullptr) {
        auto* rack = Ngs2FindRack(patch.dest_handle);
        if (rack != nullptr && !rack->voices.empty()) dest = &rack->voices.front();
    }
    if (dest == nullptr || dest->rack->system != voice.rack->system || dest->rack->rackId == SCE_NGS2_RACK_ID_SAMPLER ||
        dest->rack->rackId == SCE_NGS2_RACK_ID_CUSTOM_SAMPLER) {
        APS5_INVALID_ARG_EX;
    }
    port.dest = dest;
}

static void ApplyCommonParam(Ngs2Voice& voice, const Ngs2VoiceParamHeader& param) {
    switch (param.id) {
        case SCE_NGS2_VOICE_PARAM_MATRIX_LEVELS: {
            const auto& levels = ParamAs<Ngs2VoiceMatrixLevelsParam>(param);
            SetMatrixLevels(voice, levels.matrix_id, levels.levels, levels.num_levels);
            break;
        }
        case SCE_NGS2_VOICE_PARAM_PORT_VOLUME: {
            const auto& volume = ParamAs<Ngs2VoicePortVolumeParam>(param);
            PortAt(voice, volume.port).volume = volume.level;
            break;
        }
        case SCE_NGS2_VOICE_PARAM_PORT_MATRIX: {
            const auto& matrix = ParamAs<Ngs2VoicePortMatrixParam>(param);
            SetPortMatrix(voice, matrix.port, matrix.matrix_id);
            break;
        }
        case SCE_NGS2_VOICE_PARAM_PORT_DELAY: {
            const auto& delay = ParamAs<Ngs2VoicePortDelayParam>(param);
            PortAt(voice, delay.port);
            if (delay.num_samples != 0) ngs2_stop("NGS2: port delay is not implemented");
            break;
        }
        case SCE_NGS2_VOICE_PARAM_PATCH: Patch(voice, ParamAs<Ngs2VoicePatchParam>(param)); break;
        case SCE_NGS2_VOICE_PARAM_EVENT: voice.SetEvent(ParamAs<Ngs2VoiceEventParam>(param).event_id); break;
        case SCE_NGS2_VOICE_PARAM_CALLBACK: {
            const auto& callback = ParamAs<Ngs2VoiceCallbackParam>(param);
            voice.callback = callback.callback;
            voice.callbackData = callback.callback_data;
            voice.callbackFlags = callback.flags;
            break;
        }
        default: ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " is not implemented");
    }
}

// PS4 VAG waveforms (type 0x1c) are channel-interleaved HE-VAG groups: one 16-byte frame
// per channel, each producing 28 samples. Each channel keeps its own four-sample predictor history
// across frames and waveform blocks. Byte 0 contains predictor/shift; byte 1 contains predictor/flags.
static constexpr std::uint32_t WAVEFORM_TYPE_VAG = 0x1c;

static void DecodeVagFrame(const std::uint8_t* frame, std::array<float, 4>& history,
                           std::int16_t* output, std::uint32_t stride) {
    std::uint32_t index = (frame[1] & 0xf0u) | (frame[0] >> 4);
    if (index > 127) index = 0;
    const std::uint32_t shift = frame[0] & 0x0f;
    const std::uint32_t flag = frame[1] & 0x0f;
    const float* coefficients = HEVAG_COEFS[index];
    for (std::uint32_t i = 0; i < 28; i++) {
        float sample = 0.0f;
        if (flag != 7) {
            std::int32_t code = (i & 1) != 0 ? frame[2 + i / 2] >> 4 : frame[2 + i / 2] & 0x0f;
            if (code >= 8) code -= 16;
            sample = static_cast<float>((code * 4096) >> shift) +
                     history[0] * coefficients[0] + history[1] * coefficients[1] +
                     history[2] * coefficients[2] + history[3] * coefficients[3];
        }
        history[3] = history[2];
        history[2] = history[1];
        history[1] = history[0];
        history[0] = sample;
        const auto pcm = static_cast<std::int32_t>(std::clamp(sample, -32768.0f, 32767.0f));
        output[i * stride] = static_cast<std::int16_t>(pcm);
    }
}

static std::vector<std::int16_t> DecodeVag(const std::uint8_t* data, std::uint64_t bytes, Ngs2Voice& voice) {
    const std::uint64_t groupBytes = static_cast<std::uint64_t>(voice.channels) * 16;
    if (bytes % groupBytes != 0) ngs2_stop("NGS2: VAG data ends inside a channel group");
    const std::uint64_t groups = bytes / groupBytes;
    std::vector<std::int16_t> out(static_cast<std::size_t>(groups * 28 * voice.channels));
    for (std::uint64_t group = 0; group < groups; group++) {
        const auto* frames = data + group * groupBytes;
        auto* decoded = out.data() + group * 28 * voice.channels;
        for (std::uint32_t channel = 0; channel < voice.channels; channel++) {
            DecodeVagFrame(frames + channel * 16, voice.vagHistory[channel], decoded + channel, voice.channels);
        }
    }
    return out;
}

static void SetupSampler(Ngs2Voice& voice, const Ngs2WaveformFormat& format) {
    voice.ResetSetup();
    if (format.waveform_type == 0 && format.num_channels == 0 && format.sample_rate == 0) return;
    if (format.waveform_type != SCE_NGS2_WAVEFORM_TYPE_PCM_I16L && format.waveform_type != SCE_NGS2_WAVEFORM_TYPE_ATRAC9 &&
        format.waveform_type != WAVEFORM_TYPE_VAG) {
        ngs2_stop("NGS2: waveform type " + Ngs2Hex(format.waveform_type) + " is not implemented");
    }
    if (format.frame_offset != 0 || format.frame_margin != 0) ngs2_stop("NGS2: waveform frame offset and margin are not implemented");
    if (format.num_channels == 0 || format.num_channels > NGS2_MAX_CHANNELS || format.sample_rate == 0) APS5_INVALID_ARG_EX;
    voice.channels = format.num_channels;
    voice.sampleRate = format.sample_rate;
    voice.vag = format.waveform_type == WAVEFORM_TYPE_VAG;
    voice.waveformType = voice.vag ? SCE_NGS2_WAVEFORM_TYPE_PCM_I16L : format.waveform_type;
    if (format.waveform_type == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) Ngs2SetupAtrac9(voice, format);
}

static void AppendStreamData(Ngs2Voice& voice, const Ngs2SamplerVoiceWaveformBlocksParam& param, std::size_t frameBytes) {
    if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) {
        const bool pendingWaveform = voice.atrac9.remainingSamples != 0 ||
            std::any_of(voice.blocks.begin(), voice.blocks.end(), [](const Ngs2Block& block) { return block.info.num_samples != 0; });
        if (!pendingWaveform) ngs2_stop("NGS2: appending compressed data requires a waveform");
        for (std::uint32_t i = 0; i < param.num_blocks; ++i) {
            auto block = param.blocks[i];
            if (block.data_size == 0) continue;
            block.num_samples = 0;
            block.num_skip_samples = 0;
            block.num_repeats = 0;
            voice.blocks.push_back({static_cast<const std::uint8_t*>(param.data) + block.data_offset, block});
        }
        return;
    }
    if (voice.blocks.empty() || !voice.blocks.back().streaming) ngs2_stop("NGS2: appending waveform data to a voice without a streaming waveform is not implemented");
    auto& stream = voice.blocks.back();
    for (std::uint32_t i = 0; i < param.num_blocks; i++) {
        const auto& block = param.blocks[i];
        if (block.data_size == 0) continue;
        if (block.data_size % frameBytes != 0 || block.num_skip_samples != 0 || block.num_repeats != 0) {
            ngs2_stop("NGS2: appending a waveform block with skipped samples, repeats or a partial frame is not implemented");
        }
        stream.pieces.push_back({static_cast<const std::uint8_t*>(param.data) + block.data_offset, stream.availableFrames, block.data_size / frameBytes});
        stream.availableFrames += block.data_size / frameBytes;
    }
}

static void AddWaveformBlocks(Ngs2Voice& voice, const Ngs2SamplerVoiceWaveformBlocksParam& param) {
    constexpr std::uint32_t knownFlags = SCE_NGS2_WAVEFORM_BLOCKS_FLAG_CONTINUE | SCE_NGS2_WAVEFORM_BLOCKS_FLAG_APPEND | SCE_NGS2_WAVEFORM_BLOCKS_FLAG_RESET | SCE_NGS2_WAVEFORM_BLOCKS_FLAG_SILENCE;
    if ((param.flags & ~knownFlags) != 0) ngs2_stop("NGS2: waveform block flags " + Ngs2Hex(param.flags) + " are not implemented");
    if ((param.num_blocks != 0 && (voice.channels == 0 || param.blocks == nullptr || param.data == nullptr)) || (voice.channels == 0 && (param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_APPEND) != 0)) APS5_INVALID_ARG_EX;
    const bool reset = (param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_RESET) != 0;
    if (!voice.acceptsBlocks && !reset) ngs2_stop("NGS2: the voice waveform was already closed");
    if (reset) {
        ++voice.waveformRevision;
        voice.blocks.clear();
        voice.phase = 0;
        voice.waveformEnd = nullptr;
        if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) Ngs2RestartAtrac9(voice);
        if (voice.vag) voice.vagHistory = {};
    }
    const std::size_t frameBytes = voice.channels * sizeof(std::int16_t);
    if (voice.vag) {
        if ((param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_APPEND) != 0) ngs2_stop("NGS2: streamed VAG is not implemented");
        voice.acceptsBlocks = (param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_CONTINUE) != 0;
        for (std::uint32_t i = 0; i < param.num_blocks; i++) {
            const auto& vag = param.blocks[i];
            if (vag.num_samples == 0 && vag.data_size == 0) continue;
            auto decoded = std::make_shared<const std::vector<std::int16_t>>(
                DecodeVag(static_cast<const std::uint8_t*>(param.data) + vag.data_offset, vag.data_size, voice));
            // GOW3_NGS2_DUMP_VAG=<folder> (diagnostics): the first 40 blocks, raw and decoded.
            static const char* dump = std::getenv("GOW3_NGS2_DUMP_VAG");
            static int dumped = 0;
            if (dump && dumped < 40) {
                const std::string base = std::string(dump) + "/vag" + std::to_string(dumped++) + "_" + std::to_string(voice.sampleRate);
                if (FILE* f = std::fopen((base + ".raw").c_str(), "wb")) {
                    std::fwrite(static_cast<const std::uint8_t*>(param.data) + vag.data_offset, 1, vag.data_size, f);
                    std::fclose(f);
                }
                if (FILE* f = std::fopen((base + ".s16").c_str(), "wb")) {
                    std::fwrite(decoded->data(), 2, decoded->size(), f);
                    std::fclose(f);
                }
            }
            auto block = vag;
            block.data_offset = 0;
            block.data_size = decoded->size() * sizeof(std::int16_t);
            const std::size_t decodedFrames = decoded->size() / voice.channels;
            if (block.num_samples == 0 || static_cast<std::uint64_t>(block.num_skip_samples) + block.num_samples > decodedFrames) {
                ngs2_stop("NGS2: VAG block " + std::to_string(i) + " does not fit its data");
            }
            Ngs2Block added{reinterpret_cast<const std::uint8_t*>(decoded->data()), block};
            added.decoded = std::move(decoded);
            voice.blocks.push_back(std::move(added));
        }
        return;
    }
    if ((param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_APPEND) != 0) {
        AppendStreamData(voice, param, frameBytes);
        voice.acceptsBlocks = (param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_CONTINUE) != 0;
        return;
    }
    voice.acceptsBlocks = (param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_CONTINUE) != 0;
    const bool allowsSilence = (param.flags & SCE_NGS2_WAVEFORM_BLOCKS_FLAG_SILENCE) != 0;
    for (std::uint32_t i = 0; i < param.num_blocks; i++) {
        auto block = param.blocks[i];
        if (block.num_samples == 0 && block.data_size == 0) continue;
        if (allowsSilence && block.data_size == 0) {
            if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) ngs2_stop("NGS2: a dataless waveform block under the silence flag is not implemented for ATRAC9 voices");
            voice.blocks.push_back({nullptr, block});
            continue;
        }
        const std::uint64_t bytes = voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9 ? Ngs2Atrac9BlockBytes(voice, block)
                                  : (static_cast<std::uint64_t>(block.num_skip_samples) + block.num_samples) * frameBytes;
        const bool streaming = block.num_samples != 0 && bytes > block.data_size && voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_PCM_I16L &&
                               block.num_skip_samples == 0 && block.num_repeats == 0 && block.data_size >= frameBytes && block.data_size % frameBytes == 0;
        const bool compressedStream = voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9 && voice.acceptsBlocks && block.num_repeats == 0;
        if (block.num_samples == 0 || (bytes > block.data_size && !streaming && !compressedStream)) {
            // knack_pc: Knack once passed a block whose samples ran past its data (after 13 minutes
            // of play). The samples that fit play; a block with none is left out (logged once).
            std::uint64_t fit = block.data_size / frameBytes;
            if (voice.waveformType == SCE_NGS2_WAVEFORM_TYPE_ATRAC9) {
                fit = voice.atrac9.superframeBytes == 0 ? 0
                    : static_cast<std::uint64_t>(block.data_size) / voice.atrac9.superframeBytes *
                          voice.atrac9.frameSamples * voice.atrac9.framesInSuperframe;
            }
            fit = fit > block.num_skip_samples ? fit - block.num_skip_samples : 0;
            static bool logged = false;
            if (!logged) {
                logged = true;
                std::printf("NGS2: waveform block %u (type %u, %u channels, flags %#x) has %u samples after %u "
                            "skipped, %u repeats, %llu data bytes (needs %llu): %llu samples play\n",
                            i, voice.waveformType, voice.channels, param.flags, block.num_samples,
                            block.num_skip_samples, block.num_repeats, static_cast<unsigned long long>(block.data_size),
                            static_cast<unsigned long long>(bytes),
                            static_cast<unsigned long long>(std::min<std::uint64_t>(fit, block.num_samples)));
            }
            if (block.num_samples == 0 || fit == 0) continue;
            block.num_samples = static_cast<std::uint32_t>(std::min<std::uint64_t>(fit, block.num_samples));
        }
        voice.blocks.push_back({static_cast<const std::uint8_t*>(param.data) + block.data_offset, block});
        if (streaming) {
            auto& stream = voice.blocks.back();
            stream.streaming = true;
            stream.availableFrames = block.data_size / frameBytes;
            stream.pieces.push_back({stream.data, 0, stream.availableFrames});
        }
    }
}

void Ngs2SetFilter(Ngs2Voice& voice, const Ngs2SamplerVoiceFilterParam& param) {
    if (param.index >= voice.rack->maxFilters) APS5_INVALID_ARG_EX;
    if (voice.filters.size() <= param.index) voice.filters.resize(param.index + 1);
    auto& filter = voice.filters[param.index];
    const std::uint32_t location = param.location & 0xffu;
    const std::uint32_t type = param.type & 0xffu;
    if (type == 0) {
        filter = {};
        return;
    }
    if (param.location > 17 || (type > 10 && type != 0x20)) {
        ngs2_stop("NGS2: filter location " + std::to_string(location) + " type " + std::to_string(type) + " is not implemented");
    }
    const bool direct = type == 0x20;
    float directCoefficients[5] = {param.frequency, param.q, param.level, 0.0f, 0.0f};
    std::memcpy(&directCoefficients[3], &param.reserved[0], sizeof(float));
    std::memcpy(&directCoefficients[4], &param.reserved[1], sizeof(float));
    if (direct) {
        for (float coefficient : directCoefficients) if (!std::isfinite(coefficient)) APS5_INVALID_ARG_EX;
    } else if (!std::isfinite(param.frequency) || !std::isfinite(param.q) ||
               !std::isfinite(param.level) || param.frequency < 0.0f || param.q <= 0.0f || param.level < 0.0f) {
        APS5_INVALID_ARG_EX;
    }

    if (!filter.enabled || filter.type != type) filter.history.assign(voice.channels, {});
    filter.enabled = true;
    filter.location = location;
    filter.type = type;
    filter.bypassMask = param.channel_mask & 0xffu;
    filter.history.resize(voice.channels);

    if (direct) {
        filter.b0 = directCoefficients[0];
        filter.b1 = directCoefficients[1];
        filter.b2 = directCoefficients[2];
        filter.a1 = -directCoefficients[3];
        filter.a2 = -directCoefficients[4];
        return;
    }

    const double rate = voice.rack->system->option.sample_rate;
    const double normalized = param.frequency / rate;
    const double omega = (normalized < 0.001 ? 0.0062831853 : std::min(normalized, 0.49987)) * 2.0 * std::numbers::pi;
    const double q = std::clamp(static_cast<double>(param.q), 0.0001, 32.0);
    const double sine = std::sin(omega), cosine = std::cos(omega);
    const double alpha = sine / (2.0 * q);
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;
    switch (type) {
        case 6:
        case 7: {
            const double a = std::sqrt(std::max(static_cast<double>(param.level), 0.0001));
            const double beta = std::sqrt(a) / q * sine;
            if (type == 6) {
                b0 = a * ((a + 1.0) - (a - 1.0) * cosine + beta);
                b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cosine);
                b2 = a * ((a + 1.0) - (a - 1.0) * cosine - beta);
                a0 = (a + 1.0) + (a - 1.0) * cosine + beta;
                a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cosine);
                a2 = (a + 1.0) + (a - 1.0) * cosine - beta;
            } else {
                b0 = a * ((a + 1.0) + (a - 1.0) * cosine + beta);
                b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosine);
                b2 = a * ((a + 1.0) + (a - 1.0) * cosine - beta);
                a0 = (a + 1.0) - (a - 1.0) * cosine + beta;
                a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosine);
                a2 = (a + 1.0) - (a - 1.0) * cosine - beta;
            }
            break;
        }
        case 8: {
            const double a = std::sqrt(std::max(static_cast<double>(param.level), 0.0001));
            b0 = 1.0 + alpha * a; b1 = -2.0 * cosine; b2 = 1.0 - alpha * a;
            a0 = 1.0 + alpha / a; a1 = -2.0 * cosine; a2 = 1.0 - alpha / a;
            break;
        }
        case 9:
            b0 = b1 = sine; b2 = 0.0;
            a0 = sine + cosine + 1.0; a1 = sine - cosine - 1.0; a2 = 0.0;
            break;
        case 10:
            b0 = cosine + 1.0; b1 = -(cosine + 1.0); b2 = 0.0;
            a0 = sine + cosine + 1.0; a1 = sine - (cosine + 1.0); a2 = 0.0;
            break;
        default:
            switch (type) {
                case 2: b0 = b2 = (1.0 + cosine) * 0.5; b1 = -(1.0 + cosine); break;
                case 3: b0 = sine * 0.5; b1 = 0.0; b2 = -sine * 0.5; break;
                case 4: b0 = alpha; b1 = 0.0; b2 = -alpha; break;
                case 5: b0 = 1.0; b1 = -2.0 * cosine; b2 = 1.0; break;
                default: b0 = b2 = (1.0 - cosine) * 0.5; b1 = 1.0 - cosine; break;
            }
            a0 = 1.0 + alpha; a1 = -2.0 * cosine; a2 = 1.0 - alpha;
            break;
    }
    filter.b0 = b0 / a0;
    filter.b1 = b1 / a0;
    filter.b2 = b2 / a0;
    filter.a1 = a1 / a0;
    filter.a2 = a2 / a0;
}

// PS4 voice parameters retained by the Runtime ABI adapter.
static constexpr std::uint32_t SAMPLER_VOICE_PARAM_ENVELOPE = 0x10000006;
static constexpr std::uint32_t SUBMIXER_VOICE_PARAM_COMPRESSOR = 0x20000002;
static constexpr std::uint32_t SUBMIXER_VOICE_PARAM_FILTER = 0x20000006;
struct Ps4EnvelopeParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t num_forward, num_release;
    const Ngs2EnvelopePoint* points;
};
struct Ps4CompressorParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t flags;
    float threshold, ratio, knee, attack_time, release_time, level;
    std::uint32_t reserved;
};
struct Ps4LimiterParam {
    Ngs2VoiceParamHeader header;
    std::uint32_t enabled;
    float threshold;
};

static void ApplySamplerParam(Ngs2Voice& voice, const Ngs2VoiceParamHeader& param) {
    switch (param.id) {
        case SCE_NGS2_SAMPLER_VOICE_PARAM_SETUP:
        case SCE_NGS2_CUSTOM_SAMPLER_VOICE_PARAM_SETUP:
            SetupSampler(voice, ParamAs<Ngs2SamplerVoiceSetupParam>(param).format);
            break;
        case SCE_NGS2_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS:
        case SCE_NGS2_CUSTOM_SAMPLER_VOICE_PARAM_ADD_WAVEFORM_BLOCKS:
            AddWaveformBlocks(voice, ParamAs<Ngs2SamplerVoiceWaveformBlocksParam>(param));
            break;
        case SCE_NGS2_SAMPLER_VOICE_PARAM_EXIT_LOOP:
        case SCE_NGS2_CUSTOM_SAMPLER_VOICE_PARAM_EXIT_LOOP: {
            const auto looping = std::find_if(voice.blocks.begin(), voice.blocks.end(), [](const Ngs2Block& block) { return block.info.num_repeats != 0; });
            if (looping != voice.blocks.end()) looping->info.num_repeats = 0;
            break;
        }
        case SCE_NGS2_SAMPLER_VOICE_PARAM_PITCH:
        case SCE_NGS2_CUSTOM_SAMPLER_VOICE_PARAM_PITCH: {
            const float ratio = ParamAs<Ngs2SamplerVoicePitchParam>(param).ratio;
            if (!std::isfinite(ratio) || ratio < 0.0f) APS5_INVALID_ARG_EX;
            voice.pitch = ratio;
            break;
        }
        case SCE_NGS2_SAMPLER_VOICE_PARAM_FILTER: Ngs2SetFilter(voice, ParamAs<Ngs2SamplerVoiceFilterParam>(param)); break;
        case SAMPLER_VOICE_PARAM_ENVELOPE: {
            const auto& envelope = ParamAs<Ps4EnvelopeParam>(param);
            const std::uint32_t count = envelope.num_forward + envelope.num_release;
            if (count != 0 && envelope.points == nullptr) APS5_INVALID_ARG_EX;
            voice.envelope.forward.assign(envelope.points, envelope.points + envelope.num_forward);
            voice.envelope.release.assign(envelope.points + envelope.num_forward, envelope.points + count);
            voice.envelope.releasing = false;
            voice.envelope.segment = 0;
            voice.envelope.position = 0;
            voice.envelope.start = 0.0f;
            voice.envelope.gain = envelope.num_forward == 0 ? 1.0f : 0.0f;
            break;
        }
        case SCE_NGS2_SAMPLER_VOICE_PARAM_PEAK_METER:
            // The PS4 implementation accepts this parameter but does not gate peak reporting with it.
            break;
        default: ngs2_stop("NGS2: sampler voice param " + Ngs2Hex(param.id) + " is not implemented");
    }
}

static void SetupMixer(Ngs2Voice& voice, std::uint32_t numChannels) {
    if (numChannels == 0 || numChannels > voice.rack->maxChannels) APS5_INVALID_ARG_EX;
    voice.ResetSetup();
    voice.channels = numChannels;
    // The PS4 implementation marks configured Submixer, Reverb and Mastering voices READY.
    voice.state = Ngs2PlayState::Ready;
    voice.stateFlags = static_cast<std::uint32_t>(voice.state);
}

static void ApplyParam(Ngs2Voice& voice, const Ngs2VoiceParamHeader& param) {
    const std::uint32_t rackId = param.id >> 16;
    if (rackId == 0) {
        ApplyCommonParam(voice, param);
        return;
    }
    if (rackId == (SCE_NGS2_CUSTOM_VOICE_PARAM_USER_FX2 >> 16) || rackId == SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER) {
        Ngs2ApplyCustomParam(voice, param);
        return;
    }
    if (rackId != voice.rack->rackId) ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " belongs to another rack");
    switch (rackId) {
        case SCE_NGS2_RACK_ID_SAMPLER:
        case SCE_NGS2_RACK_ID_CUSTOM_SAMPLER: ApplySamplerParam(voice, param); return;
        case SCE_NGS2_RACK_ID_SUBMIXER:
            if (param.id == SCE_NGS2_SUBMIXER_VOICE_PARAM_USER_FX) {
                const auto& fx = ParamAs<Ngs2SubmixerVoiceUserFxParam>(param);
                voice.userFxHandler = fx.handler;
                voice.userFxData = {fx.user_data0, fx.user_data1, fx.user_data2};
                voice.userFxFlags = fx.handler ? 1 : 0;
                return;
            }
            if (param.id == SUBMIXER_VOICE_PARAM_FILTER) {
                Ngs2SetFilter(voice, ParamAs<Ngs2SamplerVoiceFilterParam>(param));
                return;
            }
            if (param.id == SUBMIXER_VOICE_PARAM_COMPRESSOR) {
                const auto& c = ParamAs<Ps4CompressorParam>(param);
                if (!std::isfinite(c.threshold) || !std::isfinite(c.ratio) || !std::isfinite(c.knee) ||
                    !std::isfinite(c.attack_time) || !std::isfinite(c.release_time) || !std::isfinite(c.level)) {
                    APS5_INVALID_ARG_EX;
                }
                const bool wasEnabled = voice.compressor.enabled;
                voice.compressor.enabled = (c.flags & 1u) == 0;
                voice.compressor.rms = (c.flags & 2u) != 0;
                voice.compressor.linkAllButLfe = (c.flags & 8u) != 0;
                voice.compressor.threshold = std::clamp(c.threshold, 0.0f, 1.0f);
                voice.compressor.ratio = std::clamp(c.ratio, 0.0f, 1.0f);
                voice.compressor.knee = std::clamp(c.knee, 0.0f, 1.0f);
                voice.compressor.attack = std::clamp(c.attack_time, 0.0f, 10.0f);
                voice.compressor.release = std::clamp(c.release_time, 0.0f, 10.0f);
                voice.compressor.level = std::clamp(c.level, 0.0f, 32.0f);
                if (!wasEnabled && voice.compressor.enabled) voice.compressor.gain.fill(1.0f);
                return;
            }
            if (param.id != SCE_NGS2_SUBMIXER_VOICE_PARAM_SETUP) break;
            if (ParamAs<Ngs2SubmixerVoiceSetupParam>(param).flags != 0) ngs2_stop("NGS2: submixer setup flags are not implemented");
            SetupMixer(voice, ParamAs<Ngs2SubmixerVoiceSetupParam>(param).num_io_channels);
            return;
        case SCE_NGS2_RACK_ID_REVERB:
            if (param.id == SCE_NGS2_REVERB_VOICE_PARAM_SETUP) {
                const auto& setup = ParamAs<Ngs2ReverbVoiceSetupParam>(param);
                if (setup.flags != 0) ngs2_stop("NGS2: reverb setup flags are not implemented");
                if (setup.num_input_channels != setup.num_output_channels) ngs2_stop("NGS2: reverb channel conversion is not implemented");
                if (setup.num_output_channels != 1 && setup.num_output_channels != 2 && setup.num_output_channels != 6 && setup.num_output_channels != 8) APS5_INVALID_ARG_EX;
                SetupMixer(voice, setup.num_output_channels);
                Ngs2SetupReverb(voice);
                return;
            }
            if (param.id == SCE_NGS2_REVERB_VOICE_PARAM_I3DL2) {
                Ngs2SetReverbParams(voice, ParamAs<Ngs2ReverbVoiceI3DL2Param>(param).i3dl2);
                return;
            }
            break;
        case SCE_NGS2_RACK_ID_MASTERING:
            if (param.id == SCE_NGS2_MASTERING_VOICE_PARAM_SETUP) {
                SetupMixer(voice, ParamAs<Ngs2MasteringVoiceSetupParam>(param).num_io_channels);
                return;
            }
            if (param.id == SCE_NGS2_MASTERING_VOICE_PARAM_LIMITER) {
                const auto& limiter = ParamAs<Ps4LimiterParam>(param);
                if (!std::isfinite(limiter.threshold)) APS5_INVALID_ARG_EX;
                const bool wasEnabled = voice.limiter.enabled;
                voice.limiter.enabled = limiter.enabled != 0;
                voice.limiter.rms = false;
                voice.limiter.linkAllButLfe = false;
                voice.limiter.threshold = std::clamp(limiter.threshold, 0.0f, 4.0f);
                voice.limiter.ratio = 0.0f;
                voice.limiter.knee = 0.0f;
                voice.limiter.attack = 0.001f;
                voice.limiter.release = 0.01f;
                voice.limiter.level = 1.0f;
                if (!wasEnabled && voice.limiter.enabled) voice.limiter.gain.fill(1.0f);
                return;
            }
            if (param.id == SCE_NGS2_MASTERING_VOICE_PARAM_GAIN) {
                const auto& gain = ParamAs<Ngs2MasteringVoiceGainParam>(param);
                if (!std::isfinite(gain.fbw_level) || !std::isfinite(gain.lfe_level)) APS5_INVALID_ARG_EX;
                voice.fbwLevel = gain.fbw_level;
                voice.lfeLevel = gain.lfe_level;
                return;
            }
            if (param.id == SCE_NGS2_MASTERING_VOICE_PARAM_OUTPUT) {
                voice.outputId = ParamAs<Ngs2MasteringVoiceOutputParam>(param).output_id;
                return;
            }
            break;
        default: break;
    }
    ngs2_stop("NGS2: voice param " + Ngs2Hex(param.id) + " is not implemented");
}

#pragma GCC visibility push(default)

extern "C" {

int APS5_VABI sceNgs2VoiceControl(uintptr_t voice_handle, const Ngs2VoiceParamHeader* param_list) {
    if (param_list == nullptr) return SCE_NGS2_ERROR_NULL_PARAM_LIST;
    const Ngs2Lock lock;
    auto* found = Ngs2FindVoice(voice_handle);
    if (found == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    auto& voice = *found;
    for (const auto* param = param_list;; param = reinterpret_cast<const Ngs2VoiceParamHeader*>(reinterpret_cast<const std::uint8_t*>(param) + param->next)) {
        ApplyParam(voice, *param);
        if (param->next == 0) break;
    }
    return SCE_NGS2_OK;
}

int APS5_VABI sceNgs2VoiceRunCommands(uintptr_t voice_handle, const Ngs2VoiceCommand* commands, size_t num_commands) {
    const Ngs2Lock lock;
    auto* found = Ngs2FindVoice(voice_handle);
    if (found == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    auto& voice = *found;
    if (commands == nullptr && num_commands != 0) APS5_INVALID_ARG_EX;
    for (std::size_t i = 0; i < num_commands; i++) {
        const auto& command = commands[i];
        const std::uint32_t index = command.id >> 24;
        const std::uint32_t id = command.id & 0xffffff;
        const std::uint8_t expectedType = id == COMMAND_EVENT ? COMMAND_TYPE_UINT
                                        : id == COMMAND_MATRIX_LEVELS ? COMMAND_TYPE_FLOAT_ARRAY
                                        : id == COMMAND_PORT_VOLUME ? COMMAND_TYPE_FLOAT
                                        : id == COMMAND_PORT_MATRIX ? COMMAND_TYPE_INT : 0;
        if (expectedType == 0) ngs2_stop("NGS2: voice command " + Ngs2Hex(command.id) + " is not implemented");
        if (command.type != expectedType) ngs2_stop("NGS2: voice command " + Ngs2Hex(command.id) + " has value type " + Ngs2Hex(command.type));
        if (id == COMMAND_EVENT) voice.SetEvent(command.value.u);
        else if (id == COMMAND_MATRIX_LEVELS) SetMatrixLevels(voice, index, command.value.levels, command.count);
        else if (id == COMMAND_PORT_VOLUME) PortAt(voice, index).volume = command.value.f;
        else SetPortMatrix(voice, index, command.value.i);
    }
    return SCE_NGS2_OK;
}

int APS5_VABI sceNgs2VoiceGetState(uintptr_t voice_handle, Ngs2VoiceState* state, size_t state_size) {
    const Ngs2Lock lock;
    auto* found = Ngs2FindVoice(voice_handle);
    if (found == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (state_size < sizeof(Ngs2VoiceState)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
    if (state == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const auto& voice = *found;
    switch (voice.rack->rackId) {
        case SCE_NGS2_RACK_ID_SAMPLER: {
            if (state_size != sizeof(Ngs2SamplerVoiceState)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
            auto& sampler = *reinterpret_cast<Ngs2SamplerVoiceState*>(state);
            sampler = {};
            sampler.voice_state.state_flags = voice.stateFlags;
            sampler.envelope_height = voice.envelope.gain;
            sampler.peak_height = voice.peak;
            sampler.num_decoded_samples = voice.decodedSamples;
            sampler.decoded_data_size = voice.decodedBytes;
            sampler.user_data = voice.blocks.empty() ? 0 : voice.blocks.front().info.user_data;
            sampler.waveform_data = voice.WaveformData();
            return SCE_NGS2_OK;
        }
        case SCE_NGS2_RACK_ID_REVERB: {
            if (state_size != sizeof(Ngs2VoiceState)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
            *state = {};
            state->state_flags = voice.stateFlags;
            return SCE_NGS2_OK;
        }
        case SCE_NGS2_RACK_ID_SUBMIXER: {
            if (state_size != sizeof(Ngs2SubmixerVoiceState)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
            auto& submixer = *reinterpret_cast<Ngs2SubmixerVoiceState*>(state);
            submixer = {};
            submixer.voice_state.state_flags = voice.stateFlags;
            submixer.envelope_height = 1.0f;
            submixer.peak_height = voice.peak;
            submixer.compressor_height = 1.0f;
            return SCE_NGS2_OK;
        }
        case SCE_NGS2_RACK_ID_CUSTOM_SUBMIXER: {
            if (state_size != sizeof(Ngs2CustomSubmixerVoiceState)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
            auto& custom = *reinterpret_cast<Ngs2CustomSubmixerVoiceState*>(state);
            custom = {};
            custom.voice_state.state_flags = voice.stateFlags;
            return SCE_NGS2_OK;
        }
        case SCE_NGS2_RACK_ID_CUSTOM_SAMPLER: {
            if (state_size != sizeof(Ngs2CustomSamplerVoiceState)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
            auto& sampler = *reinterpret_cast<Ngs2CustomSamplerVoiceState*>(state);
            sampler = {};
            sampler.voice_state.state_flags = voice.stateFlags;
            sampler.num_decoded_samples = voice.decodedSamples;
            return SCE_NGS2_OK;
        }
        default: ngs2_stop("NGS2: voice state of rack " + Ngs2Hex(voice.rack->rackId) + " is not implemented");
    }
}

int APS5_VABI sceNgs2VoiceGetPortInfo(uintptr_t voice_handle, uint32_t port, Ngs2VoicePortInfo* info, size_t info_size) {
    if (info == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    if (info_size != sizeof(Ngs2VoicePortInfo)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
    const Ngs2Lock lock;
    auto* found = Ngs2FindVoice(voice_handle);
    if (found == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    auto& voice = *found;
    const auto& source = PortAt(voice, port);
    *info = {};
    info->matrix_id = source.matrix;
    info->volume = source.volume;
    info->dest_handle = reinterpret_cast<Ngs2Handle>(source.dest);
    return SCE_NGS2_OK;
}

int APS5_VABI sceNgs2VoiceQueryInfo(uintptr_t voice_handle, uint32_t info_id, void* info, size_t info_size) {
    if (info == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const Ngs2Lock lock;
    auto* found = Ngs2FindVoice(voice_handle);
    if (found == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    const auto& voice = *found;
    switch (info_id) {
        case SCE_NGS2_VOICE_INFO_CHANNELS: {
            if (info_size != sizeof(Ngs2VoiceChannelsInfo)) return SCE_NGS2_ERROR_INVALID_OUT_SIZE;
            *static_cast<Ngs2VoiceChannelsInfo*>(info) = {voice.channels, 0};
            return SCE_NGS2_OK;
        }
        default: ngs2_stop("NGS2: voice info " + Ngs2Hex(info_id) + " is not implemented");
    }
}

int APS5_VABI sceNgs2VoiceGetStateFlags(uintptr_t voice_handle, uint32_t* state_flags) {
    if (state_flags == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    const Ngs2Lock lock;
    const auto* found = Ngs2FindVoice(voice_handle);
    if (found == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    *state_flags = found->stateFlags;
    return SCE_NGS2_OK;
}

int APS5_VABI sceNgs2VoiceGetOwner(uintptr_t voice_handle, uintptr_t* owner_rack, uint32_t* voice_index) {
    std::lock_guard lock(Ngs2Mutex());
    const auto* found = Ngs2FindVoice(voice_handle);
    if (found == nullptr) return SCE_NGS2_ERROR_INVALID_VOICE_HANDLE;
    if (owner_rack == nullptr || voice_index == nullptr) return SCE_NGS2_ERROR_INVALID_OUT_ADDRESS;
    *owner_rack = reinterpret_cast<Ngs2Handle>(found->rack);
    *voice_index = static_cast<uint32_t>(found - found->rack->voices.data());
    return SCE_NGS2_OK;
}

int APS5_VABI sceNgs2PanInit(Ngs2PanWork* work, const float* speaker_angles, float unit_angle, uint32_t num_speakers) {
    return Ngs2PanInit(work, speaker_angles, unit_angle, num_speakers);
}

int APS5_VABI sceNgs2PanGetVolumeMatrix(Ngs2PanWork* work, const Ngs2PanParam* params, uint32_t num_params, uint32_t matrix_format, float* out_volume_matrix) {
    return Ngs2PanGetVolumeMatrix(work, params, num_params, matrix_format, out_volume_matrix);
}

int APS5_VABI sceNgs2GeomApply(const Ngs2GeomListenerWork* listener, const Ngs2GeomSourceParam* source, Ngs2GeomAttribute* out_attrib, std::uint32_t flags) {
    return Ngs2GeomApply(listener, source, out_attrib, flags);
}

int APS5_VABI sceNgs2GeomResetListenerParam(Ngs2GeomListenerParam* out_listener_param) {
    return Ngs2GeomResetListenerParam(out_listener_param);
}

int APS5_VABI sceNgs2GeomCalcListener(const Ngs2GeomListenerParam* param, Ngs2GeomListenerWork* outWork, std::uint32_t flags) {
    return Ngs2GeomCalcListener(param, outWork, flags);
}

int APS5_VABI sceNgs2GeomResetSourceParam(Ngs2GeomSourceParam* out_source_param) {
    return Ngs2GeomResetSourceParam(out_source_param);
}

}

#pragma GCC visibility pop
