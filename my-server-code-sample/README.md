# MyGameServer

現在、ルートプロジェクトでビルドするのはロジックサーバー `zone` のみです。

`gateway` のコードは、プロトコルを参照するためにシンボリックリンク経由で本プロジェクト内に配置されています。独立したビルド手順を持っており、現在の CMake ルートプロジェクトではコンパイルされません。

## ディレクトリ構成

* `zone/`：C++製のロジックサーバー。
* `schema/`：FlatBuffers のプロトコル定義。**唯一の正規ソース**です。フロントエンドの Cocos プロジェクトと Java の参考実装にはそれぞれコピーがありますが、変更内容をこちらに反映することはありません。プロトコルを変更する場合は、必ずこのディレクトリを編集し、その後、それぞれの環境で再生成してください。
* `db/`：zone のローカル業務テーブルを初期化する SQL。
* `zone/nav/`：マップのナビゲーション用グリッドの元データ（JSON）。`zone/tools/nav_json_to_header.py` によって `zone/include/generated/` に生成されます。
* `bin/`：ビルド後にコピーされる実行ファイルと、ローカル用の `env.zone`。

## ビルド

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

ビルドが成功すると、`zone` が `bin/zone` にコピーされます。

## zone の起動

`zone` は起動時に、現在の作業ディレクトリから `env.zone` を読み込みます。

`ZONE_ID` は必ず `env.zone` に記述してください。起動引数での指定には対応していません。

```sh
cd bin
./zone
```

`zoneId` の仕様：

* `zoneId` は gateway のデータベースにある `zones.id` に対応します。zone リストのインデックスではありません。
* `zones.id` は `1` から始まる連番です。
* `zoneId` は符号なし整数です。`0` は無効または未設定を意味します。
* 現在の gateway の wire protocol では `zoneId` を `u8` としてエンコードするため、有効範囲は `1..255` です。
* `ZONE_ID` が未設定、空文字、`0`、数値以外、または `255` を超える場合、zone は `error:` を出力して直ちに終了します。MySQL／Redis／HTTP の起動処理には進みません。

設定ミスの例：

```env
ZONE_ID=0
```

この場合、次のようなエラーが出力されます。

```text
error: invalid or missing ZONE_ID; zoneId is an unsigned zones.id value starting from 1, current gateway wire range is 1..255.

error: Failed to load env.zone
```

## 正常終了とログの色

zone の起動時には、次のメッセージが表示されます。

```text
info: normal shutdown: type 'q', 'exit', or 'quit' then press Enter
```

コンソールで `q`、`exit`、または `quit` を入力して Enter キーを押すと、正常終了処理が開始されます。

本番環境で `SIGINT` または `SIGTERM` によってサーバーを停止する場合も、同じ安全な終了処理が実行されます。

実装では、専用スレッドが `sigwait` を使用してシグナルを受け取り、`runtime_shutdown::requestShutdown()` に変換します。HTTP/WS、game worker、Redis worker、MySQL worker は、それぞれのスレッド内で処理の排出（drain）とリソースの解放を行います。

ログのプレフィックスと色は、`zone_log` によって統一的に管理されます。

* `info:` 緑
* `debug:` 白
* `warning:` 黄
* `error:` 赤

## env.zone

`zone` はデフォルトで、現在の作業ディレクトリから `env.zone` を読み込みます。

`bin/zone` を使用する場合は、`bin/` ディレクトリから起動するか、起動時の作業ディレクトリに `env.zone` を配置してください。

最小構成の例：

```env
APP_ENV=dev

ZONE_ID=1

MYSQL_HOST=127.0.0.1

MYSQL_PORT=3306

MYSQL_USER=root

MYSQL_PASSWORD=

MYSQL_DB=game_zone_1

REDIS_HOST=127.0.0.1

REDIS_PORT=6379

REDIS_USER=

REDIS_PASSWORD=

REDIS_DB=1

LISTEN_HOST=0.0.0.0

LISTEN_PORT=10001

TICK_RATE=30

ZONE_INTERNAL_API_TOKEN=change-me
```

### MySQL 接続ルール

