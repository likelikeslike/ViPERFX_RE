#include "ViperContext.h"
#include "log.h"
#include "viper/constants.h"
#include <chrono>

#define SET(type, ptr, value) (*(type *) (ptr) = (value))

constexpr int32_t kParamGetEnabled = 1;
constexpr int32_t kParamGetConfigure = 2;
constexpr int32_t kParamGetStreaming = 3;
constexpr int32_t kParamGetSamplingRate = 4;
constexpr int32_t kParamGetConvolutionKernelId = 5;
constexpr int32_t kParamGetDriverVersionCode = 6;
constexpr int32_t kParamGetDriverVersionName = 7;
constexpr int32_t kParamGetArchitecture = 8;
constexpr size_t kParamPayloadHeaderSize =
    sizeof(uint32_t) + sizeof(int32_t) + sizeof(uint32_t);

bool DecodeParamValue(const uint8_t *bytes, size_t byte_count, viper::ParamValue *out) {
    if (bytes == nullptr || out == nullptr || byte_count < kParamPayloadHeaderSize) {
        return false;
    }

    uint32_t type;
    uint32_t count_or_size;
    std::memcpy(&type, bytes, sizeof(type));
    std::memcpy(&out->index, bytes + sizeof(type), sizeof(out->index));
    std::memcpy(
        &count_or_size, bytes + sizeof(type) + sizeof(out->index), sizeof(count_or_size)
    );

    out->type = static_cast<viper::ParamValueType>(type);
    const uint8_t *payload = bytes + kParamPayloadHeaderSize;
    const size_t payload_size = byte_count - kParamPayloadHeaderSize;

    switch (out->type) {
        case viper::ParamValueType::kBool:
            if (count_or_size != 1 || payload_size != 1) return false;
            out->bool_value = payload[0] != 0;
            return true;
        case viper::ParamValueType::kInt:
            if (count_or_size != 1 || payload_size != sizeof(int32_t)) return false;
            std::memcpy(&out->int_value, payload, sizeof(out->int_value));
            return true;
        case viper::ParamValueType::kFloat:
            if (count_or_size != 1 || payload_size != sizeof(float)) return false;
            std::memcpy(&out->float_value, payload, sizeof(out->float_value));
            return true;
        case viper::ParamValueType::kFloatArray:
            if (payload_size != count_or_size * sizeof(float)) return false;
            out->float_array = reinterpret_cast<const float *>(payload);
            out->float_count = count_or_size;
            return true;
        case viper::ParamValueType::kBytes:
            if (payload_size != count_or_size) return false;
            out->bytes = payload;
            out->byte_count = count_or_size;
            return true;
        case viper::ParamValueType::kIntArray:
            if (payload_size != count_or_size * sizeof(int32_t)) return false;
            out->int_array = reinterpret_cast<const int32_t *>(payload);
            out->int_count = count_or_size;
            return true;
        default:
            return false;
    }
}

ViperContext::ViperContext() :
    config_({}),
    disable_reason_(DisableReason::NONE),
    buffer_(std::vector<float>()),
    buffer_frame_count_(0),
    enable_(false),
    has_processed_(false),
    fade_in_remaining_(0) {
    VIPER_LOGI("ViperContext created");
}

void ViperContext::CopyBufferConfig(buffer_config_t *dest, const buffer_config_t *src) {
    if (src->mask & EFFECT_CONFIG_BUFFER) {
        dest->buffer = src->buffer;
    }

    if (src->mask & EFFECT_CONFIG_SMP_RATE) {
        dest->sampling_rate = src->sampling_rate;
    }

    if (src->mask & EFFECT_CONFIG_CHANNELS) {
        dest->channels = src->channels;
    }

    if (src->mask & EFFECT_CONFIG_FORMAT) {
        dest->format = src->format;
    }

    if (src->mask & EFFECT_CONFIG_ACC_MODE) {
        dest->access_mode = src->access_mode;
    }

    if (src->mask & EFFECT_CONFIG_PROVIDER) {
        dest->buffer_provider = src->buffer_provider;
    }

    dest->mask |= src->mask;
}

