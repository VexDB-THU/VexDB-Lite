# VexFS：数据库原生文件管理扩展设计

- 日期：2026-07-16
- 分支：`feature/agent_files`
- 文档版本：1.7
- 状态：macOS FSKit 与 Linux libfuse3 已复用同一 runtime；PostgreSQL `0.4.0-alpha.1`
  已完成数据库合同、format v2、role/ACL/审计、libpq HostStore、macOS/Linux 真实 mount、
  跨机器 OpenCode 和逻辑/物理备份；第二台 Mac 的局域网直连只差一次系统授权确认
- 产品名：暂定 `VexFS`
- 产品规格：`docs/specs/2026-07-16_vexfs-product-spec.md`
- 最终目标和阶段顺序：`docs/plans/2026-07-21_vexfs-final-goal-and-roadmap.md`

## 1. 最终定义

> 2026-07-24 默认入口变更：macOS 0.1 使用系统 NFS client + 本机用户态 gateway；FSKit
> 保留为后续可选原生 adapter。已有 FSKit 章节记录已完成实现和历史证据，不再表示默认发布路径。

同日真机传输原型已确认 macOS 26.3.1 arm64 可以由当前用户挂载只监听 localhost 的
NFSv3 服务，无需 FSKit extension；基础 Bash、mode、symlink 和 Git commit 均通过。原型同时
确认 NFS adapter 不能直接照搬开源 demo：macOS `com.apple.provenance` 会产生 `._*`
AppleDouble sidecar，优化镜像原型也只有约 75 个用户小文件/秒。正式 adapter 必须在
Workspace Engine 边界内吸收 AppleDouble/xattr，并用批量写回和 metadata cache 达到性能 Gate。
这两个问题属于 mount adapter，不允许修改数据库文件、版本、权限或备份的权威合同。

VexFS 是由 PostgreSQL、DuckDB 或 SQLite 加载和管理的数据库扩展。三个宿主共享逻辑合同，但首个闭环固定为 macOS + SQLite，不要求第一版同时完成其他数据库和操作系统。

它给数据库增加一套分层文件管理能力：

- 工作区；
- 文件和目录；
- 路径；
- 文件内容；
- 文件属性；
- 版本；
- 快照和恢复；
- 权限；
- 审计；
- 完整性检查；
- 工作区逻辑导入导出。

VexFS 的权威核心是数据库扩展。首个产品默认提供只监听 loopback 的 macOS NFS gateway，
把 SQLite 中的工作区显示为真实操作系统目录。NFS gateway 是非权威 mount adapter：它调用
公开 VexFS SQL/handle 合同，不保存最终状态，不绕过数据库事务，也不管理 SQLite 的 WAL、锁
或物理文件。FSKit 后续通过同一个 Workspace Engine 接入，不能复制文件管理逻辑。

用户通过数据库连接和 SQL 使用它：

```sql
SELECT vexfs_workspace_create('agent-workspace');
SELECT vexfs_mkdir('agent-workspace', '/reports', true);
SELECT vexfs_write('agent-workspace', '/reports/result.md', '# Result');
SELECT vexfs_read('agent-workspace', '/reports/result.md');
SELECT * FROM vexfs_list('agent-workspace', '/reports');
```

安装和配置后，用户优先直接进入挂载目录：

```bash
vexfs setup --engine sqlite \
  --workspace agent-workspace \
  --mount ~/VexFS/agent-workspace

cd ~/VexFS/agent-workspace
ls reports
rg 'Result' reports
python scripts/generate.py
```

不能挂载时，也可以用配套 CLI 执行同样的操作：

```bash
vexfs --workspace agent-workspace ls /reports
vexfs --workspace agent-workspace cat /reports/result.md
vexfs --workspace agent-workspace grep -R "Result" /reports
```

一句话定位：

> **VexFS 给 PostgreSQL、DuckDB 和 SQLite 增加数据库原生的文件管理系统。**

默认 NFS 路径不依赖 FSKit V2 的 `FSPathURLResource`，因此新的 macOS 最低版本和 CPU 支持范围
由 NFS 包真机验证决定，不再先写死为 macOS 26.0+ Apple Silicon。现有 FSKit 垂直闭环继续作为
可选 adapter 证据保留。Linux libfuse3 已接入同一合同，Windows 后续使用 WinFsp。

### 1.1 适配工作量必须按 N+M 拆分

禁止按“3 个数据库 × 3 个操作系统”开发 9 套 VexFS。数据库适配和操作系统挂载适配必须相互独立：

```text
SQLite adapter ----+
DuckDB adapter ----+--> VexFS 统一合同和 mount runtime
PostgreSQL adapter-+                 |
                                     +--> macOS NFS（默认）
                                     +--> macOS FSKit（后续可选）
                                     +--> Linux libfuse3
                                     +--> Windows WinFsp
                                     +--> 跨平台 CLI
```

数据库 adapter 只处理宿主差异：事务、锁、BLOB、内部表、SQL 注册、权限身份和备份。mount adapter 只处理操作系统差异：inode/vnode 回调、错误码、缓存失效、原生权限和挂载生命周期。路径、文件规则、版本、快照、冲突、硬链接、ACL 和 handle 语义只能定义一次。

因此完整目标是一个共享合同、三个数据库 adapter、三个 mount adapter 和一个 CLI，不是九个数据库与操作系统组合。新增第四个数据库或第四个操作系统时，只新增一侧 adapter。

数据库仍然是管理者和权威状态。统一 runtime 只是非权威翻译层，不保存最终文件状态，也不代替数据库事务、权限、备份和恢复。

交付顺序以路线图为准，当前固定为：

1. 收口 SQLite + macOS/Linux 本地开发者预览；
2. 实现 PostgreSQL adapter，提供 macOS/Linux 跨电脑、多用户和共享 workspace；
3. 实现 Windows WinFsp，并验证三种操作系统上的 SQLite 本地和 PostgreSQL 远程工作区；
4. 实现 DuckDB adapter，仅用于本地分析和单写者场景，不把 DuckDB 当多用户文件服务器；
5. 完成统一合同、发布、治理和规模验收，进入 v1。

截至 2026-07-21，第一轮平台拆分和 Linux libfuse3 预览已经落地：

- `agent_files/cli/vexfs_main.cpp` 只保留跨平台文件命令和公共 doctor 输出；
- `agent_files/cli/vexfs_platform.h` 是 CLI 唯一操作系统边界；
- `agent_files/cli/vexfs_platform_macos.cpp` 独占 FSKit、系统 mount 表和 macOS 版本检测；
- `agent_files/cli/vexfs_platform_linux.cpp` 检查 helper、`fusermount3`、`/dev/fuse` 和
  `/proc/self/mountinfo`，负责启动、检测和卸载 Linux mount；
- `agent_files/mount/linux/vexfs_fuse.cpp` 用 FUSE3 high-level API 把 POSIX 回调翻译到
  同一 C ABI；最终文件、版本和 handle staging 仍只存入 SQLite；
- `agent_files/cli/vexfs_platform_stub.cpp` 只继续承担 Windows/WinFsp 未实现边界和跨平台编译 smoke；
- CMake 在 Linux 找到 fuse3 开发包时生成 `vexfs-fuse`，并让统一 `vexdb`/`vexfs`
  与 helper 一起构建；缺少 fuse3 时 CLI 仍能构建，但 doctor 明确报告 helper 不可用；
- Alpine Linux GCC 15 已完成全量 SQLite + CLI + helper 构建；helper C ABI 自测通过；
  真实 `/dev/fuse` 下以 root 和 uid 1000 普通用户两次执行 Bash/Git Eval 均通过。
  覆盖脚本执行、mode、uid/gid、hardlink、symlink、`git init/add/commit/status`、
  卸载和重挂载；普通用户一轮为 1 passed、13 checks；
- Linux x86_64 与 AArch64 发行物已改为 manylinux_2_28 原生构建，并静态链接
  libstdc++/libgcc；三个产物均通过 `GLIBCXX_3.4.22` 上限、ELF、RPATH 和 libfuse3 检查；
- 两个架构的压缩包均包含普通用户安装/卸载脚本、中文说明、版本清单和哈希，并已在对应
  架构真实 Linux 机器的全新 HOME 中完成安装、SQLite 扩展加载、VexFS 文件读写和卸载回归；
- FUSE request id 使用每个 mount 会话的随机前缀和递增序号，避免重挂载后与数据库
  中已记录的幂等请求冲突；
- MinGW x86_64 已编译 Windows 的公共 CLI 和平台 stub，但 WinFsp mount 尚未实现。

因此当前 Linux 是可真实挂载的预览实现，Windows 仍只是平台边界。Linux doctor 只有在
helper、`fusermount3` 和可读写 `/dev/fuse` 都存在时才返回 `mount_ready=true`；不能把
缺少运行条件的环境报告为可挂载。

