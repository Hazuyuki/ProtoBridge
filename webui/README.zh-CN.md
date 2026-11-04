# ProtoBridge Web Lab 使用说明

[English](README.md) · [简体中文](README.zh-CN.md)

WebUI 在浏览器中配置实验，由 Python 服务调用真实的 `gpu-cluster-sim`
执行仿真，再显示结果。前端使用原生 HTML、CSS 和 JavaScript，启动和使用均不需要
Node 或前端构建。界面署名为 **NICS-EFC**。

## 启动

在仓库根目录执行，需要 Python 3.10 或更新版本：

```bash
python -m pip install -r webui/requirements.txt
./ns3 build gpu-cluster-sim
python -m webui --port 8080
```

在运行服务的机器上打开 **http://127.0.0.1:8080**。如果要通过容器端口、实验室网络
或 SSH 转发访问，服务启动时使用：

```bash
python -m webui --host 0.0.0.0 --port 8080
```

服务共用一个队列和历史记录，没有用户账户，适用于可信的本地/实验室网络或 SSH 隧道。
修改 C++ 仿真器后需要重新构建，WebUI 不会自动编译。

### Docker、SSH 和 Mac

浏览器访问的地址必须能到达 Python 服务。前端和 API 共用一个端口。

| 使用方式 | 访问步骤 |
| --- | --- |
| 本机直接运行 | 打开 `http://127.0.0.1:8080`。 |
| VS Code Remote SSH / Dev Containers | 在 **Ports / 端口** 面板转发 `8080`，打开面板显示的本地地址。Mac 用 `⌘⇧P` 打开命令面板即可，不需要 F1。 |
| 新建本地 Docker 容器 | 创建时映射端口，例如 `docker run -p 127.0.0.1:8080:8080 …`；容器内服务监听 `0.0.0.0`。 |
| SSH 连接的服务器在本机回环地址提供 8080 | 在 Mac 终端执行下面的转发命令，再打开 `http://localhost:8080`。 |

```bash
ssh -N -L 8080:127.0.0.1:8080 user@server
```

如果 SSH 登录的是 Docker 宿主机，而现有容器没有映射端口，可以让隧道直接指向容器
的 bridge IP。以下方式要求 Linux 宿主机能访问该容器地址：

```bash
# 在 Docker 宿主机执行，将 CONTAINER 换为容器名或 ID。
docker inspect -f '{{range .NetworkSettings.Networks}}{{println .IPAddress}}{{end}}' CONTAINER
# 在 Mac 执行，将 CONTAINER_IP 换为上一步得到的地址。
ssh -N -L 8080:CONTAINER_IP:8080 user@server
```

保持 SSH 命令运行。容器重建后 IP 可能变化。如果 Mac 的 8080 已被占用，改成
`-L 18080:CONTAINER_IP:8080`，浏览器相应打开 `http://localhost:18080`。
Mac 浏览器中的 `localhost` 指 Mac 本机，因此只在远程容器启动服务并不足以访问。

## 中英文切换

右上角 **EN / 中文** 按钮即时切换语言。首次访问默认英文，即使浏览器首选语言为中文。
选择保存在当前站点的浏览器 local storage 中，刷新后继续使用。浏览器禁用存储时，
当前页面仍可切换，新页面回到英文。

切换会更新标签、帮助、校验错误、系统提示、运行状态和图表说明，同时保留未提交的
参数和源码编辑、下拉选项、当前运行、结果标签页及对比选择。切换不会提交、取消或
重启仿真。用户填写的实验名称、配置/profile 内容、CLI 参数、原生结果字段及日志保持
原文。系统生成的默认名称随界面翻译，新运行的默认名称以英文保存。

不同地址的偏好分别保存，例如 `localhost:8080`、`127.0.0.1:8080` 和
`localhost:18080` 属于不同的站点来源。

## 第一次实验

1. 选择“集合通信”，保留 `Ring AllReduce` 模板、8 个 GPU 和每 rank 1 MiB。
2. 点击“运行仿真”，等待状态变为“已完成”。结果显示仿真通信时长、精确的
   `simTimeNs`、报文/重传计数，以及该场景生成的步骤完成时序。
3. 把链路带宽改为 `85 Gbps`，再次运行。勾选两条完成记录，点击“对比已选”。
4. 打开“原始日志”核对原生输出，或下载完整运行记录。表格右侧的载入按钮可恢复
   某次实验的输入，再修改重跑。

程序实际执行花费的时间是 wall time，与模型中的仿真通信时长分开记录。只有完整
交付的实验参与完成时长对比；未完成、失败、取消和超时不会当作正常延迟。

### 配置的含义