* zone は **MySQL のクラシックプロトコル（デフォルトポート 3306）** を使用します。実装は `zone/src/db/mysql_client.cpp` にあり、内部では libmysqlclient を使用しています。以前使用していた X DevAPI（33060）はすでに削除されています。
* `db::Session::sql()` では `?` プレースホルダーを使用します。プレースホルダーの置換処理は**引用符を考慮**しており、文字列リテラルの外にある `?` のみを置換します。値はすべて `mysql_real_escape_string` でエスケープされ、文字列を直接連結することはありません。
* 接続には `CLIENT_FOUND_ROWS` を指定します。そのため、`affected_rows` が返すのは変更された行数ではなく、**条件に一致した行数**です。これにより、値が変わらない UPDATE を「対象行が存在しない」と誤判定することを防ぎます。このフラグがないと、サーバー停止時の flush で大量の偽の `skip missing role_unit` 警告が発生します。
* `runtime_config::kMySqlXTlsMode` は TLS を必須にするかどうかを制御します。名前に含まれる `X` は移行前の名残で、現在はクラシックプロトコル接続の `SSL_MODE` に適用されます。

### 数値の解析ルール

* `MYSQL_PORT`、`REDIS_PORT`、`LISTEN_PORT` は、文字列全体が数値である必要があります。有効範囲は `1..65535` です。
* `REDIS_DB` は、文字列全体が数値である必要があります。有効範囲は `0..INT_MAX` です。
* `TICK_RATE` は、文字列全体が数値である必要があります。有効範囲は `1..240` です。
* `3306abc`、`+6379`、`-1`、空文字列など、暗黙的な型変換につながる値は受け付けません。設定に誤りがある場合、`loadFromEnvFile` は失敗を返し、起動を中止します。
* `MYSQL_HOST`、`MYSQL_USER`、`MYSQL_DB`、`REDIS_HOST` は必須設定です。未設定または空の場合、起動時にエラーとなります。
* `bin/env.zone` にある `LOG_LEVEL` は、現在**どのコードからも読み取られていません**。変更しても効果はありません。ログレベルは現在 `zone_log` のコンパイル時に決定されます。設定項目として実装するか、env から削除してください。設定可能であるかのような状態を放置しないでください。

### Redis 接続ルール

* `REDIS_PASSWORD` が空の場合、`AUTH` は送信されません。`redis_worker` は `SELECT REDIS_DB` を実行します。一方、`zone_http` は `REDIS_DB != 0` の場合に限り明示的に `SELECT` を実行し、DB 0 の場合は Redis のデフォルトデータベースを使用します。
* `REDIS_PASSWORD` が空でなく、`REDIS_USER` が空の場合は、従来形式の `AUTH <password>` を送信します。
* `REDIS_USER` と `REDIS_PASSWORD` の両方が空でない場合は、ACL 形式の `AUTH <user> <password>` を送信します。
* `REDIS_USER` が設定されているのに `REDIS_PASSWORD` が空の場合、Redis ACL 認証を確実に完了できないため、起動を拒否します。
* gateway の `/join` が ticket／reservation を書き込み、zone が ticket／passToken を消費するため、両者は同じ Redis host／port／DB／key namespace を参照する必要があります。DB または ACL の設定が一致しない場合、クライアント側では通常、`TicketNotFound`、WS upgrade の失敗、または Redis worker の再接続失敗として現れます。
* gateway の設定にある `REDIS_SCHEMA` は、このファイルの `REDIS_DB` と一致させる必要があります。たとえば gateway が `REDIS_SCHEMA=1` を使用する場合、zone も `REDIS_DB=1` にしてください。
* Redis ACL では、少なくとも zone が `zone:<id>:*` と `ship_exp:*` にアクセスでき、現在の実装で使用する `EVAL`、`GETDEL`、`ZREM`、`INCR`、`DECR`、`GET`、`SET`、`DEL`、`SELECT`、`PING` を実行できる必要があります。
* `ship_exp:*` は経験値を書き戻すためのキャッシュにすぎません。Redis が利用できない場合でも、経験値タスクは先に MySQL の最終永続化キューへ転送されます。Redis worker はバックオフ付きで再接続し、MySQL の永続化や shutdown 時の drain を妨げてはいけません。

