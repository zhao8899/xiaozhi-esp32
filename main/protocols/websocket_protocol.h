#ifndef _WEBSOCKET_PROTOCOL_H_
#define _WEBSOCKET_PROTOCOL_H_


#include "protocol.h"

#include <web_socket.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>

#define WEBSOCKET_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
// WebSocket重连间隔（毫秒）
#define WEBSOCKET_RECONNECT_INTERVAL_MS 30000
// WebSocket连接最大重试次数
#define WEBSOCKET_MAX_CONNECT_RETRIES 3

class WebsocketProtocol : public Protocol {
public:
    WebsocketProtocol();
    ~WebsocketProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel() override;
    bool IsAudioChannelOpened() const override;

private:
    EventGroupHandle_t event_group_handle_;
    std::unique_ptr<WebSocket> websocket_;
    int version_ = 1;
    int connect_retry_count_ = 0;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    bool reconnect_scheduled_ = false;

    void ParseServerHello(const cJSON* root);
    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
    bool TryConnect();
    void ScheduleReconnect();
};

#endif
