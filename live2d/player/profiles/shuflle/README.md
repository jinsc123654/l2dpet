# shuflle 的模型 profile（l2d.profile.json）

**部署位置**：`/mnt/sdcard/live2d/shuflle/l2d.profile.json`（播放器从模型目录里读；
`rk_build_tools deploy` 会自动推送本文件）。

**作用**：给 shuflle 补"模型专属手势"（参数轨道编排）与表情别名。内置通用手势
（nod/shake/tilt/surprise/sigh/think/look）不受影响；**同名会覆盖内置**。

## 手势说明（手臂参数方向实测）

| 手势 | 效果 | 关键参数 |
|---|---|---|
| `wave` | 挥手：右臂抬起向外 + 手腕摆两下 + 头轻侧摆 | ParamarmR1/R2/R3（-1=向外张开）|
| `greet` | 招呼/伸手：双臂微张 | ParamarmL/R1、L/R3 |
| `shy` | 害羞：双臂内收 + 低头（配 `expr: 脸红`）| ParamarmL/R1、ParamAngleX/Z |
| `stretch` | 伸懒腰：双臂张开 + 头后仰 + 身体微挺 | ParamarmL/R1/R2、ParamAngleX、ParamBodyAngleX |
| `hands_on_hips` | 叉腰（近似）：双臂外张 + 手腕内收 + 微微后仰 | ParamarmL/R1、L/R3 |

## 点击反应（reactions）

手指点击（未拖动）触发，按区域取定义：`head`（上 32%）/ `body`（其余）/ `any`（兜底）：

| 区域 | 反应 | 效果 |
|---|---|---|
| `head` | `shy` + `脸红` | 被摸头：歪头收臂 + 脸红 |
| `body` | `greet` + `爱心` | 被戳一下：打招呼 + 爱心 |

值：`gesture` 手势名（可空）、`expr` 表情名或 `"next"`（可空）。有 `Tap*` 动作素材的
模型（Haru 等）优先播素材动作，不走这里。

## 写 profile 的规则

- 一条轨道：`{ "param": "参数名", "mode": "add|set"（默认 add）, "keys": [[t,v],…] }`；
  `t` 归一化 0..1（相对该手势 `dur`），最多 8 个关键帧，段内 smoothstep 缓动；
- 参数名照 `*.cdi3.json`；模型没有的参数会被跳过（对模型自动降级）；
- **别加 `comment` 之类的自定义键**：SDK 自带的 CubismJson 解析器在某些组合下会
  整份解析失败（实测：本文件 5 条手势都带 comment 时失败，去掉即好；JSON 本来也
  不支持注释）——说明写在本 README 里；
- 表情别名 `exprAlias`：`{"别名": "真名"}`（真名来自 `{"cmd":"query","what":"expressions"}` 的 `name`）。

## 调参方法（不用改代码）

改这个文件 → 用桌宠的协议发一次对应手势看效果（重启桌宠自动装载，或面板"模型"页重载）：

```bash
python3 live2d/tools/pet_client.py send '{"cmd":"gesture","fv":"gesture:wave"}'
```

逐帧看手臂位置：`--render <目录> --render-every N`（离屏出帧，见 HANDOFF-DESKTOP §1）。
