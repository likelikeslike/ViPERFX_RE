#pragma once

#include "essential.h"
#include "viper/ViPER.h"
#include <string>
#include <vector>

class ViperContext {
public:
    enum class DisableReason : int32_t {
        UNKNOWN = -1,
        NONE = 0,
        INVALID_FRAME_COUNT,
        INVALID_SAMPLING_RATE,
        INVALID_CHANNEL_COUNT,
        INVALID_FORMAT,
    };

    ViperContext();

    int32_t HandleCommand(
        uint32_t cmd_code,
        uint32_t cmd_size,
        void *cmd_data,
        uint32_t *reply_size,
        void *reply_data
    );
    int32_t Process(audio_buffer_t *in_buffer, audio_buffer_t *out_buffer);

private:
    effect_config_t config_;
    DisableReason disable_reason_;
    std::string disable_reason_message_;

    // Processing buffer
    std::vector<float> buffer_;
    size_t buffer_frame_count_;

    // Viper
    bool enable_;
    ViPER viper_;

    static void CopyBufferConfig(buffer_config_t *dest, const buffer_config_t *src);
    void HandleSetConfig(const effect_config_t *new_config);

    int32_t HandleSetParam(effect_param_t *cmd_param, void *reply_data);
    int32_t HandleGetParam(
        effect_param_t *cmd_param, effect_param_t *reply_param, uint32_t *reply_size
    );

    void SetDisableReason(DisableReason reason);
    void SetDisableReason(DisableReason reason, std::string message);
};
