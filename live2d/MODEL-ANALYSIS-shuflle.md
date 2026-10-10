# 模型解析：shuflle（舒芙蕾）——VTS 主播模型的控制面与物理版图

> 2026-10-09 逐文件解析（数据源：SD 模型库里的 `shuflle.*` + `.vtube.json`）。
> 结论先行：**这是一个"面捕 + 声控口型 + 道具表情 + 全身物理"的 VTS 主播模型；
> 手臂是物理被动摆动件，不是控制通道**——所以"举手/摸头"这类动作它档案上就不支持，
> 任何强行拉变形的做法都会失真（见 §5）。

## 1. 资产清单

| 文件 | 大小 | 说明 |
|---|---|---|
| `shuflle.moc3` | 9.5 MB | 模型本体：**445 drawable / 37,394 顶点 / 254 参数** |
| `shuflle.4096/texture_00..09.png` | 10 张 | 原始 **4096² ×10**；已离线 box 缩到 768²（板载解码安全，输出逐位一致） |
| `shuflle.physics3.json` | 101 KB | **51 组物理链（82 入 → 123 出）**——本模型的"灵魂"（见 §3） |
| `shuflle.cdi3.json` | 26 KB | 显示信息：15 个参数分组（身体ph/头发ph/其他ph/耳朵/眼睛/眉毛/嘴/VB&元音口型/捕捉XYZ/物理XYZ/手柄/按键/呼吸/辅助/动作）+ 参数中文名 |
| 12 × `*.exp3.json` | ~小 | 表情（=道具/情绪换美术） |
| `IDLE.motion3.json` | 9 KB | 唯一动作素材（1 个 Idle） |
| `shuflle.vtube.json` | 77 KB | **VTS 配置**：作者定义的热键/参数绑定/物理与道具设置（下一节） |
| `items_pinned_to_model.json` | 0.4 KB | VTS"固定道具"设置 |
| `shuflle.model3.json` | 1.6 KB | Group：EyeBlink/LipSync（我们补过接线） |

VTS 元数据：`Name=舒芙蕾`、`ModelID=0f2c75f4489d405faf11b7463af659b9`、`Version=1`。

## 2. 控制面（作者在 VTS 里定义的东西 = 这个模型"打算怎么被驱动"）

**① 参数绑定 49 条**（输入跟踪信号 → 输出 Live2D 参数，含范围映射）：

| 跟踪输入 | → 模型参数 | 输出范围 |
|---|---|---|
| FaceAngleX/Y/Z | ParamAngleX/Y/Z | ±30（头） |
| FaceAngleX/Y/Z | ParamBodyAngleX/Y/Z | ±10（身，跟头联动） |
| EyeRightX/Y | ParamEyeBallX/Y | ±1（**X 是反相映射**） |
| EyeOpenLeft/Right | ParamEyeLOpen/ROpen | 0 ~ **1.9**（允许"瞪大"） |
| MouthSmile | ParamEyeL/RSmile + ParamMouthForm | ±1 |
| Brows | ParamBrowLY/RY/LForm/RForm | ±1 |
| JawOpen / CheekPuff / MouthShrug / MouthPucker / MouthPressLipOpen | 对应的嘴/颊参数 | — |
| （空输入） | ParamBreath | 0~1（自动呼吸） |
| **VoiceA/I/U/E/O + VoiceSilence** | **ParamA/I/U/E/O + ParamSilence** | 0~1（**声控口型 = VTS 唇同步标准五元音**） |
| **Controller\***（摇杆/键/肩/扳机/十字键…） | LStickX/Y、ButtonA/B/X/Y、DPadX/Y、RtoABXY、LtoUDLR、LShoulder/RShoulder、LTrigger/RTrigger | 手柄道具（见下） |

**② 热键 12 个，全是 `ToggleExpression`**：手柄 / 猫猫手 / 麦克风 / 哭哭 / 星星 / 爱心 /
生气 / 眼罩 / 眼镜 / 脸红 / 黑脸 / 水印——**没有"举手/姿态"热键**。

**③ 手柄（gamepad）专属**：手部道具 + 40 余个"手柄按键/摇杆"参数 + 物理
（PhysicsSetting49-51：手柄道具的位置/角度跟着按键输入动）。

## 3. 物理版图（51 组链；这是"不动也有戏"的来源）

