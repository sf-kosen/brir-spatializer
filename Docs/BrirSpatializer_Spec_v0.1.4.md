# 仕様書：BrirSpatializer Spatializer（Windows / VB-CABLE 経由）v0.1.4

> **目的**：Windows PC のシステム音声を VB-CABLE 経由で取り込み、映画館の座席で聴くような音響効果（主にバイノーラル化＋ホール感）に変換して物理出力デバイス（ヘッドホン/スピーカー）へ出力するリアルタイム音声処理アプリを実装する。  
> **方針**：人の手で評価しながら追い込めることを重視し、音質劣化を最小化しつつ A/B 比較・バイパス・微調整 UI・プリセット保存を提供する。

---

## 0. 改訂概要
### v0.1 → v0.1.1

v0.1 に対し、以下を **追加で仕様化**した。

1. **入力/出力クロック差（ドリフト）対策**を必須要件として明記  
2. BRIR 2×2 畳み込みの **FFT実装依存・パーティション設計・IRフォーマット**を決め打ち  
3. Bypass / A-B / IR切替の **クロスフェード位置（どこで混ぜるか）**を明記  
4. Meter / Volume Match の **測定点・ゲート閾値・ヒステリシス**を固定  
5. **初期プリセットと IR 未設定時の導線**を追加  

### v0.1.1 → v0.1.4

v0.1.1 に対し、以下を **追加で仕様化**した。

1. **Bypass の DryPath レイテンシ補償**に、ドリフト補正（ASRC/Slip）由来のバッファリング遅延を含める設計を明記  
2. FFT 実装について、**MathNet.Numerics のネイティブプロバイダ（MKL）**を利用可能にする検討項目を追加  
3. Volume Match の補正ゲインに **最大補正幅（既定 ±12 dB）**およびスムージングの安全策を追加  
4. メーター等の UI 更新について、**オーディオスレッド非干渉（ロックフリー/非同期）**の要件を追加  


### v0.1.3 → v0.1.4

v0.1.3 に対し、以下を **追加で仕様化**した。

1. プリロール中に Output が無音を出す場合、**リングバッファの Read Pointer を固定**して水位を上げ、`driftTargetBufferMs` 到達で読み出し開始＋フェードインする要件を追加
2. プリロール記述が誤ってプリセット例（13章）に混入していたため、**配置を 5.4 に統一し Markdown を修正**

---

## 1. スコープ

### 1.1 MVP（v0.1.4）に含める
- VB-CABLE（仮想オーディオデバイス）からの音声入力（ステレオ 2ch）
- 物理出力デバイスへの音声出力
- DSP チェーン（最低限）
  - Preamp（ゲイン）
  - Spatializer（立体化：BRIR 2×2 畳み込みを主）
  - Wet/Dry ミックス
  - Parametric EQ（最低 3 バンド）
  - Level Match（音量マッチ補正ゲイン）
  - Limiter（最終段）
- 人手評価支援
  - Bypass（処理ON/OFF）
  - A/B スロット（設定2系統の即時切替）
  - 音量マッチ（RMS ベースの自動補正）
- プリセット保存/読込（JSON）
- デバイス選択 UI（入力=VB-CABLE 出力、出力=物理デバイス）
- 安定動作（音切れしにくいバッファ設計、例外処理、ログ）
- **ドリフト補正**（ASRC またはバッファスリップ）を必須とする

### 1.2 v0.1.4 で除外（将来対応）
- 5.1/7.1 入力のバイノーラルレンダ
- ヘッドトラッキング
- 実時間の幾何音響（レイトレーシング等）による反射生成
- VST/ASIO 等のプラグイン/ドライバ化

---

## 2. 対象環境
- OS：Windows 10/11
- ランタイム：.NET 8（推奨）
- オーディオ：WASAPI（Shared を基本、出力はオプションで Exclusive）
- 依存：NAudio（入出力・デバイス列挙用）
- FFT：MathNet.Numerics（Complex32）
- （任意）FFTネイティブ高速化：MathNet.Numerics.Providers.MKL（導入可能な設計にする）
- 外部：VB-CABLE（ユーザーが別途インストール）

---

## 3. ユーザーフロー（利用手順）
1. Windows の既定再生デバイスを「CABLE Input（VB-CABLE）」に設定  
2. 本アプリを起動  
3. Input Device で「CABLE Output（VB-CABLE）」を選択（既定候補として自動選択）  
4. Output Device で物理ヘッドホン/スピーカーを選択  
5. Start を押すとリアルタイム処理が開始  
6. Bypass / A-B 切替 / Seat などのノブで調整し、プリセットを保存  