配置选择器读取仓库自带的 11 个 `.cfg` 模板。可以分别选择 collective、算法和物理
拓扑，也可以导入或编辑 `.cfg` / `.profile`，在高级区填写经过校验的附加 CLI JSON。
每次运行保存独立快照，不会改写仓库 `configs/` 中的文件。

| 配置区 | 可以调整什么 |
| --- | --- |
| 通信与拓扑 | AllReduce、AllGather、ReduceScatter、AllToAll、Broadcast、Reduce，以及兼容的 Ring、Tree、Full mesh、NVLS/SHARP、Hierarchical、NCCL ring、Auto 算法。物理拓扑另选，提供 14 种原生拓扑。 |
| 多节点网络 | 8 种节点间拓扑、节点内 NVSwitch/Full mesh、Ethernet/RoCE/NVLink、节点数和每节点 GPU 数。 |
| 拓扑参数 | 所选网络的结构，例如 Leaf/Spine 数、网格行列、Torus 维度。部分拓扑由维度计算实际 GPU 数。 |
| 算法参数 | Broadcast/Reduce 的 root rank、NCCL ring 通道数、分层算法的节点内阶段。与拓扑结构分开设置。 |
| 分片与链路 | Lanes、每 GPU 链路数、分片大小及链路介质。 |
| 流控与拥塞控制 | VC credit、跨节点发送窗口、VOQ、PFC 模式和 headroom、DCQCN/ECN、RTTCC、每 QP 速率上限。 |
| 错误注入 | 统一或分层 BER、独立/突发错误、随机丢包及相关参数。 |
| 纠错与重传 | FEC 开关/范围及 RS 参数，LLR 开关、Go-back-N/SACK、重试次数、超时、缓冲区和溢出策略。 |
| 光链路故障 | 单节点光互联 Leaf–spine 的链路移除及原生路由重算。 |

不需要额外参数时，拓扑/算法参数区会隐藏。可视表单使用易读名称，提交的仍是
`allreduce`、`ncclring`、`leafspine`、`sack`、`ETH` 等原生值，语言不会影响参数。

留空表示沿用配置；明确填 `0` 表示实际数值，可能是关闭某个机制。硬件参数通常由
`[stack]` 覆盖 profile；可视表单填写的值覆盖对应配置或附加 CLI 值。部分原生路径
优先使用 profile/[stack]，请看有效配置与提示，不要只看附加 CLI。rank 从 0 编号；
每项单位写在字段旁，时延通常为 ns，PFC 暂停时间为 µs。

请求/响应等自定义传输由 `transfer.*.bytes` 表达式决定实际大小，表单 `dataSize`
可能只是表达式的一个输入。`seed` 实际映射到 ns-3 的 `RngRun`，不是 `RngSeed`。

### 丢包与恢复实验

选择“多节点网络”的“多节点 AllReduce”，在“错误注入”点击 **0.1% 丢包 + SACK**。
这个预设设置 4 KiB 分片、SACK、超时主动重传、200 µs 重传超时及足够的 credit；
多节点还设置 64 个在途分片的发送窗口。核对有效配置后运行，观察重传和完成状态。
“设为无错误”会清除统一/分层 BER 和随机丢包。统一 BER 覆盖原有分层值，随后仍可
为某一层单独设置。

NVLS/SHARP 需要单节点 NVSwitch；Hierarchical 需要节点分组。非分层 AllToAll 的
多个算法名称共用 `AlltoAllInjector`，CollNet 是该路径别名。无效的 FEC 参数以及
PFC/ECN/丢包阈值顺序会在启动进程前报错。Ethernet、部分 lossy RoCE 或 PFC headroom
路径会强制开启 LLR，预览会标出这一点。

当前原生 `flowControl=window/rate` 是预留接口，界面提供已经实现的 `soTxWindow`
及 DCQCN/RTTCC。原生多机架 `2dfullmeshclos` 路由尚不完整，界面不提供这一选择。
突发错误实验要求关闭 FEC 并设置正的统一 BER，因为原生 FEC 接收路径使用独立误码
解码模型。FEC 开关已经传入 profile 及委托执行路径，更新后请重新构建仿真器。

### 如何读图

拓扑图表示选择的网络结构，是示意图。步骤时序图使用真实 `stepTraceFile` 记录，
每个点表示相对 `collectiveStartNs` 的完成时刻，并非单步持续时间。未输出步骤数据的
场景会显示说明。页面最多画 16 个 rank，完整记录可从 `steps.csv` 下载。

## 存储与运行限制

默认数据位于仓库之外的 `~/.cache/protobridge-web/runs/`。可自定义路径：

```bash
python -m webui --data-dir /path/to/run-storage
```

每次运行保存 `config.cfg`、`profile.profile`、`stdout.log`、`result.json`，以及场景
实际输出的 `steps.csv`。记录包含请求、有效参数、原生结果、精确 argv、Git 版本、
二进制 SHA-256 和 wall time；配置快照中的 profile 路径就是执行时使用的绝对路径。

