#pragma once

// ============================================================
//  ハードウェア設定
// ============================================================

// Module13.2 PwrCAN (CAN) … DIPスイッチで G17 / G18 を ON にすること
#define CAN_TX_PIN 17
#define CAN_RX_PIN 18

// Chain Encoder … CoreS3 Lite の Port A (赤) に接続 (UART 115200bps)
#define CHAIN_RX_PIN 1  // Port A 黄
#define CHAIN_TX_PIN 2  // Port A 白

// PwrCAN から本体へ 5V を給電する場合は false にする
// (USB給電で使う場合は true: 本体から Port A / M-Bus へ 5V を出力)
#define M5_BUS_OUTPUT_5V true

// ============================================================
//  DAMIAO モーター設定
// ============================================================
// モーターの CAN ID (ESC_ID) は接続時にこの範囲を走査して自動検出する
// (1台のみ接続する前提。最初に応答したモーターを使う)
#define DM_SCAN_ID_MIN 0x001
#define DM_SCAN_ID_MAX 0x7FE

// 位置・速度・トルクのマッピング範囲。接続時にモーターの
// レジスタから読み出すが、読めなかった場合はこの値を使う。
#define DM_DEFAULT_PMAX 12.5f  // [rad]
#define DM_DEFAULT_VMAX 8.0f   // [rad/s]  (J4340)
#define DM_DEFAULT_TMAX 28.0f  // [Nm]     (J4340)

// ============================================================
//  動作パラメータ
// ============================================================
#define CONNECT_TIMEOUT_MS 3000   // この時間モーターが見つからなければエラー
#define FEEDBACK_LOST_MS 1000     // 運転中にこの時間応答が無ければ通信エラー
#define CONTROL_PERIOD_MS 10      // モーター指令周期
#define ENCODER_PERIOD_MS 20      // エンコーダ読み取り周期

#define ENC_RAD_PER_COUNT 0.0349f  // エンコーダ1カウントあたりの目標位置変化 [rad] (≒2°)
#define MOVE_VEL_LIMIT 3.0f        // POS_VEL モードの速度上限 [rad/s]

// 目標位置のソフトリミット [rad] (PMAX の範囲内に収まるように設定)
#define TARGET_MIN -12.0f
#define TARGET_MAX 12.0f