### 内部 HTTP API のルール

* `/api/role`、`/api/role/create`、`/api/map-respawn`、`/api/reload-config` は gateway／運用向けの内部 API です。クライアント向けの公開プロトコルではありません。
* 呼び出し元は `X-Zone-Internal-Token: <ZONE_INTERNAL_API_TOKEN>` を付与する必要があります。
* `ZONE_INTERNAL_API_TOKEN` が空の場合、これらの内部 API は `503` を返します。これにより、パブリックネットワークで待ち受けている場合に、照会機能や DB 書き込み機能が無認証で公開されることを防ぎます。
* 例にある `change-me` はローカル開発環境でのみ使用してください。本番環境では、十分にランダムな内部 token に置き換える必要があります。
* `/game/ws` はこの header を参照しません。クライアントは join ticket または `passToken` のみを使用して WS 接続を確立します。

本番環境で推奨される設定：

```env
APP_ENV=release

ZONE_INTERNAL_API_TOKEN=<random-internal-token>

REDIS_USER=<acl-user>

REDIS_PASSWORD=<acl-password>

TICKET_AES256_GCM_V1_SLOT1_KEY_HEX=...

TICKET_AES256_GCM_V1_SLOT2_KEY_HEX=...

TICKET_AES256_GCM_V1_SLOT3_KEY_HEX=...

TICKET_AES256_GCM_V1_SLOT4_KEY_HEX=...
```

### ticket key のルール

* 各 key は、32 バイトの AES-256-GCM key を 64 バイトの hex 文字列にしたものである必要があります。
* 4 つの slot は gateway の設定と完全に一致させてください。gateway の release 環境でランダム key を設定し、zone の dev 環境で組み込み key を使用すると、`/game/ws` の upgrade 段階で `AuthenticationFailed` が発生します。
* `APP_ENV=dev` かつ key が未設定の場合に限り、zone は gateway の dev 用デフォルト key を使用します。
* `APP_ENV` がそれ以外の値の場合、key が1つでも不足している、または一部の key しか設定されていない場合、zone は起動を拒否します。

## gateway /join から zone WS への接続

クライアントは、従来の `wsToken` をロジックサーバーへ送信しなくなりました。新しい接続手順は次のとおりです。

1. クライアントが gateway にログインし、gateway token を取得します。
2. クライアントが gateway の `/join` を呼び出します。body には gateway token と接続先の `zoneId` を含めます。
3. gateway がユーザーを検証し、接続先 zone を決定して Redis に join ticket を書き込み、次のレスポンスを返します。

```json
{
  "code": "ok",
  "host": "127.0.0.1",
  "port": 10001,
  "methodId": 3,
  "ticket": "..."
}
```

4. クライアントはロジックサーバーへ接続します。

```text
ws://<host>:<port>/game/ws?methodId=<methodId>&ticket=<ticket>
```

5. zone が ticket を復号・検証します。検証に成功すると、`C2S_RequestEnterZone` を待ち受けます。
6. zone への入場が成功すると、zone が `passToken` を発行します。
7. 以降の切断・再接続では、次の URL を使用します。

```text
ws://<host>:<port>/game/ws?passToken=<passToken>
```

初回接続用のパラメータと再接続用のパラメータは併用できません。`ticket` と `passToken` が同時に指定された場合、zone は `400 Bad Request` を返します。

### passToken のライフサイクル

* key には `zone:<id>:passtoken:<token>` を使用し、value にも発行元の `zoneId` を保存します。
* token は、システムの暗号学的に安全な乱数 32 バイトを、64 文字の小文字 hex 文字列にエンコードしたものです。
* Redis への書き込みには `SET ... EX ... NX` を使用します。現在の TTL は 5 時間で、衝突した場合は回数を制限して再試行します。
* WS upgrade では token を消費せず、事前検証のみを行います。実際に zone へ入場する際、サーバーは先に新しい `passToken` を書き込み、その後、旧 token に対して原子的に `GETDEL` を実行し、`INCR zone:<id>:online` でオンライン数を増やします。
* 旧 token が別の接続によってすでに消費されていた場合、新しい token は取り消され、今回の再接続は失敗します。
* クライアントが WS upgrade 後、zone への入場前に切断した場合、旧 token は失われません。
* WS close code が `1000/1001` で、セッションがすでに online に計上されている場合、現在のセッションの token を取り消します。`1006` またはその他の異常切断では token を保持し、TTL の満了を待ちます。クライアントは猶予期間内に再接続できます。
* サーバーの正常な停止時には WS close code `1012` を使用します。この場合、online 数は減算しますが、現在の `passToken` は保持します。これにより、サービス復旧後もクライアントが再接続を試みられます。
* `passToken` による再接続時、WS upgrade で行うのは事前検証のみです。`C2S_RequestEnterZone` によって旧 token の消費、新 token の発行、session の登録が正常に完了するまでは、zone はその他の game message を game worker に転送しません。