## 2. 所有权关系

数据库是宿主、管理者和权威状态所有者。

```text
PostgreSQL / DuckDB / SQLite
  ├── 加载、升级和卸载 VexFS
  ├── 管理扩展内存和执行线程
  ├── 管理事务、锁和并发
  ├── 管理 WAL、checkpoint 和崩溃恢复
  ├── 管理权限、配额和资源限制
  ├── 管理数据库备份、复制和恢复
  └── 保存 VexFS 内部表、文件块和审计记录
                    |
                    v
            VexFS 受管扩展
              ├── 注册 SQL API
              ├── 执行文件规则
              └── 调用宿主数据库能力
                    ^
                    |
       公开 VexFS SQL / handle 合同
                    |
      +-------------+-------------+
      |                           |
 macOS FSKit mount adapter      CLI/worktree
 真实 Bash 和任意程序          受限环境回退
```

VexFS 禁止：

- 直接读写 PostgreSQL、DuckDB 或 SQLite 数据库文件；
- 自己实现 WAL、buffer pool、锁或崩溃恢复；
- 私自修改数据库全局配置；
- 启动宿主不允许的后台线程；
- 使用不受宿主管理的长期内存；
- 接管数据库备份、复制、恢复、启动或停止；
- 把文件最终状态放到数据库之外；
- 让 mount gateway 自己决定身份、权限、版本或提交结果。

## 3. 产品边界

### VexFS 是什么

- 数据库 extension；
- 数据库内部的分层 BLOB 文件管理器；
- 可以参加用户数据库事务的文件操作系统；
- 三个数据库共享同一份逻辑合同；
- 通过 SQL 查询和修改；
- 通过非权威 mount gateway 向操作系统提供普通文件接口。

### VexFS 不是什么

- 不是 ext4/APFS/NTFS 的替代品；
- mount gateway 不是独立权威文件服务器；
- 核心扩展不是数据库客户端；
- 不是数据库管理工具；
- 不是把任意业务表自动显示成文件；
- 不做内容理解、Embedding 或语义检索；
- 不提供设备文件、socket 或 FIFO。

### 模型终端兼容层

目标不是只模仿几个命令，而是让模型完成日常文件工作：浏览、读取、查找、批量修改、运行脚本、打包、生成代码和提交结果。

终端兼容层分为三种模式，按产品优先级排列。

#### 模式一：真实挂载目录（P0）

macOS 用户安装并启用 VexFS FSKit File System Extension 后，通过 `vexfs mount` 请求系统挂载。系统程序调用 `open/read/write/rename/fsync` 时，FSKit 把操作交给 adapter，adapter 再转换为公开 VexFS 合同，由 SQLite 扩展完成权限、版本和提交。

```text
Bash / Git / Python / 编译器
          |
       macOS VFS
          |
       Apple FSKit
          |
 VexFS FileSystem.appex
          |
 SQLite connection + principal
          |
  VexFS SQLite 扩展和内部表
```

一个 mount 只代表一个数据库 principal。MVP mount point 只允许当前 macOS 用户访问。操作系统用户只控制谁能进入本地 mount point，不能替代数据库身份。

FSKit extension 的进程生命周期由 macOS 管理，CLI 只负责配置、mount/unmount 和诊断。SQLite 是嵌入式数据库，所以 FSKit extension 会在自己的进程中打开 SQLite 连接并加载 VexFS 扩展；但 FSKit callback 不能由 SQLite 扩展初始化函数启动，VexFS SQL 回调也不能控制 FSKit 生命周期。进入 PostgreSQL 阶段后，mount adapter 改为通过正常数据库连接访问服务器端扩展。

#### 模式二：直接命令（回退和诊断）

#### 直接命令模式

常见文件操作直接翻译成 VexFS SQL，每个写命令默认是一个数据库事务：

```bash
vexfs ls /src
vexfs tree /src
vexfs find /src --name '*.cpp'
vexfs cat /src/main.cpp
vexfs head -n 20 /src/main.cpp
vexfs tail -n 100 /logs/run.log
vexfs grep -R -n 'TODO' /src
vexfs stat /src/main.cpp
vexfs wc -l /src/main.cpp
vexfs du -h /src

vexfs touch /src/new.cpp
vexfs mkdir -p /src/lib
vexfs cp /src/a.cpp /src/lib/a.cpp
vexfs mv /src/old.cpp /src/new.cpp
vexfs rm -r /tmp/output
vexfs truncate -s 0 /logs/run.log
```

第一组直接命令包括：

- 浏览：`ls`、`tree`、`find`；
- 读取：`cat`、`head`、`tail`、`stat`、`file`；
- 查找和统计：`grep`、`rg`、`wc`、`du`；
- 修改：`touch`、`mkdir`、`put`、`get`、`cp`、`mv`、`rm`、`rmdir`、`truncate`、`ln`、`readlink`；
- 权限：`chmod`、`chown`、`acl`；
- 版本：`history`、`diff`、`snapshot`、`restore`；
- 运维：`check`、`gc`、`export`、`import`。

普通终端使用 `vexfs ls`，避免覆盖系统命令。正常 macOS 安装优先使用真实 mount；直接命令用于脚本、诊断和不能挂载的环境。

#### 模式三：任意命令 worktree（受限环境回退）

`sed -i`、`awk`、`jq`、`python`、`tar`、`git`、编译器和 `apply_patch` 都会调用操作系统文件接口。macOS FSKit mount 中它们可以直接工作；没有挂载能力时，使用受控 worktree。

VexFS 为此提供受控 worktree。`vexfs shell` 会创建 worktree、进入目录并启动真正的系统 shell，因此模型可以直接使用系统中的全部文件工具：

```bash
# 创建一个基于数据库版本 847 的临时工作区
vexfs worktree create agent-workspace --path ./work

cd ./work
rg 'TODO' .
sed -i.bak 's/old/new/g' src/main.cpp
python scripts/generate.py
tar -czf output.tar.gz build/

# 查看差异并事务性提交回数据库
vexfs worktree status
vexfs worktree diff
vexfs worktree commit -m 'update generated files'
```

交互使用可以简化为：

```bash
vexfs shell agent-workspace
# 现在可直接使用 cd、ls、cat、rg、sed、python、git、make 等系统命令
```

也支持一次执行：

```bash
vexfs exec agent-workspace -- sh -lc \
  "python scripts/generate.py && rg 'ERROR' logs/"
```

`worktree create` 执行以下步骤：

1. 使用数据库连接身份认证；
2. 读取用户有权访问的目录和文件；
3. 固定工作区起始 commit；
4. 把普通文件和目录写入受控临时目录；
5. 保存路径、inode、版本和 checksum 清单。

创建完成后不保持数据库事务和锁。外部命令无论运行几秒还是几小时，都不会占用一个长期数据库事务。

`worktree commit` 执行以下步骤：

1. 比较起始清单和本地目录，得到新增、修改、改名和删除；
2. 拒绝越界路径、设备文件、socket、FIFO 和不安全链接；只允许指向工作区内部的相对符号链接；
3. 检查每个路径的写入、删除、改名和管理权限；
4. 检查文件大小、工作区配额和保留规则；
5. 检查起始版本之后是否存在并发修改；
6. 把全部变更作为一个 changeset 放进单个数据库事务；
7. 生成文件版本、工作区 commit 和审计记录；
8. 数据库提交成功后才报告 worktree 已提交。

如果权限、配额或并发检查失败，数据库不产生部分文件修改，本地 worktree 保留用于处理问题。

#### Mount 和 CLI 都不改变所有权

- 不直接读取数据库物理文件；
- 不保存权威文件状态；
- 不自行决定权限、版本或冲突结果；
- mount adapter 不能由数据库扩展初始化或 SQL callback 启动；SQLite 的 FSKit extension 可以在系统管理的自身进程中打开 SQLite 连接，PostgreSQL adapter 则保持为外部客户端；
- 不建立新网络协议；
- 临时 worktree 不是备份；
- 移除 gateway 和 CLI 后，全部文件仍可通过 SQL 管理。

因此，VexFS 支持的是“模型常用的普通文件和目录工作流”。P0 mount 支持明确的 POSIX 子集，不支持设备文件、socket、FIFO、setuid 或全部特殊对象。

## 4. 核心价值：SQL 事务和 Bash 入口

VexFS 的关键价值不是模仿 `ls` 和 `cat`，而是同时提供两种明确分开的入口：

1. SQL 文件函数参加调用者当前数据库事务，可以和业务表原子提交；
2. mount 让 Bash 和普通程序无感使用文件，但每个文件操作使用 gateway 自己的短数据库事务。