void ViperContext::HandleSetConfig(const effect_config_t *new_config) {
    VIPER_LOGI("Checking input and output configuration ...");

    VIPER_LOGI("Input mask: 0x%04X", new_config->input_cfg.mask);
    VIPER_LOGI("Input buffer frame count: %zu", new_config->input_cfg.buffer.frame_count);
    VIPER_LOGI("Input sampling rate: %d", new_config->input_cfg.sampling_rate);
    VIPER_LOGI("Input channels: %d", new_config->input_cfg.channels);
    VIPER_LOGI("Input format: %d", new_config->input_cfg.format);
    VIPER_LOGI("Input access mode: %d", new_config->input_cfg.access_mode);
    VIPER_LOGI("Output mask: 0x%04X", new_config->output_cfg.mask);
    VIPER_LOGI(
        "Output buffer frame count: %zu", new_config->output_cfg.buffer.frame_count
    );
    VIPER_LOGI("Output sampling rate: %d", new_config->output_cfg.sampling_rate);
    VIPER_LOGI("Output channels: %d", new_config->output_cfg.channels);
    VIPER_LOGI("Output format: %d", new_config->output_cfg.format);
    VIPER_LOGI("Output access mode: %d", new_config->output_cfg.access_mode);

    SetDisableReason(DisableReason::UNKNOWN);

    CopyBufferConfig(&config_.input_cfg, &new_config->input_cfg);
    CopyBufferConfig(&config_.output_cfg, &new_config->output_cfg);

    if (config_.input_cfg.buffer.frame_count != config_.output_cfg.buffer.frame_count) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.FC = %zu, out.FC = %zu]",
            config_.input_cfg.buffer.frame_count,
            config_.output_cfg.buffer.frame_count
        );
        SetDisableReason(
            DisableReason::INVALID_FRAME_COUNT, "Input and output frame count mismatch"
        );
        return;
    }

    if (config_.input_cfg.sampling_rate != config_.output_cfg.sampling_rate) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.SR = %d, out.SR = %d]",
            config_.input_cfg.sampling_rate,
            config_.output_cfg.sampling_rate
        );
        SetDisableReason(
            DisableReason::INVALID_SAMPLING_RATE,
            "Input and output sampling rate mismatch"
        );
        return;
    }

    if (config_.input_cfg.channels != config_.output_cfg.channels) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.CH = %d, out.CH = %d]",
            config_.input_cfg.channels,
            config_.output_cfg.channels
        );
        SetDisableReason(
            DisableReason::INVALID_CHANNEL_COUNT,
            "Input and output channel count mismatch"
        );
        return;
    }

    if (config_.input_cfg.channels != AUDIO_CHANNEL_OUT_STEREO) {
        VIPER_LOGE("ViPER4Android disabled, reason [CH != 2]");
        SetDisableReason(
            DisableReason::INVALID_CHANNEL_COUNT,
            "Invalid channel count: " + std::to_string(config_.input_cfg.channels)
        );
        return;
    }

    if (config_.input_cfg.format != AUDIO_FORMAT_PCM_16_BIT
        && config_.input_cfg.format != AUDIO_FORMAT_PCM_32_BIT
        && config_.input_cfg.format != AUDIO_FORMAT_PCM_FLOAT) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [in.FMT = %d]", config_.input_cfg.format
        );
        VIPER_LOGE(
            "We only accept AUDIO_FORMAT_PCM_16_BIT, AUDIO_FORMAT_PCM_32_BIT and "
            "AUDIO_FORMAT_PCM_FLOAT input format!"
        );
        SetDisableReason(
            DisableReason::INVALID_FORMAT,
            "Invalid input format: " + std::to_string(config_.input_cfg.format)
        );
        return;
    }

    if (config_.output_cfg.format != AUDIO_FORMAT_PCM_16_BIT
        && config_.output_cfg.format != AUDIO_FORMAT_PCM_32_BIT
        && config_.output_cfg.format != AUDIO_FORMAT_PCM_FLOAT) {
        VIPER_LOGE(
            "ViPER4Android disabled, reason [out.FMT = %d]", config_.output_cfg.format
        );
        VIPER_LOGE(
            "We only accept AUDIO_FORMAT_PCM_16_BIT, AUDIO_FORMAT_PCM_32_BIT and "
            "AUDIO_FORMAT_PCM_FLOAT output format!"
        );
        SetDisableReason(
            DisableReason::INVALID_FORMAT,
            "Invalid output format: " + std::to_string(config_.output_cfg.format)
        );
        return;
    }

    VIPER_LOGI("Input and output configuration checked.");
    SetDisableReason(DisableReason::NONE);

    // Processing buffer
    buffer_.resize(config_.input_cfg.buffer.frame_count * 2);
    buffer_frame_count_ = config_.input_cfg.buffer.frame_count;

    // ViPER
    viper_.SetSamplingRate(config_.input_cfg.sampling_rate);
    viper_.ResetAllEffects();
}

