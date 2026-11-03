# -*- coding: utf-8 -*-
"""Build the design report .docx from scratch (no external dependencies).

Every paragraph/table/figure is declared in CONTENT below; the writer emits the
OOXML parts and zips them.  Keeping it dependency free means the report can be
regenerated on any machine that has Python.
"""
import os
import zipfile
from xml.sax.saxutils import escape

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "异构2D图形渲染加速引擎-设计报告.docx")

# if the report is open in Word/WPS the file is locked - write a sibling instead
try:
    with open(OUT, "ab"):
        pass
except PermissionError:
    OUT = OUT[:-5] + "-new.docx"
    print("WARNING: original is locked, writing", os.path.basename(OUT))

CW = 8306          # content width in twips (A4, 1.25 inch side margins)

# --------------------------------------------------------------------------- #
#  helpers
# --------------------------------------------------------------------------- #

def esc(s):
    return escape(s)


def run(text, bold=False, size=None, color=None, font=None, italic=False):
    rpr = ""
    if font:
        rpr += '<w:rFonts w:ascii="%s" w:hAnsi="%s" w:eastAsia="%s"/>' % (font, font, font)
    if bold:
        rpr += "<w:b/>"
    if italic:
        rpr += "<w:i/>"
    if color:
        rpr += '<w:color w:val="%s"/>' % color
    if size:
        rpr += '<w:sz w:val="%d"/><w:szCs w:val="%d"/>' % (size, size)
    r = "<w:r>"
    if rpr:
        r += "<w:rPr>%s</w:rPr>" % rpr
    r += '<w:t xml:space="preserve">%s</w:t></w:r>' % esc(text)
    return r


def para(children, style=None, align=None, indent=True, before=0, after=0,
         line=None, keep=False, outline=None):
    ppr = ""
    if style:
        ppr += '<w:pStyle w:val="%s"/>' % style
    if keep:
        ppr += "<w:keepNext/>"
    if line:
        ppr += '<w:spacing w:line="%d" w:lineRule="auto"/>' % line
    if before or after:
        ppr += '<w:spacing w:before="%d" w:after="%d"/>' % (before, after)
    if indent:
        ppr += '<w:ind w:firstLineChars="200" w:firstLine="480"/>'
    if align:
        ppr += '<w:jc w:val="%s"/>' % align
    if outline is not None:
        ppr += '<w:outlineLvl w:val="%d"/>' % outline
    if isinstance(children, str):
        children = [run(children)]
    body = "".join(children)
    if ppr:
        return "<w:p><w:pPr>%s</w:pPr>%s</w:p>" % (ppr, body)
    return "<w:p>%s</w:p>" % body


def h1(text):
    return para([run(text, bold=True, size=30, font="黑体")], align="left",
                indent=False, before=240, after=160, line=360, keep=True)


def h2(text):
    return para([run(text, bold=True, size=26, font="黑体")], align="left",
                indent=False, before=180, after=120, line=340, keep=True)


def h3(text):
    return para([run(text, bold=True, size=24, font="黑体")], align="left",
                indent=False, before=140, after=100, line=320, keep=True)


def p(text, bold=False):
    return para([run(text, bold=bold)], line=360)


def pn(text):
    """paragraph without first line indent (for list-ish lines)"""
    return para([run(text)], indent=False, line=360)


def bullet(text, mark="•"):
    return para([run(mark + " "), run(text)], indent=False, line=340)


def caption(text):
    return para([run(text, size=20, color="404040")], align="center",
                indent=False, before=60, after=160)


def table(rows, widths, header=True, font_size=20, align_cols=None):
    assert sum(widths) == CW, (sum(widths), CW)
    tb = ['<w:tbl><w:tblPr><w:tblW w:w="%d" w:type="dxa"/>' % CW,
          '<w:jc w:val="center"/>',
          '<w:tblBorders>']
    for e in ("top", "left", "bottom", "right", "insideH", "insideV"):
        tb.append('<w:%s w:val="single" w:sz="6" w:space="0" w:color="808080"/>' % e)
    tb.append("</w:tblBorders>")
    tb.append('<w:tblCellMar><w:top w:w="60" w:type="dxa"/><w:left w:w="100" w:type="dxa"/>'
              '<w:bottom w:w="60" w:type="dxa"/><w:right w:w="100" w:type="dxa"/></w:tblCellMar>')
    tb.append("</w:tblPr><w:tblGrid>")
    for w in widths:
        tb.append('<w:gridCol w:w="%d"/>' % w)
    tb.append("</w:tblGrid>")

    for ri, row in enumerate(rows):
        tb.append("<w:tr>")
        if ri == 0 and header:
            tb.append("<w:trPr><w:tblHeader/></w:trPr>")
        for ci, cell in enumerate(row):
            shade = '<w:shd w:val="clear" w:color="auto" w:fill="DEEAF6"/>' if (ri == 0 and header) else ""
            al = (align_cols[ci] if align_cols and ci < len(align_cols) else "left")
            cells = cell.split("\n")
            runs = []
            for k, c in enumerate(cells):
                if k:
                    runs.append("<w:br/>")
                runs.append(run(c, bold=(ri == 0 and header), size=font_size))
            tb.append('<w:tc><w:tcPr><w:tcW w:w="%d" w:type="dxa"/>%s'
                      '<w:vAlign w:val="center"/></w:tcPr>' % (widths[ci], shade))
            tb.append(para(runs, indent=False, align=al, line=280))
            tb.append("</w:tc>")
        tb.append("</w:tr>")
    tb.append("</w:tbl>")
    return "".join(tb)


IMG_RELS = []


def image(fname, width_in):
    from PIL import Image
    path = os.path.join(HERE, fname)
    w, h = Image.open(path).size
    cx = int(width_in * 914400)
    cy = int(cx * h / w)
    rid = "rId%d" % (100 + len(IMG_RELS))
    IMG_RELS.append((rid, fname))
    return (
        '<w:p><w:pPr><w:jc w:val="center"/><w:spacing w:before="120" w:after="40"/></w:pPr>'
        '<w:r><w:drawing><wp:inline distT="0" distB="0" distL="0" distR="0">'
        '<wp:extent cx="%d" cy="%d"/><wp:docPr id="%d" name="pic%d"/>'
        '<a:graphic><a:graphicData uri="http://schemas.openxmlformats.org/drawingml/2006/picture">'
        '<pic:pic><pic:nvPicPr><pic:cNvPr id="%d" name="%s"/><pic:cNvPicPr/></pic:nvPicPr>'
        '<pic:blipFill><a:blip r:embed="%s"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>'
        '<pic:spPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="%d" cy="%d"/></a:xfrm>'
        '<a:prstGeom prst="rect"><a:avLst/></a:prstGeom></pic:spPr>'
        '</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>'
        % (cx, cy, len(IMG_RELS), len(IMG_RELS), len(IMG_RELS), os.path.basename(fname),
           rid, cx, cy)
    )


# --------------------------------------------------------------------------- #
#  document content
# --------------------------------------------------------------------------- #
C = []
A = C.append

