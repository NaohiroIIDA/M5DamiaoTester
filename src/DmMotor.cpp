#include "DmMotor.h"

#include <driver/twai.h>
#include <string.h>

static float uintToFloat(uint32_t x, float xMin, float xMax, int bits)
{
    return (float)x * (xMax - xMin) / (float)((1u << bits) - 1) + xMin;
}

bool DmMotor::beginBus(int txPin, int rxPin)
{
    if (started_) return true;

    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)txPin, (gpio_num_t)rxPin, TWAI_MODE_NORMAL);
    g.rx_queue_len          = 64;
    g.tx_queue_len          = 16;
    twai_timing_config_t t  = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t f  = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK) {
        Serial.println("[CAN] driver install failed");
        return false;
    }
    if (twai_start() != ESP_OK) {
        Serial.println("[CAN] start failed");
        twai_driver_uninstall();
        return false;
    }
    started_  = true;
    status_   = 0;
    lastFbMs_ = 0;
    Serial.println("[CAN] started (1Mbps)");
    return true;
}

void DmMotor::endBus()
{
    if (!started_) return;
    twai_stop();
    twai_driver_uninstall();
    started_ = false;
    Serial.println("[CAN] stopped");
}

void DmMotor::recoverIfBusOff()
{
    twai_status_info_t info;
    if (twai_get_status_info(&info) != ESP_OK) return;
    if (info.state == TWAI_STATE_BUS_OFF) {
        Serial.println("[CAN] bus-off, recovering");
        twai_initiate_recovery();
    } else if (info.state == TWAI_STATE_STOPPED) {
        twai_start();
    }
}

bool DmMotor::send(uint32_t id, const uint8_t data[8])
{
    if (!started_) return false;
    twai_message_t msg = {};
    msg.identifier     = id;
    msg.data_length_code = 8;
    memcpy(msg.data, data, 8);
    if (twai_transmit(&msg, pdMS_TO_TICKS(5)) != ESP_OK) {
        recoverIfBusOff();
        return false;
    }
    return true;
}

void DmMotor::sendSpecial(uint8_t cmd)
{
    uint8_t d[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, cmd};
    send(canId_, d);
}

void DmMotor::sendPosVel(float pos, float vel)
{
    uint8_t d[8];
    memcpy(&d[0], &pos, 4);  // little endian
    memcpy(&d[4], &vel, 4);
    send(0x100 + canId_, d);
}

void DmMotor::handleFeedback(const uint8_t d[8])
{
    if ((d[0] & 0x0F) != (canId_ & 0x0F)) return;

    uint32_t p = ((uint32_t)d[1] << 8) | d[2];
    uint32_t v = ((uint32_t)d[3] << 4) | (d[4] >> 4);
    uint32_t t = ((uint32_t)(d[4] & 0x0F) << 8) | d[5];

    status_   = d[0] >> 4;
    pos_      = uintToFloat(p, -pmax_, pmax_, 16);
    vel_      = uintToFloat(v, -vmax_, vmax_, 12);
    tor_      = uintToFloat(t, -tmax_, tmax_, 12);
    tMos_     = d[6];
    tRotor_   = d[7];
    lastFbMs_ = millis();
}

void DmMotor::poll()
{
    if (!started_) return;
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        if (msg.extd || msg.rtr || msg.data_length_code != 8) continue;
        handleFeedback(msg.data);
    }
}

bool DmMotor::waitFeedback(uint32_t timeoutMs)
{
    uint32_t prev  = lastFbMs_;
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        poll();
        if (lastFbMs_ != prev) return true;
        delay(1);
    }
    return false;
}

bool DmMotor::waitRegisterReply(uint8_t op, uint8_t rid, uint8_t out[4], uint32_t timeoutMs)
{
    uint32_t start = millis();
    while (millis() - start < timeoutMs) {
        twai_message_t msg;
        if (twai_receive(&msg, pdMS_TO_TICKS(2)) != ESP_OK) continue;
        if (msg.extd || msg.rtr || msg.data_length_code != 8) continue;
        const uint8_t *d = msg.data;
        if (d[0] == (canId_ & 0xFF) && d[1] == (canId_ >> 8) && d[2] == op && d[3] == rid) {
            if (out) memcpy(out, &d[4], 4);
            return true;
        }
        handleFeedback(d);
    }
    return false;
}

bool DmMotor::checkScanReply(const twai_message_t &msg, float *pmax)
{
    if (msg.extd || msg.rtr || msg.data_length_code != 8) return false;
    const uint8_t *d = msg.data;
    if (d[2] != 0x33 || d[3] != RID_PMAX) return false;
    uint16_t id = d[0] | ((uint16_t)d[1] << 8);
    if (id == 0 || id > 0x7FF) return false;

    canId_    = id;
    masterId_ = msg.identifier;
    memcpy(pmax, &d[4], 4);
    return true;
}

bool DmMotor::scan(uint16_t minId, uint16_t maxId, float *pmax, uint32_t replyWaitMs)
{
    if (!started_) return false;
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {}  // 古い受信を捨てる

    // 全 ID へ読み出し要求を連続送信し、途中で応答が来たら即終了
    for (uint32_t id = minId; id <= maxId; id++) {
        uint8_t d[8] = {(uint8_t)(id & 0xFF), (uint8_t)(id >> 8), 0x33, RID_PMAX, 0, 0, 0, 0};
        send(0x7FF, d);
        while (twai_receive(&msg, 0) == ESP_OK) {
            if (checkScanReply(msg, pmax)) return true;
        }
    }
    // 最後の要求への応答待ち
    uint32_t start = millis();
    while (millis() - start < replyWaitMs) {
        if (twai_receive(&msg, pdMS_TO_TICKS(2)) == ESP_OK && checkScanReply(msg, pmax)) return true;
    }
    return false;
}

bool DmMotor::readRegister(uint8_t rid, uint8_t out[4], uint32_t timeoutMs)
{
    uint8_t d[8] = {(uint8_t)(canId_ & 0xFF), (uint8_t)(canId_ >> 8), 0x33, rid, 0, 0, 0, 0};
    if (!send(0x7FF, d)) return false;
    return waitRegisterReply(0x33, rid, out, timeoutMs);
}

bool DmMotor::writeRegister(uint8_t rid, const uint8_t val[4], uint32_t timeoutMs)
{
    uint8_t d[8] = {(uint8_t)(canId_ & 0xFF), (uint8_t)(canId_ >> 8), 0x55, rid, val[0], val[1], val[2], val[3]};
    if (!send(0x7FF, d)) return false;
    return waitRegisterReply(0x55, rid, nullptr, timeoutMs);
}

bool DmMotor::readFloat(uint8_t rid, float *out, uint32_t timeoutMs)
{
    uint8_t b[4];
    if (!readRegister(rid, b, timeoutMs)) return false;
    memcpy(out, b, 4);
    return true;
}

bool DmMotor::writeU32(uint8_t rid, uint32_t v, uint32_t timeoutMs)
{
    uint8_t b[4];
    memcpy(b, &v, 4);
    return writeRegister(rid, b, timeoutMs);
}

const char *DmMotor::statusText(uint8_t s)
{
    switch (s) {
        case 0x0: return "無効";
        case 0x1: return "有効";
        case 0x8: return "過電圧";
        case 0x9: return "低電圧";
        case 0xA: return "過電流";
        case 0xB: return "MOS過熱";
        case 0xC: return "コイル過熱";
        case 0xD: return "通信断";
        case 0xE: return "過負荷";
        default:  return "不明";
    }
}
