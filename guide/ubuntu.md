# Ubuntu 上的上位机和 SDK

[返回说明](README.md)

预编译包面向 Ubuntu 24.04 x86_64。上位机里有电机控制、固件烧录、校准与辨识、多轴同步。SDK 单独使用时，Python 和 C++ 都可以直接访问 CAN 适配器。

## 上位机

解压上位机的 `.tar.gz` 后，在解压目录执行：

```sh
./yfoc-host --version
./yfoc-host --list-can --interface socketcan
./yfoc-host --gui
```

图形界面需要 X11 或 Wayland、OpenGL/EGL 和中文字体。精简系统可以安装：

```sh
sudo apt-get install libudev1 libxkbcommon-x11-0 libx11-6 libxcursor1 libxi6 libxrandr2 libegl1 libgl1 libgl1-mesa-dri fonts-noto-cjk
```

烧录页使用 SWD 探针时，当前用户要有该探针的 USB 权限。镜像用本仓库编译出的 `firmware/build/gd32f303/YHorizon-JM.bin`。

## CAN 与 SDK

先用 `ip -br link` 看接口名。下面把处于 DOWN 的经典 CAN 接口设成 1 Mbit/s，把 `can0` 换成实际名称：

```sh
sudo ip link set can0 type can bitrate 1000000
sudo ip link set can0 up
ip -details link show can0
./yfoc-host --scan --interface socketcan --channel can0
```

在 SDK 包目录里可以跑只读示例：

```sh
python3 examples/read_motor.py socketcan can0 1
cmake -S . -B build && cmake --build build
./build/yfoc_inspect socketcan can0 1
```

Python 把 SDK 的 `python` 目录加入 `PYTHONPATH`。C++ 链接 CMake 目标 `yfoc::motor`。通信库是同一包里的 `lib/libyfoc_motor_sdk.so`。在 WSL 里连实机时，先在 Linux 里确认 CAN 接口已经出现。Windows 的 `PCAN_USBBUS1` 只在 Windows 上使用。

## 从源码构建

在仓库根目录：

```sh
sudo apt-get install build-essential cmake pkg-config libudev-dev
cargo build --release --locked --manifest-path host-rs/Cargo.toml -p yfoc-host
python3 sdk/build_native.py
```

上位机在 `host-rs/target/release/yfoc-host`，动态库在 `sdk/lib`。SDK 发布包解压后，在该包根目录执行 `python3 build_native.py` 可以单独重建动态库。
