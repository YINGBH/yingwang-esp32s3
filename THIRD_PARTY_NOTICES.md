# 第三方组件与署名（THIRD-PARTY NOTICES）

本项目使用了以下第三方组件。原始版权归各自作者所有，此处按其许可证要求署名。

---

## 1. ESP-IDF v5.5.6

- **作者**：Espressif Systems
- **许可证**：Apache License 2.0
- **用途**：芯片驱动、WiFi/BLE 协议栈、NVS、HTTP 客户端、分区管理
- **地址**：https://github.com/espressif/esp-idf

```
Copyright 2015-2026 Espressif Systems (Shanghai) CO LTD

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```

---

## 2. FreeRTOS

- **作者**：Amazon.com, Inc. / Richard Barry
- **许可证**：MIT
- **用途**：任务调度（随 ESP-IDF 分发）

```
Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
```

---

## 3. Apache NimBLE (Apache Mynewt)

- **作者**：The Apache Software Foundation
- **许可证**：Apache License 2.0
- **用途**：BLE 主机栈（扫描电子价签、GATT 通信）
- **地址**：https://github.com/apache/mynewt-nimble

```
Copyright 2017-2023 The Apache Software Foundation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0
```

---

## 4. cJSON

- **作者**：Dave Gamble / cJSON contributors
- **许可证**：MIT
- **用途**：串口命令输出、网页接口、节假日 JSON 解析

```
Copyright (c) 2009-2017 Dave Gamble and cJSON contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.
```

---

## 5. pngle

- **作者**：kikuchan
- **许可证**：MIT
- **用途**：解码门户返回的验证码 PNG（`components/pngle/`）
- **地址**：https://github.com/kikuchan/pngle

```
MIT License

Copyright (c) 2019 kikuchan

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
```

---

## 6. miniz

- **作者**：Rich Geldreich / miniz contributors
- **许可证**：公有领域（Unlicense）/ MIT 双许可
- **用途**：PNG 解压（pngle 依赖，`components/pngle/miniz.c`）
- **地址**：https://github.com/richgel999/miniz

```
This is free and unencumbered software released into the public domain.

Anyone is free to copy, modify, publish, use, compile, sell, or distribute this
software, either in source code form or as a compiled binary, for any purpose,
commercial or non-commercial, and by any means.
```

---

## 7. 中国法定节假日数据 holiday-cn

- **作者**：NateScarlet
- **许可证**：MIT
- **用途**：在线补全日历节假日（数据来自国务院公告）
- **地址**：https://github.com/NateScarlet/holiday-cn

```
MIT License

Copyright (c) 2019 NateScarlet

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
```

数据本身（节假日安排）来自国务院办公厅公开文件，无版权问题。

---

## 8. 字体（开源字体，可随源码分发）

仓库里的点阵头文件是用开源字体渲染生成的，随源码分发没有问题：

| 生成物 | 大小 | 来源字体 | 许可 |
|---|---|---|---|
| `components/bsp_nologo/font_data.h` | 112 KB | 思源黑体 Regular + Noto Sans Mono | SIL OFL-1.1 |
| `components/esl_ble/esl_font.h` | 274 KB | 思源黑体 Medium + Noto Sans Mono | SIL OFL-1.1 |

- 思源黑体 https://github.com/adobe-fonts/source-han-sans （OFL-1.1）
- Noto Sans Mono https://github.com/googlefonts/noto-fonts （OFL-1.1）

字体文件本身不在仓库里，`tools/fetch_fonts.py` 会按需下载（约 8.5 MB），
然后用 `tools/gen_font.py` / `tools/gen_esl_font.py` 重新渲染点阵。

> 开发过程中曾用 Windows 自带的中易黑体（SimHei）和 Segoe UI Symbol 做对比渲染，
> 但那两个是微软商业字体、不允许把字形提取成位图再分发，所以发布版全部改成了
> OFL 字体。如果你自己改界面文字后要重新生成字库，请继续用这两个开源字体，
> 别把 `tools/gen_font.py` 指回系统字体。
---

## 9. 本项目自身

- **许可证**：MIT（见 `LICENSE`）
- **验证码识别模型**：自行用门户公开验证码训练，不含第三方模型权重
- **电子价签协议**：通过对自家设备的蓝牙流量抓包分析得到，未使用厂商 SDK

---

## 附：许可证兼容性

| 组件 | 协议 | 与 MIT 兼容 |
|---|---|---|
| ESP-IDF | Apache-2.0 | ✅ |
| FreeRTOS | MIT | ✅ |
| NimBLE | Apache-2.0 | ✅ |
| cJSON | MIT | ✅ |
| pngle | MIT | ✅ |
| miniz | Public Domain | ✅ |
| holiday-cn 数据 | MIT | ✅ |
| 思源黑体 | OFL-1.1 | ✅ |
| Noto Sans Mono | OFL-1.1 | ✅ |

**全部组件都与 MIT 兼容，可以直接分发。**

## 点阵字库用到的字表

- **《通用规范汉字表》一级字表（3500 字）** —— 2013 年教育部 / 国家语言文字工作委员会发布的国家标准，
  字表本身不受著作权保护；数据经 [shengdoushi/common-standard-chinese-characters-table](https://github.com/shengdoushi/common-standard-chinese-characters-table) 整理。
- **一言（Hitokoto）语料字表** —— 从 [hitokoto-osc/sentences-bundle](https://github.com/hitokoto-osc/sentences-bundle)
  的 11,273 句里统计出 3,780 个汉字。**仓库只提交去重后的汉字表**
  （`tools/hitokoto_chars.txt`），不包含任何句子原文，因此不涉及语料本身的授权；
  使用一言接口请遵守 [developer.hitokoto.cn](https://developer.hitokoto.cn/sentence/) 的说明。

---

## 厂商图标（扫描页）

`components/tracker_scan/vendor_icons.h` 里的图标来自
[Simple Icons](https://simpleicons.org)（CC0 1.0），由 `tools/gen_vendor_icons.py` 生成。

Apple / Google / Huawei / Bluetooth 等标志的**商标权归各自公司所有**，
这里仅用于在本地屏幕上标识"扫到的广播属于哪家生态"，不暗示任何官方关联或背书。
Samsung / Tile 的官方图标是整词 logo，压到 20px 不可辨认，因此用字母标记代替。