---

## 4. 非機能要件（音質・安定性）
- 内部処理：32-bit float
- 変換最小化：可能なら入出力とも 48kHz に統一し、不要な SRC を避ける
- クリップ対策：標準で -6 dB 以上のヘッドルームを確保、最終段 Limiter で保護
- 無音/断音回避：音声スレッドで重い処理（IR 再計算等）をしない
- レイテンシ目標：実用域（概ね 30–120ms の範囲で破綻なく動くことを優先）
- 連続運用：30 分以上の連続再生で破綻しない（ドリフト対策込み）
- ログ：ドロップ/スリップ/ASRC比率・バッファ水位・クリップ検出を記録できる

---

## 5. オーディオ I/O 仕様

### 5.1 入力
- デバイス：WASAPI Capture（VB-CABLE の “CABLE Output” を想定）
- チャンネル：2ch（ステレオ）
- サンプルレート：デバイスに従う（推奨 48kHz）
- フォーマット：float を基本（PCM の場合は float に変換）

### 5.2 出力
- デバイス：WASAPI Render（物理デバイス）
- チャンネル：2ch（ステレオ）
- サンプルレート：基本は入力と一致させる  
  - 一致しない場合の v0.1.4 挙動：UI 警告＋設定により「起動拒否」または「補正（ASRC）」を選択可能

### 5.3 バッファリング
- 処理ブロック：256/512/1024 samples（既定 512）
- 入出力：Shared を既定
- ドロップ時：ログ記録、バッファ増加のガイド表示

### 5.4 追加：入力（VB-CABLE）と出力（物理）のクロック差（ドリフト）対策（必須）

**問題定義**  
WASAPI Shared で異なるデバイス間をルーティングすると、同一サンプルレートでもクロック差によりリングバッファ水位が徐々に偏り、アンダーラン/オーバーラン→音切れが起きうる。

**v0.1.4 の採用方針（デフォルト）**
- 既定：方針A（推奨）＝ **常時微小なドリフト補正 ASRC**  
- フォールバック：方針B＝ **閾値超過時の間引き/複製（スリップ）**  
- 制約モード：方針C＝ **Strict（補正なし、条件不一致は起動不可）**

#### 方針A：可変レート ASRC（v0.1.4 デフォルト）
- リングバッファ水位 `fillLevel` を `targetLevel` に維持する
- 変換比率 `r` を微小に変化させる（例：0.9998〜1.0002）
- `r` は PI 制御で更新：`r = 1 + Kp*e + Ki*∫e dt`（上限/下限あり）
- 補間：MVP は **4-point Lagrange / cubic** を許容（将来高品質に差替え可能）

#### 方針B：バッファスリップ（フォールバック）
- 水位が閾値を超えたらサンプルの drop / duplicate を実施
- クリック回避：5〜20ms のクロスフェードで挿入/削除する

#### 方針C：Strict（デバッグ用）
- ドリフト補正を無効
- SR一致等の条件不一致は起動不可（または強い警告）

**設定（JSON）**
```json
"audio": {
  "blockSize": 512,
  "driftCompensationMode": "Asrc",
  "driftTargetBufferMs": 60,
  "startup": { "prerollMs": 800, "muteDuringPreroll": true, "holdReadPointerWhileMuted": true, "startWhenBufferReachesTarget": true, "fadeInMs": 80 },
  "driftAsrc": { "kp": 0.00002, "ki": 0.0000005, "rMin": 0.9998, "rMax": 1.0002 },
  "driftSlip": { "hiWaterMs": 90, "loWaterMs": 30, "xfadeMs": 10 }
}
```

---

## 6. DSP チェーン仕様

### 6.1 チェーン順序（固定）
1. Input Gain（Preamp）
2. Spatializer（立体化）
3. Wet/Dry Mix（0–100%）
4. EQ（3バンド）
5. Level Match（音量マッチ補正ゲイン）
6. Limiter（最終段）
7. Meter（Peak/RMS、クリップ検出）

---

## 7. Spatializer（BRIR 2×2 畳み込み）詳細仕様

### 7.1 数式（2×2 畳み込み）
- outL = conv(inL, IR_LL) + conv(inR, IR_RL)  
- outR = conv(inL, IR_LR) + conv(inR, IR_RR)