mount 不能加入应用已经打开的数据库事务，多条 Bash 命令也不是一个事务。需要多文件原子提交时使用 changeset；未来的 `vexfs txn run` 通过隔离工作视图生成一个 changeset。

```sql
BEGIN;

UPDATE tasks
SET status = 'completed'
WHERE id = 42;

SELECT vexfs_write(
  'task-files',
  '/tasks/42/report.md',
  '# Task 42 report'
);

COMMIT;
```

结果只能是：

- 任务状态和报告文件同时提交；或
- 两者同时回滚。

插件不能在内部偷偷开启一个独立事务，也不能在数据库事务回滚后保留文件修改。

### 4.1 平台无关句柄和提交边界

mount 不能把一次 `open()` 到 `close()` 变成长期数据库事务。编辑器、编译器和 Agent 可能持有文件几分钟，长期事务会占连接、持锁并阻止 SQLite checkpoint 或 PostgreSQL 正常回收。

共享核心不使用 `flush/release` 等单个平台名称，而定义五个动作：

```text
mount_open(path, flags)
  └── 读取 inode、权限和 expected version，创建带租约的 handle

mount_stage_write(handle, offset, bytes, request_id)
  └── 短事务写入未发布 staging；成功后才向系统返回 write 成功

mount_publish(handle, generation, durability, request_id)
  └── 幂等复查 principal、quota 和 expected version
      生成 manifest、file version、workspace commit 和审计

mount_close(handle, retained_modes)
  └── 最终 close 时发布 dirty generation；错误可返回给平台 adapter

mount_reclaim(handle/item)
  └── 释放 adapter 引用；不能删除未确认 staging 或已发布版本
```

macOS FSKit 映射如下：

- `openItem` → `mount_open`；
- `write` → `mount_stage_write`；
- 没有 retained open mode 的最终 `closeItem` → `mount_publish` 后 `mount_close`；
- `synchronize` → 发布该 volume 的全部 dirty generation，再按该 mount adapter 声明的
  durability 策略完成数据库提交；
- `reclaimItem` → 释放 FSItem 映射，只做可恢复清理，不作为唯一发布点。

Phase 0 必须用真实 `fsync(2)`、`close(2)` 和 FSKit callback 日志确认系统调用到 `synchronize/closeItem` 的实际映射。如果当前稳定 SDK 不提供可报告的 durability 路径，MVP Gate 失败，不能用猜测代替。

句柄状态机固定为：

```text
OPEN_CLEAN
   └─ write ─> DIRTY
                 ├─ publish/synchronize ─> PUBLISHING ─> COMMITTED ─> OPEN_CLEAN
                 └─ 失败 ─> DIRTY_FAILED

final close
   ├─ clean/committed ─> CLOSED
   └─ dirty ─> publish ─> CLOSED 或 DIRTY_FAILED

reclaim with dirty/failed
   └─ RECOVERABLE_STAGING ─> retry 或过期回收
```

所有 publish/synchronize 请求都带稳定 request id。连接结果不确定时先查询请求结果，不能盲目重复生成 commit。每次 write 增加 `dirty_generation`；成功发布后记录 `published_generation` 和 commit id，没有新 write 的重复 synchronize 直接返回同一个结果。已完成请求至少保留最近 65,536 条，并每 4,096 条批量清理一次更早记录；清理只影响超过重试窗口的结果，不触碰 handle、staging、文件版本或 workspace commit。

SQLite schema 检查属于连接初始化工作。`vexfs_init()` 每次都执行完整验证和迁移，失败后可在同一连接修复并重试；普通 `stat/read/write/xattr` 等热路径复用本连接已经验证的状态。连接关闭时扩展自动清除缓存，不依赖 SQLite 3.44 才增加的 client-data API，因此仍保持 SQLite 3.24 的宿主门槛。

成功 `write` 表示数据库 staging 已保存，但不表示新版本已经对其他 handle 可见；成功
`synchronize` 表示版本已发布，并按 adapter 声明的 durability 策略完成提交。普通 SQL
函数遵守调用者连接配置；macOS SQLite mount 使用自己的专用连接，明确采用
`WAL + NORMAL staging + FULL publish/synchronize/close`。它不修改其他业务连接，也不
修改 PostgreSQL `synchronous_commit`。doctor 必须显示该连接的实际配置和风险。

FSKit extension 被终止时，已发布版本保持完整，未发布 staging 不出现在目录树中。重新挂载只检查和恢复 staging，只有超过保留期且确认未发布后才能回收。adapter 还必须限制单次 write、dirty handle 和 staging 字节数，对 SQLite 单 writer 施加背压。

`mkdir`、`unlink`、`rename` 和 `truncate` 各自使用一个短数据库事务。多条 Bash 命令默认不是同一个数据库事务；多文件或“文件 + 业务表”原子提交仍使用 SQL 事务或 changeset。

### 4.2 FSKit 调用集

最初 MVP 使用当前稳定 Xcode SDK 中的 `FSUnaryFileSystem`、`FSVolume.Operations`、`FSVolume.OpenCloseOperations` 和 `FSVolume.ReadWriteOperations`。macOS adapter 单独封装这些 API，避免共享核心依赖 Apple 类型，并为后续 `Handler` API 迁移保留边界。

MVP 支持：

- volume：probe、load/unload resource、activate/deactivate、mount/unmount、synchronize、statfs；
- 查找和属性：lookupItem、getAttributes、enumerateDirectory；
- 文件：openItem、closeItem、createItem、read、write、truncate、reclaimItem；
- 目录：createItem、removeItem、renameItem；
- flags：O_CREAT、O_EXCL、O_TRUNC、O_APPEND、只读和读写。

最初 MVP 必须覆盖原子 rename、open 后 unlink、最终 close、O_APPEND 和版本冲突。当前合同已经继续加入 chmod、chown、symlink、hardlink 和 xattr，并由各 mount adapter 按能力矩阵接入；资源 fork、共享可写 mmap、设备文件、socket 和 FIFO 仍不支持。任何尚未接入的平台能力返回稳定 `EPERM` 或 `ENOTSUP`，不能假装成功。

数据库 `inode_id` 稳定映射为 FSKit `FSItem`：

- lookup 返回或复用同一个 inode 的 FSItem 表示；
- removeItem 只移除 dentry，已打开 item 继续引用原 inode 版本；
- rename replace 在一个数据库事务中替换目标 dentry，目标 item 延迟到 reclaim 后释放；
- reclaimItem 只释放 adapter 对象，数据库 inode 必须在没有 dentry、handle、版本或快照引用后才能进入 GC。

FSKit 的 open-unlink emulation、advisory lock、Finder xattr 和文件协调行为必须通过真实回调测试确认；未确认前不进入 MVP 承诺。

### 4.3 缓存与一致性

MVP 采用 close-to-open 一致性：

- 同一 handle 读取时叠加自己的 staged writes；
- 其他已打开 handle 固定读取其打开时版本；
- publish/synchronize 成功后，新 open 读取新版本；
- 多 mount 主动失效完成前不承诺立即看到新版本，但发布时必须检测 expected version 冲突。

缓存策略以权限和正确性为先：

- MVP 不启用 FSKit kernel data cache；
- 在提供 `DataCacheHandler` 的 SDK 上显式协商 no-cache；稳定旧 SDK 通过 ReadWriteOperations 让读写进入 extension；
- 每次 publish 前重新检查数据库版本、权限和配额；
- SQLite MVP 只有一个 mount，不实现跨 mount 主动失效；
- Phase 0 记录属性、目录和数据缓存的真实 callback 行为，发现无法避免的陈旧权限时必须阻断 MVP。

当前 FSKit 的已实现优化不使用 `DataCacheHandler`，也不把缓存变成权威状态：

- 不超过 1 MiB 的只读普通文件在 FSItem 的 open/close 生命周期内保存完整内容快照；
- 同一 vnode 出现可写打开或 truncate 时立即清除快照；
- rename/unlink 后快照继续代表原 inode，使已打开文件仍可读；
- 超过 1 MiB 的文件和所有可写文件继续使用数据库 handle；
- close/reclaim 清除进程内快照，重挂载后重新从数据库读取；
- 该策略只解决本地单 mount 文本搜索的 handle 开销，不解决多 mount、远程权限失效或 mmap。

同一 mount 内对同一 vnode 的本地写和 truncate 会主动失效快照，所以已有只读描述符会看到
本地已提交变更；独立数据库 handle 和未来其他 mount 仍按 close-to-open 与版本冲突规则处理。

只读 kernel cache、主动 invalidation 和 mmap 在后续性能数据表明必要时再设计。启用前必须先证明缓存不会绕过权限、版本和 close-to-open 合同。

### 4.4 身份与沙箱模型

FSKit 本地入口只服务当前 macOS 用户；数据库 principal 由所选后端决定：

