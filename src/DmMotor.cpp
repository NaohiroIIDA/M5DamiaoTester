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
    rxCount_  = 0;
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

bool DmMotor::scan(uint16_t minId, uint16_t maxId, float *pmax, uint32_t deadlineMs)
{
    if (!started_) return false;
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {}  // 古い受信を捨てる

    // 受信したフレームを確認する。診断用に、応答以外のフレームも最初の数個をログに出す
    auto check = [&](const twai_message_t &m) {
        rxCount_++;
        if (rxCount_ <= 8) {
            Serial.printf("[CAN] rx id=0x%03X %s dlc=%d :", m.identifier, m.extd ? "EXT" : "STD", m.data_length_code);
            for (int i = 0; i < m.data_length_code && i < 8; i++) Serial.printf(" %02X", m.data[i]);
            Serial.println();
        }
        return checkScanReply(m, pmax);
    };

    // 全 ID へ読み出し要求を連続送信し、途中で応答が来たら即終了
    int sendFails = 0;
    for (uint32_t id = minId; id <= maxId; id++) {
        if ((int32_t)(millis() - deadlineMs) >= 0) return false;
        uint8_t d[8] = {(uint8_t)(id & 0xFF), (uint8_t)(id >> 8), 0x33, RID_PMAX, 0, 0, 0, 0};
        if (send(0x7FF, d)) {
            sendFails = 0;
        } else if (++sendFails >= 10) {
            // 送信できない = ACK が返っていない (配線・ピン・ボーレート・終端を確認)
            Serial.println("[CAN] transmit failed repeatedly (no ACK?)");
            printDiag();
            delay(50);
            return false;
        }
        while (twai_receive(&msg, 0) == ESP_OK) {
            if (check(msg)) return true;
        }
    }
    // 最後の要求への応答待ち
    uint32_t start = millis();
    while (millis() - start < 50) {
        if (twai_receive(&msg, pdMS_TO_TICKS(2)) == ESP_OK && check(msg)) return true;
    }
    Serial.println("[CAN] no register reply, probing with disable command");

    // レジスタ読み出しに応答しない場合の予備: 各 ID に「無効化」を送り、フィードバックを待つ。
    // 0x100 以上は他 ID のモード別指令と重なるので送らない。無効化なのでモーターは動かない。
    uint16_t probeMax = maxId < 0xFF ? maxId : 0xFF;
    for (uint32_t id = minId; id <= probeMax; id++) {
        if ((int32_t)(millis() - deadlineMs) >= 0) break;
        uint8_t d[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFD};
        if (!send(id, d)) continue;
        uint32_t t0 = millis();
        while (millis() - t0 < 3) {
            if (twai_receive(&msg, pdMS_TO_TICKS(1)) != ESP_OK) continue;
            if (check(msg)) return true;
            if (!msg.extd && msg.data_length_code == 8 && (msg.data[0] & 0x0F) == (id & 0x0F)) {
                canId_    = id;
                masterId_ = msg.identifier;
                *pmax     = 0;  // 不明 (呼び出し側で既定値を使う)
                Serial.printf("[CAN] feedback reply from id=0x%03X\n", id);
                return true;
            }
        }
    }
    printDiag();
    return false;
}

String DmMotor::diagText()
{
    twai_status_info_t info = {};
    if (!started_ || twai_get_status_info(&info) != ESP_OK) return "CAN停止中";
    static const char *st[] = {"STOP", "RUN", "BUSOFF", "RECOV"};
    char buf[64];
    snprintf(buf, sizeof(buf), "%s TEC%lu REC%lu BE%lu RX%lu", st[info.state & 3],
             (unsigned long)info.tx_error_counter, (unsigned long)info.rx_error_counter,
             (unsigned long)info.bus_error_count, (unsigned long)rxCount_);
    return buf;
}

void DmMotor::printDiag()
{
    twai_status_info_t info = {};
    if (!started_ || twai_get_status_info(&info) != ESP_OK) return;
    Serial.printf("[CAN] state=%d TEC=%lu REC=%lu bus_err=%lu tx_failed=%lu arb_lost=%lu rx_missed=%lu "
                  "tx_queued=%lu rx_total=%lu\n",
                  info.state, (unsigned long)info.tx_error_counter, (unsigned long)info.rx_error_counter,
                  (unsigned long)info.bus_error_count, (unsigned long)info.tx_failed_count,
                  (unsigned long)info.arb_lost_count, (unsigned long)info.rx_missed_count,
                  (unsigned long)info.msgs_to_tx, (unsigned long)rxCount_);
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
