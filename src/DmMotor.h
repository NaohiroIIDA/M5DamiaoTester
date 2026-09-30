#pragma once
#include <Arduino.h>
#include <driver/twai.h>

// DAMIAO モーター (DM-J4340 等) の CAN 通信 (ESP32 TWAI 使用)
class DmMotor {
public:
    // レジスタ ID
    static constexpr uint8_t RID_CTRL_MODE = 0x0A;
    static constexpr uint8_t RID_PMAX      = 0x15;
    static constexpr uint8_t RID_VMAX      = 0x16;
    static constexpr uint8_t RID_TMAX      = 0x17;

    // CTRL_MODE の値
    static constexpr uint32_t MODE_MIT     = 1;
    static constexpr uint32_t MODE_POS_VEL = 2;

    DmMotor() = default;

    bool beginBus(int txPin, int rxPin);
    void endBus();
    bool busStarted() const { return started_; }

    // minId〜maxId の全 ID に PMAX 読み出しを送り、最初に応答したモーターの ID を採用する。
    // 見つかれば canId() / masterId() が更新され、pmax に PMAX が入る。
    bool scan(uint16_t minId, uint16_t maxId, float *pmax, uint32_t replyWaitMs = 50);
    uint16_t canId() const { return canId_; }
    uint16_t masterId() const { return masterId_; }  // 応答フレームの CAN ID

    bool readRegister(uint8_t rid, uint8_t out[4], uint32_t timeoutMs = 50);
    bool writeRegister(uint8_t rid, const uint8_t val[4], uint32_t timeoutMs = 50);
    bool readFloat(uint8_t rid, float *out, uint32_t timeoutMs = 50);
    bool writeU32(uint8_t rid, uint32_t v, uint32_t timeoutMs = 50);

    void enable()     { sendSpecial(0xFC); }
    void disable()    { sendSpecial(0xFD); }
    void clearError() { sendSpecial(0xFB); }

    // POS_VEL モード指令 (位置 [rad], 速度上限 [rad/s])
    void sendPosVel(float pos, float vel);

    // 受信フレームを処理してフィードバックを更新
    void poll();

    // フィードバック待ち (poll しながら lastFeedbackMs が更新されるまで)
    bool waitFeedback(uint32_t timeoutMs);

    void setLimits(float pmax, float vmax, float tmax) { pmax_ = pmax; vmax_ = vmax; tmax_ = tmax; }
    float pmax() const { return pmax_; }

    float position() const { return pos_; }
    float velocity() const { return vel_; }
    float torque() const { return tor_; }
    uint8_t status() const { return status_; }  // 0:無効 1:有効 8〜E:エラー
    uint8_t tempMos() const { return tMos_; }
    uint8_t tempRotor() const { return tRotor_; }
    uint32_t lastFeedbackMs() const { return lastFbMs_; }
    bool hasError() const { return status_ >= 0x08; }

    static const char *statusText(uint8_t status);

private:
    bool send(uint32_t id, const uint8_t data[8]);
    void sendSpecial(uint8_t cmd);
    bool waitRegisterReply(uint8_t op, uint8_t rid, uint8_t out[4], uint32_t timeoutMs);
    void handleFeedback(const uint8_t d[8]);
    void recoverIfBusOff();

    bool checkScanReply(const twai_message_t &msg, float *pmax);

    uint16_t canId_    = 0;
    uint16_t masterId_ = 0;
    bool started_ = false;

    float pmax_ = 12.5f, vmax_ = 8.0f, tmax_ = 28.0f;
    float pos_ = 0, vel_ = 0, tor_ = 0;
    uint8_t status_ = 0, tMos_ = 0, tRotor_ = 0;
    uint32_t lastFbMs_ = 0;
};