### 7.2 FFT 実装（決め打ち）
- FFT：**MathNet.Numerics** を標準採用（Complex32）
- （任意）環境が許せば **MathNet.Numerics.Providers.MKL** を用いて FFT をネイティブ高速化できる設計にする
  - 実装要件：アプリ起動時に MKL プロバイダのロードを試行し、失敗時は自動的に Managed プロバイダへフォールバック
- **配布方針（v0.1.4 既定）**：MKL DLL をアプリに同梱しない。
  - 起動時に `./providers/` 等の既定パスから MKL プロバイダ DLL のロードを試行し、存在すれば有効化（なければ Managed）
  - 目的：ポータブル性（配布サイズ/依存）を保ちつつ、高性能環境では高速化できるようにする
  - 設定（任意）：`fftProvider = Managed | MKL`（未指定時は Auto）
- 自前 FFT 実装は v0.1.4 では非推奨（将来オプション）

### 7.3 ブロック/パーティション設計（既定値）
- audio blockSize：512 samples（既定）
- convolver FFT size：2048 samples（=4×blockSize）
- partition length：1024 samples（=FFT size/2、Overlap-Save）
- アルゴリズム：Uniform partitioned convolution（Overlap-Save）
- **FFT size / partition length はアプリ全体で固定**（A/B比較の一貫性確保）

### 7.4 IR 長の上限（MVP制約）
- 最大 IR 長：2048 ms（48kHz換算で 98,304 samples）
- 超過はエラー（UIで明示）し、Fallback に切替可能

### 7.5 IR フォーマット（4ch WAV）とチャンネル順（固定）
4ch WAV のチャンネル順を以下に固定する。

- ch0 = IR_LL（入力L → 耳L）
- ch1 = IR_LR（入力L → 耳R）
- ch2 = IR_RL（入力R → 耳L）
- ch3 = IR_RR（入力R → 耳R）

**自己テスト（必須）**
- Lのみインパルス入力 → 出力Lが IR_LL、出力Rが IR_LR  
- Rのみインパルス入力 → 出力Lが IR_RL、出力Rが IR_RR  

---

### 7.6 IR ロード時の安全策（正規化・必須）

**リスク**：ユーザーが読み込む IR のレベルがバラバラだと、Spatializer 通過後に過大ゲインとなりクリップ/耳への負担が起きる。

**要件（v0.1.4）**
- IR 読込時に以下のいずれかの **正規化モード**を適用可能にする（既定：Peak 正規化）
  - `None`：正規化しない（上級者向け）
  - `Peak`：IR のピークを `targetPeakDbFS` に合わせる（既定：-6 dBFS）
  - `Rms`：IR の RMS を `targetRmsDbFS` に合わせる（既定：-18 dBFS、測定窓は全長）
- 正規化適用後も、最終出力は Limiter で保護する（11章）
- 安全策：IR のピークが極端に小さい/無音の場合は正規化をスキップし、UI に警告を出す

**設定（Preset 追加）**
```

**起動時プリロール（必須・v0.1.4）**
- 起動直後はリングバッファが空のため、ASRC（PI制御）やデバイス間ルーティングが安定するまで出力が不安定になり得る
- 仕様：プリロール中は **Output コールバックで無音（0）を出力**しつつ、**リングバッファの Read Pointer を固定（消費しない）**する
  - Input 側は書き込みを継続し、水位（fillLevel）が上昇する
  - 水位が `driftTargetBufferMs`（target 水位）に達した瞬間に、Read Pointer の固定を解除して **読み出しを開始**し、`fadeInMs` でフェードインする
- 安全策：プリロール中にリングバッファが溢れないよう、リングバッファ容量は `driftTargetBufferMs + margin` を満たすこと（margin 既定 250ms など）
- 例外：入力が長時間無音/停止で水位が上がらない場合に備え、`maxPrerollMs`（任意）を超えたら待機継続または停止（実装で選択可）

json
"spatializer": {
  "mode": "BRIR",
  "brir": {
    "format": "4chWav",
    "path": "C:\\IR\\BrirSpatializer\\Seat_Center_Mid.wav",
    "normalize": { "mode": "Peak", "targetPeakDbFS": -6.0, "targetRmsDbFS": -18.0 }
  }
}
```


## 8. クロスフェード仕様（どこで混ぜるかを固定）

### 8.1 共通要件
- クリック回避のため、切替は必ずクロスフェードで行う
- 既定 xfade：50ms（設定可：20〜200ms）
- IR切替 xfade：100ms（設定可：50〜300ms）