# ---- 封面 ----
A(para([run("全国大学生嵌入式芯片与系统设计竞赛 · 2026", size=22, color="595959")],
       align="center", indent=False, before=200, after=900))
# The title is the name registered on the competition site, character for
# character: 异构2D图形渲染加速引擎.  Do not add spaces or split the line, or the
# document will not match the entry form.
A(para([run("异构2D图形渲染加速引擎", bold=True, size=48, font="黑体")],
       align="center", indent=False, after=120))
A(para([run("基于 RISC-V 软核与 FPGA 异构架构的 BitBlt 硬件渲染引擎", size=26, color="1F4E79")],
       align="center", indent=False, after=120))
A(para([run("—— 硬件 BitBlt 引擎 · 指令流渲染层 · 弹幕游戏 STARFALL", size=24, color="1F4E79")],
       align="center", indent=False, after=700))
A(para([run("作品名称：异构2D图形渲染加速引擎", size=24, bold=True)], align="center",
       indent=False, after=100))
A(para([run("（作品中不出现任何学校名称）", size=20, color="808080")], align="center",
       indent=False, after=200))

# ---- 摘要 ----
A(h1("摘  要"))
A(p("在嵌入式 GUI 与 2D 游戏中，“把像素从内存搬到屏幕上”这件事长期由 CPU 用循环完成，"
    "一幅 640×720 的画面纯软件渲染就要 50~76 ms、整屏刷新只有 7~8 Hz，帧率与交互体验被严重限制。本作品"
    "《异构2D图形渲染加速引擎》在 Efinix Ti60F225 平台上，用 FPGA 可编程逻辑实现了一条完整的 "
    "2D 图层渲染流水线（BitBlt 引擎），把像素搬运与混合从 RISC-V 软核手中彻底剥离，"
    "并在其上运行了一个稳定 60 FPS 的弹幕射击游戏《星坠 STARFALL》"
    "以及一套覆盖赛题全部条目的演示与验证程序。"))
A(p("硬件上，引擎以 AXI4 从机形式挂载。RISC-V 通过 AXI4-Lite 写入渲染指令（操作类型、源地址、"
    "目的地址、宽度、高度、透明键、Alpha），指令在 SUBMIT 时完成参数合法性检查后压入 16 深的"
    "指令 FIFO；按行调度器把每个任务拆成“一行一个描述符”，用整行信用控制流量，交给 128 位读 DMA；"
    "数据经 1024×145 的 SRC FIFO 进入像素单元，像素单元每拍处理 8 个像素、三级流水，硬件完成"
    "块拷贝 COPY、纯色填充 FILL、颜色键控 KEY 与 Alpha 混合 ALPHA 四种运算；结果经 512×145 的 "
    "DST FIFO 由写 DMA 带回显存。显示端输出 1280×720@60 Hz RGB565，双缓冲换页由引擎在“全部"
    "描述符响应返回”后自动完成，CPU 全程不参与像素与页面时序，从机制上消除了画面撕裂。"))
A(p("软件上，我们抽象出一层“指令流渲染”：上层只描述这一帧要画什么，由 C 驱动翻译成寄存器写序列。"
    "同一份场景描述既可以交给硬件引擎，也可以交给纯 C 软件光栅器——两者能同屏渲染、逐像素互校。"
    "这套双后端结构既是演示手段，也是验证手段：把“渲染是否正确”从“看着像”变成“可以测量”。"))
A(p("作品最终交付 10 个演示场景：目录页、CPU 与硬件同屏帧率对比（FILL 与 COPY 各一套）、整屏大图与 "
    "GIF 快速切换对比、颜色键控、Alpha 阶梯、完整弹幕游戏、Alpha 与颜色键控同时开启时的精灵数极限、"
    "片上自校验、实测数据汇总。精灵数极限由测速探针在板上一秒一次地自动逼近，屏幕上显示的是实测值。"
    "全部场景按固定时长自动轮播，录像时无需任何操作。"))
A(p("关键的量化结果均来自板上实测：整屏 1280×720 填充 1.59 ms、Alpha 混合 4.53 ms；"
    "同一幅 640×720 画面交给 CPU 用纯 C 渲染需要 50~76 ms、整屏刷新 7~8 Hz，单帧耗时相差 30 倍以上。"
    "一帧 16.7 ms 内引擎可以完成 5 次整屏 COPY 或 3 次整屏 ALPHA，"
    "弹幕游戏在同屏 445 个精灵时稳定 60 FPS。"))
A(p("为了证明“算得对”而不只是“算得快”，作品增加了一个片上自校验场景：把 COPY / FILL / KEY / ALPHA "
    "四种运算的执行结果与 CPU 逐像素计算出的参考值在 DDR 中直接比对，16.4 万像素全量比较，"
    "屏上给出 PASS/FAIL 与失配像素的首个坐标。这一步把“渲染正确性”从主观观感变成了可复现的测量。"))
A(p("作品的创新之处在于：以双后端互校（PC 预览器、片上自校验两层）保证正确性，"
    "以帧预算自适应 LOD 保证 60 FPS 恒定，以算力与像素解耦让一颗 100 MHz 的单核软核驱动千级同屏精灵。"))
A(para([run("关键词：", bold=True), run("BitBlt；异构计算；RISC-V；FPGA；2D 图形加速；弹幕游戏")],
       indent=False, before=120))

# ---- 第一部分 ----
A(h1("第一部分  作品概述"))

A(h2("1.1  功能与特性"))
A(p("作品由三层构成：硬件 BitBlt 引擎、软件指令流渲染层、上层演示应用。"))
A(p("硬件引擎在 1280×720 RGB565 显存上支持四种 2D 运算：图像块拷贝 COPY、纯色填充 FILL、"
    "颜色键控去背 KEY（指定透明色，逐像素跳过）、Alpha 混合 ALPHA（输出 = 前景×α + 背景×(1−α)，"
    "由硬件乘法器实现，α 取 0~255）。每条指令携带操作类型、源/目的基址、源行跨度、宽、高，"
    "因此支持“从大图集里取一块子矩形”贴到屏幕任意位置；目标矩形越界时由驱动与硬件两侧共同裁剪。"))
A(p("显示通路与引擎共用 DDR3。硬件生成标准 1280×720@60 Hz HDMI 时序，双缓冲换页由引擎完成，"
    "屏幕左上角叠加硬件统计的每秒换页次数（FPS），这个数字与操作系统无关，是真正的“上屏次数”。"))
A(p("软件侧提供 C 语言驱动、双缓冲管理、软件光栅器（作为对照与黄金参考）以及十套演示场景。"
    "上层应用《星坠 STARFALL》是一个完整的纵向弹幕射击游戏：滚动星空背景（多层视差 + 星云）、"
    "玩家机体（带引擎尾焰与受击闪烁）、三类敌机、三阶段 Boss、六种弹型、血条 / 分数 / 擦弹数 / "
    "Boss 阶段等 UI、自动避弹 AI，画面中的每一个像素都由引擎绘制。Boss 被击破后显示通关卡片，"
    "随即自动切换到下一场景，不会重复播放同一段。"))
