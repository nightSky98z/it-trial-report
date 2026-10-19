# it-trial-report

最近1年で、自分で調べて試した IT 関連のことをまとめたレポートです。どのレポートも「きっかけ」「どこまで試したか」「効果（作業が楽になった・時短になった点）」の順に、初心者にも分かるように書いています。

## レポート一覧

| レポート | 内容 |
| --- | --- |
| [自作の個人ブログ「nightSky98z」](it-trial-report-web-blog.md) | フレームワークを使わずに、HTML / CSS / JavaScript だけで個人ブログをゼロから作りました。ブラウザ上で記事を書けるエディタも自作し、JSON を手で書き換える手間がなくなりました。 |
| [データ指向（Unity ECS）でゲームを作る「ECS-Sample」](it-trial-report-ecs.md) | 「データ指向」の考え方を、冷蔵庫から食材を取ってくる例で説明しました。Unity ECS を使い、大量のモンスターが同時に動くゲームを作りました。 |
| [C++ でオンラインゲームのサーバーを作る「MyGameServer」](it-trial-report-game-server.md) | ログイン・戦闘・ショップ・データ保存まで、ゲームサーバーの仕組みを C++ で作りました。設定を再起動なしで反映できるようにし、接続の確認も自動化しました。 |
| [AI エージェントで暮らすキャラクターを動かす「Generative Agents」](it-trial-report-generative-agents.md) | AI エージェントをゲームのキャラクター AI に応用するために、論文の仕組みを再現したプロジェクトの日本語会話版を開発しました。プロンプトも日本語に書き直しました。 |

## コードサンプル

- [my-server-code-sample](my-server-code-sample/)：ゲームサーバー（MyGameServer）のソースコード
- [generative-agents-code-sample](generative-agents-code-sample/)：Generative Agents 日本語会話版のソースコード