### 8.2 Bypass クロスフェード（必須）
- **ドリフト補正（ASRC/Slip）由来の遅延を含めて**、Dry/Wet の時間整合を取る
- クロスフェード対象（推奨の取り方）：
  - `DryPath`：**ドリフト補正ステージ（ASRC/Slip）通過後**の素通し信号（= 出力サンプルレートに揃った信号）
  - `ProcessedPath`：同じ `DryPath` を入力として DSP チェーンを通した出力
  - `DryPathAligned`：`DryPath` に **DSP 固定遅延分（Convolver 等）** の Delay を入れたもの
    - 重要：ASRC の内部バッファリング遅延は `DryPath` 側に自然に含まれるため、追加 Delay は **DSP 固定遅延のみ**でよい
- 混合地点：**Limiter 前（Level Match 後）** を標準

### 8.3 A/B クロスフェード（必須）
- クロスフェード対象：Slot A チェーン出力と Slot B チェーン出力
- 混合地点：**Limiter 前（Level Match 後）** を標準

### 8.4 IR 切替クロスフェード（必須）
- Convolver 内部で旧IR系と新IR系を **並列稼働**し、出力をクロスフェード
- 推奨：新Convolverを **プリロール**（直近入力履歴を与えて状態を作る）してからフェード開始

---

## 9. Meter / Volume Match 仕様（測定点固定）

### 9.1 Meter 測定点（固定）
- Peak/RMS のタップ位置：**Limiter 直前（PreLimiterTap）**

### 9.2 Volume Match 測定点（固定）
- RMS 計測：Bypass / Slot A / Slot B の **PreLimiterTap**
- 補正ゲイン適用点：EQ 後〜Limiter 前（Level Match ブロック）

### 9.3 ゲート閾値とヒステリシス（固定）
- 凍結条件：RMS が -50 dBFS 未満で補正更新停止
- 解除条件：RMS が -45 dBFS 超で更新再開（ヒステリシス）
- 補正追従：0.5〜2秒時定数でスムージング（ポンピング回避）
- **最大補正幅（必須）**：Level Match 補正ゲインは既定で **±12 dB** にクランプ（設定可）
- （推奨）**ゲイン変化速度の上限**：急変を避けるため 1秒あたりの変化量（dB/s）に上限を設ける

### 9.4 リファレンス（既定）
- 既定：Bypass を基準に Slot A/B を一致させる
- 将来：A基準/B基準を追加可能

---

## 10. EQ 仕様（MVP）
- 3バンド・パラメトリック EQ（biquad）
  - Low Shelf：freq, gain(dB)
  - Bell：freq, Q, gain(dB)
  - High Shelf：freq, gain(dB)
- デフォルト：全て 0 dB（フラット）

---

## 11. Limiter 仕様（MVP）
- 目的：クリップ防止（透明系の簡易リミッタで可）
- 仕様例：
  - Threshold：-1.0 dBFS 既定
  - Release：50–200ms
  - Lookahead：任意（可能なら数ms）
- クリップ検出：0 dBFS 超過回数/秒を計測し UI 表示

---

## 12. UI 仕様（MVP）

### 12.1 画面要素（1ウィンドウで完結）
- Input Device（ドロップダウン）
- Output Device（ドロップダウン）
- Sample Rate 表示（Input/Output）
- Start/Stop
- Bypass トグル
- A/B 切替
- Volume Match トグル
- 6ノブ/スライダー（SeatX, SeatY, Width, Early, Late, Damping）
- Wet/Dry
- Preamp（dB）
- EQ（折り畳み可）
- Limiter（On/Off + Threshold）
- メーター（Peak/RMS、クリップ警告）
- Preset：Save / Load（JSON）
- Drift 状態表示（buffer水位、ASRC比率、Slip発生回数）

### 12.2 ショートカット（推奨）
- Space：Bypass
- Tab：A/B 切替
- Ctrl+S：保存
- Ctrl+O：読込

### 12.3 初期導線（必須）
- 同梱プリセット（IR不要）：
  - Slot A：`CrossfeedRoom (Fallback)`
  - Slot B：`CrossfeedRoom Wide (Fallback)`
- BRIR モードで IR 未設定の場合：
  - 「IR未設定」を明示し、Fallback に自動フォールバック
  - IRファイル選択ボタンと、4ch順（LL, LR, RL, RR）を表示
- IRロード失敗：
  - 音を止めず Fallback 継続
  - エラーをログ＋UI表示（形式、ch数、SR等）

### 12.4 UI更新・スレッドセーフ（必須）

