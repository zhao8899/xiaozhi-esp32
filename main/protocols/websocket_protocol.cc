#include "websocket_protocol.h"
#include "board.h"
#include "system_info.h"
#include "application.h"
#include "settings.h"

#include <cstring>
#include <cJSON.h>
#include <esp_log.h>
#include <arpa/inet.h>
#include "assets/lang_config.h"

#define TAG "WS"

WebsocketProtocol::WebsocketProtocol() {
    event_group_handle_ = xEventGroupCreate();

    // 初始化重连定时器
    esp_timer_create_args_t reconnect_timer_args = {
        .callback = [](void* arg) {
            WebsocketProtocol* protocol = (WebsocketProtocol*)arg;
            protocol->reconnect_scheduled_ = false;
            ESP_LOGI(TAG, "WebSocket重连定时器触发");
            // 重连逻辑会在下次 OpenAudioChannel 时自动处理
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ws_reconnect",
        .skip_unhandled_events = true
    };
    esp_timer_create(&reconnect_timer_args, &reconnect_timer_);
}

WebsocketProtocol::~WebsocketProtocol() {
    if (reconnect_timer_ != nullptr) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
    }
    vEventGroupDelete(event_group_handle_);
}

bool WebsocketProtocol::Start() {
    // Only connect to server when audio channel is needed
    return true;
}

bool WebsocketProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (version_ == 2) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol2) + packet->payload.size());
        auto bp2 = (BinaryProtocol2*)serialized.data();
        bp2->version = htons(version_);
        bp2->type = 0;
        bp2->reserved = 0;
        bp2->timestamp = htonl(packet->timestamp);
        bp2->payload_size = htonl(packet->payload.size());
        memcpy(bp2->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else if (version_ == 3) {
        std::string serialized;
        serialized.resize(sizeof(BinaryProtocol3) + packet->payload.size());
        auto bp3 = (BinaryProtocol3*)serialized.data();
        bp3->type = 0;
        bp3->reserved = 0;
        bp3->payload_size = htons(packet->payload.size());
        memcpy(bp3->payload, packet->payload.data(), packet->payload.size());

        return websocket_->Send(serialized.data(), serialized.size(), true);
    } else {
        return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
    }
}

bool WebsocketProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }

    if (!websocket_->Send(text)) {
        // 不记录完整文本内容，可能包含敏感信息
        ESP_LOGE(TAG, "发送文本失败，长度: %zu", text.size());
        SetError(Lang::Strings::SERVER_ERROR);
        return false;
    }

    return true;
}

bool WebsocketProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void WebsocketProtocol::CloseAudioChannel() {
    websocket_.reset();
}

bool WebsocketProtocol::TryConnect() {
    Settings settings("websocket", false);
    std::string url = settings.GetString("url");
    std::string token = settings.GetString("token");
    int version = settings.GetInt("version");
    if (version != 0) {
        version_ = version;
    }

    // 安全检查：强制使用 WSS 加密连接
    if (url.substr(0, 5) == "ws://") {
        ESP_LOGW(TAG, "检测到不安全的 ws:// 连接，自动升级为 wss://");
        url = "wss://" + url.substr(5);
    } else if (url.substr(0, 6) != "wss://") {
        ESP_LOGE(TAG, "无效的 WebSocket URL，必须使用 wss:// 协议");
        SetError(Lang::Strings::SERVER_NOT_FOUND);
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "创建WebSocket失败");
        return false;
    }

    if (!token.empty()) {
        // If token not has a space, add "Bearer " prefix
        if (token.find(" ") == std::string::npos) {
            token = "Bearer " + token;
        }
        websocket_->SetHeader("Authorization", token.c_str());
    }
    websocket_->SetHeader("Protocol-Version", std::to_string(version_).c_str());
    websocket_->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    websocket_->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());

    // 不记录完整 URL，可能包含敏感参数
    ESP_LOGI(TAG, "连接 WebSocket 服务器 (版本: %d)", version_);
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "WebSocket连接失败, code=%d", websocket_->GetLastError());
        websocket_.reset();
        return false;
    }
    return true;
}