## join ticket の検証範囲

gateway が暗号化する ticket の平文には、次の情報が含まれます。

* `ticketKey`：Redis の ticket key。
* `uid`：認証済みユーザー ID。
* `zoneId`：gateway が選択したデータベース上の区服 ID。
* `expiresAtMs`：ticket の有効期限。Unix ミリ秒。

zone は次の順序で検証します。

1. `methodId` が `1..4` の範囲内であることを確認します。

2. `ticket` を hex から `nonce || ciphertext || tag` にデコードします。

3. 同じ slot の AES-256-GCM key を使用して復号します。`methodId` は AAD として認証処理に含めます。

4. `expiresAtMs` が期限切れでないことを確認します。

5. ticket 内の `zoneId` が `env.zone` の `ZONE_ID` と一致することを確認します。

6. Redis の ticket 消費と容量状態の遷移を原子的に実行します。

   `GETDEL ticketKey`、`ZREM zone:<id>:reservations ticketKey`、`INCR zone:<id>:online`

7. Redis の value が `uid_len:u8 + uid bytes + zone_id:u8` と一致することを確認します。

いずれかの手順に失敗した場合、WS upgrade は拒否されます。

## 入図と実行時の境界

クライアントが zone に入場した後も、マップへの入場処理は引き続き2段階で行われます。

1. `C2S_RequestEnterMap` で、現在の role／passToken／map を検証し、現在の接続に紐づく `GameSession` に pending map を記録します。
2. `S2C_EnterMapAccepted` は入場申請が承認されたことだけを示し、マップリソースは含みません。
3. クライアントがリソースの読み込みを完了した後、`C2S_EnterMapReady` を送信します。
4. サーバーが pending map を消費し、セッションの現在のマップを更新したうえで、`S2C_JoinMapSnapshot` を送信します。

### 実行時の境界

* `C2S_EnterMapReady` で `role.current_map_id` を暗黙的に使用してはいけません。pending map が存在しない場合は、必ず拒否します。
* 同じ role の新しい接続が確立すると、`role -> conn` の対応関係は新しい接続に引き継がれます。古い接続から後続メッセージが届いた場合は、stale data として拒否します。
* `C2S_AssignShipToArea` で選択できるのは、すでに解放済みの主城エリアのみです。目標エリアは直ちに更新され、状態は `MovingToAnchor` に移行します。ただし、`CAreaMember` の戦闘上有効なエリアが切り替わるのは、anchor に到達してからです。
* `role_units.level/exp` と `role_unit_area_assignments.area_id` は、プレイヤーの船の実行時データに読み込まれます。経験値の書き戻しには dirty threshold を使用し、通常の切断時またはインスタンスのクリーンアップ時に flush します。
* `role_units.exp` は `BIGINT UNSIGNED` を使用し、実行時の `uint64_t` 型の経験値フィールドと一致させています。旧 DB からの起動時マイグレーションでは、`exp` を `INT UNSIGNED` から64ビットに拡張します。
* `S2C_JoinMapSnapshot` は現在の `MapInstance` と `TemplateCache` から組み立てます。マップに入るたびにテンプレート DB を再検索することはありません。
* AOI の初回入図では、まず `S2C_JoinMapSnapshot` で静的キャッシュを送信し、その後 cache-only の `S2C_UpdateAoiSnapshot` を送信します。このとき `entered_ids` が空でなくても、`entered_units` が空の場合があります。実行中に新しいエンティティが視界に入った場合は、`entered_units` と `entered_ids` が1対1で対応します。
* 戦闘イベントは AOI の送信周期に合わせて蓄積されます。AOI が sim tick ごとに送信されるわけではないため、1つのバッチに複数 tick 分のイベントが含まれる場合があります。送信前に、現在の viewer から見えるエンティティに基づいてフィルタリングし、視界外の戦闘イベントがクライアントへ漏れないようにします。