- 默认 SQLite 数据库位于 `~/Library/Application Support/VexDB-Lite/`；
- CLI 把数据库所在目录作为 security-scoped `FSPathURLResource` 交给 FSKit；
- extension 只访问该目录内经过描述文件校验的数据库、WAL 和 SHM；
- mount point 只允许当前用户访问；
- SQLite principal 固定为 workspace owner，不接受 CLI 自报用户；SQLite 保存便携 ACL，但
  当前不执行完整路径 ACL 授权和继承；
- PostgreSQL principal 来自经过 libpq 认证的真实 role，并在 open、目录修改和 publish 时
  重新执行路径 ACL；CLI 传入的名称或操作系统 uid 不能提升数据库权限；
- 文件和目录保存并报告数据库中的 mode 和 owner/group 元数据；
- App sandbox、entitlement、签名和公证配置必须进入安装回归。

新版 FSKit `FSContext` 可以提供调用者 uid/gid，但当前不据此实现本机多用户共享。PostgreSQL
已经增加数据库 role、路径 ACL，以及每次 read/write/publish 所需的权限复查规则。操作系统
身份负责保护本地入口，数据库 principal 负责决定数据库文件权限，两者不能互相替代。

## 5. 三端统一 SQL 合同

三端注册相同函数名、参数顺序和错误分类。返回表的具体声明按宿主扩展 API 适配。

### 5.1 工作区

```sql
SELECT vexfs_workspace_create('workspace');
SELECT * FROM vexfs_workspace_list();
SELECT vexfs_workspace_stat('workspace');
SELECT vexfs_workspace_freeze('workspace');
SELECT vexfs_workspace_drop('workspace');
```

工作区是最高隔离单位，拥有独立根目录、版本序列、权限和空间统计。

### 5.2 文件和目录

```sql
SELECT vexfs_mkdir('workspace', '/a/b', true);
SELECT vexfs_write('workspace', '/a/b/file.txt', 'hello');
SELECT vexfs_write_range('workspace', '/a/b/file.txt', 5, ' world');
SELECT vexfs_read('workspace', '/a/b/file.txt');
SELECT vexfs_read_range('workspace', '/a/b/file.txt', 0, 5);
SELECT vexfs_move('workspace', '/a/b/file.txt', '/a/file.txt');
SELECT vexfs_copy('workspace', '/a/file.txt', '/a/file-copy.txt');
SELECT vexfs_remove('workspace', '/a/file-copy.txt', false);
```

### 5.3 查询

```sql
SELECT * FROM vexfs_list('workspace', '/a');
SELECT * FROM vexfs_stat('workspace', '/a/file.txt');
SELECT * FROM vexfs_history('workspace', '/a/file.txt');
SELECT * FROM vexfs_diff('workspace', 100, 120);
SELECT vexfs_grep('workspace', '/a', 'error', 0, 1000);
SELECT vexfs_grep_index('enable');
SELECT vexfs_grep_index('status');
```

`vexfs_grep(workspace, path, pattern, flags, limit)` 是普通文本查找，不是语义检索。
`flags=1` 表示 ASCII 忽略大小写，`flags=2` 表示每个文件只返回一次。它保证 UTF-8
字面匹配、行号和递归目录查询，二进制文件跳过；正则表达式不在当前合同内。

默认实现用一个 SQLite 连接批量扫描当前文件版本，不经过 mount 的逐文件回调。用户可
显式启用数据库级 FTS5 trigram 索引；索引关闭时没有额外写放大，开启后由文件写入和
恢复事务同步维护。短查询或不支持 FTS5 的宿主自动回退批量扫描。

### 5.4 快照和恢复

```sql
SELECT vexfs_snapshot_create('workspace', 'before-change');
SELECT * FROM vexfs_snapshot_list('workspace');
SELECT vexfs_restore_file('workspace', '/a/file.txt', 'before-change');
SELECT vexfs_restore_workspace('workspace', 'before-change');
SELECT vexfs_snapshot_drop('workspace', 'before-change');
```

### 5.5 权限和审计

```sql
SELECT vexfs_grant('workspace', '/reports', 'report_writer', 'read,write');
SELECT vexfs_revoke('workspace', '/reports', 'report_writer', 'write');
SELECT * FROM vexfs_audit('workspace', since_commit => 100);
```

PG 中的 principal 是数据库 role。DuckDB 和 SQLite 使用宿主数据库能提供的身份边界；不能假装三个数据库都有 PG 级别的角色系统。

### 5.6 检查和维护

```sql
SELECT vexfs_check('workspace', 1); -- deep: 结构、引用、长度和 SHA-256
SELECT vexfs_check('workspace', 0); -- quick: 不读取 BLOB 计算 SHA-256
SELECT * FROM vexfs_repair_plan('workspace');
SELECT vexfs_gc('workspace', 1000);
SELECT vexfs_retention_set('workspace', keep_versions => 20, keep_days => 30);
```

repair 必须先给出计划。缺失文件块时不能填空内容后报告成功。

SQLite 当前实现只读 check，没有 repair。默认 CLI 使用 deep。当前 `chunked-v1` 中，canonical
版本引用不可变 manifest，manifest 再按顺序引用不超过 64 KiB 的物理 chunk；版本恢复生成的
alias 必须直接引用同 inode 的 canonical 版本，并复制相同的 size 和 SHA-256。quick 检查引用、
块数量、顺序和大小，deep 再逐块及整文件计算 SHA-256，全程不组装完整历史文件。

### 5.7 Worktree changeset

CLI 不能直接写内部表。worktree 差异必须通过数据库公开的 changeset 接口提交：

```sql
BEGIN;

SELECT vexfs_changeset_open('workspace', 847);
SELECT vexfs_changeset_put('/src/main.cpp', :content, :expected_version);
SELECT vexfs_changeset_move('/src/old.cpp', '/src/new.cpp', :expected_version);
SELECT vexfs_changeset_remove('/tmp/output', :expected_version, true);
SELECT vexfs_changeset_apply('update generated files');

COMMIT;
```

这是概念合同，三个数据库可以使用事务级临时表或流式接口承载大文件，但必须满足：

- `open` 固定 workspace 和 base commit，并锁定最终发布条件；
- `put/move/remove` 只写当前事务的暂存 changeset，不立即发布目录树；
- `apply` 一次完成权限、配额、冲突、版本和审计检查；
- 未执行 `apply`、数据库 rollback 或连接中断时，不产生可见变更；
- 一个 changeset 只产生一个 workspace commit；
- 内容太大时使用分块流，不要求 CLI 把整个工作区装进一个 SQL 参数。

### 5.8 Mount handle 合同

gateway 不能直接写内部表。它使用一组窄的、可版本化的数据库合同：

```text
vexfs_handle_open(workspace, path, flags, request_id)
  -> handle_id, inode_id, expected_version, size, mode

vexfs_handle_read(handle_id, offset, length)
  -> bytes, inode_version

vexfs_handle_stage_write(handle_id, offset, bytes, request_id)
  -> staged_size

vexfs_handle_setattr(handle_id, size?, times?, request_id)
vexfs_handle_publish(handle_id, generation, durability, request_id)
  -> publish_state, new_inode_version, workspace_commit, durable
vexfs_mount_synchronize(workspace, request_id)
  -> published_handles, workspace_commit?, durable
vexfs_handle_close(handle_id, retained_modes, request_id)
  -> close_state, recoverable_staging_id?
vexfs_item_reclaim(workspace, inode_id, request_id)
vexfs_handle_request_status(handle_id, request_id)
  -> request_state, result
vexfs_handle_renew(handle_id)
vexfs_handle_abort(handle_id)
```

目录操作继续复用 `lookup/list/mkdir/move/remove` 合同。每个写入和发布请求带稳定 request id，连接结果不确定时先查询 request status，而不是盲目重复写。`publish` 和 `synchronize` 对同一个 dirty generation 必须返回同一个 commit 结果。close/reclaim 不接受绕过 publish 状态机的快捷参数。

handle 保存在数据库的未发布区，带 owner principal、expected version、租约和状态；它不是数据库事务，也不能越过重新认证后的权限检查。gateway 只保存数据库 handle id 和短期读取缓存，进程退出后不拥有任何已提交状态。

## 6. 外部 API 要保持小

上面的 SQL 是概念合同，最终不应把每个内部步骤都公开。

建议公开：

- 工作区管理；
- 目录管理；
- 文件整体和范围读写；
- move/copy/remove；
- list/stat/history；
- snapshot/restore；
- grant/revoke/audit；
- check/GC；
- export/import；
- mount handle 的 open/read/stage/publish/synchronize/close/reclaim/request-status；
- worktree changeset 暂存和原子提交。

以下只作为内部函数：

