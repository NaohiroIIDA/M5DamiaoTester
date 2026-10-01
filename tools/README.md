# tools

## dm_baud — DAMIAO モーターの CAN 速度切り替え (GUI)

[CANable 2.0](https://canable.io/) を使って、DAMIAO モーターの CAN 速度を
**1M (CAN 2.0)** と **5M (CAN FD)** の間で切り替える GUI アプリです。

M5DamiaoTester (CoreS3) で使うときは 1M (CAN 2.0)、OpenArm などに戻すときは 5M (CAN FD) にします。

### 起動

```sh
tools/dm_baud/run.sh
```

初回は `tools/dm_baud/.venv` に仮想環境を作り、pyserial をインストールします。
tkinter が使える Python が必要です (macOS + Homebrew なら `brew install python-tk@3.12`)。

### 使い方

1. CANable 2.0 を USB に挿し、モーターの CANH / CANL / GND をつないで、モーターの電源を入れます。
2. ポートを選んで **接続** を押すと、自動でモーターを検索します。
   CANable は 通常部 1Mbps / データ部 5Mbps の CAN FD で開くので、どちらの設定のモーターも見つかります。
3. 一覧に、CAN ID、Master ID、現在の CAN 速度設定、応答の形式 (CAN 2.0 / CAN FD) が表示されます。
4. モーターを選び、**1M (CAN 2.0) に変更** または **5M (CAN FD) に変更** を押します。
   モーターを無効化してから設定を書き込み、フラッシュに保存します。
5. **モーターの電源を入れ直してから** もう一度 **検索** を押し、設定が変わったことを確認します。

> [!WARNING]
> - 変更中、モーターは無効化 (フリー) されます。負荷がかかった状態では実行しないでください。
> - モーターのフラッシュの書き込み回数には上限 (約 1 万回) があります。

### 仕組み

| 操作 | CAN ID | データ |
|---|---|---|
| CAN 速度の読み出し | 0x7FF | `ID_L ID_H 33 23 00 00 00 00` |
| CAN 速度の書き込み | 0x7FF | `ID_L ID_H 55 23 <コード> 00 00 00` (1M = 4、5M FD = 9) |
| 無効化 | モーター ID | `FF FF FF FF FF FF FF FD` |
| フラッシュ保存 | 0x7FF | `ID_L ID_H AA 01 00 00 00 00` |

要求はすべて CAN 2.0 形式で送ります。CAN FD に設定されたモーターも CAN 2.0 形式の要求は受け取れるので、
どちらの設定からでも切り替えられます。
参考: [enactic/openarm_can](https://github.com/enactic/openarm_can) の `change_motor_baudrate_commands.cpp`

## canable_fw — CANable 2.0 のファームウェア書き込み

`dm_baud` は CANable 2.0 の slcan ファームウェア
[Nakakiyo092/canable2-fw v1.4.1](https://github.com/Nakakiyo092/canable2-fw/releases/tag/v1.4.1) を前提にしています。
`flash_canable2.sh` はこのタグを取得してビルドし、DFU で書き込みます。

```sh
# 1. CANable 2.0 の BOOT ボタンを押しながら USB に挿す (DFU モード)
# 2. 実行
tools/canable_fw/flash_canable2.sh
# 3. 書き込み後、USB を挿し直す
```

必要なもの: `arm-none-eabi-gcc`、`make`、`dfu-util` (`brew install dfu-util`)

### 公式版ではなくこの派生版を使う理由

公式版 ([normaldotcom/canable2-fw](https://github.com/normaldotcom/canable2-fw)) では、
5M (CAN FD) に設定した DAMIAO モーターの応答を受信できませんでした。データ部 5Mbps の設定に余裕がないためです。

| 項目 | 公式版 | Nakakiyo092 版 v1.4.1 |
|---|---|---|
| データ部 5M の同期補正幅 (SJW) | 1/17 ビット | 6/32 ビット |
| データ部のサンプル点 | 88% | 78% |
| 送信遅延補正 (TDC) | なし | あり |

コマンド (`S8`、`Y5`、`O`、`t`…) は公式版と互換です。

### 通信タイミング

`dm_baud` は CANable を次のタイミングで開きます (FDCAN クロック 160MHz)。

| | 速度 | コマンド | サンプル点 |
|---|---|---|---|
| 通常部 | 1Mbps | `s023B1414` | 75% |
| データ部 | 5Mbps | `y01170808` | 75% |

ファームウェアの既定 (`S8`、サンプル点 87.5%) では、5M (CAN FD) に設定した DAMIAO モーターの応答を
受信できませんでした (形式エラー)。通常部を 75% にすると受信できます (OpenArm の設定と同じ値)。
`s`/`y` に対応していないファームウェアでは `S8`/`Y5` に戻します。

### 当てているパッチ

派生版は互換機 (Walfront) に合わせて、トランシーバのスタンバイを解除する処理 (PA0 を Low) を削除しています。
Openlight Labs 製の CANable 2.0 では PA0 が基板上で High に引き上げられているため、そのままでは
トランシーバがスタンバイのままになり、送信のたびに BIT0 エラーが出て BUS_OFF になります。

[0001-drive-transceiver-standby-low.patch](canable_fw/0001-drive-transceiver-standby-low.patch) で公式版と同じく PA0 を Low にしてから
ビルドします。そのため、版の表示は `v1.4.1-dirty` になります。ファームウェアは GPLv3 のため、バイナリはこのリポジトリに含めていません。