int32_t ViperContext::HandleSetParam(effect_param_t *cmd_param, void *reply_data) {
    // The value offset of an effect parameter is computed by rounding up
    // the parameter size to the next 32 bit alignment.
    const uint32_t offset =
        ((cmd_param->psize + sizeof(int32_t) - 1) / sizeof(int32_t)) * sizeof(int32_t);

    *static_cast<int *>(reply_data) = 0;

    const int param = *reinterpret_cast<int *>(cmd_param->data);
    viper::ParamValue value;
    if (!DecodeParamValue(
            reinterpret_cast<const uint8_t *>(cmd_param->data + offset),
            cmd_param->vsize,
            &value
        )) {
        return -EINVAL;
    }
    return viper_.DispatchParamValue(param, value) ? 0 : -EINVAL;
}

int32_t ViperContext::HandleGetParam(
    effect_param_t *cmd_param, effect_param_t *reply_param, uint32_t *reply_size
) {
    // The value offset of an effect parameter is computed by rounding up
    // the parameter size to the next 32 bit alignment.
    const uint32_t offset =
        ((cmd_param->psize + sizeof(int32_t) - 1) / sizeof(int32_t)) * sizeof(int32_t);

    memcpy(reply_param, cmd_param, sizeof(effect_param_t) + cmd_param->psize);

    switch (*reinterpret_cast<uint32_t *>(cmd_param->data)) {
        case kParamGetEnabled: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(int32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) = enable_;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetConfigure: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(int32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) =
                disable_reason_ == DisableReason::NONE;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetStreaming: {
            const uint64_t frames = viper_.GetProcessedFrames();
            const int32_t is_processing =
                (frames != last_streaming_frames_ && frames > 0) ? 1 : 0;
            last_streaming_frames_ = frames;

            reply_param->status = 0;
            reply_param->vsize = sizeof(int32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) = is_processing;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetSamplingRate: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(uint32_t);
            *reinterpret_cast<uint32_t *>(reply_param->data + offset) =
                viper_.GetSamplingRate();
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetConvolutionKernelId: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(uint32_t);
            *reinterpret_cast<uint32_t *>(reply_param->data + offset) =
                viper_.GetConvolverKernelID();
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetDriverVersionCode: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(uint32_t);
            *reinterpret_cast<int32_t *>(reply_param->data + offset) = VERSION_CODE;
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetDriverVersionName: {
            reply_param->status = 0;
            reply_param->vsize = strlen(VERSION_NAME);
            memcpy(reply_param->data + offset, VERSION_NAME, reply_param->vsize);
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        case kParamGetArchitecture: {
            reply_param->status = 0;
            reply_param->vsize = sizeof(VIPER_ARCHITECTURE) - 1;
            memcpy(reply_param->data + offset, VIPER_ARCHITECTURE, reply_param->vsize);
            *reply_size =
                sizeof(effect_param_t) + reply_param->psize + offset + reply_param->vsize;
            return 0;
        }
        default: {
            return -EINVAL;
        }
    }
}

int32_t ViperContext::HandleCommand(
    uint32_t cmd_code,
    uint32_t cmd_size,
    void *cmd_data,
    uint32_t *reply_size,
    void *reply_data
) {
    const uint32_t rs = reply_size == nullptr ? 0 : *reply_size;
    switch (cmd_code) {
        case EFFECT_CMD_INIT: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_INIT called with invalid reply_size = %d, reply_data = "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_SET_CONFIG: {
            if (cmd_size < sizeof(effect_config_t) || cmd_data == nullptr
                || rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_SET_CONFIG called with invalid cmd_size = %d, cmd_data = "
                    "%p, reply_size = %d, reply_data = %p, expected cmd_size = %zu, "
                    "reply_size = %zu",
                    cmd_size,
                    cmd_data,
                    rs,
                    reply_data,
                    sizeof(effect_config_t),
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            HandleSetConfig(static_cast<effect_config_t *>(cmd_data));
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_RESET: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_RESET called with invalid reply_size = %d, reply_data = "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            viper_.ResetAllEffects();
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_ENABLE: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_ENABLE called with invalid reply_size = %d, reply_data = "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            viper_.ResetAllEffects();
            if (!buffer_.empty()) {
                memset(buffer_.data(), 0, buffer_.size() * sizeof(float));
            }
            has_processed_ = false;
            fade_in_remaining_ = 0;
            enable_ = true;
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_DISABLE: {
            if (rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_DISABLE called with invalid reply_size = %d, reply_data "
                    "= "
                    "%p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            enable_ = false;
            SET(int32_t, reply_data, 0);
            return 0;
        }
        case EFFECT_CMD_SET_PARAM: {
            if (cmd_size < sizeof(effect_param_t) || cmd_data == nullptr
                || rs != sizeof(int32_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_SET_PARAM called with invalid cmd_size = %d, reply_data "
                    "= "
                    "%p, reply_size = %d, reply_data = %p, expected cmd_size = %zu, "
                    "reply_size = %zu",
                    cmd_size,
                    cmd_data,
                    rs,
                    reply_data,
                    sizeof(effect_param_t),
                    sizeof(int32_t)
                );
                return -EINVAL;
            }
            return HandleSetParam(static_cast<effect_param_t *>(cmd_data), reply_data);
        }
        case EFFECT_CMD_GET_PARAM: {
            if (cmd_size < sizeof(effect_param_t) || cmd_data == nullptr
                || rs < sizeof(effect_param_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_GET_PARAM called with invalid cmd_size = %d, reply_data "
                    "= "
                    "%p, reply_size = %d, reply_data = %p, expected cmd_size = %zu, "
                    "reply_size = %zu",
                    cmd_size,
                    cmd_data,
                    rs,
                    reply_data,
                    sizeof(effect_param_t),
                    sizeof(effect_param_t)
                );
                return -EINVAL;
            }
            return HandleGetParam(
                static_cast<effect_param_t *>(cmd_data),
                static_cast<effect_param_t *>(reply_data),
                reply_size
            );
        }
        case EFFECT_CMD_GET_CONFIG: {
            if (rs != sizeof(effect_config_t) || reply_data == nullptr) {
                VIPER_LOGE(
                    "EFFECT_CMD_GET_CONFIG called with invalid reply_size = %d, "
                    "reply_data = %p, expected reply_size = %zu",
                    rs,
                    reply_data,
                    sizeof(effect_config_t)
                );
                return -EINVAL;
            }
            *static_cast<effect_config_t *>(reply_data) = config_;
            return 0;
        }
        default: {
            VIPER_LOGE("HandleCommand called with unknown command: %d", cmd_code);
            return -EINVAL;
        }
    }
}

template <typename T>
void PcmToFloat(float *dst, const T *src, const size_t frame_count) {
    constexpr auto max_val = static_cast<float>(std::numeric_limits<T>::max());
    for (size_t i = 0; i < frame_count * 2; i++) {
        dst[i] = static_cast<float>(src[i]) / max_val;
    }
}

template <typename T>
static const T &clamp(const T &v, const T &lo, const T &hi) {
    return std::min(std::max(v, lo), hi);
}

static void FloatToFloat(
    float *dst, const float *src, const size_t frame_count, const bool accumulate
) {
    if (accumulate) {
        for (size_t i = 0; i < frame_count * 2; i++) {
            dst[i] = clamp(dst[i] + src[i], -1.0f, 1.0f);
        }
    } else {
        memcpy(dst, src, frame_count * 2 * sizeof(float));
    }
}

template <typename T, typename U>
void FloatToPcm(
    T *dst, const float *src, const size_t frame_count, const bool accumulate
) {
    constexpr T max_val = std::numeric_limits<T>::max();
    constexpr T min_val = std::numeric_limits<T>::min();

    for (size_t i = 0; i < frame_count * 2; i++) {
        T pcm = static_cast<T>(src[i] * static_cast<float>(max_val));
        if (accumulate) {
            U temp = static_cast<U>(dst[i]) + pcm;
            dst[i] = static_cast<T>(
                clamp(temp, static_cast<U>(min_val), static_cast<U>(max_val))
            );
        } else {
            dst[i] = pcm;
        }
    }
}

static audio_buffer_t *GetBuffer(buffer_config_s *config, audio_buffer_t *buffer) {
    if (buffer != nullptr) return buffer;
    if (config->mask & EFFECT_CONFIG_BUFFER) return &config->buffer;
    // EFFECT_CONFIG_PROVIDER not implemented, it's not used by any known effect
    return nullptr;
}

int32_t ViperContext::Process(audio_buffer_t *in_buffer, audio_buffer_t *out_buffer) {
    if (disable_reason_ != DisableReason::NONE) {
        return -EINVAL;
    }

    if (!enable_) {
        return -ENODATA;
    }

    in_buffer = GetBuffer(&config_.input_cfg, in_buffer);
    out_buffer = GetBuffer(&config_.output_cfg, out_buffer);
    if (in_buffer == nullptr || out_buffer == nullptr || in_buffer->raw == nullptr
        || out_buffer->raw == nullptr || in_buffer->frame_count != out_buffer->frame_count
        || in_buffer->frame_count == 0) {
        return -EINVAL;
    }

#if defined(__aarch64__)
    uint64_t orig_fpcr;
    asm volatile("mrs %0, fpcr" : "=r"(orig_fpcr));
    asm volatile("msr fpcr, %0" ::"r"(orig_fpcr | (1 << 24)));
#elif defined(__arm__)
    uint32_t orig_fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(orig_fpscr));
    asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr | (1 << 24)));
#endif

    auto now = std::chrono::steady_clock::now();
    if (has_processed_) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - last_process_time_
        )
                           .count();
        if (elapsed > 100) {
            viper_.ResetAllEffects();
            fade_in_remaining_ = 128;
        }
    } else {
        viper_.ResetAllEffects();
        fade_in_remaining_ = 128;
        has_processed_ = true;
    }
    last_process_time_ = now;

    size_t frame_count = in_buffer->frame_count;
    if (frame_count > buffer_frame_count_) {
        buffer_.resize(frame_count * 2);
        buffer_frame_count_ = frame_count;
    }

    switch (config_.input_cfg.format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            PcmToFloat<int16_t>(buffer_.data(), in_buffer->s16, frame_count);
            break;
        case AUDIO_FORMAT_PCM_32_BIT:
            PcmToFloat<int32_t>(buffer_.data(), in_buffer->s32, frame_count);
            break;
        case AUDIO_FORMAT_PCM_FLOAT:
            FloatToFloat(buffer_.data(), in_buffer->f32, frame_count, false);
            break;
        default:
#if defined(__aarch64__)
            asm volatile("msr fpcr, %0" ::"r"(orig_fpcr));
#elif defined(__arm__)
            asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr));