- inode 分配；
- dentry 版本更新；
- chunk 插入；
- manifest 发布；
- commit id 分配；
- 引用计数更新；
- 审计 hash 链更新。

内部表和内部函数必须由数据库权限系统保护。

## 7. 统一核心模块

### NamespaceCore

- 路径解析；
- 文件和目录创建；
- move、copy、remove；
- 名称校验；
- 目录唯一性；
- 安全的工作区内相对 symlink；
- hardlink 的 dentry 共享 inode 和引用关系；
- 循环和越界检查。

### ContentCore

- BLOB 整体读写；
- 范围读写；
- chunk 切分；
- 稀疏区间；
- manifest；
- checksum；
- 小文件内联；
- 大文件分块。

### TransactionCore

- 工作区 head 锁；
- inode generation；
- 唯一冲突；
- 幂等 request id；
- 后端错误转统一错误；
- 所有写入服从当前数据库事务；
- rollback 后无残留状态。

### VersionCore

- inode 和 dentry 显式版本；
- 文件历史；
- 工作区 commit 序列；
- 快照；
- 单文件恢复；
- 目录恢复；
- 工作区恢复；
- diff；
- 版本保留规则。

### AccessCore

- 数据库 principal 映射；
- workspace/path 权限；
- read/write/delete/admin 权限；
- 权限继承；
- 空间限制；
- 内部对象保护。

### AuditCore

- 当前数据库用户；
- 事务和 commit id；
- 操作类型；
- 路径和 inode；
- 修改前后版本；
- 审计 hash 链；
- 审计查询。

### IntegrityCore

- chunk checksum；
- manifest checksum；
- inode/dentry 引用；
- hardlink 启用后的引用计数；
- 孤立对象；
- 缺失 chunk；
- commit 链；
- 审计链；
- 修复计划。

### MaintenanceCore

- 不可达版本回收；
- 不可达 chunk 回收；
- 保留策略；
- 批量 GC；
- 统计信息；
- schema 版本；
- 扩展升级检查。

## 8. 数据模型

内部对象使用宿主数据库正常管理的表、索引和 BLOB，不建立私有磁盘格式。

### 8.1 工作区

```text
vexfs_workspaces
  workspace_id
  name
  root_inode
  head_commit
  next_inode
  frozen
  owner_principal
  created_at
```

所有修改先锁定 workspace head，再分配 commit id。这样同一工作区的版本顺序稳定。

### 8.2 inode 版本

```text
vexfs_inode_versions
  workspace_id
  inode_id
  version_id
  valid_from_commit
  valid_to_commit
  kind
  mode
  owner_principal
  size
  manifest_id
  deleted
  created_at
  modified_at
```

版本记录不可原地改写。`valid_from_commit <= snapshot_commit < valid_to_commit` 表示该快照看到的版本。

### 8.3 目录项版本

```text
vexfs_dentry_versions
  workspace_id
  parent_inode
  raw_name
  child_inode
  valid_from_commit
  valid_to_commit
  tombstone
```

文件名按字节比较，不使用语言区域排序。禁止 NUL 和 `/`。

### 8.4 文件 manifest 和块

```text
vexfs_manifests
  workspace_id
  manifest_id
  inode_id
  file_size
  block_size
  chunk_count
  checksum

vexfs_chunks
  workspace_id
  chunk_id
  inode_id
  size
  data_blob
  checksum

vexfs_manifest_chunks
  manifest_id
  chunk_no
  chunk_id
```

规则：

- SQLite `chunked-v1` 固定使用 64 KiB 块，空文件使用零块 manifest；
- manifest 发布后不可修改；
- 物理 chunk 发布后不可修改；同 inode 的版本可以复用未变化的物理块；
- 随机写只产生受影响的新块；版本恢复使用 alias，不复制 manifest 或 chunk；
- 当前不做跨 inode/跨文件去重；
- 文件内容全部由数据库 BLOB 管理。

### 8.5 快照

```text
vexfs_snapshots
  workspace_id
  snapshot_name
  commit_id
  created_by
  created_at
```

快照只记录 commit id，接近 O(1)。长期历史由显式版本表保证，不依赖数据库 MVCC 旧版本保留。

### 8.6 权限和审计

```text
vexfs_acl
  workspace_id
  inode_id
  principal
  permissions

vexfs_audit_events
  workspace_id
  commit_id
  seq
  principal
  operation
  inode_id
  before_version
  after_version
  previous_hash
  event_hash
```

权限不只分为 read/write。统一权限集合为：

```text
traverse   经过目录解析下级路径
list       查看目录成员
read       读取文件内容和基本属性
create     在目录中创建文件或子目录
write      修改已有文件内容和属性
delete     删除目录项
rename     移动或改名目录项
history    查看历史版本和 diff
snapshot   创建和恢复快照
share      修改 ACL
admin      修改 owner、配额和保留规则
```

规则：

- 每次 SQL 操作、mount open/发布和 worktree commit 都在数据库内重新检查权限；
- checkout 时只能导出当前用户可读的路径；
- worktree 中的本地 chmod 不能改变数据库权限；
- `vexfs chmod/chown/acl` 必须调用数据库 SQL，并要求 `share` 或 `admin`；
- 删除和改名同时检查源目录、目标目录及目标文件权限；
- 快照恢复不能绕过当前权限；
- owner 和 ACL 的每次变化都写入审计记录；
- PostgreSQL principal 对应数据库 role；DuckDB 和 SQLite 使用宿主可证明的连接身份，不能接受 CLI 自报身份。

### 8.7 Mount handle 和未发布暂存

```text
vexfs_open_handles
  handle_id
  workspace_id
  inode_id
  principal
  expected_version
  open_flags
  state                  # OPEN_CLEAN / DIRTY / PUBLISHING / DIRTY_FAILED / RELEASED
  dirty_generation
  published_generation
  published_commit_id
  lease_until
  last_error_code
  last_request_id
  created_at

vexfs_staging
  handle_id
  generation
  logical_size
  capacity
  created_at
  updated_at

vexfs_staging_data
  handle_id
  content_blob
```

handle 和 staging 都是数据库受管的临时状态：普通 list/read 看不到，备份恢复后也不会被当成已发布文件。`publish/synchronize` 在短事务中把 staging 组装成不可变 manifest 并发布；close/reclaim 只结束 adapter 引用并记录仍需恢复的 staging。

SQLite Phase 0 的物理表为 `_vexfs_handles`、`_vexfs_staging` 和
`_vexfs_staging_data`。小而频繁变化的 generation、logical size 和 capacity 与大 BLOB
分开；每个 handle 在数据表中只有一行，使用 SQLite incremental BLOB I/O 做分块和
随机写。`_vexfs_meta.staging_layout=split-v1` 标记该布局。打开旧的 0.2 内联布局时，
插件在 savepoint 内把 BLOB 迁移到数据表；旧元数据表里的 `content` 兼容列保留为空。
后续 PostgreSQL 或 DuckDB 可以按宿主特点使用 chunk 表，但不能重新把热点元数据和
大内容绑在同一条反复改写的记录中。

重挂载时，gateway 先扫描自己 principal 和 workspace 下的 `DIRTY_FAILED`、过期 `DIRTY` 和未确认 `PUBLISHING`：

1. 有 request result 的 `PUBLISHING` 直接恢复为对应 committed 状态；
2. 完整且未冲突的 dirty staging 可以由显式 `vexfs recover` 重试；
3. 自动清理必须等待保留期，并再次确认没有已发布 manifest 引用；
4. 回收未发布块不能删除任何已发布版本。

## 9. 事务规则

### 参加用户事务

VexFS 函数必须使用当前数据库事务。

```sql
BEGIN;
SELECT vexfs_write(...);
SELECT vexfs_move(...);
ROLLBACK;
```

rollback 后，write 和 move 都不可见。

### 每次修改

1. 检查数据库用户权限；
2. 锁定 workspace head；
3. 解析当前路径和 inode 版本；
4. 校验冲突和配额；
5. 写新 chunk/manifest；
6. 关闭旧 inode/dentry 版本；
7. 写新版本；
8. 增加 head commit；
9. 写审计记录；
10. 返回结果，等待数据库事务 commit/rollback。

插件不能自行 commit。

### 并发

- PG 使用数据库行锁、唯一约束和 MVCC；
- DuckDB 使用宿主事务和 optimistic conflict；
- SQLite 使用宿主事务、单 writer 和 busy 处理；
- 三端统一返回 `VEXFS_CONFLICT`、`VEXFS_BUSY` 等错误分类。

## 10. 数据库原生备份与 VexFS 导出

VexFS 的保护分为四层：