A(p("信息层（标题、数据卡片、说明条）全部由引擎绘制，与画面同一条通路输出；"
    "每个场景的分辨率、帧率、指令数、耗时都取自上板实测的计数器与计时器，"
    "而不是事先算好写死的数字。"))

A(h2("1.2  应用领域"))
A(p("本引擎面向一切“需要稳定 60 FPS、但 CPU 算力有限”的嵌入式显示场景："))
A(bullet("嵌入式 HMI 与工业仪表：车载仪表、医疗监护仪、工控触摸屏的界面刷新，本质就是“大量矩形"
         "填充 + 图标贴图 + 半透明面板”。把这些搬运交给引擎后，CPU 可以专心跑业务逻辑与通信协议，"
         "界面不会因为 CPU 一忙就卡顿。"))
A(bullet("2D 游戏与数字标牌：广告机、点唱机、街机的动画、滚动字幕与精灵图层。"))
A(bullet("教学与人机交互：在低成本 FPGA 平台上演示完整的“软硬件协同”闭环——同一个功能用 CPU 做"
         "和用硬件做，差别可以当场用数字看到，比任何 PPT 都有说服力。"))
A(bullet("作为通用加速单元复用：引擎“按行描述符 + FIFO + 像素单元”的结构可以扩展为视频叠加、"
         "OSD 字符叠加、图像标注框绘制、摄像头预览画中画等。"))
A(p("从更长的尺度看，本作品的工程价值不在于“画出一个游戏”，而在于沉淀了一层可复用的"
    "“指令流渲染层”：上层应用只描述场景，下层是 CPU 还是专用硬件对上层透明。这正是异构计算在"
    "嵌入式领域的通用范式。"))

A(h2("1.3  主要技术特点"))
A(pn("（1）指令级解耦。CPU 与引擎之间只有 5 个寄存器（OPF / SRC / SIZE / XY / SUBMIT），"
      "一帧 300~600 条指令也只占 CPU 不到 1% 的时间；引擎在 CPU 准备下一帧时并行执行当前帧，"
      "天然形成“指令流双缓冲”。"))
A(pn("（2）按行调度 + 整行信用。一个任务被拆成“一行一个描述符”，调度器用信用计数控制在途行数"
      "（最多 10 行），读、算、写三级因此天然重叠。仿真中观测到读、算、写三行同时进行。"))
A(pn("（3）大容量片上缓冲压住 DDR 抖动。SRC FIFO 1024×145 bit、DST FIFO 512×145 bit，"
      "把零散的按行访问聚合成 128 位突发：整屏 COPY 的写速率 618 MB/s、FILL 1156 MB/s，"
      "约为 DDR 峰值的 72%。"))
A(pn("（4）硬件自动换页。引擎在全部描述符响应返回后才提交页面并领取下一个后台页，CPU 完全不"
      "参与垂直同步，从机制上不可能出现撕裂。"))
A(pn("（5）参数在硬件侧校验。非法宽高、越界地址、奇地址、行跨度不足等直接在 SUBMIT 时返回 "
      "SLVERR 并记录 4 位原因码，软件可以据此定位，而不是“画到一半崩掉”。"))

A(h2("1.4  主要性能指标"))
A(table([
    ["项目", "指标"],
    ["FPGA / 开发工具", "Efinix Ti60F225（Titanium），Efinity 2026.1"],
    ["软核", "Sapphire RISC-V，100 MHz，4 KB D-Cache（写透）"],
    ["显示输出", "HDMI 1280×720@60 Hz，RGB565，像素时钟 74.25 MHz，双缓冲"],
    ["引擎接口", "AXI4-Lite 从机（指令） + AXI4 128 bit 主机（DMA）"],
    ["支持运算", "COPY / FILL / KEY / ALPHA（α = 0~255）"],
    ["指令粒度", "一条指令完成一次二维矩形运算，支持图集子矩形与越界裁剪"],
    ["整屏 1280×720 FILL", "1.59 ms（Questa 仿真，真实 DDR3 控制器模型）"],
    ["整屏 COPY / KEY", "2.98 ms / 3.00 ms"],
    ["整屏 ALPHA", "4.53 ms"],
    ["帧内并行能力", "16.7 ms 内可完成 5 次整屏 COPY 或 3 次整屏 ALPHA"],
    ["同屏精灵", "AIMD 探针确认 1024（峰值 1280 仍 60 FPS），弹幕场景稳定 400+ 精灵"],
    ["每帧指令数", "游戏场景数百条；极限场景由探针自动逼近到 1280 个精灵"],
    ["CPU 对照", "640×720 纯软件渲染 50~76 ms（整屏 7~8 Hz）"],
    ["片上自校验", "COPY / FILL / KEY / ALPHA 四种运算，16.4 万像素全量比对"],
    ["换页机制", "引擎在全部描述符响应返回后自动提交，CPU 不参与垂直同步"],
], [2780, 5526]))

A(h2("1.5  主要创新点"))
A(pn("（1）双后端互校，分两层落地。第一层在 PC：预览器复用同一份场景源码，把驱动替换成精确复刻"
      "裁剪规则与 SUBMIT 校验的模型，逐像素输出参考图；第二层在板：同一块 DDR 里由 CPU 用纯 C 算出"
      "期望值，再由引擎对同样的输入执行 COPY / FILL / KEY / ALPHA，读回后逐像素比较，"
      "屏上直接给出 PASS/FAIL、比较像素数与失配坐标。仿真累计 215 万次断言 0 错误，"
      "PC 端场景逐像素校验 0 差异，板上自校验 16.4 万像素 0 失配。"))
A(pn("（2）帧预算自适应 LOD：软件实时感知引擎吞吐与屏上帧率，动态调整同屏精灵数与特效档位，"
      "帧率恒定而画质弹性，并自动标定“60 FPS 下的同屏精灵数上限”。探针采用 AIMD（乘性增、"
      "加性减），命中上限后自动回退，因此屏幕上看到的既不是理论值也不是峰值，而是稳定的可复现值。"))
A(pn("（3）算力与像素解耦：CPU 只产出指令流，像素全部由引擎完成，100 MHz 单核软核即可驱动"
      "千级同屏精灵。游戏的渲染循环与仿真步进严格 1:1 对齐（串口同时打印仿真步数与提交帧数，"
      "两者必须相等），避免“画面看起来 60 FPS、实际逻辑只有 30 Hz”这类隐性降级。"))
A(para([run("三个创新关键词：", bold=True), run("双后端互校、帧预算自适应 LOD、算力与像素解耦",
                                              bold=True, color="C00000")],
       indent=False, before=120, after=120))

