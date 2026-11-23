# MEA 窗口到 Unitree G1 速度指令

`mea_g1` 每行接收一个符合 `DecodedEvent v1`（参见上级目录 `schema/decoded_event.v1.json`）的已解码 MEA 窗口，根据 `decision.status` 和 `intent` 生成速度指令 `(vx, vy, vyaw)`。默认后端将每条结果写为一行 JSON；启用 Unitree SDK2 的构建也默认使用日志后端，直到显式传入 `--g1 <网卡>`。

## 构建依赖

目标机需要 Linux、支持 C++17 的编译器、CMake 3.16 或更新版本，以及 nlohmann JSON 头文件。在 Ubuntu 或 Debian 上安装：

```bash
sudo apt update
sudo apt install build-essential cmake nlohmann-json3-dev
```

默认启用测试，需要 Python 3.7 或更新版本。回放测试仅使用 Python 标准库。只构建程序时，在配置命令中加 `-DBUILD_TESTING=OFF`，并跳过 `ctest`。

迁移时复制 `CMakeLists.txt` 和 `src/`，回放或运行测试时带上 `examples/`，运行测试时再带上 `tests/`。在目标机重新生成 `build/` 和 `build-g1/`，因为 CMake 缓存记录了原机器上的绝对路径。

## 构建和回放

```bash
cmake -S . -B build
cmake --build build -j
(cd build && ctest --output-on-failure)

./build/mea_g1 --capabilities
./build/mea_g1 --input examples/mock_windows.jsonl
./build/mea_g1 < examples/mock_windows.jsonl
```

两种回放方式的输出一致。日志记录 `event_id`、`action`、`vx`、`vy`、`vyaw`、`reason` 和 `applied`。在日志后端，`applied` 表示生成了速度指令；在 G1 后端，`applied` 表示 `LocoClient::Move` 返回 `0`。重复事件的 `action` 为 `skip`，`applied` 为 `false`。无法读取事件 ID 时记录 `null`。

| 意图 (`intent.name`) | `parameters.direction` | `action` | `vx`（m/s） | `vy`（m/s） | `vyaw`（rad/s） |
| --- | --- | --- | ---: | ---: | ---: |
| `locomotion.translate` | `forward` | `forward` | `+linear` | `0` | `0` |
| `locomotion.translate` | `backward` | `backward` | `-linear` | `0` | `0` |
| `locomotion.translate` | `left` | `strafe_left` | `0` | `+lateral` | `0` |
| `locomotion.translate` | `right` | `strafe_right` | `0` | `-lateral` | `0` |
| `locomotion.turn` | `left` | `turn_left` | `0` | `0` | `+angular` |
| `locomotion.turn` | `right` | `turn_right` | `0` | `0` | `-angular` |
| `locomotion.stop` | `{}` | `stop` | `0` | `0` | `0` |

可使用 `--linear-speed`、`--lateral-speed`（省略时跟随 `--linear-speed`）、`--angular-speed` 和 `--watchdog-ms`（50–1000，默认 500）调节参数；在无真机环境演练实时安全看门狗与过期机制时，可传入 `--live`（仅接受标准输入）。

## `DecodedEvent v1` 协议

`mea_g1` 严格按正式 `DecodedEvent v1` 解析：

- `schema_version`: 必须为整数 `1`（旧版 `"mock-v1"` 或其他版本触发 `unsupported_schema` 停车）。
- `event_id`: 非空字符串，每个进程维护有界去重表（最近 100,000 条）避免重复执行。
- `decision.status`: 必须为 `"accepted"` 或 `"rejected"`。
- `intent`: 接受时为 `{"name": "...", "parameters": {...}}`，拒判时必须为 `null`。
- `live_timing`: 离线回放为 `null`；实时模式（`--live` 或 `--g1`）下非零动作必须提供包含正整数 `issued_at_unix_ms` 与 `expires_at_unix_ms`（且 `expires > issued` 且尚未过期）的对象。
- 单行上限 1 MiB，超长行触发 `line_too_long` 停车并丢弃到该行结束。实时生产者应以 `\n` 提交每条窗口并立即刷新标准输出。

## 可选 G1 真机后端

在基础构建依赖之外，启用 G1 后端还需要安装与目标机架构匹配的 Unitree SDK2（已用 SDK2 2.0.0 验证）及其 Cyclone DDS 动态库 `libddsc.so.0` 和 `libddscxx.so.0`。

```bash
cmake -S . -B build-g1 -DMEA_ENABLE_G1=ON -DCMAKE_PREFIX_PATH=/opt/unitree_robotics
cmake --build build-g1 -j
(cd build-g1 && ctest --output-on-failure)
```

实机运行时通过标准管道对接上游流式解码器（例如 `python -m organoid_g1.live_cli` 或 Web 实操平台）：

```bash
python -m organoid_g1.live_cli bxr \
  --recording /data/20260826-0903-D90-CTRL/20260904-90-4_00.bxr \
  --decoder results/d90-g1-pilot-01-v1/decoder.npz \
| ./build-g1/mea_g1 --g1 "$G1_NIC" \
  --linear-speed 0.15 --lateral-speed 0.12 --angular-speed 0.25
```

SDK 调用使用 `LocoClient::Move(vx, vy, vyaw, false)`（请求超时 100 毫秒），每条指令发送后同时向标准输出写入相同的 JSON 审计日志。

- 拒判、空意图、无效记录、未注册意图、旧协议版本和过期窗口均触发零速度。实时非零动作缺少有效时间戳时也发送零速度。
- 当前动作在窗口到期或距接收满 `--watchdog-ms`（默认 500 毫秒）时触发停车，取较早者。重复窗口和未完成行无法刷新计时。
- EOF、`SIGINT`、`SIGTERM`、输入错误或 SDK 调用失败时，程序尝试发送零速度后退出。SDK 错误输出到标准错误并返回非零退出码。