1. **文件版本**：每次提交保留旧版本，用于查看和恢复单个文件；
2. **工作区快照**：固定一个 commit，用于恢复目录或整个工作区；
3. **数据库原生备份**：灾难恢复的完整备份，包含业务数据和 VexFS；
4. **工作区逻辑导出**：用于单工作区离线保存和跨数据库迁移。

数据库原生备份是第一灾难恢复机制：

- PG 的原生备份必须包含 VexFS schema、表、索引和 BLOB；
- DuckDB 数据库备份必须包含 VexFS 对象；
- SQLite backup API 或安全数据库备份必须包含 VexFS shadow tables；
- VexFS 不控制备份进程，不建立自己的物理备份系统。

VexFS 可以额外提供**工作区逻辑导出**，用途只有：

- 导出单个 workspace；
- 恢复单个 workspace；
- PG、DuckDB、SQLite 之间迁移；
- 验证工作区级内容。

概念 API：

```sql
SELECT * FROM vexfs_export('workspace', 'snapshot-name');
SELECT vexfs_import_begin('new-workspace');
SELECT vexfs_import_record('new-workspace', record_type, record_key, payload, checksum);
SELECT vexfs_import_finish('new-workspace');
```

CLI 可以提供：

```bash
vexfs snapshot create workspace before-change
vexfs snapshot list workspace
vexfs restore workspace --snapshot before-change --path /reports
vexfs export workspace --snapshot before-change --output workspace.vexfs
vexfs import workspace.vexfs --workspace restored-workspace
vexfs backup verify workspace
```

`backup verify` 只检查当前数据库、快照和逻辑导出的完整性，不假装替数据库执行物理备份。

实际大数据导出使用数据库自己的 COPY、结果流或应用接口。VexFS 不上传 S3、不调度定时任务、不保存密钥。备份调度、保存位置、加密和异地复制仍由数据库运维系统负责。

## 11. 三个宿主适配器

### PostgreSQL

- 随 PG extension 生命周期安装、升级和卸载；
- 使用 PG memory context；
- 不跨 PG boundary 抛 C++ 异常；
- 使用 PG table、index、BLOB/TOAST、事务、锁、WAL 和权限；
- SQL 函数默认 `SECURITY INVOKER`；
- 内部 schema 不授予普通用户直接写权限；
- PostgreSQL 数据库进程不启动 FSKit/FUSE/WinFsp loop 或网络线程；外部 mount adapter 使用正常 PostgreSQL 连接和 role。
- 当前 `0.4.0-alpha.1` 已实现 chunk/manifest、版本、snapshot、ACL、审计、配额、retention、
  GC、跨 gateway handle/lock/cache invalidation 和 libpq HostStore；
- 变更审计由数据库在同一事务内写入，使用认证后的 `session_user` 和 role OID；记录 workspace
  名称/ID、commit、操作、路径、inode 和前后版本。workspace 删除前先记录，删除后把 ID
  置空但保留名称和证据。details 只保存安全元数据，不保存文件正文、xattr 值、密码或 DSN；
- macOS FSKit 与 Linux FUSE 复用同一 runtime。macOS 的 PostgreSQL passfile 只在挂载资源存在
  期间以 `0600` 暂存，最后卸载和 `doctor` 会清理；
- mount 成功后，runtime 通过预先打开的目录 fd 把底层 mountpoint 设为 `0500`。FSKit/FUSE
  异常退出时，普通用户不能把文件写进没有数据库语义的本机目录；显式卸载恢复 `0700`；
- 网络中断时失败的 mutation 不做隐式重放，因为客户端无法确认服务端事务是否已经提交；
  恢复后先用可重试读操作确认状态，再接受新的写请求；
- 局域网 PostgreSQL 需要用户给 VexDB Lite 一次 macOS“本地网络”权限，程序不绕过系统授权。

### DuckDB

- 随 DuckDB extension 加载和 catalog 管理；
- 使用 DuckDB allocator、Value、transaction 和 storage API；
- 使用普通表和 BLOB；
- optimistic conflict 转统一错误；
- 不直接读写 `.duckdb` 文件；
- 不自建 WAL 或后台服务。
- 首个 DuckDB 版本只承诺 SQL、CLI 和 worktree；
- 可写 mount 必须让 gateway 成为唯一 DuckDB 写进程，或者等待宿主提供并验证可用的多进程写入能力；
- 不允许 gateway 和外部应用进程同时以普通方式写同一个 `.duckdb` 文件。

### SQLite

- 随 SQLite extension 连接生命周期注册；
- 使用 SQLite allocator、transaction、BLOB 和 VFS；
- 使用受保护的 shadow tables；
- 不直接读写 `.db`、`-wal` 或 `-shm`；
- `SQLITE_BUSY` 转统一错误；
- 普通 SQL 函数不改变调用者的 journal mode 或 synchronous；
- macOS mount 使用独立读写连接，打开时固定 `journal_mode=WAL`、`foreign_keys=ON`、
  `synchronous=FULL`；未发布 stage write/truncate 临时使用 NORMAL，任何发布、sync、
  close、目录修改和 session 结束前切回 FULL；
- NORMAL 阶段允许电源故障丢失最近的未发布 staging，但不得产生半个已发布版本；
  publish/synchronize 成功返回时，权威事务和它依赖的 WAL frame 必须达到 FULL；
- 宿主负责 SQLite 版本和文件系统安全，busy timeout 和实际 PRAGMA 值由 doctor 显示。

## 12. Mount 平台适配器

### macOS FSKit（MVP）

首个 mount adapter 由 `VexFS.app` 内的 FSKit File System Extension 提供：

```text
macOS VFS / mount(8)
        |
      FSKit
        |
VexFSFileSystem.appex (Swift)
        |
MountContract C ABI
        |
SQLite C API + VexFS SQLite extension
        |
security-scoped 目录中的数据库
```

- Swift 层只负责 FSKit 类型、callback、result 和 POSIXError 映射；
- 平台无关 mount contract 使用窄 C ABI，不让共享 C++ 核心依赖 Swift 或 FSKit；
- adapter 通过 SQLite C API 调用公开 VexFS SQL/handle 合同，不直接写 shadow table；
- 一个串行 writer connection 处理 staging 和 publish，有限 read connection 处理读取；
- SQLite WAL、busy_timeout、连接数和重试上限由 context 配置并在 doctor 中显示；
- 每个 mount 把数据库父目录作为 security-scoped `FSPathURLResource`，不使用 App Group entitlement；
- 父目录内的 `.vexfs-volume.json` 只保存合同版本、数据库文件名和 workspace，不保存密码、绝对路径或文件数据；FSKit `loadResource` 校验文件名不能逃逸该目录后再打开数据库；
- MVP 要求一个数据库文件独占一个父目录，避免两个挂载改写同一个描述文件；
- 当前稳定 SDK 的 `FSVolume.Operations` 封装在单独 compatibility layer；新版 `FSVolume.Handler` 稳定后只替换该层；
- App Extension 由 macOS 启停，重启后通过数据库 handle/staging 状态恢复，不依赖进程内存。
- 小文件只读缓存只存在 extension 内存中，最大 1 MiB；14 项生命周期测试覆盖 rename、
  unlink、同名重建、并发改写、truncate 和大文件 handle 回退。

### Linux libfuse3（预览已实现）

- 当前预览使用 FUSE3 high-level path API，并把数据库 inode_id 返回为稳定 inode number；
- 已处理 getattr/readdir/open/create/read/write/truncate/rename/remove、chmod/chown、
  hardlink、symlink、xattr、flush/fsync/release、direct I/O、statfs 和 `fusermount3`；
- 新建节点采用发起请求的 uid/gid；默认 root inode 在首次普通用户挂载时接管默认的
  0:0 owner，避免 `default_permissions` 阻止用户在根目录创建文件；
- 复用同一 mount contract，不把 FUSE 语义或最终文件状态写到数据库之外；
- 四类时间戳、最小 `flock`/`fcntl`、原子 append、打开文件 rename/unlink、强制卸载和
  helper 崩溃恢复已接入公共合同并通过真机 eval；
- 进入稳定版前仍需补 low-level lookup/forget、内核缓存和 TTL 调优、
  fallocate/copy_file_range、更多 Linux 发行版、长时间运行和规模测试。

### Windows WinFsp（后续）

- 使用 WinFsp user-mode file system API；
- 单独映射 CreateFile share mode、delete-pending、盘符、ACL/SID、reparse point 和 attributes；
- 复用同一 mount contract，不把 NT 内核类型写入数据库核心。

## 13. HostStore 合同

共享核心通过小接口调用宿主能力：