A(h2("1.6  设计流程"))
A(p("需求拆解 → 寄存器映射与架构设计 → RTL 分模块实现（指令寄存器 / 调度器 / 像素单元 / 三路互连）"
    " → Questa 分级仿真（单元 → 子系统 → 真实 DDR3 控制器整机）→ Efinity 综合布线 → 上板联调 → "
    "软件与演示迭代。每一步都以“可测量的数字”作为通过标准：仿真阶段逐字节比对，软件阶段用 PC 端"
    "预览器逐像素校验，上板阶段用屏上硬件计数器与串口日志核对。"))

# ---- 第二部分 ----
A(h1("第二部分  系统组成及功能说明"))

A(h2("2.1  整体介绍"))
A(p("系统由四部分组成：RISC-V 软核（业务逻辑与指令下发）、BitBlt 引擎（像素搬运与混合）、"
    "DDR3 控制器（三者共享的存储）、HDMI 显示通路（时序生成与上屏）。"))
A(image("fig_arch.png", 6.0))
A(caption("图 1  BitBlt 图形加速器结构框图。控制链路（AXI4-Lite）与数据通路（AXI4 主机）完全分离："
          "CPU 只写控制寄存器与任务 FIFO，像素数据不经过 CPU；fb_page_ctrl 负责双缓冲与换帧，"
          "CPU 与 BitBlt 都能驱动它，因此同一套硬件可以跑“CPU 渲染模式”与“硬件加速模式”。"))
A(p("数据流可以概括为两条环路。第一条是“指令环”：CPU 把一帧要画的矩形逐个写入引擎的控制寄存器并触发 "
    "SUBMIT，引擎把参数快照压入任务 FIFO 后立刻返回，CPU 继续写下一帧，两侧通过 FIFO 深度解耦。"
    "第二条是“像素环”：二维任务拆解器把每个矩形切成一行一个描述符，调度器带 ID 分发给读 DMA 与写 DMA，"
    "读 DMA 从 DDR 取源数据，经读数据分流器按 ID 放入 SRC FIFO / DST FIFO，数据分配器取数对齐后"
    "进入像素处理流水线，结果由写 DMA 带回显存。"))
A(p("三个主机（HDMI 读、CPU、BitBlt DMA）共享一条 DDR3 通道，仲裁优先级为 HDMI 读 > CPU > BitBlt，"
    "保证显示永远不会因为引擎满负荷而断流；引擎一侧用大容量 FIFO 吸收被抢占的周期，因此优先级最低"
    "也不影响吞吐。读通道还有一层保护：地址一旦发到 DDR 面前就被锁存，后到的请求无法抢占，"
    "所以显示读不会被引擎写饿死——这也是“帧率不变但画面异常”类问题永远不该归因于带宽的原因。"))
A(image("fig_datapath.png", 2.35))
A(caption("图 2  任务级到像素级的数据通路细节。任务 ID 贯穿调度器、描述符与数据分流器，"
          "使读、算、写三级可以同时处理不同任务而不互相串数据；"
          "画面上每一行都是一条独立描述符，因此越界裁剪、图集子矩形都能在行级完成。"))

A(h2("2.2  硬件系统介绍"))

A(h3("2.2.1  硬件整体介绍"))
A(p("硬件平台为厂商标准 Ti60F225 开发板：Ti60F225I3 器件、256 MB DDR3、HDMI 输出、FT4232 提供 "
    "JTAG 与 UART、板上 50 MHz 晶振经 PLL 产生 100 MHz 系统时钟与 74.25 MHz 像素时钟。"
    "本作品未改动开发板电路，全部工作在 FPGA 内部完成。"))
A(p("引擎占用的片上资源主要为分布式 RAM 构成的 FIFO：SRC FIFO 1024 深 × 145 bit、DST FIFO 512 深 × "
    "145 bit，以及任务 FIFO（16 深）、读/算/写命令 FIFO 各若干。像素单元中的 8 路并行混合使用 8 组"
    "乘法-累加结构，由逻辑实现并按 3 级流水排布。整个设计跑在 100 MHz 单时钟域上。"))
A(table([
    ["地址区间", "用途"],
    ["0xE100_0000", "显示寄存器（FB0/FB1 基址、帧长、行跨度、写就绪、换页、欠载计数）"],
    ["0xE100_0100", "BitBlt 指令寄存器（OPF / SRC / DST / SIZE / SUBMIT / STATUS / ERROR）"],
    ["0x0100_0000", "显存页 FB0，1280×720×2 B"],
    ["0x0180_0000", "显存页 FB1"],
    ["0x0300_0000 起", "背景图、精灵图集、48×48 与 32×32 图集、径向光晕图集"],
    ["0x0400_0000 起", "CPU 软件光栅器三缓冲（消除 CPU 写与引擎读的竞态）"],
    ["0x0500_0000", "自校验临时区（源/目标/参考，各 640×64 RGB565）"],
    ["0x0520_0000", "半透明面板色带纹理（960×224，引擎 ALPHA 的源）"],
    ["0x0600_0000 起", "图片/GIF 素材包，由调试器写入，可容纳 168 帧 640×720"],
], [2200, 6106]))
A(p("把素材放在 DDR 而不是编译进程序镜像，是为了演示可替换性：换一套图片、换一段视频，"
    "只需重新打包并写入 DDR，不必重新编译或重新综合；DDR 掉电即丢，因此写入脚本在同一次上电内"
    "完成“写入 + 三窗口读回校验”，避免带着半份素材上板。"))

A(h3("2.2.2  机械设计介绍"))
A(p("本作品为纯板级电子设计，无运动机构，因此不涉及机械结构设计。演示时开发板以亚克力支架直立放置，"
    "HDMI 线缆接显示器，USB 线接 PC 提供 JTAG 与串口。"))

A(h3("2.2.3  电路各模块介绍"))
A(p("电路部分沿用开发板既有设计，各模块与本作品的关系如下："))
A(table([
    ["模块", "关键信号", "在本作品中的作用"],
    ["HDMI 输出", "TMDS 四对差分（CLK + D0~D2）", "输出 1280×720p60，由显示通路产生时序"],
    ["DDR3", "16 bit 数据、地址/命令、DQS", "显存 + 图集 + 软件渲染缓冲，三主机共享"],
    ["FT4232", "Channel A = UART，Channel B = JTAG", "串口日志/按键，OpenOCD 下载调试"],
    ["50 MHz 晶振 / PLL", "CLK_IN → 100 MHz / 74.25 MHz", "系统时钟与像素时钟"],
    ["按键与 LED", "GPIO", "本作品未使用（全部交互走串口）"],
], [1900, 3000, 3406]))

A(h2("2.3  软件系统介绍"))

A(h3("2.3.1  软件整体介绍"))
A(p("软件分为四层：应用层（游戏/演示场景）、渲染层（把场景翻译成指令流）、驱动层（寄存器操作与"
    "双缓冲）、硬件层。关键设计是“双后端”：渲染层的同一份场景描述可以交给两个后端执行——"
    "硬件后端把每条图元翻译成一条引擎指令，软件后端用纯 C 光栅器画到一块 DDR 缓冲区。"
    "因此在同一块屏幕上可以左边跑软件后端、右边跑硬件后端，用同一块秒表对比。"))
