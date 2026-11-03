# 异构2D图形渲染加速引擎 — 源代码包说明

平台：易灵思 Ti60F225I3（Efinity 2023.2 及以上）+ Sapphire RISC-V 软核
输出：HDMI 1280×720@60，RGB565，双缓冲换页

## 目录结构

```
rtl/                          RTL 源码（全部自研部分集中在前三个目录）
  ddr3_example_top.v          板级顶层：CPU / BitBlt / 显示三主机接入
  define.v                    全局参数
  bitblt/                     BitBlt 加速器本体
    bitblt_core.v             任务 FIFO（16 深）、按行拆解、读/算/写三级调度
    bitblt_sched.v            行级信用与描述符生成
    bitblt_pix.v              像素流水线：FILL / COPY / KEY / ALPHA，8 像素/拍
    bb_fifo.v                 同步 FIFO（SRC 1024×145、DST 512×145）
  common/
    Ddr_Read.v                128 bit AXI4 读主机 + 突发聚合
    Ddr_Write.v               128 bit AXI4 写主机 + WSTRB 掩码
    simple_dual_port_ram.v    简单双口 RAM
  fb/                         显示子系统
    fb_page_ctrl.v            双缓冲换页（FRAME_LAST）
    fb_regs_axil.v            AXI4-Lite 控制口与提交参数校验
    fb_hdmi_subsys.v          CEA-861 1280×720@60 时序与上屏
    fb_read_path.v / hdmi_rd_master.v / pix_reader.v   显示读通路
    axi_mux_3to1.v / axi_mux_2to1.v   AXI 仲裁（HDMI 读 > CPU > BitBlt）
    fps_overlay.v             硬件 FPS 叠字（5×7 点阵 ×4）
  hdmi/                       dvi_encoder.v / video_timing.v 等 TMDS 编码与视频时序
  ddr3_controller/            DDR3 控制器封装
  memory_test/                DDR3 带宽与误码测试（独立工程，不在演示路径上）

par/ddr_demo_ti60/            Efinity 工程
  ddr_demo_ti60.xml           工程文件（器件、顶层、源文件列表）
  *.peri.xml                  引脚分配
  sdc/                        时序约束
  src/                        工程顶层
  ip/                         锁相环、DDR3 控制器等 IP 配置
  outflow/ddr_demo_ti60.bit   已生成的 bitstream

embedded_sw/bbShow/           RISC-V 固件（Sapphire，裸机）
  src/main.c                  主循环、场景调度、帧率统计、串口日志
      scenes.c                10 个演示场景（含片上自校验）
      game.c                  STARFALL 弹幕游戏与自动驾驶
      bitblt_driver.c         引擎驱动（FILL/COPY/KEY/ALPHA，含绝对地址版本）
      swr.c / font.c / art.c / content.c   软件渲染对照与图形素材
  tools/                      主机侧工具链（见下）
  build/                      编译产物 bbShow.elf / .bin / .hex / .map
  build.ps1                   一键编译脚本

docs/                         设计报告（.docx）、生成脚本与全部插图
```

## 编译

**RTL**：Efinity 打开 `par/ddr_demo_ti60/ddr_demo_ti60.xml`，依次 Run Synthesis / Place & Route / Generate Bitstream。

**固件**：运行 `embedded_sw/bbShow/build.ps1`（需 Efinity 自带 RISC-V GCC 在 PATH 中）。

**主机侧工具**（`tools/`，Python 3 + Pillow / OpenCV）：

| 脚本 | 作用 |
|---|---|
| `img2assets.py` | 图片 / GIF → RGB565 图集（`assets.bin`） |
| `load_assets.ps1` | 通过调试器把 `assets.bin` 写入 DDR 0x0600_0000 |
| `host_preview.c` | PC 端用同一份 `scenes.c` 渲染 10 个场景为 PPM，无需上板即可检查排版 |
| `grab_shots.py` | 从录像中取帧，供设计报告使用 |
| `check_layout.py` | 静态检查界面元素是否出屏 / 重叠 |
| `serial_log.ps1` / `goto_scene.bat` | 串口日志与场景跳转 |

## 运行要点

1. 先烧写 bitstream，再下载 `bbShow.elf` 运行。
2. `assets.bin` 需单独写入 DDR（`load_assets.ps1`），否则 IMAGES 场景显示 NONE LOADED。
3. 场景自动轮播：除极限测试（15 s）外每张卡 10 s；游戏场景在 Boss 被击破后自动转场。
4. 串口 115200，每秒打印一行 `hw / cpu / sprites / frames / sim` 统计，可据此复现实测数据。

## 实测数据（1280×720@60）

| 项目 | 结果 |
|---|---|
| 整屏 FILL | 1.59 ms，写速率 1156 MB/s |
| 整屏 COPY | 2.98 ms，618 MB/s |
| 整屏 ALPHA | 4.53 ms |
| CPU 纯软件对照 | 50~76 ms/帧，整屏刷新 7~8 Hz |
| 片上自校验 | 4 项各 40 960 像素，共 163 840 像素，0 失配 |
| 60 FPS 精灵数 | AIMD 探针实测 1024（KEY 与 ALPHA 同时开启） |