void WebsocketProtocol::ScheduleReconnect() {
    if (!reconnect_scheduled_ && connect_retry_count_ < WEBSOCKET_MAX_CONNECT_RETRIES) {
        reconnect_scheduled_ = true;
        esp_timer_start_once(reconnect_timer_, WEBSOCKET_RECONNECT_INTERVAL_MS * 1000);
        ESP_LOGI(TAG, "WebSocket将在%d秒后重连 (重试 %d/%d)",
                 WEBSOCKET_RECONNECT_INTERVAL_MS / 1000,
                 connect_retry_count_ + 1, WEBSOCKET_MAX_CONNECT_RETRIES);
    }
}

bool WebsocketProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    connect_retry_count_ = 0;

    // 尝试连接，最多重试 WEBSOCKET_MAX_CONNECT_RETRIES 次
    while (connect_retry_count_ < WEBSOCKET_MAX_CONNECT_RETRIES) {
        if (TryConnect()) {
            break;
        }
        connect_retry_count_++;
        if (connect_retry_count_ < WEBSOCKET_MAX_CONNECT_RETRIES) {
            ESP_LOGW(TAG, "WebSocket连接失败，立即重试 (%d/%d)",
                     connect_retry_count_, WEBSOCKET_MAX_CONNECT_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(1000));  // 短暂延迟后重试
        }
    }

    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "WebSocket连接失败，已重试%d次", WEBSOCKET_MAX_CONNECT_RETRIES);
        SetError(Lang::Strings::SERVER_NOT_CONNECTED);
        return false;
    }

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            if (on_incoming_audio_ != nullptr) {
                if (version_ == 2) {
                    // 检查数据长度是否足够包含协议头
                    if (len < sizeof(BinaryProtocol2)) {
                        ESP_LOGE(TAG, "数据包太短，无法解析v2协议头: len=%zu", len);
                        return;
                    }
                    BinaryProtocol2* bp2 = (BinaryProtocol2*)data;
                    bp2->version = ntohs(bp2->version);
                    bp2->type = ntohs(bp2->type);
                    bp2->timestamp = ntohl(bp2->timestamp);
                    bp2->payload_size = ntohl(bp2->payload_size);

                    // 验证payload_size不超过实际数据长度
                    size_t max_payload = len - sizeof(BinaryProtocol2);
                    if (bp2->payload_size > max_payload) {
                        ESP_LOGE(TAG, "payload_size(%u)超过可用数据(%zu)", bp2->payload_size, max_payload);
                        return;
                    }

                    auto payload = (uint8_t*)bp2->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = bp2->timestamp,
                        .payload = std::vector<uint8_t>(payload, payload + bp2->payload_size)
                    }));
                } else if (version_ == 3) {
                    // 检查数据长度是否足够包含协议头
                    if (len < sizeof(BinaryProtocol3)) {
                        ESP_LOGE(TAG, "数据包太短，无法解析v3协议头: len=%zu", len);
                        return;
                    }
                    BinaryProtocol3* bp3 = (BinaryProtocol3*)data;
                    bp3->type = bp3->type;
                    bp3->payload_size = ntohs(bp3->payload_size);

                    // 验证payload_size不超过实际数据长度
                    size_t max_payload = len - sizeof(BinaryProtocol3);
                    if (bp3->payload_size > max_payload) {
                        ESP_LOGE(TAG, "payload_size(%u)超过可用数据(%zu)", bp3->payload_size, max_payload);
                        return;
                    }

                    auto payload = (uint8_t*)bp3->payload;
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>(payload, payload + bp3->payload_size)
                    }));
                } else {
                    on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                        .sample_rate = server_sample_rate_,
                        .frame_duration = server_frame_duration_,
                        .timestamp = 0,
                        .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                    }));
                }
            }
        } else {
            // Parse JSON data
            auto root = cJSON_Parse(data);
            if (root == nullptr) {
                ESP_LOGE(TAG, "JSON解析失败，数据: %.100s", data);
                return;
            }
            auto type = cJSON_GetObjectItem(root, "type");
            if (cJSON_IsString(type)) {
                if (strcmp(type->valuestring, "hello") == 0) {
                    ParseServerHello(root);
                } else {
                    if (on_incoming_json_ != nullptr) {
                        on_incoming_json_(root);
                    }
                }
            } else {
                ESP_LOGE(TAG, "缺少消息类型字段，数据: %.100s", data);
            }
            cJSON_Delete(root);
        }
        last_incoming_time_ = std::chrono::steady_clock::now();
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    // 连接已在 TryConnect() 中完成，直接发送 hello 消息
    // Send hello message to describe the client
    auto message = GetHelloMessage();
    if (!SendText(message)) {
        return false;
    }

    // Wait for server hello
    EventBits_t bits = xEventGroupWaitBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT, pdTRUE, pdFALSE, pdMS_TO_TICKS(10000));
    if (!(bits & WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT)) {
        ESP_LOGE(TAG, "Failed to receive server hello");
        SetError(Lang::Strings::SERVER_TIMEOUT);
        return false;
    }

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}