A(p("为了让“正确性”可验证，我们做了两层工具。第一层是 PC 端预览器：它复用同一份场景源码，但把驱动"
    "替换成一个模型——精确复刻驱动侧的裁剪规则、引擎侧 SUBMIT 的参数检查、以及四种运算的像素结果，"
    "然后把每一帧输出成图片，在没有板子的情况下也能看到画面并统计“有几条指令会被引擎拒绝”。"
    "第二层是片上自校验（见 3.4 节）：不依赖 PC，直接用 CPU 算出期望值，让引擎算同一份数据，"
    "再从 DDR 读回逐像素比对。两层互补——前者能覆盖全部分支却只是模型，后者只测四种基本运算"
    "但跑的是真实硅片。"))
A(p("软件还必须处理一个容易被忽略的一致性问题：上层的帧率统计如果按“换页次数”计，会把一次多余的"
    "提交也算成两帧，于是屏幕显示 60 FPS、逻辑却只有 30 Hz。因此串口日志同时打印仿真步进数（sim）"
    "与提交帧数（frames），两者必须相等；这条约束在后来的调试中直接暴露了一个真实缺陷（见 3.5 节）。"))
A(image("shot_split.jpg", 5.35))
A(caption("图 3  分屏对比：左半屏由 CPU 用普通 load/store 逐像素渲染，右半屏由引擎执行 300 条 FILL "
          "指令。同一种子生成，方块位置完全一致；屏上读数左侧 58 ms/帧、8 Hz，右侧 60 FPS。"))

A(h3("2.3.2  软件各模块介绍"))
A(table([
    ["模块", "关键输入 / 输出", "职责"],
    ["bitblt_driver.c", "图元参数 → 5 次寄存器写 + SUBMIT", "指令下发；越界裁剪；SUBMIT 超时检测；"
                                                          "写屏障保证 CPU 数据先落地"],
    ["game.c", "输入：帧号 / 串口 → 输出：约 350 条指令", "弹幕游戏：实体更新、图案生成、碰撞、"
                                                        "自动避弹、LOD 调节、通关判定"],
    ["art.c", "— → 精灵图集（DDR）", "程序化生成机体、敌机、Boss、六种弹型、爆炸帧、星空、"
                                    "半透明面板色带"],
    ["hwt.c", "字符串 → 每字一条 KEY 指令", "把 5×7 点阵预渲染成 24×32 / 48×64 两级字形图集"],
    ["swr.c", "图元 → CPU 缓冲区", "纯 C 光栅器，作为对照后端与黄金参考"],
    ["scenes.c", "— → 一帧的指令流", "10 个演示场景的指令编排；统一的 HUD 布局系统（卡片宽度由文字"
                                    "长度算出，数值右对齐，杜绝文字溢出面板）；片上自校验"],
    ["main.c", "—", "帧循环：提交 → 换页 → 把剩余时间还给软件光栅器；串口按键；每秒统计"],
], [1900, 2900, 3506]))
A(p("以驱动为例：bb_fill / bb_copy / bb_key / bb_alpha 四个函数都先做一次 rel() 裁剪——把完全在屏幕"
    "外的图元直接丢弃，把部分越界的图元裁到屏幕内并相应修正源地址偏移，然后把参数写进影子寄存器，"
    "最后写 SUBMIT 触发。SUBMIT 会在有限次轮询内等待引擎接受，超时则上报错误码，避免死等。"
    "CPU 自己写过的图像数据在提交前会执行一次 fence，确保 DMA 读到的是新数据而不是缓存里的旧值。"))

# ---- 第三部分 ----
A(h1("第三部分  完成情况及性能参数"))

A(h2("3.1  整体介绍"))
A(p("整套系统已在开发板上完整跑通：上电后由 Efinity Programmer 加载 bit 流，RISC-V IDE 下载应用程序，"
    "屏幕立刻进入演示首页，串口输出每秒一行的统计信息。程序本体只有约 124 KB（RAM 占用 94%），"
    "所有图片、精灵、字体都在启动时由 CPU 程序化生成写入 DDR，约耗时 0.5 秒；"
    "唯一的“外部素材”是可选的图片/GIF 数据包，掉电后需重新写入，因此写入脚本自带读回校验。"))
A(p("演示流程是全自动的：10 个场景按固定时长依次轮播（目录 10 s、两个分屏对比各 15~20 s、"
    "图片 15 s、Alpha 15 s、键控 15 s、游戏 75 s、极限 30 s、自校验 20 s、总结 25 s，合计约 3 分 55 秒），"
    "游戏场景在 Boss 被击破、通关卡片播放完毕后会提前转入下一场景。录制视频时全程不需要触碰"
    "电脑或开发板；需要手动控制时，串口输入空格切下一场景、数字键 1~9 直接跳转、0 到总结页、"
    "a 恢复自动轮播。"))
A(para([run("（此处插入作品实物照片：正面常规视角一张、斜 45° 全局一张，能同时看到开发板、"
            "HDMI 线缆与显示器画面。）", size=20, color="808080")], indent=False))

A(h2("3.2  工程成果"))

A(h3("3.2.1  机械成果"))
A(p("本作品无机械结构，不适用。"))

A(h3("3.2.2  电路成果"))
A(p("使用厂商标准 Ti60F225 开发板，未改动电路。系统接线为：HDMI 接显示器、USB 接 PC（JTAG + 串口）、"
    "12 V 电源。"))

A(h3("3.2.3  软件成果"))
A(p("软件成果包括 10 个演示场景、1 个 PC 端预览/校验工具，以及一套素材与验证工具链"
    "（图片/PDF/GIF 转 RGB565 图集、DDR 素材写入与三窗口读回校验、串口日志与场景跳转脚本、"
    "界面布局静态检查、录像自动剪辑）。下面六张图为运行截图，全部截自板上实拍录像"
    "（手机拍摄后做透视校正），因此保留轻微摩尔纹与反光，而非 PC 端模拟渲染。"))
A(image("shot_key.jpg", 5.35))
A(caption("图 4  颜色键控：300 个 48×48 精灵在棋盘背景上运动，屏幕整体品红像素数为 0；"
          "底部一排精灵特意放在奇数坐标，首尾各有一半在屏幕外，用于验证裁剪。"))
A(image("shot_alpha.jpg", 5.35))
A(caption("图 5  Alpha 阶梯：α = 0 / 64 / 128 / 192 / 255 五块固定方块，透过它们能看到背后的彩条；"
          "下方 8 个方块的 α 在 0~255 之间来回扫描。"))
A(image("shot_limit.jpg", 5.35))
A(caption("图 6  精灵数极限：Alpha 与颜色键控在同一帧内同时工作。每 4 个元素含 3 个键控精灵与 1 个"
          "半透明光晕。屏上大字 1024 是 AIMD 探针已经确认的 60 FPS 精灵数，左上实时计数正试到 1280"
          "（KEY 960 + ALPHA 320）帧率仍为 60 FPS，再加量即跌到 31 FPS 并自动回退。"))