```text
CurrentTransaction()
CurrentPrincipal()

LockWorkspace(workspace_id)
GetWorkspace(workspace_id)
UpdateWorkspaceHead(workspace_id, expected_head, new_head)

GetInodeAt(workspace_id, inode_id, commit_id)
PutInodeVersion(version)
LookupDentryAt(workspace_id, parent_inode, raw_name, commit_id)
ListDentriesAt(workspace_id, parent_inode, commit_id, cursor, limit)
PutDentryVersion(version)

PutManifest(manifest)
PutChunk(chunk)
GetChunk(manifest_id, chunk_no)

PutSnapshot(snapshot)
PutAudit(events)
```

适配器只负责把这些调用映射到宿主 API。它不拥有事务，也不能改变核心文件规则。

## 14. 错误合同

```text
VEXFS_NOT_FOUND
VEXFS_ALREADY_EXISTS
VEXFS_NOT_DIRECTORY
VEXFS_IS_DIRECTORY
VEXFS_NOT_EMPTY
VEXFS_PERMISSION_DENIED
VEXFS_READ_ONLY
VEXFS_QUOTA_EXCEEDED
VEXFS_CONFLICT
VEXFS_BUSY
VEXFS_NO_SPACE
VEXFS_CORRUPTION
VEXFS_INVALID_PATH
VEXFS_UNSAFE_LINK
VEXFS_UNSUPPORTED
VEXFS_VERSION_MISMATCH
VEXFS_UNAVAILABLE
VEXFS_TIMEOUT
VEXFS_STALE_HANDLE
```

数据库适配器保留底层诊断信息，但用户 API 返回统一错误类型和通俗错误消息。macOS FSKit adapter 在实现前冻结以下 POSIXError 映射；Linux 和 Windows adapter 再做平台专项映射：

| VexFS 错误 | errno |
|---|---:|
| `NOT_FOUND` | `ENOENT` |
| `ALREADY_EXISTS` | `EEXIST` |
| `NOT_DIRECTORY` | `ENOTDIR` |
| `IS_DIRECTORY` | `EISDIR` |
| `NOT_EMPTY` | `ENOTEMPTY` |
| `PERMISSION_DENIED` | `EACCES` |
| `READ_ONLY` | `EROFS` |
| `QUOTA_EXCEEDED` | `EDQUOT` |
| `NO_SPACE` | `ENOSPC` |
| `CONFLICT` | `EAGAIN` |
| `BUSY` | `EBUSY` |
| `CORRUPTION`、`UNAVAILABLE` | `EIO` |
| `INVALID_PATH` | `EINVAL` |
| `UNSAFE_LINK` | `ELOOP` |
| `UNSUPPORTED` | `ENOTSUP` |
| `VERSION_MISMATCH` | `EPROTO` |
| `TIMEOUT` | `ETIMEDOUT` |
| `STALE_HANDLE` | `ESTALE` |

当前平台已接入的 chmod/chown 调用数据库合同；尚未接入时返回 `EPERM` 或 `ENOTSUP`。尚未支持的跨 workspace rename 返回 `EXDEV`。同一错误不能因为数据库宿主不同而改变 errno。

## 15. 仓库结构

```text
vexdb_lite/
├── common/                         # 原向量算法，不改职责
├── vexdb_pg/
├── vexdb_duckdb/
├── vexdb_sqlite/
├── agent_files/
│   ├── include/
│   │   ├── vexfs_types.h
│   │   ├── vexfs_error.h
│   │   └── host_store.h
│   ├── core/
│   │   ├── namespace.cpp
│   │   ├── content.cpp
│   │   ├── transaction.cpp
│   │   ├── version.cpp
│   │   ├── access.cpp
│   │   ├── audit.cpp
│   │   └── integrity.cpp
│   ├── adapters/
│   │   ├── pg/
│   │   ├── duckdb/
│   │   └── sqlite/
│   └── tests/
├── tools/
│   ├── vexfs_cli/                    # 安装、mount、直接命令和 worktree 入口
│   └── vexfs_mount/
│       ├── common/
│       │   ├── mount_contract/       # 平台无关 open/read/stage/publish/sync/close/reclaim
│       │   ├── db_client/            # 公开 handle/SQL 合同客户端
│       │   ├── handle_manager/       # handle、租约、generation 和恢复
│       │   └── error_map/            # VexFS 错误到平台错误
│       ├── macos/
│       │   ├── VexFSApp/             # macOS App 和首次启用引导
│       │   ├── FileSystemExtension/  # FSKit App Extension
│       │   ├── FSKitCompatibility/   # Operations/Handler API 隔离
│       │   └── MountContractBridge/  # Swift 到 C ABI
│       ├── linux/fuse_gateway/       # libfuse3 预览 adapter
│       └── windows/winfsp_gateway/   # 后续 WinFsp adapter
├── packaging/vexfs/
│   ├── macos/                        # App、appex、CLI、SQLite 扩展、签名和公证
│   ├── linux/                        # FUSE 组合包（待补正式打包）
│   └── windows/                      # 后续 WinFsp 组合包
└── tests/spec/agent_files/
```

仓库不增加对外远程文件协议、SDK 或独立权威服务。macOS 默认 NFS 只绑定 loopback，作为
系统 mount client 到 Workspace Engine 的本机传输层；跨电脑仍使用正常 PostgreSQL 连接。
后续 FSKit extension 通过同一 runtime 调用公开合同。任何 adapter 启动前都必须与扩展交换
合同版本和能力位，主版本不兼容时拒绝可写挂载。

## 16. 验收测试

### SQL 合同

- 三端相同函数名和参数顺序；
- 相同操作序列得到相同目录树；
- 内容 checksum 一致；
- 错误分类一致；
- list/stat/history 结果字段一致。

### CLI

- `vexfs setup/mount/unmount/mount status/doctor` 生命周期正确；
- `vexfs ls/cat/grep/stat` 与对应 SQL 结果一致；
- 支持 PostgreSQL、DuckDB 和 SQLite 连接配置；
- 默认一个写命令使用一个数据库事务；
- 权限错误、文件不存在和连接失败返回不同退出码；
- 标准输出可以安全用于管道，诊断信息写入标准错误；
- worktree 只导出有权读取的普通文件和安全链接；
- 系统 `sed/python/tar/git/apply_patch` 修改可以正确生成 changeset；
- worktree 运行期间不持有长期数据库事务或锁；
- commit 时重新检查权限、配额和起始版本；
- changeset 全部成功或全部回滚；
- 命令失败或提交冲突时保留本地差异；
- 越界链接、设备文件、socket 和 FIFO 不能提交；
- `vexfs exec` 只有在命令退出成功且数据库提交成功时返回 0。

### macOS FSKit mount

- 真实 coreutils：ls、cat、cp、mv、rm、mkdir、truncate；
- 真实文本工具：rg、grep、sed、awk、jq；
- MVP 真实程序：Python、基础 Git、apply_patch；编译器和大型构建目录只记录兼容性，不作为 MVP SLA；
- flags：O_CREAT、O_EXCL、O_TRUNC、O_APPEND；
- FSKit probe/load/activate/mount/unmount/deactivate 生命周期；
- lookup/get attributes/enumerate/open/close/create/read/write/remove/rename/set attributes/synchronize/reclaim；
- 原子 rename、open-unlink 和最终 close；
- 同一 handle read-your-writes、旧 handle 版本固定、新 open 读取已发布版本；
- synchronize 重复调用只产生一个 commit；真实 fsync(2) 到 FSKit callback 的映射有证据；
- close/reclaim 兜底失败后 staging 可检查、可恢复且不会被发布为半文件；
- 终止 FSKit extension、SQLite reopen 和重新挂载；
- MVP 不启用 FSKit kernel data cache；
- SQLite busy、freeze、quota、空间不足、stale handle 和固定 errno；
- mount point 当前用户私有，security-scoped 数据库目录权限正确；
- 开发签名，以及 Developer ID + 公证或 Mac App Store/TestFlight 至少一条真实分发路径；完成 extension 启用和卸载回归；
- App/appex/CLI 与扩展版本不兼容时拒绝可写挂载；
- 不启用 FSKit extension 时 SQL 和 CLI 仍然完整可用。

### 事务

- 文件写入和业务表更新同时 commit；
- 文件写入和业务表更新同时 rollback；
- 每个内部写步骤故障注入；
- 不出现半个 dentry 或缺块 manifest；
- savepoint/rollback-to 行为明确并一致。

### 并发

- 同名创建；
- 同文件双写；
- move 与 remove；
- workspace freeze 与写入；
- PG deadlock；
- DuckDB optimistic conflict；
- SQLite busy。

### 版本和恢复

- 快照不随当前状态变化；
- 单文件恢复；
- 目录恢复；
- 工作区恢复；
- retention 后受保护快照仍可读取；
- GC 不删除活动版本引用的 chunk。

### 数据库管理

