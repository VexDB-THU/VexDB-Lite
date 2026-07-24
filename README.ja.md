# VexDB

**[English](README.md)** | **[中文](README.zh.md)** | **[日本語](README.ja.md)**

`VexDB` には現在、同じコアグラフアルゴリズムと距離計算スタックを共有する 2 つのベクトルインデックス統合が含まれています。

- `vexdb_pg`：PostgreSQL 拡張機能 `vexdb_vector`
- `vexdb_duckdb`：DuckDB 拡張機能 `vex`

共有コアのディレクトリ：

- `include/graph_index/`：グラフインデックスのヘッダーと共有 HNSW ロジック
- `distance/`、`src/distance/`：距離関数、ISA ディスパッチ、変換テンプレート
- `vtl/`：共有テンプレート／コンテナ層
- `vexdb_duckdb/`：DuckDB 統合層
- `src/`、`include/`、`sql/`：PostgreSQL 統合層

---

## 1. コンポーネント

### 1.1 PostgreSQL：`vexdb_vector`

現在の機能：

- `floatvector(N)` 型
- 距離演算子／関数：
  - L2：`<->`
  - 内積：`<#>`
  - コサイン：`<=>`
- `CREATE INDEX ... USING vexdb_graph`
- `m`、`ef_construction`、`parallel_workers` などの HNSW オプション
- `vexdb_vector.ef_search`、`vexdb_vector.vec_architecture` などの実行時設定
- オプティマイザー／エグゼキューターによる ANN インデックススキャン
- 共有メモリのベクトルバッファマネージャーと並列ビルド対応

### 1.2 DuckDB：`vexdb_duckdb`

現在の機能：

- `GRAPH_INDEX` を `FLOAT[N]` ベクトル列に適用
- ベクトル距離関数／演算子：
  - `l2_distance`、`<->`
  - `inner_product`、`<#>`
  - `cosine_distance`、`<=>`、`<~>`
- `vector_dims()`、`l2_normalize()`、`vex_version()`、`vex_index_info()`
- `CREATE INDEX ... USING GRAPH_INDEX (vec [, metadata...])`
- `VEX_INDEX_SCAN` へのオプティマイザー書き換え
- メタデータ列を使ったフィルター付きベクトルインデックス構文

DuckDB の実行時設定：

- `vex_ef_search`
- `vex_brute_force_threshold`

---

## 2. 機能マトリックス

| カテゴリ | 機能 | 説明 | vexdb_vector（オープンソース） | VexDB（商用版） |
|---|---|---|:---:|:---:|
| ベクトルインデックス | HNSW グラフインデックス | 独自開発の高性能汎用グラフインデックス | ✅ | ✅ |
| 距離 | 距離関数ディスパッチ | インライン距離関数、コンパイル時最適化 | ✅ | ✅ |
| キャッシュ | ベクトルバッファ | あらゆる用途に対応する汎用ベクトルキャッシュ | ✅ | ✅ |
| キャッシュ | 一括バッファ | 最大スループットのための完全インメモリキャッシュ | ❌ | ✅ |
| キャッシュ | 非同期 I/O キャッシュ | メモリ逼迫時のディスクからキャッシュへの読み込みを高速化 | ❌ | ✅ |
| データ型 | floatvector | 標準 float32 ベクトル型 | ✅ | ✅ |
| データ型 | halfvector | Float16 ベクトル型 | 🟡 | ✅ |
| データ型 | int8vector | Int8 ベクトル型 | 🟡 | ✅ |
| 量子化 | PQ 量子化 | 最大限に圧縮しつつ、生ベクトルに近い QPS | ✅ | ✅ |
| 量子化 | RaBitQ 量子化 | 中程度の圧縮、生ベクトルより高い QPS | 🟡 | ✅ |
| 量子化 | 自動量子化 | バックグラウンドで自動有効化、空テーブルでのインデックス構築に対応 | ❌ | ✅ |
| インデックス | 非同期挿入 | 書き込み負荷の高いワークロード向けの高速取り込み | ❌ | ✅ |
| インデックス | グラフシャーディング | メモリの少ないマシンで大規模ベクトルを処理 | ❌ | ✅ |
| HA | プライマリ／レプリカ HA | 同期レプリケーションとバックアップ復元 | ❌ | ✅ |
| メンテナンス | 並列 vacuum | インデックスの並列クリーンアップと領域回収 | ❌ | ✅ |