A(image("shot_game.jpg", 5.35))
A(caption("图 7  弹幕游戏 STARFALL：三阶段 Boss、血条与阶段指示、玩家血条 / 残机 / 擦弹数 / 分数，"
          "同屏 445 个精灵（屏上 SPR 0445）、每帧数百条指令，稳定 60 FPS。"))
A(image("shot_images.jpg", 5.35))
A(caption("图 8  多张大图切换：左半屏由 C 代码逐行 memcpy 到软件缓冲再贴屏（76 ms/帧，整屏 7 Hz），"
          "右半屏由引擎执行一条 COPY 指令（60 FPS，单帧 1.49 ms），两边读的是 DDR 里同一份帧数据；"
          "右上 PIC 12/14 表示当前播到第 12 张。"))

A(h2("3.3  特性成果"))
A(p("逐项功能与量化指标如下表。其中“仿真”列来自 Questa 对真实 DDR3 控制器模型的仿真，"
    "“上板”列是屏幕上硬件计数器的读数和串口日志。"))
A(table([
    ["赛题要求", "实现情况", "验证方式与结果"],
    ["解析渲染指令（类型/源/目的/宽/高）", "已完成", "指令寄存器 + 硬件参数校验；仿真 427 条指令全部接受，"
                                            "非法指令返回 SLVERR"],
    ["图像块拷贝 COPY", "已完成", "整屏 2.98 ms；仿真 198 万字节逐字节比对 0 错误"],
    ["纯色填充 FILL", "已完成", "整屏 1.59 ms，写入速率 1156 MB/s"],
    ["标准 HDMI 时序 ≥640×480@60", "已完成", "1280×720@60 Hz，屏上 FPS 计数稳定 60"],
    ["突发传输 + 内部 FIFO", "已完成", "128 bit 突发；SRC 1024×145 / DST 512×145；"
                                     "读/算/写三行同时进行"],
    ["C 语言驱动 + 双缓冲防撕裂", "已完成", "驱动 4 个 API；引擎自动换页；铺满屏幕的滚动画面无撕裂"],
    ["CPU 与硬件帧率对比", "已完成", "同屏左右分屏；CPU 50~76 ms/帧（整屏 7~8 Hz），硬件 60 FPS"],
    ["硬件 Alpha 混合", "已完成", "整屏 4.53 ms；α = 0 完全透明、255 完全不透明"],
    ["颜色键控去背", "已完成", "屏上品红像素数 = 0；出屏精灵被正确裁剪"],
    ["Alpha + Keying 同时开启的精灵数极限", "已完成", "AIMD 探针从 64 起乘性增、加性减，越过 60 FPS 边界再退回；确认值 1024，峰值 1280 仍为 60 FPS、继续加量跌到 31 FPS 后回退；每 4 个元素含 3 个 KEY 精灵 + 1 个 ALPHA 光晕，帧率低于 59 时该行转红"],
    ["高负载游戏 Demo", "已完成", "STARFALL 弹幕游戏：同屏 445 个精灵、每帧数百条指令、三阶段 Boss、稳定 60 FPS；击破 Boss 后显示通关卡片再自动转场"],
    ["多张大图快速切换", "已完成", "最多 168 帧 640x720 RGB565（每帧 900 KiB）由调试器写入 DDR，左右分屏：左半由 C 代码 memcpy、右半由一条 COPY 指令，同一份数据 60 fps 播放；引擎半屏 1.49 ms/帧，CPU 50~76 ms/帧"],
    ["加速器正确性验证", "已完成", "片上自校验：COPY / FILL / KEY / ALPHA 四种运算各比较 40960 像素，"
                              "共 16.4 万像素全量比对，0 失配；失配时屏上给出首个错误坐标"],
], [2500, 1500, 4306]))

A(p("几点值得说明的量化事实："))
A(bullet("同样一幅 640×720 的画面，CPU 纯软件渲染需要 50~76 ms（整屏刷新 7~8 Hz），而引擎完成整屏 "
         "1280×720 的填充只要 1.59 ms，单帧耗时相差 30 倍以上；帧率则从 7~8 Hz 直接顶到 60 FPS。"))
A(bullet("一帧 16.7 ms 的时间预算里，引擎可以完成 5 次整屏 COPY 或 3 次整屏 Alpha 混合，"
         "这意味着一个 2D 游戏可以叠 5 层全屏图层仍然保持 60 FPS。"))
A(bullet("弹幕游戏场景实测每帧向引擎提交 320~400 条任务，CPU 侧（实体更新 + 图案生成 + 碰撞 + "
         "自动避弹 + 指令下发）的耗时不到 2 ms，其余时间全部空余。"))
A(bullet("仿真阶段累计执行 215 万次像素/字节级断言，0 错误；整机级仿真连续 5 帧、276 万像素"
         "与参考模型逐字节一致。"))

# ---- 3.4 自校验 ----
A(h2("3.4  片上自校验：证明“算得对”"))
A(p("性能数字容易展示，正确性却很难。为了不把“看着没问题”当成结论，作品增加了一个专门的验证场景："
    "把加速器的输出与 CPU 逐像素算出的期望值放在同一块 DDR 里比对，结果直接显示在屏幕上。"))
A(p("四个测试项覆盖全部四种运算，每一项都是“CPU 先写期望值 → 引擎对同样的输入执行一次运算 → "
    "读回逐像素比较”："))

A(table([
    ["测试项", "输入构造", "判定条件", "比较像素"],
    ["FILL", "引擎向临时缓冲填充 0x1234", "每个像素都等于 0x1234", "40960"],
    ["COPY", "CPU 写入伪随机图案，引擎整块搬运", "目标与源逐像素相同", "40960"],
    ["KEY", "源为 8×8 棋盘格品红，目标预填 0x0A5A",
     "品红处保留 0x0A5A，其余等于源", "40960"],
    ["ALPHA", "α = 255 叠加到黑色目标", "结果精确等于源", "40960"],
], [1400, 3400, 2406, 1100]))
A(caption("表 3-1  自校验的四个测试项。缓冲区为 640×64 RGB565，位于独立的 DDR 区域，不占用显示页。"))
A(image("shot_check.jpg", 5.35))
A(caption("图 9  自校验场景的屏上结果：四项各比对 40 960 像素，合计 163 840 像素，失配 0，"
          "因此整行判 PASS；一旦出现失配，行尾立刻转红并给出首个错误像素的索引与所属测试项。"))

A(p("几个设计上的考虑值得说明："))
A(bullet("图案必须“邻居不等”。伪随机图案的三个通道分别按不同步长递增，任何平移一位、卡住一个 bit "
         "或交换通道的错误都会立刻暴露；如果用一个纯色或周期很短的图案，这类错误会互相抵消。"))
A(bullet("ALPHA 只取 α = 255（与 α = 0 对称）。这两种极端值有明确的数学期望——完全不透明必须精确"
         "还原源、完全透明必须保持目标——因此不需要假设混合器的内部精度或舍入方式就能判定；"
         "中间值的正确性由逐级仿真和 PC 模型保证。"))
