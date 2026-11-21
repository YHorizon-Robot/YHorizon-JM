# YHorizon-JM 关节电机通信 SDK

应用程序通过 CAN 适配器直接连接电机。Python 和 C++17 使用同一份原生通信库，可共用一个总线连接访问节点 1～63。固件 1.6 支持 1～16 轴的预装轨迹同步。

## 文件与源码

- `src/yfoc-motor-sdk/src/` 包含[单轴控制](src/yfoc-motor-sdk/src/joint.rs)、[多轴同步](src/yfoc-motor-sdk/src/multi_axis.rs)及 [C ABI 实现](src/yfoc-motor-sdk/src/ffi.rs)。
- [CAN 适配器实现](src/yfoc-can/src/)包含 PCAN、SocketCAN 和 SLCAN 后端。[协议源码](src/yfoc-protocol/src/lib.rs)负责 CAN 帧编解码。
- `python/yfoc_motor.py` 提供 Python API，使用标准库 ctypes。
- `include/yfoc_motor.hpp` 提供 C++17 API，`include/yfoc_motor.h` 定义底层 C ABI。
- `lib/` 存放当前平台的动态库。Windows x64 包还附带 MSVC 和 MinGW 导入库。
- `examples/read_motor.py` 和 `examples/inspect.cpp` 是只读的设备查询示例。

## Python

把 `sdk/python` 加入应用的 `PYTHONPATH`，保留旁边的 `sdk/lib`。如果将动态库放在其他位置，可通过 `YFOC_SDK_LIBRARY` 指定其绝对路径。

```python
from yfoc_motor import Bus

with Bus("socketcan", "can0") as bus:
    info = bus.info(1)
    sample = bus.feedback(1)
    print(info.fw_major, info.fw_minor, sample.position, sample.velocity)
```

在 Windows 上用 `Bus("pcan", "PCAN_USBBUS1")`。在 sdk 目录运行 `python examples/read_motor.py pcan PCAN_USBBUS1 1` 或 `python3 examples/read_motor.py socketcan can0 1` 即可查询节点。

## C++

在已有 CMake 项目中加入 SDK 目录并链接导入目标。

```cmake
add_subdirectory(path/to/sdk yfoc-sdk)
target_link_libraries(my_app PRIVATE yfoc::motor)
```

```cpp
#include <yfoc_motor.hpp>

int main() {
    yfoc::Bus bus("socketcan", "can0");
    const auto sample = bus.feedback(1);
    return 0;
}
```

独立构建示例可在 sdk 目录执行 `cmake -S . -B build` 和 `cmake --build build --config Release`。Windows 部署时将 `yfoc_motor_sdk.dll` 放在程序旁边。Linux 应让动态加载器能找到 `libyfoc_motor_sdk.so`。

## 控制接口

- `info`、`status`、`feedback` 主动查询设备信息、故障及输出轴状态。`wait_feedback(node, timeout_ms)` 等待普通运动反馈，超时返回 Python 的 `None` 或 C++ 的空 `optional`。普通反馈中的 `torque` 为输出轴指令力矩，单位 Nm。
- `current` 读取 Id、Iq、Iq 指令（A）及 Vq（pu），`current_config` 读取电流环配置。`set_inner` 切换内环。
- 在发送目标前，用 `set_mode` 选好位置或速度模式。`set_position(node, rad, max_rad_s)` 和 `set_velocity(node, rad_s)` 均以输出轴计。`motion_setup(node, kp, kd)` 选择 Motion 模式并写入增益，随后用 `set_motion(node, rad, rad_s, torque_nm)` 下发目标。
- 普通模式保持最后目标，结束时调用 `disable(node)` 并等待关桥应答。`close()` 释放 CAN 连接。普通模式同时操作多轴时，应逐轴收发，等待指定节点应答会消费总线上的其他帧。
- `run_group(axes, seconds, on_cycle)` 阻塞执行同组一至两段轨迹。每 5 ms 同步并接收每轴反馈，运行中可从另一个线程调用 `cancel_group()`。同一总线上的其他事务在组运行期间返回忙。参考位置和实测位置的单位均为 rad。

下面的 Python 示例在两台电机的当前位置执行一段同步保持，轨迹完成后固件关桥。将节点号及目标改为实际台架所需的值。

```python
from yfoc_motor import Axis, Bus

with Bus("socketcan", "can0") as bus:
    axes = [Axis(node, bus.feedback(node).position) for node in (1, 7)]
    bus.run_group(axes, seconds=1.0)
```

对应的 C++ 调用为 `bus.run_group({{1, p1}, {7, p7}}, 1.0);`。如需记录反馈，Python 回调接收一组 `Sample`，返回 `False` 可取消。C++ 回调接收样本指针和样本数，返回 `true` 继续，指针仅在该次调用内有效。回调在 CAN 调度线程上执行。取消或故障由 Python 的 `MotorError`、C++ 的 `std::runtime_error` 报告。固件 1.6 在启动前装入目标；运行期间每拍反馈，逐拍替换整组目标需要扩展固件协议。

## 适配器与源码构建

Windows PCAN 需要已安装 PEAK 驱动及 64 位 PCAN-Basic。Ubuntu 使用 SocketCAN，接口名称和波特率由系统配置。可用 `ip -details link show can0` 核对链路，组轨迹要求经典 CAN 1 Mbit/s。

工程中的 `sdk/` 目录和各平台 SDK 发布包均包含完整的 SDK 源码和 Cargo 锁文件。进入含本文件的目录后，安装 Rust 工具链并执行以下命令即可独立测试和构建。Ubuntu 还需安装 `build-essential pkg-config libudev-dev`，并将下面的 `python` 换成 `python3`。

```sh
cargo test --workspace --locked
python build_native.py
```

生成的动态库位于当前目录的 `lib/`，Python 和 C++ 接口都从这里加载。