## TemplateCache の設定範囲

現在の `schema/game.fbs` では、`map_id`／`current_map_id` は `uint8` です。そのため、`map_templates.map_id`、`areas.map_id`、`area_spawn_points.map_id` は `0..255` の範囲内でなければなりません。運用上、通常のマップ ID は引き続き `1` から始めることを推奨します。

DB に `map_id > 255` が存在する場合、TemplateCache は起動を失敗させます。これにより、プロトコルで表現できない ID が暗黙的に切り詰められることを防ぎます。

`monster_spawns_point.spawn_point_id` は、読み込み済みの `area_spawn_points.id` を参照している必要があります。参照先が存在しない場合は設定エラーです。`loadAll` は `false` を返し、そのスポーンルールを黙ってスキップすることはありません。

現在、主城の業務エリア ID は `1..4` で、`0` は無効なエリアとして予約されています。`areas.area_id` または `area_spawn_points.area_id` がこの範囲を外れる場合、TemplateCache は起動を失敗させます。

## 設定のホットリロード

ゲームデザイナーがテーブルを変更した場合、zone を再起動する必要はありません。

```sh
curl -X POST http://127.0.0.1:10001/api/reload-config \
  -H "X-Zone-Internal-Token: <ZONE_INTERNAL_API_TOKEN>"
```

リロード対象は、次の3つのキャッシュです。

* `TemplateCache`：ユニット／マップ／エリア／スポーン設定
* `ItemCache`：アイテム／ショップ設定
* `BuildingCache`：建築物設定

実装の要点は `zone/include/config/config_reload.h` にあります。

* **構築完了後に切り替える**：新しい設定は HTTP スレッド上ですべて読み込まれ、全検証に合格した後で初めて game worker に渡されます。検証に失敗した場合は破棄し、**旧設定でのサービスを継続**します。API は `409` を返し、設定が中途半端に切り替わったり、不正な設定に置き換わったりすることはありません。
* **tick の境界でのみ切り替える**：game worker はフレーム開始時、各システムの実行前に設定全体を切り替えます。そのため、1フレーム内のすべてのシステムが同じスナップショットを参照します。
* **3つのキャッシュを一括で置き換える**：アイテムテーブルだけが新しく、ショップテーブルは古いままといった、テーブル間の不整合を防ぎます。

遡って変更されないものは、すでにマップ上に生成されているエンティティです。モンスターの属性は生成時にコピーされるため、リロードの影響を受けるのは、その後に新しく生成されるエンティティと、テーブルを参照して処理するロジック（ショップ／招募／建築／ドロップ）です。

設定の読み込みは DB にアクセスし、処理をブロックするため、**HTTP スレッドからのみ実行してください**。game worker 上から呼び出してはいけません。

## nav グリッドのレイヤー構成

nav データは2つのレイヤーに分かれています。それぞれの役割を混同しないでください。

| レイヤー   | データ元                                                  | 内容                                                             |
| ------ | ----------------------------------------------------- | -------------------------------------------------------------- |
| 地形レイヤー | `zone/nav/*.json` → `generated/main_city_nav_32x32.h` | **地形のみ**。現在、主城では1504マスが通行不可です。                                 |
| 都市レイヤー | 実行時の `CityNavGrid::build()`                           | 地形に加え、DB の `role_buildings` に基づいて建築物の輪郭を重ねます。現在は1591マスが通行不可です。 |

**建築物を地形レイヤーに含めてはいけません。**

初期の nav JSON は、「治療ステーションが唯一のハードコードされた建築物」だった時期に手作業で作成され、病院の輪郭が地形データに焼き込まれていました。そのため、将来この建築物を移動すると、元の場所に障害物が残り続ける問題がありました。