✅ 対応済み · 🟡 近日対応予定 · ❌ オープンソース版には含まれません

---

## 3. PostgreSQL 構文例

### 2.1 インストールとテーブル作成

```sql
CREATE EXTENSION vexdb_vector;

CREATE TABLE items (
    id  BIGSERIAL PRIMARY KEY,
    vec floatvector(128)
);

INSERT INTO items (vec) VALUES
    ('[0.10, 0.20, 0.30]'),
    ('[0.40, 0.50, 0.60]');
```

### 2.2 インデックスの構築

```sql
CREATE INDEX idx_items_vec
ON items
USING vexdb_graph (vec floatvector_l2_ops)
WITH (
    m = 16,
    ef_construction = 64
);
```

### 2.3 ANN クエリ

```sql
SET vexdb_vector.ef_search = 100;
SET enable_seqscan = off;

SELECT id, vec <-> '[0.15, 0.25, 0.35]' AS dist
FROM items
ORDER BY vec <-> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

### 2.4 その他の距離指標

```sql
SELECT id
FROM items
ORDER BY vec <#> '[0.15, 0.25, 0.35]'
LIMIT 10;

SELECT id
FROM items
ORDER BY vec <=> '[0.15, 0.25, 0.35]'
LIMIT 10;
```

---

## 3. DuckDB 構文例

### 3.1 拡張機能の読み込み

```sql
LOAD '/path/to/vex.duckdb_extension';
SELECT vex_version();
```

一般的な Python での使用方法：

```python
import duckdb

con = duckdb.connect(config={"allow_unsigned_extensions": "true"})
con.execute("LOAD '/path/to/vex.duckdb_extension'")
```

### 3.2 テーブルとインデックスの作成

```sql
CREATE TABLE items (
    id       INTEGER,
    category VARCHAR,
    vec      FLOAT[128]
);

CREATE INDEX idx_items_vec
ON items
USING GRAPH_INDEX (vec)
WITH (
    metric = 'l2',
    m = 16,
    ef_construction = 64
);
```

### 3.3 ANN クエリ

```sql
SET vexdb_vector.ef_search = 100;

SELECT id
FROM items
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 3.4 フィルター付きインデックスの例

```sql
CREATE INDEX idx_items_vec_meta
ON items
USING GRAPH_INDEX (vec, category);

SELECT id
FROM items
WHERE category = 'book'
ORDER BY l2_distance(vec, [0.15, 0.25, 0.35]::FLOAT[3])
LIMIT 10;
```

### 3.5 その他の関数

```sql
SELECT inner_product([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT cosine_distance([1.0, 0.0]::FLOAT[2], [0.5, 0.5]::FLOAT[2]);
SELECT vector_dims([1.0, 2.0, 3.0]::FLOAT[3]);
SELECT l2_normalize([3.0, 4.0]::FLOAT[2]);
SELECT * FROM vex_index_info();
```

---

## 4. ビルド

## 4.1 PostgreSQL 版のビルド

### 依存関係

- PostgreSQL 16 ～ 19（PG 16/17/18/19 に対応。主な検証対象は `19devel`）
- CMake
- C++17 コンパイラ
- Boost ヘッダー

### PostgreSQL のビルド（リリース例）

```bash
cd /path/to/postgresql-19-source
./configure \
  --prefix=/opt/postgresql-19rel-install \
  --without-icu \
  --without-readline \
  --without-zlib \
  CFLAGS="-O3 -DNDEBUG"
make -j$(nproc)
make install
```

### `vexdb_vector` のビルド

```bash
cd /path/to/VexDB
mkdir -p build-pg19rel-release
cd build-pg19rel-release

export PG_CONFIG=/opt/postgresql-19rel-install/bin/pg_config
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
make install
```

### PostgreSQL の設定

最低限、以下を設定します。

```conf
shared_preload_libraries = 'vexdb_vector'
```

その後 PostgreSQL を再起動し、次を実行します。

```sql
CREATE EXTENSION vexdb_vector;
```

---

## 4.2 DuckDB 版のビルド

**推奨：`build_duck.sh` を使用してください。** DuckDB のクローン、CMake の設定、コンパイル、メタデータ処理を 1 つのコマンドで実行します。

```bash
bash build_duck.sh setup   # First time: clone DuckDB v1.5.2 and cmake configure
bash build_duck.sh build   # Compile the extension (incremental)
```

出力：`build/duck/build/extension/vex/vex.duckdb_extension`

