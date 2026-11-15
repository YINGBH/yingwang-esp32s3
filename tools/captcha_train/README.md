# 验证码训练工具（换学校用）

三个脚本对应三步：**采样本 → 训练 → 量化打包**。

```
harvest.py         采样本 + 自动标注（改 CONFIG 区适配你学校；模板，未实测）
train.py           纯 numpy 训练（只需要 numpy + Pillow）
quantize_model.py  量化成 int16 + 导出 model_q.bin
```

数据约定（`train.py` 和 `quantize_model.py` 都认这个格式）：

```
data/captcha/
├── labels.csv        file,code,sha1,ts
└── samples/          验证码 PNG，尺寸必须和 --size 一致
```

## 最短路

```bash
pip install numpy pillow requests ddddocr

cd tools/captcha_train
# 1. 采 200 张先跑通闭环（先改 harvest.py 顶部的 CONFIG）
python harvest.py --n 200

# 2. 训练（字符集/尺寸按你学校改）
python train.py --chars 0123456789 --size 62x25 --ink red --epochs 40

# 3. 量化打包
python quantize_model.py

# 4. 烧到 storage 分区
python -m esptool --chip esp32s3 -p COM5 -b 921600 write_flash 0xC10000 out/model_q.bin
```

## 固件那边要同步改的东西

| 常量 | 位置 | 说明 |
|---|---|---|
| `CAPTCHA_IMG_W` / `CAPTCHA_IMG_H` | `components/captcha/captcha_infer.h` | 和 `--size` 一致 |
| `CAPTCHA_CHARS` | 同上 | 验证码位数 |
| `CM_*` 网络形状 | `components/captcha/captcha_loader.h` | 量化脚本导出的头文件里有正确值，抄过来 |
| `CAPTCHA_CONF_MIN` | `components/portal/portal_client.c` | 置信度门限，要重新标定（见 `训练指南.md`） |

详细流程、坑、以及怎么标定门限，看仓库根目录的 **`训练指南.md`**。