該当する9マスはすでに通行可能な状態に戻しています。現在の `H` は位置を示すマーカーにすぎず、衝突判定を意味しません。

経路探索でグリッドを取得する場合は、必ず `game::navGridOf(world)` を使用してください。`mainCityNavGrid()` を直接呼び出してはいけません。こちらは建築物を含まず、将来プレイヤーが自由に配置を変更できるようになった際に、誤ったグリッドを使用することになります。

建築物の重ね合わせには矩形ではなく円を使用します（DB の `buildings.default_collider_radius` は半径を意味します）。マスの中心が円の内側に入っていれば、そのマス全体を通行不可とします。建築物の縁にユニットが入り込むことを防ぐため、多少大きめに判定します。

プレイヤーによる建築物の配置検証は `zone/include/game/building_placement.h` にあります。検証順序は次のとおりです。

テンプレートの存在確認 → グリッド内か確認 → 許可エリア内か確認（現在、主城はエリア1）→ 地形と重ならないことを確認 → 他の建築物と重ならないことを確認 → **マップを分断しないことを確認**

最後の検証が最も重要で、かつ最も処理コストが高いため、最後に実行します。通路を塞ぐと精霊がその場から永久に動けなくなる可能性があります。オンライン環境では通常の AI バグと区別しにくいため、配置時点で拒否する必要があります。

なお、建築物の配置プロトコル自体はまだ実装されていません。

## 経済・育成システム

| システム      | エントリーポイント                                   | 説明                                                 |
| --------- | ------------------------------------------- | -------------------------------------------------- |
| ショップ購入／売却 | `game/shop_handler.cpp`                     | 価格、在庫、所持金、スタック上限をすべてサーバー側で再計算します。                  |
| 装備の着脱     | `game/equip_handler.cpp`                    | 能力値の加算処理は `equip_stats.h` に集約されています。               |
| 建築物の解放    | `game/role_buildings.h`                     | 解放コストと状態を `BuildingCache` で検証します。                  |
| ドロップと拾得   | `ecs/item_drop_system.cpp`                  | ドロップアイテムはマップ上のエンティティで、拾得にはインタラクション可能な距離にいる必要があります。 |
| Boss 挑戦   | `ecs/boss_challenge_system.cpp`             | 挑戦時にのみ生成され、同時に挑戦できるのは1体だけです。                       |
| チーム編成     | `game/player_team_runtime.cpp`、`team_ops.h` | 編成への追加／解除とエリアへの割り当てを扱います。                          |

**クライアントから送られてくる価格、数量、能力値は一切信用しません。**

サーバーは、それらを「プレイヤーが何を購入したいか」という意思表示としてのみ扱います。金額と処理結果は、すべて現在の設定に基づいてサーバー側で再計算します。

ショップ購入の検証順序は固定です。

数量 → 商品の存在 → アイテム設定の存在 → 在庫 → 合計金額 → 所持金 → スタック上限

売却の場合は次の順序です。

数量 → アイテム設定の存在 → 販売価格が設定されていること → 所持数量 → 所持金の上限

この順序を固定するのは、失敗理由を安定してテストできるようにするためです。安易に変更しないでください。

所持金などの経済データは、`db/role_economy_db_writer.cpp` の**絶対値方式**の非同期永続化キューを通じて保存します。

書き込むのは増減量ではなく、精算後の最終値です。そのため、キュー内で処理順序が変わったり、再試行が発生したりしても、残高が誤って計算されることはありません。

## 実行時メモリの境界

* `ENTT_SPARSE_PAGE=64`／`ENTT_PACKED_PAGE=64`（`zone/CMakeLists.txt` を参照）。

  EnTT のデフォルトページサイズは、1つの registry に数万エンティティを格納するケースを想定して調整されています。一方、ここでは**都市ごとに1つの registry** を持ち、各都市のエンティティ数は少数です。デフォルト値のままでは、プレイヤーごとに数百 KB の sparse page が無駄に消費されます。
* 各都市は独自の `entt::registry` と `networkEntityIdAlloc` を持ち、AOI は都市単位で厳密にフィルタリングされます。
* 設定テーブルはサーバー全体で共有する読み取り専用キャッシュであり、プレイヤー数に応じて増加しません。プレイヤー固有のデータのみ、オンラインプレイヤーに応じて読み込みます。