- extension 安装、升级和卸载；
- 数据库原生备份恢复后 VexFS 文件完整；
- 数据库权限阻止内部表直写；
- 数据库 crash/reopen；
- 数据库只读模式；
- 无宿主允许时不创建线程或外部文件。

## 17. 实施顺序

详细阶段范围、状态和完成门槛只在
`docs/plans/2026-07-21_vexfs-final-goal-and-roadmap.md` 维护。技术实现顺序与之对应：

### Phase 0：统一合同与垂直闭环 — 已完成

- SQLite schema、SQL、C ABI、CLI 和 mount runtime 形成单一合同；
- macOS FSKit 跑通数据库权威状态、真实 Bash、Git、同步和恢复；
- Linux libfuse3 使用同一 runtime 跑通 root 和普通用户真实挂载；
- version、workspace commit、snapshot、restore 和基础 POSIX metadata 进入自动测试。

### Phase 1：SQLite 本地开发者预览 — 进行中

- 把 macOS FSKit 与 Linux FUSE 组合到同一 mount 一致性 eval；
- 补齐 utimens、O_APPEND 并发、flock/fcntl 最小合同、open-unlink、崩溃和强制卸载；
- 冻结缓存、TTL、owner/group 和便携 ACL 的跨平台规则；
- 验证 macOS 与 Linux 交换同一个 backup/export 后的内容、元数据、历史和快照；
- 完成 macOS 公证、Linux x86_64/AArch64 包、干净机器安装和卸载；
- 完成真实项目、Coding Agent、10 万文件和长时间运行 eval；
- 完成开发者预览所需的 retention、GC、quota、check 和 export/import 最小集合。

### Phase 2：PostgreSQL 共享工作区 Alpha — 功能与验收完成

- 已完成数据库内路径、内容、版本、workspace commit、snapshot 和冲突保护；
- 已完成 chunk/manifest、checksum/check、quota、retention、分批 GC 和 format v2；
- 已完成 libpq PG HostStore，没有重新定义文件语义；
- 已完成真实 role、principal、owner、ACL、完整变更审计和配额；普通 role 不能读取审计，
  `vexfs_check` 会检查审计对象与前后版本字段；
- 已完成多 gateway 锁、缓存失效、冲突、断线重连和数据库重启；
- 已完成两台 Mac 同时挂载下的 extension 崩溃、网络中断和数据库停机 25 项故障测试；
- 已完成 Linux root/uid 1000 helper 崩溃后的底层目录防误写、租约超时接管和 staging 发布；
- 已验证文件与普通业务 SQL 参加同一数据库事务；
- macOS FSKit、Linux FUSE、跨系统同一 PG、第二台 M1 Mac OpenCode 已通过；
- pg_dump/pg_restore、pg_basebackup 和逻辑导出恢复已通过；
- 16 MiB 有界备份性能与 RSS/OOM Gate 23 项已通过；当前审计实现下 format v2 导出/导入
  为 72.727/106.667 MiB/s，1 GiB 容器没有 OOM；
- 当前数据库源码的自动测试、Linux 真挂载、本机 PostgreSQL FSKit 13 组 247 项、凭据生命周期
  9 项、macOS↔Linux 跨 gateway 47 项、两台 Mac 文件/快照往返 50 项和 extension/网络/数据库
  故障恢复 25 项已完成；最后发行验收是从干净提交构建、公证并 staple `preview.38`，再在
  本机和第二台 Mac 重跑安装 Gate。局域网直连仍需第二台 Mac 的“本地网络”权限。

### Phase 3：Windows 与跨系统预览 — 未开始

- 实现 WinFsp adapter，复用 mount runtime；
- 单独处理盘符、CreateFile share mode、delete-pending、ACL/SID 和 reparse point；
- 验证 Windows SQLite 本地工作区和 PostgreSQL 远程工作区；
- 运行 macOS/Linux/Windows 三端合同和跨系统可移植性 eval。

### Phase 4：DuckDB 本地分析工作区 Beta — 未开始

- 实现 DuckDB HostStore 和相同 SQL/C ABI 能力矩阵；
- 验证事务冲突、checkpoint、reopen、backup/copy/export；
- 默认先提供 SQL、CLI 和 worktree；
- 只有 gateway 可以成为唯一写进程时才开放可写 mount。

### Phase 5：VexDB-Lite Files v1 — 未开始

- 稳定 SQL、C ABI、CLI、schema、能力协商和逻辑导出格式；
- 完成 retention、GC、quota、audit、check、安全和故障修复工具；
- 完成正式平台/数据库能力矩阵、升级保护、规模和长期回归；
- 形成同一版本的发行物、用户文档、管理员文档和故障处理手册。

当前编码优先级固定为：共用 mount eval → 时间戳/锁/append/生命周期 → macOS/Linux 可移植性 → 正式安装包 → 真实项目和规模基线。除关键设计验证外，不应跳过这些门槛提前展开 PostgreSQL、Windows 或 DuckDB 大规模实现。

## 18. 安装发布与可观测性

Phase 1 本地预览发行物是一组经过同版本测试的组合，而不是互不校验的二进制：

```text
VexFS.app
  └── VexFSFileSystem.appex
vexfs CLI
vexfs-sqlite extension
contract/schema version manifest

Linux package
  ├── vexdb/vexfs CLI
  ├── vexfs-fuse helper
  ├── vexdb_lite.so SQLite extension
  ├── install/uninstall + 中文说明
  └── contract/schema/source version manifest + SHA256SUMS
```

macOS 可挂载预览支持 macOS 26.0+ Apple Silicon。Phase 1 必须验证 Developer ID + 公证或 Mac App Store/TestFlight 至少一条真实分发路径；CI 检查 App、appex、CLI 的签名链、FSKit entitlement 和系统信任状态。首次启动必须引导用户在系统设置中启用文件系统扩展，不能尝试绕过系统授权。Linux 包必须检查 libfuse3、`fusermount3` 和 `/dev/fuse`，普通用户不能依赖 root helper 才能工作。

默认 SQLite 数据库位于 `~/Library/Application Support/VexDB-Lite/default.sqlite3`。每个 mount
创建不含密码的 `.vexfs-volume.json`，再把数据库父目录作为 security-scoped FSKit source
resource 传给系统；extension 只能打开描述文件指定的同目录数据库。用户选择其他位置时沿用
同一目录授权模型。卸载 App 不能自动删除数据库，数据删除必须是独立且明确的动作。

每次 mount 的启动顺序固定为：

1. 检查 macOS、CPU 架构、FSKit、App/appex 签名和 extension 启用状态；
2. 在数据库父目录创建并解析 `.vexfs-volume.json`，把该目录作为 security-scoped resource；
3. 打开 SQLite，加载 VexFS 扩展并认证 principal；
4. 查询 schema、合同版本和能力位；
5. 检查或恢复本 workspace 的未发布 handle；
6. 请求 FSKit mount，成功后才报告可写目录。

主合同版本不兼容时拒绝可写挂载。安全卸载先停止新 open，再执行 synchronize，列出未发布 staging，最后调用 FSKit unmount/unload；不能为了卸载干净而删除未确认数据。

gateway 至少公开以下状态和指标：

- 每类 FSKit callback 的次数、P50/P95/P99 和 POSIXError；
- 数据库调用耗时、SQLite busy 次数和重试次数；
- 活动 handle、dirty handle、recoverable staging 数量和字节数；
- publish/synchronize/close 成功、失败和结果不确定次数；
- expected version 冲突、配额失败和空间失败；
- 当前 principal、workspace、扩展版本、gateway 版本和合同版本；
- 最近一次成功 mount、unmount、数据库断线和恢复时间。

`vexfs doctor` 汇总上述关键状态，但日志和指标不能包含密码、文件正文或完整敏感路径。

## 19. 最终判断

VexFS 的产品本体是数据库 extension：

> **数据库管理 VexFS，VexFS 管理数据库中的文件。**

用户安装并完成对应系统授权后，优先通过 macOS FSKit、Linux libfuse3 或后续 Windows WinFsp mount 直接使用 Bash 和现有程序；SQL 是完整权威合同，CLI/worktree 是回退。mount adapter 只是访问路径，最终变更仍服从数据库事务、权限、版本、审计、备份和恢复规则。

这个设计同时满足两件事：用户获得真实文件路径，数据库继续拥有全部已提交权威状态。VexDB-Lite 的差异点不是单独做一个文件服务，而是让数据库扩展提供受管文件能力，再用 mount 把它交给 Bash。

实施上不按“三数据库 × 三操作系统”开发九套文件系统。SQLite 和 PostgreSQL 已经复用
macOS/Linux 的同一 mount runtime 与一致性测试。PostgreSQL 多用户共享工作区代码完成后，
下一步只选择 Windows WinFsp 或 DuckDB HostStore 其中一条线继续，不能重新复制文件语义。