### 依存関係

- CMake 3.14+
- C++17 コンパイラ（GCC 9+ または Clang 10+）
- Git

### 単純な CMake ではなくシェルスクリプトを使う理由

DuckDB 拡張機能は DuckDB のソースツリー内でコンパイルする必要があり、`cmake -B build vexdb_duckdb/` を単独で実行することはできません。`build_duck.sh` は次の処理を自動化します。
1. DuckDB v1.5.2 のクローン
2. vex 拡張機能を登録する `extension_config_local.cmake` の書き込み
3. `cmake` + `cmake --build` の実行
4. 拡張機能のメタデータフッターの追加（DuckDB のリリース形式で必須）

---

## テストの実行

### DuckDB 拡張機能のテスト

```bash
bash build_duck.sh build          # Build the extension
bash tests/spec/_lib/docker/run_duckdb.sh test  # Run full spec tests (requires Docker)
```

### PostgreSQL プラグインのテスト

```bash
bash tests/spec/_lib/docker/run_pg.sh test      # Run PG spec tests (requires Docker + PG19)
```

テストは YAML spec DSL で駆動されます。テストファイルは `tests/spec/` にあります。

---

## 5. ベンチマーク結果

データセット：SIFT-1M 128 次元、`m=16`、`ef_construction=128`。各列：`QPS (reads=1)` / `QPS (reads=16)` / `Recall@10`。

テスト環境：Intel Core Ultra 7-265K（20c/20t、3.9 GHz）/ 16 GB DDR5 / x86_64 Linux

### 5.1 pgvector / VSS との比較（x86_64）

**ef_search = 50**

| システム | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 507.9 | 7153.5 | 96.22% |
| **vexdb_vector (PostgreSQL)** | **994.7** | **12084.6** | 95.97% |
| **vexdb_vector (DuckDB)** | **717.5** | **8667.8** | 95.06% |
| duckdb-vss | 496.1 | 5360.9 | 94.07% |

**ef_search = 100**

| システム | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 313.4 | 4272.5 | 98.82% |
| **vexdb_vector (PostgreSQL)** | **618.5** | **7883.1** | 98.62% |
| **vexdb_vector (DuckDB)** | **547.2** | **5379.1** | 98.40% |
| duckdb-vss | 405.2 | 4433.3 | 98.04% |

**ef_search = 200**

| システム | QPS (r=1) | QPS (r=16) | Recall@10 |
|---|---:|---:|---:|
| pgvector | 193.1 | 2694.1 | 99.66% |
| **vexdb_vector (PostgreSQL)** | **421.3** | **5038.0** | 99.58% |
| **vexdb_vector (DuckDB)** | **383.6** | **4298.8** | 99.53% |
| duckdb-vss | 321.9 | 3809.3 | 99.42% |

---

## 6. 既知の制限事項

全一覧については [docs/known-limitations/](docs/known-limitations/) を参照してください。

### PostgreSQL

- PostgreSQL 16 ～ 19 に対応。主な検証対象は PostgreSQL 19
- ARM PG の SIMD はまだ完全には再統合されておらず、現状では正しさとビルド可能性を優先
- WAL／quantizer 関連は完全なロードマップと比べて未完成

### DuckDB

- 現在は `GRAPH_INDEX`、オプティマイザー統合、共有アルゴリズムの整合性に注力
- `threads` や `pq_m` など、一部の処理経路では互換性のためだけに受け付けるオプションがあります
- ARM の DuckDB ビルドも現在は `GENERAL` 距離ディスパッチに依存

---

## 7. 次に参照する場所

- PostgreSQL 実装：`src/`、`include/`、`sql/`
- DuckDB 実装：[vexdb_duckdb/README.md](vexdb_duckdb/README.md) と `vexdb_duckdb/`

---

## コミュニティ

| チャンネル | 説明 |
|---|---|
| [GitHub Issues](https://github.com/VexDB-THU/VexDB-Lite/issues) | バグ報告と機能リクエスト |
| [GitHub Discussions](https://github.com/VexDB-THU/VexDB-Lite/discussions) | 質問、提案、一般的なディスカッション |
| [Discord](https://discord.gg/vexdb) | リアルタイムチャットと Q&A |
| WeChat グループ | [vexdb.com/community](https://vexdb.com/community) の QR コードをスキャン · 中国語コミュニティ |

---

## ライセンス

MIT ライセンス。[LICENSE](LICENSE) を参照してください。