## よく使うコマンド

### zone のみをビルドする

```sh
cmake --build build --target zone
```

`zone/nav/*.json` を変更した場合、ヘッダーファイルを手動で生成する必要はありません。`zone_generate_nav` ターゲットは JSON と生成スクリプトを依存関係としているため、通常のビルドで自動的に再実行されます。

生成結果だけを個別に確認したい場合に限り、次のコマンドを手動で実行してください。

```sh
python3 zone/tools/nav_json_to_header.py zone/nav/main_city_nav_32x32.json zone/include/generated/main_city_nav_32x32.h
```

現在、テストターゲットは合計28個あります。確認したい領域に応じて、次のように実行してください。

### 接続・認証・DB 契約関連

```sh
ctest --test-dir build -R "join_ticket|pass_token|redis_auth|main_startup|db_schema_contract|session_contract" --output-on-failure
```

### nav・建築配置・移動関連

```sh
ctest --test-dir build -R "nav|city_nav_grid|building_placement|team_switch_movement" --output-on-failure
```

### 経済・装備・アイテム・チーム関連

```sh
ctest --test-dir build -R "shop_handler|equip_handler|equip_stats|item_drop|boss_challenge|team_ops" --output-on-failure
```

`zone_db_schema_contract_test` は、この README を読み込み、いくつかの規約文字列（`REDIS_USER`、`ZONE_INTERNAL_API_TOKEN`、`1..255` など）を検証します。

これらの説明を削除するとテストが失敗します。これは意図的な仕様であり、ドキュメントとプロトコル上の制約が知らないうちに乖離することを防ぐためです。

### 全体検証

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

## gateway／zone の live probe

### 手動で接続を検証する

```sh
cmake --build build --target zone_live_probe_codec

python3 tools/zone_gateway_live_probe.py --mode join --join-zone-id 1
```

`join` モードでは、次の一連の処理を検証します。

`/healthz -> /login -> /zones -> /join -> WS EnterZone`

成功すると、今回 zone が発行した `passToken` を出力します。ただし、いずれのプロセスも停止しません。

### 停止時の close code `1012` を検証する

実行前に、対象となる zone プロセスだけが選択されていることを確認してください。

```sh
ZONE_PID="$(pgrep -n -x zone)"

python3 tools/zone_gateway_live_probe.py --mode join-shutdown --join-zone-id 1 --zone-pid "$ZONE_PID"
```

`join-shutdown` は、まず次の一連の処理を実行します。

`/healthz -> /login -> /zones -> /join -> WS EnterZone`

その後、`--zone-pid` で指定されたプロセスに `SIGTERM` を送り、WebSocket の close code が `1012` であることを検証します。

スクリプトは zone を再起動しません。現在の `passToken` と、次に実行する reconnect コマンドを出力します。

zone を手動で再起動した後、次を実行してください。

```sh
python3 tools/zone_gateway_live_probe.py --mode reconnect --pass-token "<前の手順で出力された passToken>" --zone-host 127.0.0.1 --zone-port 10001
```

### よく使うオプション

* `--gateway-host`／`--gateway-port`：gateway の HTTP JSON エンドポイント。デフォルトは `127.0.0.1:8080` です。
* `--join-zone-id`：gateway の `/join` に渡すデータベース上の区服 ID。接続先 zone の `ZONE_ID` と一致している必要があります。
* `--zone-host-override`：gateway の `/join` が `0.0.0.0`、コンテナの待ち受けアドレス、またはパブリック IP を返す一方、probe ではローカル／内部ネットワークのアドレスに接続したい場合に使用します。例：`--zone-host-override 127.0.0.1`
* `--codec`：FlatBuffers codec helper のパス。デフォルトは `build/zone/zone_live_probe_codec` です。

live probe は、実際に稼働している gateway、zone、MySQL、Redis、および一致する ticket key に依存します。

デフォルトの `ctest` で実行されるのは、スクリプトのオフライン単体テストと FlatBuffers codec の自己検証のみです。実際のサービスへの接続や停止は行いません。
