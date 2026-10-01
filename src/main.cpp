// DAMIAO DM-J4340-2EC テストツール
//   M5Stack CoreS3 Lite + Module13.2 PwrCAN + Chain Encoder

#include <M5Unified.h>
#include <M5Chain.h>

#include "DmMotor.h"
#include "config.h"

// ------------------------------------------------------------
//  状態
// ------------------------------------------------------------
enum class CtrlState { Off, Connecting, Running };

static DmMotor motor;
static Chain chain;
static M5Canvas canvas(&M5.Display);

static CtrlState state = CtrlState::Off;
static String errorMsg;     // 空ならエラーなし
static String errorDetail;  // CAN バスの診断情報 (接続失敗時)

static float targetPos = 0;
static float presets[3] = {0, 0, 0};
static bool presetValid = false;

static bool memoArmed = false;  // エンコーダボタンで位置を記憶済み
static float memoPos  = 0;

// エンコーダ
static uint8_t encId       = 0;  // 0 = 未検出
static int16_t encLast     = 0;
static uint8_t encBtnLast  = 0;
static uint32_t encRetryMs = 0;

// ------------------------------------------------------------
//  画面レイアウト (320x240)
// ------------------------------------------------------------
struct Rect {
    int x, y, w, h;
    bool hit(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
static const Rect BTN_CTRL = {8, 34, 110, 76};
static const Rect BTN_PRESET[3] = {{8, 176, 98, 58}, {111, 176, 98, 58}, {214, 176, 98, 58}};

static float radToDeg(float r) { return r * 180.0f / PI; }

static void drawButton(const Rect &r, uint16_t bg, uint16_t fg, const char *l1, const char *l2 = nullptr)
{
    canvas.fillRoundRect(r.x, r.y, r.w, r.h, 8, bg);
    canvas.drawRoundRect(r.x, r.y, r.w, r.h, 8, TFT_WHITE);
    canvas.setTextColor(fg);
    canvas.setTextDatum(middle_center);
    int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    if (l2) {
        canvas.drawString(l1, cx, cy - 11);
        canvas.drawString(l2, cx, cy + 12);
    } else {
        canvas.drawString(l1, cx, cy);
    }
}

static void drawScreen()
{
    const bool running = state == CtrlState::Running;
    canvas.fillScreen(TFT_BLACK);
    canvas.setFont(&fonts::lgfxJapanGothic_16);

    // タイトルバー
    canvas.fillRect(0, 0, 320, 28, 0x18E3);
    canvas.setTextDatum(middle_left);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("DM-J4340 Tester", 8, 14);
    if (running) {
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "ID:0x%02X", motor.canId());
        canvas.setTextColor(TFT_YELLOW);
        canvas.drawString(idbuf, 150, 14);
    }
    canvas.setTextDatum(middle_right);
    canvas.setTextColor(encId ? TFT_GREEN : TFT_DARKGREY);
    canvas.drawString(encId ? "ENC OK" : "ENC --", 312, 14);

    // 制御 ON/OFF ボタン
    switch (state) {
        case CtrlState::Off:        drawButton(BTN_CTRL, 0x4208, TFT_WHITE, "制御", "OFF"); break;
        case CtrlState::Connecting: drawButton(BTN_CTRL, TFT_ORANGE, TFT_BLACK, "接続中", "..."); break;
        case CtrlState::Running:    drawButton(BTN_CTRL, TFT_DARKGREEN, TFT_WHITE, "制御", "ON"); break;
    }

    // 現在位置
    canvas.setTextDatum(top_left);
    canvas.setTextColor(TFT_LIGHTGREY);
    canvas.drawString("現在位置", 128, 36);
    char buf[48];
    if (running) {
        canvas.setFont(&fonts::Font7);  // 7セグ風
        canvas.setTextDatum(top_right);
        canvas.setTextColor(TFT_CYAN);
        snprintf(buf, sizeof(buf), "%.1f", radToDeg(motor.position()));
        canvas.setTextSize(0.8f);
        canvas.drawString(buf, 290, 56);
        canvas.setTextSize(1);
        canvas.setFont(&fonts::lgfxJapanGothic_16);
        canvas.setTextDatum(top_left);
        canvas.drawString("°", 294, 56);
        canvas.setTextColor(TFT_LIGHTGREY);
        snprintf(buf, sizeof(buf), "目標 %.1f°  %.2frad", radToDeg(targetPos), motor.position());
        canvas.drawString(buf, 128, 100);
    } else {
        canvas.setTextColor(TFT_DARKGREY);
        canvas.setFont(&fonts::Font7);
        canvas.setTextSize(0.8f);
        canvas.setTextDatum(top_right);
        canvas.drawString("---", 290, 56);
        canvas.setTextSize(1);
        canvas.setFont(&fonts::lgfxJapanGothic_16);
    }

    // エラー表示
    canvas.setTextDatum(middle_left);
    String err = errorMsg;
    if (err.isEmpty() && running && motor.hasError()) err = String("モーター: ") + DmMotor::statusText(motor.status());
    if (err.isEmpty()) {
        canvas.fillRoundRect(8, 120, 304, 24, 4, 0x0200);
        canvas.setTextColor(TFT_GREEN);
        canvas.drawString("エラー: なし", 16, 132);
    } else {
        canvas.fillRoundRect(8, 120, 304, 24, 4, TFT_RED);
        canvas.setTextColor(TFT_WHITE);
        canvas.drawString(("エラー: " + err).c_str(), 16, 132);
    }

    // 記憶位置 (登録待ち)
    if (memoArmed) {
        canvas.setTextColor(TFT_YELLOW);
        snprintf(buf, sizeof(buf), "記憶 %.1f° → 登録先 1/2/3 を押す", radToDeg(memoPos));
        canvas.drawString(buf, 12, 160);
    } else if (!running && !errorDetail.isEmpty()) {
        canvas.setTextColor(TFT_ORANGE);
        canvas.drawString(errorDetail.c_str(), 12, 160);
    } else if (running && motor.status() == 1) {
        canvas.setTextColor(TFT_DARKGREY);
        snprintf(buf, sizeof(buf), "T:%.2fNm  MOS:%d℃  Rotor:%d℃", motor.torque(), motor.tempMos(), motor.tempRotor());
        canvas.drawString(buf, 12, 160);
    }

    // プリセットボタン
    for (int i = 0; i < 3; i++) {
        char l1[8], l2[16];
        snprintf(l1, sizeof(l1), "%d", i + 1);
        if (presetValid) snprintf(l2, sizeof(l2), "%.1f°", radToDeg(presets[i]));
        else snprintf(l2, sizeof(l2), "---");
        uint16_t bg = !running ? 0x2104 : (memoArmed ? 0x8400 : 0x0319);
        drawButton(BTN_PRESET[i], bg, running ? TFT_WHITE : TFT_DARKGREY, l1, l2);
    }

    canvas.pushSprite(0, 0);
}

// ------------------------------------------------------------
//  Chain Encoder
// ------------------------------------------------------------
static void findEncoder()
{
    encId = 0;
    if (!chain.isDeviceConnected(3, 20)) return;
    uint16_t num = 0;
    if (chain.getDeviceNum(&num) != CHAIN_OK || num == 0) return;

    device_list_t list;
    list.count   = num;
    list.devices = (device_info_t *)malloc(sizeof(device_info_t) * num);
    if (list.devices && chain.getDeviceList(&list)) {
        for (uint16_t i = 0; i < list.count; i++) {
            if (list.devices[i].device_type == CHAIN_ENCODER_TYPE_CODE) {
                encId = list.devices[i].id;
                break;
            }
        }
    }
    free(list.devices);

    if (encId) {
        chain.getEncoderValue(encId, &encLast);
        chain.getEncoderButtonStatus(encId, &encBtnLast);
        Serial.printf("[ENC] found id=%d\n", encId);
    }
}

// エンコーダの増減量を返す。ボタンの押下エッジを pressed に返す。
static int16_t readEncoder(bool *pressed)
{
    *pressed = false;
    if (!encId) return 0;

    int16_t v = 0;
    if (chain.getEncoderValue(encId, &v) != CHAIN_OK) {
        Serial.println("[ENC] read failed");
        encId = 0;  // 再検出させる
        return 0;
    }
    int16_t delta = (int16_t)(v - encLast);  // int16 のラップアラウンドを考慮
    encLast       = v;

    uint8_t btn = 0;
    if (chain.getEncoderButtonStatus(encId, &btn) == CHAIN_OK) {
        *pressed   = (btn && !encBtnLast);
        encBtnLast = btn;
    }
    return delta;
}

// ------------------------------------------------------------
//  モーター制御
// ------------------------------------------------------------
static void stopControl()
{
    if (motor.busStarted()) {
        for (int i = 0; i < 3; i++) {  // 確実にフリーにする
            motor.disable();
            delay(5);
        }
        motor.poll();
        motor.endBus();
    }
    memoArmed = false;
    state     = CtrlState::Off;
}

static void failConnect(const char *msg)
{
    Serial.printf("[MOTOR] %s\n", msg);
    errorMsg = msg;
    stopControl();
}

static void startControl()
{
    errorMsg    = "";
    errorDetail = "";
    memoArmed   = false;
    state       = CtrlState::Connecting;
    drawScreen();

    if (!motor.beginBus(CAN_TX_PIN, CAN_RX_PIN)) {
        failConnect("CAN初期化失敗");
        return;
    }

    // --- モーター探索: ID を走査して PMAX レジスタに応答したものを採用 ---
    float pmax = 0;
    uint32_t deadline = millis() + CONNECT_TIMEOUT_MS;
    bool found = false;
    while ((int32_t)(millis() - deadline) < 0) {
        if (motor.scan(DM_SCAN_ID_MIN, DM_SCAN_ID_MAX, &pmax, deadline)) {
            found = true;
            break;
        }
        M5.update();
        if (M5.Touch.getDetail().wasPressed() && BTN_CTRL.hit(M5.Touch.getDetail().x, M5.Touch.getDetail().y)) {
            stopControl();  // 接続中のキャンセル
            return;
        }
    }
    if (!found) {
        errorDetail = motor.diagText();
        motor.printDiag();
        failConnect("モーターが見つかりません");
        return;
    }

    float vmax = DM_DEFAULT_VMAX, tmax = DM_DEFAULT_TMAX;
    if (!(pmax > 0.1f && pmax < 1000.0f)) pmax = DM_DEFAULT_PMAX;
    if (!motor.readFloat(DmMotor::RID_VMAX, &vmax) || !(vmax > 0)) vmax = DM_DEFAULT_VMAX;
    if (!motor.readFloat(DmMotor::RID_TMAX, &tmax) || !(tmax > 0)) tmax = DM_DEFAULT_TMAX;
    motor.setLimits(pmax, vmax, tmax);
    Serial.printf("[MOTOR] found. CAN_ID=0x%03X MST_ID=0x%03X PMAX=%.2f VMAX=%.2f TMAX=%.2f\n", motor.canId(),
                  motor.masterId(), pmax, vmax, tmax);

    // --- 位置速度モードへ切り替え (RAM のみ。フラッシュ保存はしない) ---
    if (!motor.writeU32(DmMotor::RID_CTRL_MODE, DmMotor::MODE_POS_VEL, 100)) {
        failConnect("制御モード設定失敗");
        return;
    }

    // --- 無効状態のまま現在位置を取得 ---
    motor.clearError();
    delay(5);
    motor.disable();
    if (!motor.waitFeedback(200)) {
        failConnect("フィードバックなし");
        return;
    }
    float pos = motor.position();

    // --- 有効化して現在位置で静止 ---
    targetPos = constrain(pos, TARGET_MIN, TARGET_MAX);
    motor.enable();
    delay(2);
    motor.sendPosVel(targetPos, MOVE_VEL_LIMIT);
    if (!motor.waitFeedback(200)) {
        failConnect("有効化の応答なし");
        return;
    }

    // プリセットは電源投入(接続)時の位置で初期化
    for (auto &p : presets) p = targetPos;
    presetValid = true;

    // エンコーダの基準値を取り直す
    if (encId) {
        chain.getEncoderValue(encId, &encLast);
        chain.getEncoderButtonStatus(encId, &encBtnLast);
    }

    Serial.printf("[MOTOR] running. pos=%.3f rad\n", pos);
    state = CtrlState::Running;
}

// ------------------------------------------------------------
//  タッチ処理
// ------------------------------------------------------------
static void handleTouch()
{
    auto t = M5.Touch.getDetail();
    if (!t.wasPressed()) return;

    if (BTN_CTRL.hit(t.x, t.y)) {
        if (state == CtrlState::Off) startControl();
        else stopControl();
        return;
    }

    if (state != CtrlState::Running) return;
    for (int i = 0; i < 3; i++) {
        if (!BTN_PRESET[i].hit(t.x, t.y)) continue;
        if (memoArmed) {
            presets[i] = memoPos;
            memoArmed  = false;
            Serial.printf("[PRESET] %d <- %.3f rad\n", i + 1, memoPos);
        } else {
            targetPos = presets[i];
            Serial.printf("[PRESET] move to %d (%.3f rad)\n", i + 1, targetPos);
        }
        return;
    }
}

// ------------------------------------------------------------
void setup()
{
    auto cfg         = M5.config();
    cfg.output_power = M5_BUS_OUTPUT_5V;
    M5.begin(cfg);
    Serial.begin(115200);

    M5.Display.setRotation(1);
    canvas.setPsram(true);
    canvas.setColorDepth(16);
    canvas.createSprite(M5.Display.width(), M5.Display.height());

    chain.begin(&Serial2, 115200, CHAIN_RX_PIN, CHAIN_TX_PIN);
    findEncoder();

    drawScreen();
}

void loop()
{
    M5.update();
    handleTouch();

    static uint32_t lastCtrl = 0, lastEnc = 0, lastDraw = 0;
    uint32_t now = millis();

    // エンコーダ (未検出なら定期的に再探索)
    if (!encId && now - encRetryMs > 2000) {
        encRetryMs = now;
        findEncoder();
    }
    if (now - lastEnc >= ENCODER_PERIOD_MS) {
        lastEnc = now;
        bool pressed = false;
        int16_t delta = readEncoder(&pressed);
        if (state == CtrlState::Running) {
            if (delta) targetPos = constrain(targetPos + delta * ENC_RAD_PER_COUNT, TARGET_MIN, TARGET_MAX);
            if (pressed) {
                memoPos   = motor.position();
                memoArmed = true;
                Serial.printf("[MEMO] %.3f rad\n", memoPos);
            }
        }
    }

    // モーター指令
    if (state == CtrlState::Running && now - lastCtrl >= CONTROL_PERIOD_MS) {
        lastCtrl = now;
        motor.poll();
        motor.sendPosVel(targetPos, MOVE_VEL_LIMIT);

        if (millis() - motor.lastFeedbackMs() > FEEDBACK_LOST_MS) {
            failConnect("通信途絶");
        }
    }

    if (now - lastDraw >= 50) {
        lastDraw = now;
        drawScreen();
    }
}