- オーディオスレッドは **UIスレッドに依存しない**こと（UIハングが音に影響しない）
- メーター等の共有は **ロックフリー**を基本とし、オーディオスレッド側での待ち（lock/await）を禁止する
- 推奨方式（どちらか）
  - **方式1：ポーリング**  
    - オーディオスレッド：`volatile` / `Interlocked` を用いて最新値（RMS/Peak/Clip/BufferLevel/ASRCRatio）を書き込む  
    - UIスレッド：`DispatcherTimer` 等で 30〜60fps で読み出して描画（読み出し頻度はUI側で制御）
  - **方式2：メッセージキュー（ドロップ可）**  
    - オーディオスレッド：固定長リングキューにメータースナップショットを書き込む（満杯なら古いものを破棄）  
    - UIスレッド：キューを取り出して最新のみ反映
- 禁止事項：オーディオコールバック内での大量アロケーション、LINQ、多量ログ出力、UI直接呼び出し

---

## 13. プリセット（JSON）フォーマット

### 13.1 例（差分を含む）
```json
{
  "name": "Default",
  "version": "0.1.4",
  "audio": {
    "blockSize": 512,
    "driftCompensationMode": "Asrc",
    "driftTargetBufferMs": 60,
    "startup": {
      "prerollMs": 800,
      "maxPrerollMs": 5000,
      "muteDuringPreroll": true,
      "holdReadPointerWhileMuted": true,
      "startWhenBufferReachesTarget": true,
      "fadeInMs": 80
    },
    "driftAsrc": { "kp": 0.00002, "ki": 0.0000005, "rMin": 0.9998, "rMax": 1.0002 }
  },
  "routing": {
    "inputDeviceId": "WASAPI_DEVICE_ID_STRING",
    "outputDeviceId": "WASAPI_DEVICE_ID_STRING"
  },
  "controls": {
    "bypass": false,
    "activeSlot": "A",
    "volumeMatch": true,
    "xfadeMs": 50,
    "irSwitchXfadeMs": 100
  }
}
```


## 14. アーキテクチャ（推奨）

### 14.1 主要コンポーネント
- `AudioEngine`：WASAPI入出力、リングバッファ、ドリフト補正
- `DspGraph`：チェーン管理、Bypass/A-B、クロスフェード
- `SpatializerProcessor`：BRIR Convolver / Fallback
- `BRIRConvolver`：パーティションFFT、IR切替（並列＋プリロール）
- `EqProcessor`：Biquad
- `LevelMatchProcessor`：RMS測定（PreLimiterTap）＋補正ゲイン
- `LimiterProcessor`：最終段保護
- `Metering`：Peak/RMS、クリップ検出
- `PresetManager`：JSON I/O
- `DeviceManager`：デバイス列挙、ID解決

### 14.2 インターフェース例
- `IAudioProcessor.Process(float[] interleavedStereo, int frames)`
- `IConfigurable.SetParameters(...)`（スレッドセーフ、スムージング）
- `ISwitchable`（A/B、Bypass、IR切替）

---

## 15. 受け入れ基準（Acceptance Criteria）

### 15.1 機能
- VB-CABLE から取り込み、物理出力へ連続出力できる
- Start/Stop が安定して開始/停止し、クラッシュしない
- Bypass/A-B/IR切替がクリックノイズ少なく動作する（クロスフェードで許容）
- Volume Match が有効に機能し、比較時の音量差が縮む
- プリセット Save/Load が動く

### 15.2 品質
- 連続再生 30 分で破綻しない（ドリフト補正込み）
- 起動直後（プリロール→フェードイン）で顕著なプチノイズ/クリックが発生しない
- クリップ時に警告表示が出る
- パラメータ操作で破綻しない（スムージング）

---

## 16. 実装マイルストーン（推奨順）
1. 素通し（VB-CABLE入力→物理出力）＋リングバッファ＋ログ  
2. **ドリフト補正（ASRC または Slip）**を入れて、30分連続で破綻しないことを確認  
3. DspGraph（Bypassクロスフェード、DryPathレイテンシ補償）  
4. A/B（チェーン出力同士をクロスフェード）  
5. Meter（PreLimiterTap）  
6. Preamp / WetDry / EQ / LevelMatch / Limiter  
7. Spatializer：Fallback → BRIR Convolver（MathNet FFT、設計固定）  
8. IR切替（並列2系統＋プリロール＋クロスフェード）  
9. Preset Save/Load と初期導線の仕上げ  

---


