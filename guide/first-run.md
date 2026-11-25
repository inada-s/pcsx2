---
title: 初回設定
---

# 初回設定

初めて起動すると、設定ウィザードが開きます。画面に従って「次へ」で進みます。

## PCSX2 の設定ウィザード

### 1. 言語を選ぶ

「言語」で日本語を選びます。

{% include shot.html file="wizard-language.png" alt="ウィザード最初の画面で言語を選ぶ" %}

ほかの項目はそのままで「次へ」を押します。

### 2. BIOS を指定する

「参照...」を押して、BIOS ファイルを入れたフォルダを選びます。
リストに出てきた BIOS を選んで「次へ」を押します。

{% include shot.html file="wizard-bios.png" alt="BIOS フォルダを選び、リストの BIOS を選ぶ" %}

> リストが空のときは、選んだフォルダが違うか、ファイルが BIOS ではありません。
> フォルダを確かめて「リストを更新」を押してください。

### 3. ゲームのフォルダを指定する

「追加...」を押して、ゲームのディスクイメージ (ISO など) を入れたフォルダを選びます。

{% include shot.html file="wizard-games.png" alt="ゲームのディスクイメージがあるフォルダを追加する" %}

### 4. コントローラーを設定する

最初はキーボードがコントローラーとして割り当てられています。

ゲームパッドを使うときは、PC につないでから「コントローラー端子 1」の「自動マッピング」を押し、
自分のゲームパッドを選びます。

{% include shot.html file="wizard-controller.png" alt="自動マッピングを押してゲームパッドを選ぶ" %}

> 細かい割り当ては、あとで 設定 → コントローラー で変えられます。

### 5. Discord でサインインする

「Discord でサインイン」を押します。

{% include shot.html file="wizard-account.png" alt="zdxsv アカウントの画面で「Discord でサインイン」を押す" %}

ブラウザに「ZDXSV にサインイン」のページが開くので、もう一度「Discord でサインイン」を押します。

{% include shot.html file="signin-page.png" alt="ブラウザの「ZDXSV にサインイン」ページで「Discord でサインイン」を押す" %}

Discord の画面で「認証」を押すと、PCSX2 に戻ってサインイン済みになります。

### 6. 完了

「設定が完了しました」と表示されたら「完了」を押します。
「デスクトップにショートカットを作成」をオンにしておくと、次から起動が楽になります。

{% include shot.html file="wizard-complete.png" alt="設定が完了しました の画面で「完了」を押す" %}

## ゲーム内の初回設定

### 7. ゲームを起動する

ゲームリストに出てきたゲームをダブルクリックします。

{% include shot.html file="gamelist.png" alt="ゲームリストのゲームをダブルクリックする" %}

### 8. 通信対戦の準備をする

1. メインメニューで「通信対戦」を選びます。
   「NET ファイルが見つかりませんでした」と出たら「はい」を選びます。
   続けて「MEMORY CARD 差込口 1」を選び、フォーマットも「はい」を選びます。

   {% include shot.html file="game-netfile.png" alt="NET ファイルが見つかりませんでした の画面で「はい」を選ぶ" %}

2. 「ブロードバンド接続」を選び、次の画面で「進む」を選びます。

   {% include shot.html file="game-connect.png" alt="「ブロードバンド接続」を選ぶ" %}

   {% include shot.html file="game-terms.png" alt="規約の画面で「進む」を選ぶ" %}

3. Windows のファイアウォールの確認が出たら「許可」を押します。
   キャンセルすると、対戦がつながらないことがあります。

   {% include shot.html file="win-firewall.png" alt="Windows セキュリティの画面で「許可」を押す" %}

4. ID 選択で「未登録」の欄を選び、新しい ID を作ります。
   ユーザー登録の画面は「登録しない」で進みます。

5. ハンドルネームを入力して「登録」を選びます。

   {% include shot.html file="game-hn.png" alt="HN 入力の画面でハンドルネームを入れて「登録」を選ぶ" %}

### 9. ロビーに入れたら完了

戦場選択 (ロビー) が表示されたら、セットアップは完了です。

{% include shot.html file="game-lobby.png" alt="戦場選択 (ロビー) の画面" %}

> 設定 → zdxsv の「高速起動」をオンにすると、次からはゲームを起動するだけで ID 選択画面まで自動で進みます。

<p class="next">次へ: <a href="{{ '/play' | relative_url }}">遊び方と設定</a></p>