A(bullet("比较过程不碰显示页。所有读写都在 0x0500_0000 起的临时区完成，因此自校验可以在演示"
         "过程中随时运行，不会打断或污染正在播放的画面。"))
A(bullet("结果是“计数”而不是“是否”。屏上给出 PASS/FAIL、比较像素总数、失配像素数，以及失配时"
         "首个错误像素的线性索引与所属测试项——这比一个对勾包含多得多信息。"))

# ---- 3.5 缺陷定位 ----
A(h2("3.5  三个典型缺陷的定位过程"))
A(p("演示程序的逻辑缺陷最难的地方不是修，而是“看起来一切正常”。下面三个问题都曾让我们误判方向，"
    "记录在这里，因为它们分别代表了三种不同的陷阱：统计口径、查表方向、状态判定。"))

A(table([
    ["现象", "误判方向", "真正原因", "定位手段"],
    ["整屏轻微闪烁，且只有游戏场景出现；静态画面不闪，引擎停止写帧时也不闪",
     "先后怀疑 HDMI 时序、线材、显示器动态背光",
     "每帧提交了两次 FRAME_LAST：第一次提交整帧内容，第二次是空的重复提交。"
     "页面每秒变 60 次，内容只更新 30 次，屏幕在新页与两帧前的旧页之间交替",
     "在日志里同时打印“仿真步进数”与“提交帧数”，发现 60 : 30 恒为 2:1"],
    ["Boss 每隔一段时间横向瞬移约 150 px",
     "怀疑数值溢出、随机数、碰撞逻辑",
     "余弦查表只覆盖 0~90°，第二/四象限本应倒序索引却用了顺序索引，"
     "相位跨过 64/128/192 时返回值满量程跳变",
     "用 Python 复现同一个函数，对比真实 cos 值；相邻角度最大跳变 1049/1024，"
     "而正确实现只有 25/1024"],
    ["Boss 本体上下急促抖动，背景与子弹不动",
     "怀疑精灵对齐、绘制行取整、抖屏特效",
     "入场结束的判定写成“y 是否越过台位”，而摆动幅度会越过台位，"
     "于是每帧在“入场分支下移 3 px”与“摆动分支拉回”之间来回跳",
     "把入场判定改为独立的 boss_arrived 标志后现象消失；"
     "顺带发现该分支还会冻结 x 更新并跳过开火，与观察到的“弹幕断拍”一致"],
], [2100, 1700, 2906, 1600]))
A(caption("表 3-2  三个缺陷的现象、误判方向、真实原因与定位手段。"))

A(p("这三条经验后来都写进了代码约束里：统计口径必须成对出现（sim 与 frames），"
    "查表类函数必须有数值单元测试，状态判定不能复用位置量。第 1 条尤其值得强调——"
    "问题出现时屏幕下方的“硬件帧率”一直稳定显示 60，正是因为统计的是换页次数；"
    "如果不把“换页次数”和“逻辑更新次数”分开打印，这个缺陷几乎不可能被发现。"))

# ---- 第四部分 ----
A(h1("第四部分  总结"))

A(h2("4.1  可扩展之处"))
A(p("（1）指令流压缩与批处理。当前每条图元需要 5 次 AXI4-Lite 写；若在引擎侧增加一个“指令 RAM + "
    "自动递增地址”，CPU 可以连续写参数、由硬件自己生成 SUBMIT，指令下发开销还能再降一个数量级。"))
A(p("（2）图元能力扩展。现有四种运算已覆盖 2D 游戏与 GUI 的大部分需求；在此结构上增加 1.5 倍/2 倍"
    "整数缩放、矩形描边、九宫格拉伸与简单旋转，即可支撑更完整的 UI 系统。"))
A(p("（3）与摄像头通路结合。Ti60 自带 MIPI D-PHY 硬核，把摄像头画面作为一路源，引擎就能做"
    "画中画、视频叠加、OSD 字符叠加与检测框绘制，直接对接智能视觉类应用。"))
A(p("（4）走向嵌入式 HMI 产品化。把渲染层封装成一个小型 GUI 框架（控件树 + 脏矩形 + 指令流），"
    "本引擎即可作为低端 MCU 的“显卡”，在车规仪表、医疗监护等场景替代 GPU 方案。"))

A(h2("4.2  心得体会"))
A(p("这个项目最初看起来只是“写一个 DMA 搬方块”，真正做下去才发现它把数字逻辑设计里几乎所有容易"
    "踩的坑都串了一遍：AXI 握手的时序、跨时钟域、仲裁与饥饿、FIFO 反压、非对齐访问、缓存一致性、"
    "以及最要命的——如何在“看起来能跑”和“确实正确”之间划出界限。"))
A(p("第一个深刻的教训来自“验证”。项目初期我们靠肉眼看画面判断对错，结果一个只在特定行号才出现的"
    "错位问题整整找了两天。后来我们把验证方式改成“逐字节比对参考模型”，问题在仿真里 10 分钟就"
    "暴露了。从那以后，每加一个功能，第一件事都是先写它的参考实现和断言，再看波形。整个项目最终"
    "累计 215 万次断言、0 错误——这个数字比任何截图都更能说明“它是对的”。这套方法后来也被我们"
    "搬到了软件侧：PC 端预览器复用同一份场景源码逐像素校验，板上再用自校验场景直接比对真实硅片的"
    "输出，把“上板才发现画错了”的概率降到很低。"))
A(p("最贵的一课来自“统计口径”。有一段时间画面会轻微闪烁，我们先后怀疑过 HDMI 时序、线材、"
    "显示器的动态背光，甚至换过屏——因为屏幕下方的硬件帧率始终稳稳显示 60，看起来显示通路毫无问题。"
    "真正的原因却是那个 60 统计的是“换页次数”，而一次多余的提交让换页翻到了两帧前的旧页："
    "页面每秒变 60 次，内容只更新 30 次。把“提交帧数”和“仿真步进数”并排打印后，60 : 30 恒定成 2 : 1，"
    "问题五分钟就定位了。这件事之后我们给自己定了一条规矩：任何一个数字上屏之前，先问清楚它统计的"
    "究竟是什么，以及有没有另一个数字可以和它互相校验。"))
A(p("第二个教训来自“瓶颈会转移”。把像素搬运交给硬件之后，瓶颈立刻从“算像素”变成了“发指令”和"
    "“DDR 带宽”。我们最初每帧只想发十几条指令，后来做弹幕游戏时每帧要发三百多条，这时才发现"
    "寄存器写的开销、描述符的粒度、FIFO 的深度都是需要重新权衡的。最终我们引入了“按行调度 + "
    "整行信用”和“帧预算自适应 LOD”：前者让读、算、写三级真正重叠起来，后者让软件在带宽不够时"
    "主动降级而不是掉帧。这两种思路其实都来自真实图形引擎的工程实践。"))
