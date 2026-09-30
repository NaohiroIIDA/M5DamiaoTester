# M5DamiaoTester

M5Stack CoreS3 Lite で DAMIAO **DM-J4340-2EC** モーターを動かすためのテストツールです。
Chain Encoder で位置を微調整し、タッチ画面の 3 つのプリセットボタンで登録位置へ移動できます。

## ハードウェア

| 機器 | 役割 |
|---|---|
| [M5Stack CoreS3 Lite](https://shop.m5stack.com/products/m5stack-cores3-lite-esp32s3-iot-dev-kit) | 本体 (ESP32-S3、タッチ画面) |
| [Module13.2 PwrCAN](https://docs.m5stack.com/ja/module/Module13.2_PwrCAN) | 絶縁 CAN トランシーバ + 9〜24V 電源バス |
| [Chain Encoder](https://shop.m5stack.com/products/chain-encoder-stm32g031) | 位置の調整と記憶操作 |
| [DAMIAO DM-J4340-2EC](https://damiao.enactic.ai/products/hardware/dm-j4340-2ec-v1.0/) | 制御対象のモーター (CAN 1Mbps) |

### 接続

- **PwrCAN**：CoreS3 Lite の M-Bus に取り付けます。DIP スイッチで **G17 (TX) / G18 (RX)** を ON にしてください。
  モーターを CAN バスの末端につなぐ場合は、120Ω 終端抵抗の DIP も ON にしてください。
- **Chain Encoder**：CoreS3 Lite 本体の **Port A (赤)** につなぎます。Port A は UART (115200bps) として使い、黄線が G1 (RX)、白線が G2 (TX) です。
- **モーター**：PwrCAN の CAN 端子 (CANH / CANL / GND) につなぎ、モーター用の電源 (24V) を供給します。

ピン番号などは [include/config.h](include/config.h) で変更できます。

## 使い方

1. 電源を入れると、**制御 OFF** の状態で待機します。
2. **制御ボタン**を押すと ON になり、モーターを探します。
   - CAN ID を 0x001〜0x7FE の範囲で走査し、応答したモーターを使います (1 台のみ接続する前提です)。
   - 3 秒以内に見つからなければエラーを表示し、OFF に戻ります。
3. 接続すると、モーターは現在位置で静止します。このときプリセット 1〜3 も現在位置に設定されます。
4. **エンコーダを回す**と、回した量に比例して目標位置が変わり、モーターが動きます。
5. **エンコーダのボタン**を押すとその時の位置を記憶し、プリセットボタンが黄色になります。
   続けて **1 / 2 / 3** のどれかを押すと、そのボタンに記憶した位置を登録します。
6. 記憶していない状態で **1 / 2 / 3** を押すと、登録した位置へ移動します。
7. 制御ボタンをもう一度押すと、モーターを無効 (フリー) にして通信を終了します。

運転中に 1 秒以上モーターから応答がない場合は「通信途絶」と表示して OFF に戻ります。
過電流や過熱などモーター側のエラーは、画面の赤い帯に表示されます。

### 画面

```
┌──────────────────────────────────────┐
│ DM-J4340 Tester    ID:0x01    ENC OK │
├───────────┬──────────────────────────┤
│   制御    │ 現在位置                 │
│    ON     │               123.4°     │
│           │ 目標 123.4°  2.15rad     │
├───────────┴──────────────────────────┤
│ エラー: なし                         │
│ T:0.12Nm  MOS:35℃  Rotor:33℃        │
├────────────┬────────────┬────────────┤
│     1      │     2      │     3      │
│   0.0°     │   45.0°    │   90.0°    │
└────────────┴────────────┴────────────┘
```

## ビルドと書き込み

[PlatformIO](https://platformio.org/) を使います。

```sh
pio run                 # ビルド
pio run -t upload       # 書き込み
pio device monitor      # シリアルモニタ (115200bps)
```

シリアルモニタには、検出したモーターの ID、PMAX などの値、プリセット操作のログが出力されます。

## 設定 ([include/config.h](include/config.h))

| 項目 | 初期値 | 内容 |
|---|---|---|
| `CAN_TX_PIN` / `CAN_RX_PIN` | 17 / 18 | PwrCAN の CAN ピン |
| `CHAIN_RX_PIN` / `CHAIN_TX_PIN` | 1 / 2 | Chain Encoder の UART ピン |
| `M5_BUS_OUTPUT_5V` | true | 本体から 5V を出力するかどうか。PwrCAN 側から本体へ給電する場合は false |
| `DM_SCAN_ID_MIN` / `DM_SCAN_ID_MAX` | 0x001 / 0x7FE | モーター ID の走査範囲 |
| `DM_DEFAULT_PMAX` / `VMAX` / `TMAX` | 12.5 / 8.0 / 28.0 | モーターから読めなかった場合の換算用上限値 |
| `CONNECT_TIMEOUT_MS` | 3000 | モーター探索のタイムアウト [ms] |
| `FEEDBACK_LOST_MS` | 1000 | 運転中の通信途絶判定時間 [ms] |
| `ENC_RAD_PER_COUNT` | 0.0349 | エンコーダ 1 カウントあたりの目標位置変化 [rad] (約 2°) |
| `MOVE_VEL_LIMIT` | 3.0 | 移動時の速度上限 [rad/s] |
| `TARGET_MIN` / `TARGET_MAX` | -12.0 / 12.0 | 目標位置のソフトリミット [rad] |

## モーター制御の仕組み

DAMIAO の CAN プロトコルを **位置速度モード (POS_VEL)** で使います。

| 用途 | CAN ID | データ |
|---|---|---|
| 有効化 / 無効化 / エラー解除 | モーター ID | `FF FF FF FF FF FF FF FC` / `FD` / `FB` |
| 位置速度指令 (10ms 周期) | 0x100 + モーター ID | 目標位置 float + 速度上限 float (リトルエンディアン) |
| レジスタ読み書き | 0x7FF | `ID_L ID_H 33/55 RID データ(4byte)` |

接続時の手順は次のとおりです。

1. ID を走査し、PMAX レジスタに応答したモーターを見つけます。
2. VMAX / TMAX を読み、フィードバックの換算に使います。
3. 制御モードを POS_VEL に切り替えます。フラッシュには保存しないので、モーターの電源を入れ直すと元のモードに戻ります。
4. 無効状態のまま現在位置を読み、有効化してその位置を目標として送ります。

## ソース構成

```
platformio.ini      ビルド設定 (M5Unified, M5Chain)
include/config.h    ピン・パラメータ設定
src/DmMotor.h/.cpp  DAMIAO モーターの CAN 通信 (ESP32 TWAI)
src/main.cpp        画面表示・タッチ操作・制御の流れ
```

## 注意

- 実機での動作確認は、まだ十分にはできていません。初めて動かすときは、モーターを負荷から外し、すぐ電源を切れる状態で試してください。
- CoreS3 と PwrCAN の組み合わせで CAN の送信エラーが出るという報告が [M5Stack コミュニティ](https://community.m5stack.com/topic/7411/cores3-pwrcan-13-2-module-working-demo-required) にあります。
  通信できないときは、DIP スイッチ、終端抵抗、24V の供給を確認してください。
- エンコーダのボタンは「押下 = 1」として判定しています。動作が逆になる場合は `src/main.cpp` の `readEncoder()` を修正してください。
