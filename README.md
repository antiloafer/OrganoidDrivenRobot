# MEA 窗口到 Unitree G1 速度指令

`mea_g1` 每行接收一个已经解码的 MEA 窗口，根据 `decision.accepted` 和 `intent` 生成速度指令。默认后端将每条结果写为一行 JSON。启用 SDK2 的构建也默认使用日志后端。

## 构建依赖

目标机需要 Linux、支持 C++17 的编译器、CMake 3.16 或更新版本，以及 nlohmann JSON 头文件。在 Ubuntu 或 Debian 上安装：

```bash
sudo apt update
sudo apt install build-essential cmake nlohmann-json3-dev
```

默认启用测试，需要 Python 3.7 或更新版本。在 Ubuntu 或 Debian 上可用 `sudo apt install python3` 安装。回放测试仅使用 Python 标准库。只构建程序时，在配置命令中加 `-DBUILD_TESTING=OFF`，并跳过 `ctest`。

迁移时复制 `CMakeLists.txt` 和 `src/`，回放或运行测试时带上 `examples/`，运行测试时再带上 `tests/`。在目标机重新生成 `build/` 和 `build-g1/`，因为 CMake 缓存记录了原机器上的绝对路径。

## 构建和回放

```bash
cmake -S . -B build
cmake --build build -j
(cd build && ctest --output-on-failure)

./build/mea_g1 --input examples/mock_windows.jsonl
./build/mea_g1 < examples/mock_windows.jsonl
```

两种回放方式的输出一致。日志记录 `event_id`、`action`、`vx`、`vy`、`vyaw`、`reason` 和 `applied`。在日志后端，`applied` 表示生成了速度指令。重复事件的 `action` 为 `skip`，`applied` 为 `false`，该行的零值是日志占位。无法读取事件 ID 时记录 `null`。

| 样例事件 | 意图 | `vx`（m/s） | `vy`（m/s） | `vyaw`（rad/s） |
| --- | --- | ---: | ---: | ---: |
| `mock-01` | `locomotion.translate` + `forward` | +0.2 | 0 | 0 |
| `mock-02` | `locomotion.translate` + `backward` | −0.2 | 0 | 0 |
| `mock-03` | `locomotion.turn` + `left` | 0 | 0 | +0.3 |
| `mock-04` | `locomotion.turn` + `right` | 0 | 0 | −0.3 |
| `mock-05` | `locomotion.stop` | 0 | 0 | 0 |

其余样例覆盖拒判、重复事件、`translate + left` 横移、未知意图、空意图和坏行。可用 `--linear-speed` 和 `--angular-speed` 调整回放速度的幅值。

## 临时 JSONL 协议

字段参考见 [json_config.jpg](json_config.jpg)。临时协议使用 `schema_version: "mock-v1"` 和非空字符串 `event_id`。`decision.accepted` 是解码器给出的布尔接受状态。接受的窗口通过 `intent.type` 和 `intent.params.direction` 表达指令。停车意图可省略 `params`，拒判窗口可使用 `intent: null`。

离线样例使用 `live_timing: null`。实时模式的非零动作要求 `live_timing.expires_at_unix_ms` 为尚未到期的正整数 Unix 毫秒时间戳。每个进程按 `event_id` 对已解析窗口去重。单行上限为 1 MiB，超长行触发停车并丢弃到该行结束。实时生产者应以换行符提交每个窗口并及时刷新标准输出。离线回放也接受省略末尾换行符的最后一条记录。

样例保留了分类分数、解码器标识和采样元数据。控制器直接使用接受状态。拿到真实数据后，可在 `src/mea_json.cpp` 对齐协议字段。

## 可选 G1 后端

在基础构建依赖之外，启用 G1 后端还需要安装与目标机架构匹配的 Unitree SDK2。本项目已用 SDK2 2.0.0 验证。同架构迁移时可复制完整的 SDK2 安装目录，保留头文件、库、CMake 配置和符号链接。换到 ARM64 等其他架构时，安装对应架构的 SDK2 后重新构建。 [Unitree SDK2 安装说明](https://github.com/unitreerobotics/unitree_sdk2#installation)

G1 版在运行时需要 SDK2 配套的 Cyclone DDS 动态库 `libddsc.so.0` 和 `libddscxx.so.0`。它们位于 SDK2 安装目录的 `lib/` 下。下例假设 SDK2 安装在 `/opt/unitree_robotics`，安装在其他位置时修改 `CMAKE_PREFIX_PATH`。

```bash
cmake -S . -B build-g1 -DMEA_ENABLE_G1=ON -DCMAKE_PREFIX_PATH=/opt/unitree_robotics
cmake --build build-g1 -j
(cd build-g1 && ctest --output-on-failure)
```

实机运行还需要单独部署实际的 MEA 解码器及其运行环境和配置。以下命令中的 `./live-mea-decoder` 是占位名称，实际解码器应通过标准输出提供 JSONL。操作者先将 G1 切到可接受速度指令的状态，再按目标机的网卡名设置 `G1_NIC`，并设置 `G1_LINEAR_SPEED` 和 `G1_ANGULAR_SPEED`。

```bash
./live-mea-decoder | ./build-g1/mea_g1 --g1 "$G1_NIC" \
  --linear-speed "$G1_LINEAR_SPEED" --angular-speed "$G1_ANGULAR_SPEED"
```

两个速度幅值必须显式提供，并为可表示为 `float` 的有限正数。G1 后端仅从标准输入接收窗口。SDK 调用使用 `LocoClient::Move(vx, vy, vyaw, false)`，每条指令的 SDK 时长为 1 秒，停车使用全零速度。SDK 请求超时设置为 100 毫秒。 [Unitree G1 SDK 实现](https://github.com/unitreerobotics/unitree_sdk2/blob/main/include/unitree/robot/g1/loco/g1_loco_client.hpp)

- 拒判、空意图、无效记录、未注册意图和过期窗口触发零速度。实时非零动作缺少有效时间戳时也发送零速度。
- 当前动作在窗口到期或距接收该有效动作满 500 毫秒时触发停车，取两者中较早者。重复窗口和未完成的输入行无法刷新计时。
- EOF、`SIGINT`、`SIGTERM`、输入错误或 SDK 调用失败时，程序尝试发送零速度后退出。SDK 错误输出到标准错误并返回非零退出码。

两种构建均可运行相同的回放和假后端测试。测试覆盖速度符号、输入中断、过期、去重、退出和后端失败。实机联调时确认实际网卡和机器人方向。