A(p("第三个体会是“架构比代码重要”。引擎的寄存器接口只有 5 个寄存器，看起来简陋，但正因为接口"
    "足够窄，我们才可能同时实现硬件后端和软件后端，并且让两个后端的输出可以直接对比。窄接口换来"
    "的是可替换性、可测试性和可移植性——这大概是这个项目留给我们最有价值的经验。"))
A(p("最后，感谢指导老师与队友。硬件部分的分工协作让我们体会到，一个能演示的作品背后，往往是"
    "大量“看不见的”验证工作和一版又一版被推翻的方案。"))

# ---- 第五部分 ----
A(h1("第五部分  参考文献"))
refs = [
    "[1] Efinix Inc. Efinity Software User Guide, 2026.",
    "[2] Efinix Inc. Titanium Ti60 F225 Development Kit User Guide, 2025.",
    "[3] Efinix Inc. Sapphire SoC User Guide (RISC-V), 2025.",
    "[4] ARM Ltd. AMBA AXI and ACE Protocol Specification (ARM IHI 0022), 2021.",
    "[5] Micron Technology. DDR3 SDRAM datasheet, 2015.",
    "[6] 全国大学生嵌入式芯片与系统设计竞赛组委会. 2026 FPGA 创新设计赛道选题指南（易灵思）, 2026.",
    "[7] RISC-V International. The RISC-V Instruction Set Manual, Volume I: Unprivileged ISA, 2019.",
    "[8] 夏宇闻. Verilog 数字系统设计教程（第 3 版）. 北京航空航天大学出版社, 2017.",
    "[9] 吴厚航. 深入浅出玩转 FPGA（第 2 版）. 北京航空航天大学出版社, 2017.",
    "[10] Wikipedia. Bit blit. https://en.wikipedia.org/wiki/Bit_blit",
]
for r in refs:
    A(para([run(r, size=21)], indent=False, line=300))

# --------------------------------------------------------------------------- #
#  package writer
# --------------------------------------------------------------------------- #
NS = ('xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main" '
      'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships" '
      'xmlns:wp="http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing" '
      'xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" '
      'xmlns:pic="http://schemas.openxmlformats.org/drawingml/2006/picture"')

SECT = ('<w:sectPr><w:footerReference r:id="rIdFooter" w:type="default"/>'
        '<w:pgSz w:w="11906" w:h="16838"/>'
        '<w:pgMar w:top="1440" w:right="1800" w:bottom="1440" w:left="1800" '
        'w:header="851" w:footer="992" w:gutter="0"/>'
        '<w:cols w:space="425"/><w:docGrid w:type="lines" w:linePitch="312"/></w:sectPr>')

DOC = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
       '<w:document %s><w:body>%s%s</w:body></w:document>' % (NS, "".join(C), SECT))

STYLES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
<w:docDefaults><w:rPrDefault><w:rPr>
<w:rFonts w:ascii="Times New Roman" w:hAnsi="Times New Roman" w:eastAsia="宋体" w:cs="Times New Roman"/>
<w:sz w:val="24"/><w:szCs w:val="24"/></w:rPr></w:rPrDefault>
<w:pPrDefault><w:pPr><w:spacing w:line="360" w:lineRule="auto"/></w:pPr></w:pPrDefault></w:docDefaults>
<w:style w:type="paragraph" w:default="1" w:styleId="Normal"><w:name w:val="Normal"/><w:qFormat/></w:style>
<w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/><w:basedOn w:val="Normal"/>
<w:next w:val="Normal"/><w:qFormat/><w:pPr><w:outlineLvl w:val="0"/></w:pPr>
<w:rPr><w:rFonts w:eastAsia="黑体"/><w:b/><w:sz w:val="30"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading2"><w:name w:val="heading 2"/><w:basedOn w:val="Normal"/>
<w:next w:val="Normal"/><w:qFormat/><w:pPr><w:outlineLvl w:val="1"/></w:pPr>
<w:rPr><w:rFonts w:eastAsia="黑体"/><w:b/><w:sz w:val="26"/></w:rPr></w:style>
<w:style w:type="paragraph" w:styleId="Heading3"><w:name w:val="heading 3"/><w:basedOn w:val="Normal"/>
<w:next w:val="Normal"/><w:qFormat/><w:pPr><w:outlineLvl w:val="2"/></w:pPr>
<w:rPr><w:rFonts w:eastAsia="黑体"/><w:b/><w:sz w:val="24"/></w:rPr></w:style>
</w:styles>
"""

FOOTER = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:ftr xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main"
       xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
<w:p><w:pPr><w:jc w:val="center"/></w:pPr>
<w:r><w:rPr><w:sz w:val="20"/></w:rPr><w:t xml:space="preserve">- </w:t></w:r>
<w:r><w:fldChar w:fldCharType="begin"/></w:r>
<w:r><w:instrText xml:space="preserve"> PAGE </w:instrText></w:r>
<w:r><w:fldChar w:fldCharType="separate"/></w:r>
<w:r><w:rPr><w:sz w:val="20"/></w:rPr><w:t>1</w:t></w:r>
<w:r><w:fldChar w:fldCharType="end"/></w:r>
<w:r><w:rPr><w:sz w:val="20"/></w:rPr><w:t xml:space="preserve"> -</w:t></w:r>
</w:p></w:ftr>
"""

CT = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
<Default Extension="xml" ContentType="application/xml"/>
<Default Extension="png" ContentType="image/png"/>
<Default Extension="jpg" ContentType="image/jpeg"/>
<Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
<Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
<Override PartName="/word/footer1.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.footer+xml"/>
<Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
<Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>"""

RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
<Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>"""

rels = ['<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">',
        '<Relationship Id="rIdFooter" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/footer" Target="footer1.xml"/>',
        '<Relationship Id="rIdStyles" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>']
for rid, fn in IMG_RELS:
    rels.append('<Relationship Id="%s" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/image" Target="media/%s"/>' % (rid, fn))
rels.append("</Relationships>")

CORE = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
 xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/"
 xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
<dc:title>异构2D图形渲染加速引擎</dc:title>
<dc:subject>全国大学生嵌入式芯片与系统设计竞赛 2026 · FPGA 创新设计赛道 · 赛题二</dc:subject>
<cp:keywords>BitBlt;RISC-V;FPGA;异构计算;2D图形加速</cp:keywords>
</cp:coreProperties>"""

APP = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"
 xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes">
<Application>Python</Application></Properties>"""

with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", CT)
    z.writestr("_rels/.rels", RELS)
    z.writestr("docProps/core.xml", CORE)
    z.writestr("docProps/app.xml", APP)
    z.writestr("word/document.xml", DOC)
    z.writestr("word/styles.xml", STYLES)
    z.writestr("word/footer1.xml", FOOTER)
    z.writestr("word/_rels/document.xml.rels", "".join(rels))
    for _, fn in IMG_RELS:
        z.write(os.path.join(HERE, fn), "word/media/" + fn)

print("written", OUT)
print("paragraphs:", len(C), " images:", len(IMG_RELS))