```
头角 AngleX ─►●镜像 X2/X3/Z3─┬─►身体 BodyAngleX2/Y2/Y3/Z2─┬─►手柄道具 ControllerPhX/Y/Z
AngleY ─►AngleY2/Y3          │                            （叠按键/摇杆输入）
AngleZ ─►AngleZ2             ├─►头发：bang/cefa/houfa/hairX/Y（≈30 个输出参数）
                             ├─►耳：earph、控制/hole/ts/eyecontrol（眼+眉驱动耳朵！）
                             ├─►蝴蝶结/云/雨滴/掉落：bowph/cloudph/dropph
                             ├─►裙/荷叶边/侧摆：dressph/sideph
                             └─►脸部细节：睫毛/泪/高光 ph、嘴形链、腮红
眼开合 EyeLOpen/R ─►睫毛/泪/高光/耳朵链；颧骨 CheekPuffC ─►CheekPuffR/L
**呼吸 Breath ─►armL1-5 / armR1-5（手臂摆动）、dropph、Breath2、AngleY2**
```

**关键发现（PhysicsSetting45/46）**：
```
入: ParamAngleX, ParamAngleY, ParamAngleZ, ParamBreath  →  出: ParamarmL1..L5
入: ParamAngleX, ParamAngleY, ParamAngleZ, ParamBreath  →  出: ParamarmR1..R5
```
**手臂的 10 个参数（armL1-5 / armR1-5）是物理链的"输出"**——作者把手臂做成了
**被动摆件**：头一动、一起呼吸，手臂像钟摆一样跟着荡。它们**不是给外部驱动的**，
美术也只画/绑了"自然下垂小幅摆动"这一种状态。

## 4. 所以，这个模型的"正确驱动方式"

1. **头（AngleX/Y/Z）是总开关**：转头 = 带动 ●镜像 → 身体 → 头发/耳朵/蝴蝶结/云雨/裙摆
   ——全身联动（我们的 BODY `head.*` 已经走这条，且必须写在物理之前，见 HANDOFF §7）。
2. **面捕六件套**：眼（注视/开合/笑眼）、眉（高度/形状）、嘴（开合/形状/撇嘴）、颊。
3. **声控口型**：五元音 + Silence——我们的 `TALK` 驱动的正是这组参数（
   `ParamA/I/U/E/O` + `ParamJawOpenBL/MouthOpenY`），与 VTS 的唇同步同源。
4. **表情 = 道具/姿态**（12 个热键，也就是我们 `EXPR` 的那套）：麦克风=举话筒、
   猫猫手=手举起来变爪、手柄=拿手柄（还能按键亮灯）、爱心/星星/脸红/黑脸/眼罩/眼镜/哭哭/生气/水印。
5. **手臂**：让它自己摆（物理），要"抬臂"只能走我们外挂的变形（§5），且有上限。

## 5. 外挂"抬臂变形"的性质与上限（我们做的，非作者能力）

参数层驱动 armL/R1-5 只有"摆动级"位移（模型高度的 ~5%）——因为 **① 它们是物理输出、
② 美术只有垂手状态**。为此我们做了两级"纸片人"变形（渲染器层，见 HANDOFF §7）：
- **刚性旋转**：整条手臂一张画绕肩转 → 大幅但**肩部脱袖**（用户实测："肩从袖子里飞出来"）；
- **弯曲变形（bend，现行默认）**：角度沿臂轴渐变（肩根不转、指尖转满）+ 轻延伸 →
  接缝基本消失、手臂成自然弧线；**但角度上限已钳到 90°**：再高手臂会"绕过头顶"、
  2D 切图被弯成软管（用户实测："太吓人"）。
- 现行甜点区：`arm.r≈0.45` 手到胸/肩、`0.6` 斜上举、`1.0` 平举（**大字型**）。
  **摸头/过肩高举 = 此模型档案上不支持**（那类要先补美术+绑定，或换 yumi 这类作者绑好的模型）。
  旋钮：`L2D_LIMB_MAXDEG`（默认 90）、`L2D_LIMB_STRETCH`（0.12）、`L2D_LIMB_PIVOT`（0.75）、
  `L2D_LIMB_RIGID=1`（退回刚性对照）。

## 6. 给"要更大动作"的三条路

1. **用表情做姿态**（现在就能用）：麦克风/猫猫手/手柄就是作者画好的大幅姿态（静态、干净）；
2. **面捕式表演**（现在就能用）：头/眼/眉/嘴 + 物理全身联动 + 口型——这是它设计的主场，
   "像人一样"的连续感主要来自这里；
3. **补美术+绑定**（要作者或我们用 Cubism Editor 做）：给手臂画"抬手"的覆盖美术 + 肩/肘
   旋转变形器 + 参数——做完我们的 `arm.*` 通道直接映射过去（profile 一行改动）。
