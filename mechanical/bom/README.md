# 机械 BOM

[返回项目说明](../../README.md)

下表列出铨州 CNC 套件所需的标准件规格。部分轴承附有采购链接，其余按规格选购。无框电机选 **WK4310H 磁片版**。

官网与教学站建设中。现阶段可从闲鱼购买套件，口令 **CZ007**。[闲鱼套件页面](https://m.tb.cn/h.jNge4x4?tk=GeXY6fdh1uc)

[采购表](组装购买清单.xlsx) 保留了早期铨州下单时的零件名，加工请使用下列 STEP 文件。套件和量产机按 24 V 交付，驱动板选型和 64 V 改版条件见[整机规格](../../README.md#当前电机)。

两套结构的齿轮不能互换。铨州零件按铨州规则改过公差，和量产模型混装会装不上。

## 标准件

| 名称 | 规格 | 数量 | 用途 | 购买 |
| --- | --- | --- | --- | --- |
| 内六角螺钉 | M2×18 | ≥18 | 前后端盖 | 按规格自购 |
| 尼龙套 | 4×2.7×5 | ≥4 | PCB 隔套 | 按规格自购 |
| 内六角螺钉 | M2.5×10 | ≥4 | PCB 固定 | 按规格自购 |
| 圆柱销 | 3×18 | ≥3 | 行星齿固定 | 按规格自购 |
| 圆柱销 | 3×30 | ≥1 | 太阳齿固定 | 按规格自购 |
| 圆柱销 | 2×13 | ≥4 | 转子与减速箱同轴 | 按规格自购 |
| 薄壁轴承 | 6707VV，35×44×5 | 1 | 输出端 | [淘宝](https://item.taobao.com/item.htm?id=566478619519) |
| 薄壁轴承 | 6704ZZ，20×27×4 | 1 | | 同规格自购 |
| 薄壁轴承 | 6701ZZ，12×18×4 | 2 | | 同规格自购 |
| 轴承 | 603ZZ，3×9×5 | 4 | 行星轮 | [天猫](https://detail.tmall.com/item.htm?id=980707569008) |
| 法兰轴承 | 4×7×2 开式 | 1 | 旁置磁编 | [天猫](https://detail.tmall.com/item.htm?id=801235908709) |
| 无框电机 | 海泰 WK4310H 磁片版 | 1 | 与套件结构件配合 | 按型号向官方店询价 |
| 径向磁铁 | 5×2 mm | 按编码器 | 磁编 | 按规格自购 |

## 铨州 CNC 结构件

目录 [`mechanical/cnc/`](../cnc/)。材料按 6061。后壳如果自己有打印机可以改打印，其余建议 CNC。早期整包加工大约 250 元，用券后会浮动，下单前重新询价。

| 文件 |
| --- |
| [YH-JM行星保持架lowerbase开源版本.STEP](../cnc/YH-JM行星保持架lowerbase开源版本.STEP) |
| [YH-JM行星保持架upperbase开源版本.STEP](../cnc/YH-JM行星保持架upperbase开源版本.STEP) |
| [YH-JM后壳开源版本.STEP](../cnc/YH-JM后壳开源版本.STEP) |
| [YH-JM输出端盖开源版本.STEP](../cnc/YH-JM输出端盖开源版本.STEP) |
| [YH-JM外齿圈支架开源版本.STEP](../cnc/YH-JM外齿圈支架开源版本.STEP) |
| [YH-JM定子固定和电机主体开源版本.STEP](../cnc/YH-JM定子固定和电机主体开源版本.STEP) |
| [YH-JM无框电机转子架开源版本.STEP](../cnc/YH-JM无框电机转子架开源版本.STEP) |

装配体：[YH-JM装配体开源版本.STEP](../asm/YH-JM装配体开源版本.STEP)。

套件齿轮在 [`mechanical/gears/`](../gears/)：太阳轮、行星轮、内齿圈为 45 钢淬火图纸，中心磁编齿轮 30 齿和侧边磁编齿轮 31 齿为 POM。

## 量产开源版本

目录 [`mechanical/MPdrawing/cad/`](../MPdrawing/cad/)。这是完整模型，含总装配体。太阳齿是花键销，输出保持架是三销，和铨州那套齿轮不能通用。

| 文件 |
| --- |
| [YH-JM总装配体量产开源版本.STEP](../MPdrawing/cad/YH-JM总装配体量产开源版本.STEP) |
| [YH-JM总装配体（50Z2）量产开源版本.STEP](../MPdrawing/cad/YH-JM总装配体（50Z2）量产开源版本.STEP) |
| [YH-JM输出端盖量产开源版本.STEP](../MPdrawing/cad/YH-JM输出端盖量产开源版本.STEP) |
| [YH-JM下端盖量产开源版本.STEP](../MPdrawing/cad/YH-JM下端盖量产开源版本.STEP) |
| [YH-JM后壳量产开源版本.STEP](../MPdrawing/cad/YH-JM后壳量产开源版本.STEP) |
| [YH-JM外齿圈支架（带内齿圈销位）量产开源版本.STEP](../MPdrawing/cad/YH-JM外齿圈支架（带内齿圈销位）量产开源版本.STEP) |
| [YH-JM行星保持架lowerbase量产开源版本.STEP](../MPdrawing/cad/YH-JM行星保持架lowerbase量产开源版本.STEP) |
| [YH-JM行星保持架upperbase（输出改三销）量产开源版本.STEP](../MPdrawing/cad/YH-JM行星保持架upperbase（输出改三销）量产开源版本.STEP) |
| [YH-JM电机输出盘量产开源版本.STEP](../MPdrawing/cad/YH-JM电机输出盘量产开源版本.STEP) |
| [YH-JM太阳齿销（带花键）量产开源版本.STEP](../MPdrawing/cad/YH-JM太阳齿销（带花键）量产开源版本.STEP) |
| [YH-JM花键紧配示意图量产开源版本.PDF](../MPdrawing/cad/YH-JM花键紧配示意图量产开源版本.PDF) |