std::string WebsocketProtocol::GetHelloMessage() {
    // keys: message type, version, audio_params (format, sample_rate, channels)
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "hello");
    cJSON_AddNumberToObject(root, "version", version_);
    cJSON* features = cJSON_CreateObject();
#if CONFIG_USE_SERVER_AEC
    cJSON_AddBoolToObject(features, "aec", true);
#endif
    cJSON_AddBoolToObject(features, "mcp", true);
    cJSON_AddItemToObject(root, "features", features);
    cJSON_AddStringToObject(root, "transport", "websocket");
    cJSON* audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", 16000);
    cJSON_AddNumberToObject(audio_params, "channels", 1);
    cJSON_AddNumberToObject(audio_params, "frame_duration", OPUS_FRAME_DURATION_MS);
    cJSON_AddItemToObject(root, "audio_params", audio_params);
    auto json_str = cJSON_PrintUnformatted(root);
    std::string message(json_str);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return message;
}

void WebsocketProtocol::ParseServerHello(const cJSON* root) {
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (transport == nullptr || !cJSON_IsString(transport)) {
        ESP_LOGE(TAG, "transport字段缺失或无效");
        return;
    }
    if (strcmp(transport->valuestring, "websocket") != 0) {
        ESP_LOGE(TAG, "不支持的transport类型: %s", transport->valuestring);
        return;
    }

    auto session_id = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(session_id)) {
        session_id_ = session_id->valuestring;
        ESP_LOGI(TAG, "Session ID: %s", session_id_.c_str());
    }

    // 解析音频参数并进行范围验证
    auto audio_params = cJSON_GetObjectItem(root, "audio_params");
    if (cJSON_IsObject(audio_params)) {
        auto sample_rate = cJSON_GetObjectItem(audio_params, "sample_rate");
        if (cJSON_IsNumber(sample_rate)) {
            int rate = sample_rate->valueint;
            // 验证采样率范围: 8000-48000 Hz
            if (rate >= 8000 && rate <= 48000) {
                server_sample_rate_ = rate;
            } else {
                ESP_LOGW(TAG, "采样率超出有效范围(%d)，使用默认值", rate);
            }
        }
        auto frame_duration = cJSON_GetObjectItem(audio_params, "frame_duration");
        if (cJSON_IsNumber(frame_duration)) {
            int duration = frame_duration->valueint;
            // 验证帧时长范围: 10-120 ms (Opus 支持的范围)
            if (duration >= 10 && duration <= 120) {
                server_frame_duration_ = duration;
            } else {
                ESP_LOGW(TAG, "帧时长超出有效范围(%d)，使用默认值", duration);
            }
        }
    }

    xEventGroupSetBits(event_group_handle_, WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT);
}