队列一次执行一个仿真，最多接收 16 个未完成任务。默认 wall time 上限 120 秒，最多
300 秒。取消或超时会终止进程组。Linux 上存在 `prlimit` 时，子进程另有限制：8 GiB
地址空间、32 MiB 文件大小、禁用 core dump；所有支持平台均监控 16 MiB 日志上限。
进程通过参数数组启动；下载只允许当前运行的已知文件。

服务重启后保留历史，未结束任务标记为“已中断”，不会自动重跑。使用一个 Uvicorn
worker，`python -m webui` 已固定为 1。也可以运行
`uvicorn webui.app:app --workers 1`。环境变量 `PROTOBRIDGE_WEB_REPO` 和
`PROTOBRIDGE_WEB_DATA_DIR` 分别指定仓库和数据目录，对应 `--repo`、`--data-dir`。

## 常见问题

| 现象 | 检查方法 |
| --- | --- |
| Mac 打不开 `localhost:8080` | 确认 SSH 隧道还在运行，目标是服务所在的容器地址或已映射端口，而不是另一台机器的回环地址。 |
| 容器内可访问，外部不行 | 确认监听 `0.0.0.0`，并检查端口映射或转发目标。 |
| 提示需要构建仿真器 | 在服务使用的仓库执行 `./ns3 build gpu-cluster-sim`。 |
| 下拉框一直显示加载中 | 检查页面连接错误和浏览器控制台后刷新。HTML/静态文件会重新校验，资源 URL 包含内容哈希。 |
| 无法加载翻译 | 恢复服务连接后刷新；加载失败时页面使用英文并暂时禁用语言按钮。 |
| 换 URL 后语言改变 | 偏好按协议、主机名、端口分别保存。 |
| 无法运行或对比 | 阅读表单校验信息；对比只接受完成的实验，未完成交付没有正常完成延迟。 |
| 参数看起来没生效 | 核对有效配置、提示和运行快照；部分路径会强制开启 LLR 或优先使用 profile/[stack]。 |

## 测试和维护

```bash
python -m pip install -r webui/requirements-dev.txt
python -m pytest webui/tests -q
node --check webui/static/app.js
node --check webui/static/options.js
node --check webui/static/i18n.js
```

API 测试使用明确的假程序验证失败、超时、取消、队列继续执行、持久化、快照、下载和
CSV 等路径。`test_native_options.py` 另用真实二进制检查 FEC、collective、光链路移除
和丢包恢复，没有构建二进制时跳过。翻译测试检查双语占位符一致性、默认英文错误及
源码/用户命名边界。Python 检查和原生测试会在 CI 构建后一起运行。

可选浏览器检查会提交两次真实仿真。请另开一个使用外部空目录的服务：

```bash
python -m webui --port 8081 --data-dir /tmp/protobridge-browser-runs
```

在另一终端安装检查工具并运行，依赖留在仓库之外：

```bash
npm install --prefix /tmp/protobridge-browser-tools playwright
/tmp/protobridge-browser-tools/node_modules/.bin/playwright install --with-deps chromium
NODE_PATH=/tmp/protobridge-browser-tools/node_modules node webui/tests/browser_i18n.cjs http://127.0.0.1:8081
```

它验证英文默认值、语言记忆、存储受限、翻译加载失败、参数保留、错误翻译、真实结果
一致性、对比和移动端布局。测试结束后停止临时服务。Node/Playwright 仅用于这个可选
检查，不是 WebUI 运行依赖。

HTTP API 说明位于运行中服务的 `/docs`。常用端点包括 `/api/catalog`、`/api/status`、
`POST /api/preview`、`POST /api/runs`、`GET /api/runs/{id}`，以及该运行下的 `cancel`、
`trace`、`log`、`artifacts/{filename}`；`/api/runs/export.csv` 导出历史。
API 的应用提示和元数据使用英文，浏览器负责中文显示。状态值、参数、结果字段和
下载内容不随语言改变。运行记录的 `name_is_default` 用于区分系统默认名和用户命名，
旧中文系统记录仅在展示时翻译，不改写保存文件。

`static/locales.json` 是前后端共享的文案表，以英文短语为 ID，包含英文和中文及兼容
旧中文消息的 `aliases`。`static/i18n.js` 处理静态绑定和动态文本，`webui/i18n.py`
规范 API 的应用文案。新增文案时同时维护两种语言及相同的 `{placeholder}`，静态
节点加 `data-i18n`，动态文本使用 `t()`；参数值、源码和日志不进入翻译。脚本和翻译
JSON 都有独立内容哈希，修改后不会继续使用旧 URL。