#endif
            return -EINVAL;
    }

    // TODO: Remove fade-in.
    if (fade_in_remaining_ > 0) {
        uint32_t fade_samples =
            fade_in_remaining_ < frame_count ? fade_in_remaining_ : frame_count;
        for (uint32_t i = 0; i < fade_samples; i++) {
            float gain = static_cast<float>(128 - fade_in_remaining_ + i) / 128.0f;
            buffer_[i * 2] *= gain;
            buffer_[i * 2 + 1] *= gain;
        }
        fade_in_remaining_ -= fade_samples;
    }

    viper_.Process(buffer_, frame_count);

    const bool accumulate =
        config_.output_cfg.access_mode == EFFECT_BUFFER_ACCESS_ACCUMULATE;
    switch (config_.output_cfg.format) {
        case AUDIO_FORMAT_PCM_16_BIT:
            FloatToPcm<int16_t, int32_t>(
                out_buffer->s16, buffer_.data(), frame_count, accumulate
            );
            break;
        case AUDIO_FORMAT_PCM_32_BIT:
            FloatToPcm<int32_t, int64_t>(
                out_buffer->s32, buffer_.data(), frame_count, accumulate
            );
            break;
        case AUDIO_FORMAT_PCM_FLOAT:
            FloatToFloat(out_buffer->f32, buffer_.data(), frame_count, accumulate);
            break;
        default:
#if defined(__aarch64__)
            asm volatile("msr fpcr, %0" ::"r"(orig_fpcr));
#elif defined(__arm__)
            asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr));
#endif
            return -EINVAL;
    }

#if defined(__aarch64__)
    asm volatile("msr fpcr, %0" ::"r"(orig_fpcr));
#elif defined(__arm__)
    asm volatile("vmsr fpscr, %0" ::"r"(orig_fpscr));
#endif

    return 0;
}

void ViperContext::SetDisableReason(const DisableReason reason) {
    SetDisableReason(reason, "");
}

void ViperContext::SetDisableReason(const DisableReason reason, std::string message) {
    this->disable_reason_ = reason;
    this->disable_reason_message_ = std::move(message);
}
