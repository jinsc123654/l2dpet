# PROTOCOL-LLM：LLM ↔ Live2D 桌宠交互协议（v2）

（2026-10-09 重写。先在**本机做成桌宠**开发验证，协议与板卡（RK3506）通用，后续原样移植。
旧版"意图层"草案作废；v1 文本指令保留作调试用。**状态：L1~L5 已在本机实现并验证（§8）**。）

## 0. 流程总览

```
① 导入模型 ──▶ ② 桌宠自动发现能力（骨架 / 表情 / 动作 / 参数），生成能力表并缓存
                        │
③ LLM 发 query ────────▶│
   ◀──────── ④ 上报能力：每一项都带 name（名称）+ fv（特征值）
                        │
⑤ LLM 定期下发：单个动作 / 表情 / 姿势组合 / 时间线（引用 fv）
                        └──▶ ⑥ 模型动起来，回执说明执行结果
```

- **有骨骼**（作者画了多套手臂 + 关节链）→ 上报骨架和"大范围"动作，LLM 可以做举手、手到脸等大动作；
- **无骨骼** → 骨架项为空，只上报头/眼/嘴/表情/作者动作等已有能力，LLM 只在这些能力里组合。
  桌宠**不会**拿渲染器去硬转 2D 手臂冒充大动作（已证实会扭曲，见 `live2d/DESIGN-ARM-JOINTS.md` §7）。

## 1. 传输

- 本机桌宠：TCP `127.0.0.1:23333`（只监听本机），**JSON Lines**：一行一个 JSON，UTF-8，`\n` 结尾。
- 请求可带 `seq`（任意字符串/数字），回复原样带回，便于对应。
- 一条连接可以连续发多条；回复按请求顺序返回。
- 板卡上同样的 JSON 走现有控制端口（2333，经 adb forward），桌面进程转发给播放器。
- **只收 JSON 行**（2026-10-10 安全修复）：任何不以 `{` 开头的行（HTTP 请求行/头）→ 立刻断开整条连接。
  否则任意网页 `fetch('http://127.0.0.1:23333', {method:'POST', body:'\n{"cmd":…}\n'})` 就能跨协议执行命令
  （实测本机旧版可被 `status` 打穿；能改 LLM 接口地址送走 API Key、导出记忆、删模型）。同时连接数上限 32。
  客户端就照规范发 JSON 行，别发别的。
- **嵌套深度上限 64**：解析器是递归下降，`[[[[…` 六万层能把栈打爆（实测段错误）。协议行、LLM 回复、备份文件都经这道闸。

## 2. 名称 + 特征值

能力表里的**每一项**都是 `{"name": 显示名, "fv": 特征值, …属性}`：

- `name`：给人和 LLM 看的名字（来自模型自带的中文/日文显示名，没有就用 id）。
- `fv`：**特征值**，本模型内唯一、稳定的引用键，格式 `类型:键`。**下发控制时只认 fv**，名字仅作说明。

| 类型 | fv 格式 | 例子 | 来源 |
|---|---|---|---|
| 模型 | `model:<指纹>` | `model:0x8c1f6a2e9b03d417` | 参数表派生的 64 位指纹（换包即变）|
| 表情 | `exp:<名>` | `exp:F03`、`exp:爱心` | model3.json 的 Expressions[].Name |
| 动作 | `mtn:<组>/<序号>` | `mtn:TapBody/1` | model3.json 的 Motions |
| 骨架关节 | `joint:<臂>.<套>.<关节>` | `joint:arm.r.B.elbow`、`joint:arm.r.B.finger2` | 自动识别（§3.2）；关节名 shoulder/elbow/wrist/finger1…/forearm/upperarm/extra1… |
| 手臂姿势套 | `pose:<臂>.<套>` | `pose:arm.r.B` | pose3.json 分组 + 部件（§3.2）；无分组的模型只有一套 `default` |
| 手型 | `hand:<臂>.<套>.<编号>` | `hand:arm.r.B.05` | 同一套手臂的不同手型（作者的"表示"开关）|
| 通用部位 | `part:<部位>` | `part:head.x`、`part:mouth.open` | 标准参数映射 |
| 预置动作 | `act:<名>` | `act:wave_big`、`act:adjust_tie` | 内置动作库 + profile，按能力筛选 |
| 原始参数 | `param:<参数id>` | `param:ParamArmBR02` | moc3 参数表（专家用）|

模型本身的 `fv` 是**整份能力表的版本**：LLM 下发的每条控制都带上它，桌宠发现不一致（换了模型/换了包）就拒绝执行并提示重新查询。

## 3. 能力自动发现（导入模型时）

桌宠加载模型时自动完成（骨架识别每次装载现算，毫秒级；参数效果探测结果缓存在模型目录 `l2d.capability.json`）：

### 3.1 读取来源

| 来源 | 得到 |
|---|---|
| `*.model3.json` | 表情列表、动作组（每个动作的时长取自 motion3 的 Meta）、物理/姿势/眨眼/口型配置 |
| `*.cdi3.json` | 参数与部件的**显示名**、参数分组（"右腕A""頭""目"…）|
| `*.moc3`（Cubism Core）| 参数 id / 范围 / 默认值、部件层级、drawable |
| `*.pose3.json` | 互斥部件组（= 作者画的多套手臂/道具）|
| 效果探测（已有）| 每个参数是否真的会动画面（直连 / 经物理 / 换美术 / 无效）|

### 3.2 骨架识别（核心）

Live2D 的"骨骼"是作者做的**旋转变形器链**，moc3 里不直接暴露，按以下线索自动识别：

1. **姿势套**：pose3.json 的互斥组里，部件 id 含 `Arm`（或显示名含"腕"）的 = 手臂的多套姿势。
   左右与套名从 id 的字母解析（`PartArmBR` → 右/B，`ArmLA` → 左/A），显示名里的"右/左""腕C"优先（作者 id 偶有笔误）。
   不分左右的套（Hiyori 的"腕 A/B"同时含两臂）两侧都登记，切换时联动。
2. **关节**：参数 id 含 `Arm`（或纯 `Hand`+左右/套名，如 Hiyori 的 `ParamHandLB`），按显示名关键字定关节，没有关键字按编号：
   - 肘 `肘`/02、腕 `手首`/03、肩 `肩`/01、手指 `指`/`Roll`、前臂 `前腕`、上臂 `上腕`，其余编号记为 extra；
   - 例：`ParamArmBR02`"右腕B 肘の回転" → `joint:arm.r.B.elbow`；Natori 的 `ParamArmCR01` 显示名是"左腕C" → 归左手 C 套。
   - **物理链的输出参数不算关节**（被动摆动，写了也会被物理覆盖：舒芙蕾的 `armR1~5`）。
3. **手型开关**：名字含"表示"或 `ArmXXHandNN` 形式的参数 = 这套手臂的手型（`hand:` fv），默认第一种显示、其余关（不设的话这套手臂会半透明）。
4. **子部件**：只压 pose 组里的**根部件**——Core 会把父部件不透明度乘到子部件上（前臂 `PartArmARFore`、手跟着走）。
   **不要单独压子部件**：早期版本连子部件一起压 0，交回自动后子部件停在 0，作者动作切到那套时就"有袖子没手"（实测踩过）。
   交回自动（`pose.set:[]` / reset）时根部件伪参数恢复为"第一套可见"、子部件恢复为 1。
5. **pose 的坑**：SDK 的 pose 判"哪套可见"看的是**与部件同名的伪参数**，切套时部件不透明度和伪参数要一起压（实现已处理）。
6. **分级**：

| 骨架等级 | 判定 | 能力 |
|---|---|---|
| `full` | 至少一侧手臂有 ≥2 套作者姿势 | 大范围：切姿势套 + 关节连续调 |
| `chain` | 只有一套，但有 ≥2 个关节 | 中等：关节连续调（作者画的范围内）|
| `swing` | 只有单个手臂参数（摆动）| 小幅手臂摆动 |
| `none` | 没有手臂参数 | 无手臂动作 |

**本机实测（自动识别结果）**：

| 模型 | 等级 | 识别结果 |
|---|---|---|
| Natori | full | 右 A(4 关节)/B(7+2 手型)/E(8+2 手型)，左 A/C/D(各 4) |
| Mao | full | 左右各 A/B 两套，3~5 关节 |
| Haru | full | 左右各 A/B 两套（单关节 + 手腕）|
| Hiyori | full | A/B 两套（两臂共用）+ 手腕 |
| Ren / Rice | chain | 各一套 5 关节 |
| Mark / 舒芙蕾 | swing | 单参数摆动 / 物理链手臂 |
| Wanko / sage | none | 无手臂参数 |

**profile 补语义**（结构不用写，自动识别）：模型目录 `l2d.profile.json` 里
`"poseNames": {"arm.r.B":"右手举起张掌", …}` 给每套姿势一句话描述（LLM 靠它理解姿势长什么样，**强烈建议补**）；
`"poseHands": {"arm.r.B":"05"}` 指定默认手型。仍可写 `armSets` 完全手工指定（覆盖自动识别）。
`L2D_SKEL_AUTO=1` 强制自动识别（与手写版对拍用）。

### 3.3 预置动作的筛选

| 来源 | 上报规则 | needs |
|---|---|---|
| profile `actions` | 全部上报（按作者骨骼写的角色化动作；可用 `{"ch":"arm.r.set","anchor":"B/05","keys":[[0.06,1],[0.9,0]]}` 开"切套窗口"，再叠关节关键帧）| `arm.pose`（切套）/ `arm.joint` / `face` |
| 内置动作库 | **执行时不带手臂轨道**（手臂大动作只走作者骨骼）；"本质是手臂"的（挥手/握拳/指向/伸懒腰/话筒/抱胸/摊手）不上报 | `face` |

所以**每个模型都至少有 13 个头身表情版动作**（欢呼、思考、低头致意、大笑、惊讶、跳舞、沮丧、生气、害羞、点头、摇头、蹦跳、东张西望）；
有骨骼且 profile 写了动作的模型再加上手臂版（Natori 共 20 个，其中 11 个带作者手臂）。profile 同名动作覆盖内置。

## 4. 查询：query

请求：

```json
{"cmd":"query","seq":1,"what":["summary","skeleton","expressions","motions","actions"]}
```

`what` 可选：`summary`（默认）、`skeleton`、`expressions`、`motions`、`actions`、`parts`、`zones`（交互区域）、`params`（很长，专家用）、`all`。

回复（Natori 实测，节选；全量不含 params 约 7 KB，可直接放进 LLM 上下文）：

```json
{"seq":1,"ok":true,"type":"capabilities",
 "model":{"name":"Natori","fv":"model:0x0710bfe35ee440bc","fps":30},
 "summary":{"skeleton":"full","expressions":11,"motions":8,"actions":20,"head":"full","body":"full","mouth":"open"},
 "skeleton":{
   "arm.r":{"name":"右腕","poses":[
      {"name":"右手自然下垂","fv":"pose:arm.r.A","joints":[…]},
      {"name":"右手举起张掌","fv":"pose:arm.r.B","joints":[
         {"name":"右腕B　肩の回転","fv":"joint:arm.r.B.shoulder","range":[-30,30],"default":0},
         {"name":"右腕B　肘の回転","fv":"joint:arm.r.B.elbow","range":[-40,20],"default":0},
         {"name":"右腕B　手首の回転","fv":"joint:arm.r.B.wrist","range":[-30,30],"default":0},
         {"name":"右手05　指曲げ1","fv":"joint:arm.r.B.finger2","range":[0,30],"default":0}, …],
       "hands":[{"name":"右手01　表示","fv":"hand:arm.r.B.01"},{"name":"右手05　表示","fv":"hand:arm.r.B.05"}]},
      {"name":"右手到脸前（扶眼镜/托腮）","fv":"pose:arm.r.E", …}]},
   "arm.l":{"name":"左腕","poses":[…"左手抚胸"…"左手侧伸托掌"…]}},
 "expressions":[{"name":"Angry","fv":"exp:Angry"},{"name":"Smile","fv":"exp:Smile"}, …],
 "motions":[{"name":"TapBody 0","fv":"mtn:TapBody/0","dur":5,"arm":true}, …],
 "actions":[{"name":"推眼镜","fv":"act:adjust_glasses","dur":2.2,"needs":"arm.pose"},
            {"name":"用力点头","fv":"act:nod_yes","dur":1.3,"needs":"face"}, …],
 "parts":[{"name":"头左右转","fv":"part:head.x","range":[-30,30]},
          {"name":"身体左右转","fv":"part:body.x","range":[-10,10]}, …]}
```

- 无骨骼模型（sage）：`"skeleton":{}`、`"summary":{"skeleton":"none",…}`，`actions` 只有 `needs:"face"` 的 13 个。
- `joints[].range` 是**作者参数的绝对范围**（作者单位，Natori 多为 ±30）；`parts[].range` 是相对自然姿态的**增量**。
- `motions[].arm`：这段作者动作是否会动手臂（LLM 判断"能不能和手臂姿势同时用"）。

## 5. 控制

控制请求应带 `"model":"<模型 fv>"`：不一致 → `ok:false, code:"model_mismatch"`；不带也执行，但回执里给一条警告。

### 5.1 单项命令

| cmd | 字段 | 说明 |
|---|---|---|
| `expression` | `fv`（`null` = 恢复默认）| 切表情 |
| `motion` | `fv` | 播作者动作 |
| `action` | `fv`、`speed`(0.2~3)、`scale`(0.1~2)、`repeat`(1~8) | 播预置动作 |
| `pose` | `set`：要切的姿势套 fv 列表（`[]` = 全部收回）；`hands`：手型 fv；`joints`：{关节 fv: **绝对值**}；`parts`：{部位 fv: **增量**}；`t`：过渡秒数（默认 0.4）；`hold`：保持秒数后自动收回（缺省一直保持）| **组合姿势**：一次设定手臂套 + 手型 + 关节角 + 头身 |
| `look` | `target`：`viewer` `left` `right` `up` `down` 或 `[x,y]`（窗口像素）；`look_hold` 秒（默认 3，期间不跟鼠标）| 视线 |
| `speak` | `text`，可选 `speak_dur` | 说话口型（按字数估时长）|
| `emotion` | `name`（见 §7 连续情绪）、`intensity` 0~1（默认 0.7）| 推一下连续情绪（时间线里写 `"emotion":"happy"` 或 `{"name":…,"intensity":…}`）|
| `reset` | — | 停止一切，回到自主待机 |
| `status` | — | 当前在做什么、队列深度；桌宠另带 `mood`（内在状态 + 情绪）与 `config` |
| `mood` | 可选 `state`(awake/drowsy/asleep/sulk)、`energy`/`affection`/`annoy`/`lonely` 0~1 | 读/调内在状态（调试、剧情）|
| `config` | 可选 `set`：{键: 值} | 读/改桌宠设置（存 `~/.config/l2dpet/config.json`，推 `config` 事件）|
| `touch` | `zone`、`kind`(rub/click) | 等同用户摸/戳某区域（走同一套状态与反应）|
| `reactions` | `lines`：{键: [台词…]} | 下发本地即时台词库（见 §7）|
| `panel` | `page` 0~8 / `menu`:true / `page`:-1 收起 | 打开"状态与设置"某页 / 弹右键菜单；`page:-1` 收起设置窗口与菜单（由 UI 进程 `l2dpet-ui` 显示）。页序：0 状态 / 1 行为 / 2 窗口 / **3 模型** / 4 对话 / 5 记忆 / 6 关于 / 7 日志 / **8 外观（主题·语言，2026-10-10 追加在最后 → 老页号含义不变）** |
| `panel_state` | — | UI 进程轮询用：`state` = 心情/情绪/配置/模型库/**模型解算信息**/LLM 统计与日志/记忆…，`panel_req` = 待切页 |
| `panel_request` | `page` | 让已打开的设置窗口切到某页（seq 自增）|
| `import_model` | `path`（绝对路径：`.zip` 或目录） | 导入新模型：zip 解压 / 目录拷贝进模型库（重名自动加 `-2`），自动重扫并切过去；结果推 `model_import` 事件 |
| `delete_model` | `name`（模型库一级目录名） | **删除模型**（面板"模型"页）：只删用户模型库 `~/.local/share/l2dpet/models/<name>`（名字含 `/`、`..`、以 `.` 开头一律拒）；**正在使用的模型拒删**（`code:"in_use"`，先切走）；删完自动重扫列表（不换模型）。结果推 `model_delete` 事件（`ok`/`msg`）|
| `persona` | — / `regen`:true | **模型性格**（面板"模型"页）：不带参数读当前（agent 生成后存着，`panel_state.persona` 同款）；`regen:true` 让内置 agent 按模型能力重新生成（约 3~10 秒，完成后推 `persona` 事件）|
| `probe` | 可选 `all`:true | **能力探测**：解算参数/bind/部件并写 `<模型目录>/l2d.capability.json`，完成后自动重载（臂骨架/分区生效）；过程推 `probe` 事件（`started` / `done`+`saved`）|
| `choice` | `index` | 气泡第 n 个选项被点（UI 发；桌宠照旧推 `choice` 事件，等同用户点了）|
| `window` | `visible` | 显示/隐藏角色窗口（托盘/菜单用；隐藏时不渲染）|
| `quit` | — | 退出桌宠 |

**UI 进程**（`l2dpet-ui`，EUI-NEO；托盘/右键菜单/气泡/设置窗口都在它里面，见 `live2d/DESIGN-PET-LIFE.md` §12）用
`{"cmd":"subscribe","role":"ui"}` 订阅；这种订阅**不算 LLM agent**（不影响"已连接"计数）。桌宠向所有订阅者推下列
`ui_*` 事件——**agent 应忽略以 `ui_` 开头的事件**：

| 事件 | 字段 | 含义 |
|---|---|---|
| `ui_menu` | `x` `y` `scale` `time` `wa`[4] | 在屏幕 (x,y) 弹右键菜单；`time` = 右键 X 事件时间（焦点用）；`wa` = 工作区 |
| `ui_close_menu` | — | 收起菜单 |
| `ui_close` | — | 收起设置窗口与菜单（`panel page:-1` 触发） |
| `ui_open` | `page` | 打开设置窗口某页 |

> **气泡已改回桌宠进程直绘**（2026-10-09 深夜，用户定案：旧版设计 = 椭圆鼓泡 + 双小圆点 + 淡入淡出）。
> `ui_bubble` / `ui_bubble_anchor` / `ui_bubble_hide` 事件**不再发送**；`speak` 的 `options` 仍有效，
> 选项点击走桌宠自己的 X11 气泡窗 → 照旧推 `choice` 事件（等同用户点了）。

### 5.2 组合 / 时间线：sequence

```json
{"cmd":"sequence","seq":7,"model":"model:0x8c1f6a2e9b03d417","mode":"queue","dur":12,
 "steps":[
   {"at":0.0,"speak":"欢迎回来。"},
   {"at":0.2,"action":"act:wave_big"},
   {"at":3.0,"pose":{"set":["pose:arm.r.E"],"joints":{"joint:arm.r.E.elbow":-20},"parts":{"part:head.z":8},"t":0.5}},
   {"at":3.2,"expression":"exp:F05"},
   {"at":6.0,"look":"viewer"},
   {"at":8.0,"pose":{"set":[]},"expression":null}
 ]}
```

- `mode`：`queue`（排在当前之后，默认）/ `replace`（打断当前）。
- `steps[].at`：相对开始的秒数（真实时间）；同一步里可以同时写多个键（表情 + 姿势 + 说话）。
- `pose.set: []` = 收回所有姿势套（手臂先移动回默认那套的自然姿势，再交回作者动作）。
- **换套不跳变（关节过桥）**：两套手臂是两张画，不能互相变形。桌宠在两套各自的"肩×肘"空间里
  （各 5×5 采样，首次换套时用 Core 解算，毫秒级）找轮廓最接近的一对姿势作交接点：
  ① 旧手臂用关节转到交接姿势（`t` 秒）→ ② 在两者重合处交给 SDK pose 交叉淡化（约 0.25 s）→
  ③ 新手臂从交接姿势转到目标（`t` 秒；同一步里给的 `joints` 就是这里的终点）。看起来是一条手臂连续移动过去。
  一次换套总时长约 `2t + 0.25` 秒（`t` 钳到 0.3~1.5；预置动作里的切套用 0.35）。
  残留：换手那 ~0.25 s 两条手臂有轻微叠影（两套画在关节范围内无法完全重合，作者自己的动作切换也一样）。
  两臂共用一套部件的模型（Hiyori）不过桥，直接 pose 淡化。
- 时长上限 60 s，队列上限 4 段。

### 5.3 回执

```json
{"seq":7,"ok":true,"accepted":6,"dur":12.0,"warnings":[]}
```

部分 fv 不存在/超范围时：整条**仍执行能执行的部分**，并在 `warnings` 里逐条说明；
`"strict":true` 时任一错误整条拒绝。

```json
{"seq":7,"ok":true,"accepted":5,"warnings":[
  {"step":2,"fv":"joint:arm.r.E.elbow","msg":"值 -50 超出范围 [-40,10]，已钳到 -40"},
  {"step":3,"fv":"exp:F12","msg":"表情不存在"}]}
```

错误码：`bad_json` `unknown_cmd` `model_mismatch` `too_long` `queue_full`。

### 5.4 事件（可选订阅）

`{"cmd":"subscribe"}` 的连接会收到：

- **`{"event":"click","zone":"head","n":2}`** — 用户点了角色某区域；同区域 0.5s 内连点合并成一条（`n`=次数）。
- **`{"event":"hover","zone":"belly"}`** — 鼠标在某区域上驻留 0.6s（同一驻留每 7s 最多一条；移开即重置）。
- **`{"event":"rub","zone":"head","count":4}`** — 鼠标在某区域上 1.5s 内左右反向 ≥3 次（搓/抚摸）。
- **`{"event":"choice","index":1,"label":"要！"}`** — 用户点了气泡里的选项（见 §5.5）。
- `{"event":"done","seq":7}` — 时间线播完。
- **`{"event":"mood","state":"sulk","why":"被戳烦了","energy":…,"affection":…,"annoy":…,"lonely":…,"emotion":{"name","intensity","v","a","d"}}`**
  — 桌宠内在状态变化（另每 20s 一条 `why:"tick"`）；见 §7"内在状态与连续情绪"。
- **`{"event":"config","config":{…}}`** — 设置被改（面板或 `config` 命令）；agent 据此调整参与度。
- 面板/`touch` 命令触发的摸/戳也会产生 `click`/`rub` 事件。

**区域（16 区，`query` 的 `what` 加 `zones` 可得；左右 = 观看者屏幕口径）**：
`head 头部 / face 脸部 / neck 颈部 / chest 胸部 / belly 肚子 / crotch 胯部 / thigh.l·r 左右大腿 /
shin.l·r 左右小腿 / foot.l·r 左右脚 / hand.l·r 左右手 / elbow.l·r 左右手肘`。
判定 = 命名锚点投影（eyes/chin/waist）+ 臂骨架腕/肘关节圆域 + 包围盒分带，随模型自适应；
点击仍会同时触发角色自己的即时反应（模型 Tap 素材 / profile reactions）。
（`model_changed` 预留给板卡多模型切换；本机桌宠一次只开一个模型。）

### 5.5 说话气泡 + 选项（本机桌宠）

`speak` 步的文字会**同步显示成角色头顶的气泡**（Xft 渲染，含口型动画；时长 = 说话时长 + 阅读余量，
最长 16s）。要向用户提问，在该步加 `options`：

```json
{"at":2.0, "speak":"要不要一起玩？", "options":["要！","先不了"], "look":"viewer"}
```

- 最多 3~4 个选项、每个建议 ≤6 字；按钮画在气泡下方，**点击不穿透**到角色；
- 用户点选 → 订阅连接收到 `choice` 事件（带 `label`），气泡立即收起——LLM 据此继续；
- **选项可带能力动作**（2026-10-10）：选项除了字符串，还可以写成
  `{"label":"一起打游戏","do":{"expression":"exp:手柄","action":"act:hop","emotion":"excited","speak":"好耶！"}}`。
  `do` 用和步骤相同的键（action / expression / motion / emotion / look / speak；**不许 options**），fv 照常按能力表校验——
  收到 sequence 时就 dry-run，不合法的 fv 随回执的 `warnings` 返回。主人点这个选项的瞬间，`do` **在桌宠本地立刻执行**
  （不等 LLM），随后照常推 choice 事件，并多一个 `"did":[…键名…]` 告诉 LLM 已经演了什么——
  **did 只列真能演的键**（dry-run 过不了能力表的那些既不执行也不列进去，2026-10-10 实测：`do` 里混一个不存在的表情 →
  warnings 有"表情不存在"、只执行 2 项、did 恰为 `["emotion","speak"]`）；
  内置 agent 收到带 did 的 choice 时，续戏段用 `queue`（排在动作后面，不打断它），提示词里也会写明"别重复同样的动作"。
- 没点 → 到时自动消失；`reset` 也会收起气泡。单项命令 `{"cmd":"say","text":"…","options":[…]}`
  是 speak 的别名（立即说+显示）。

## 6. LLM 侧用法

1. 启动或收到 `model_changed`：发 `query`（`summary` + `skeleton` + `expressions` + `motions` + `actions` + `zones`），
   把结果原样放进 system prompt。
2. 每隔 15~30 s（或收到 `click`/`rub`/`choice`/`done`）让 LLM 生成一条 `sequence`；**只能引用能力表里出现的 fv**。
3. 回执的 `warnings` 放回下一轮上下文，LLM 自己纠正；`hover` 之类弱事件作为背景信息并进下一段。

提示词骨架：

```
你在控制一个 Live2D 桌宠。下面是它的能力表（JSON），每项有 name（含义）和 fv（引用键）。
<能力表>
规则：
- 只输出一个 JSON：{"cmd":"sequence","model":"<model.fv>","steps":[…]}，不要其它文字。
- 只能引用能力表里出现的 fv；关节值必须在 range 内。
- skeleton 为空时不要写 pose.set / joint，用表情、头部、视线、说话和 needs=face 的动作来表达。
- 每段 8~20 秒，以说话为主线，动作点缀在重音处。
```

## 7. 本机桌宠

### 身体区域与视线（2026-10-09 修订）

- **16 区本地即时反应**（不经 LLM）：先看向点到的位置，再按部位做动作 + 表情——头/脸 害羞、颈/肚子 怕痒大笑、
  胸/胯 生气、大腿 吃惊、小腿/手肘 歪头、脚 蹦跳、手 点头；同区连点 ≥3 次 → 生气；搓（rub）另一套
  （摸头害羞、挠脖子/肚子/脚心大笑）。表情 3 s 后自动回默认。冷却 0.8 s（冷却中也会"看过去"）。
  profile 覆盖：`"zoneReactions": {"head": {"action":"shy","gesture":"","expr":"害羞"}, "rub.belly": {…}}`。
- **分区依据**：人物轮廓 = 渲染帧缓冲里真正画出来的像素（不是网格顶点包围盒——后者带透明留白，
  舒芙蕾的下沿比脚底低一大截，"脚"曾经点不到）；腰以上身体两侧（垂下的长发）算头部。
  腿：大腿 40% / 小腿 40% / 脚 20%。
- **上报**：click 事件在停手 0.5 s 后发出（合并连点）；rub/hover 照旧。
- **调试命令**：`{"cmd":"zonemap","step":4}` 返回分区网格（a=head … p=elbow.r，顺序同 zones）；
  `{"cmd":"simulate","x":..,"y":..}` 模拟一次点击（与真实点击同一路径）。实测舒芙蕾 16 区全部命中、事件全部上报。
- **视线**：只在鼠标靠近人物（轮廓外扩 40 px）时跟随；离开 1 s 后放开，交回自主待机（东张西望/小动作）。
- **作者绑定模型的兜底分区（2026-10-10）**：模型没有刚性臂骨架（profile `"rigid": false`，如 yumi——手臂走作者自己的
  `Paramarmup*`）时，臂骨架建不出来。此时用**几何帧推算**代替：眼/嘴锚点取"动该参数的顶点质心"（骨架模型同一算法），
  下颌/胸口/腰按包围盒比例，手部锚点取作者 `hand.l`/`hand.r` 通道参数的顶点质心（半径 0.065×包围盒高）。
  结果 = **14 区**（无肘部），远好于退化的头/身两带。分区模式在 `model_info.zone_mode`：`rig`（16 区含肘）/
  `bound`（14 区，作者绑定）/ `basic`（2 区，连锚点都没有）。

### 模型页（能力解算 / 导入 / 切换，2026-10-10）

- **面板第 3 页"模型"**（"窗口"页只留显示设置：大小/不透明度/总在最前/鼠标穿透/**锁定大小和位置**）：
  当前模型解算卡（名字/指纹/骨架等级 swing|chain|full|none/臂骨架/
  交互分区/动作组 ×N/表情列表/躯干通道列表——**骨架驱动**与**作者绑定**分色标注）+ 模型库列表（● 使用中 / 切换 / 删除）+
  导入卡（路径输入 + 「导入并识别」）+「能力探测」。
- **`panel_state.state.llm_state` / `llm_err`**（2026-10-10）：LLM 连接状态——`no_agent`（没有 agent）/ `no_key`（没配 Key）/
  `pending`（有 Key 但还没真正调用过）/ `error`（最近一次调用失败，原因在 `llm_err`）/ `ok`（最近一次调用成功）。
  **只有 `ok` 才算"已连接"**：`agents` 只是"有 agent 进程"，内置 agent 总在跑，新电脑没配 Key 时以前就误显示已连接。
  内置 agent 取 LlmClient 记录的最近一次真实请求结果（睡眠暂停/退出/没 Key 不算请求）；外部 agent 取它上报的 `agent_info.key_source`
  与最近一条 `llm_log` 的 `ok`。
- **`panel_state.state.model_info`**（面板渲染用；协议里只读）：
  `{name, fv, dir, skeleton, motions:[[组名,数]…], body:[[通道,rigid01]…], rig, zone_mode, zone_count, profile, caps}`。
- **导入**：`{"cmd":"import_model","path":"/abs/x.zip"}` → 解压（`unzip`）/拷贝到 `~/.local/share/l2dpet/models/<目录名>`
  （重名 `-2`/`-3`）→ 重扫模型库 → 切过去（加载时自动算指纹 + VTS 缺项登记）。结果事件
  `{"event":"model_import","ok":true,"name":"yumi"}` / `ok:false,"msg":…`。
- **模型库自愈**：桌宠每 ~2 s 温和重扫一次模型库——直接往目录里拷/删模型，面板列表会自动跟上（删掉的死条目不会留在列表里）。
- **探测**：没有 `l2d.capability.json`（或想要更全）时点「能力探测」= `{"cmd":"probe"}`：后台按帧预算推进（约 30 s，
  不卡渲染），完成后写缓存 + **自动重载**（学到的部件变成臂骨架 → 分区补全）。事件 `probe started / done(saved)`。
  注意：**zip 里通常没有 `l2d.profile.json`**（那是本仓库为特定模型手写的：手势/表情别名/作者通道映射）——
  导入后若面板提示 "缺少 l2d.profile.json"，分区仍是 14 区兜底（见上一条），要作者绑定的手势/别名需另配 profile。

### 模型性格：能力 → 性格 → 台词与动作（2026-10-10）

原来所有模型共用同一套通用提示词，切换模型只是换了张皮（用户原话："都是一套性格，没有区别"）。
现在每个模型有**自己的性格**，由该模型**解算出的能力**（模型名 / 自带表情与道具 / 骨架等级 / 可交互区域）
交给 LLM 生成，按**模型指纹**落盘：

- **文件**：`~/.config/l2dpet/personas/<指纹>.json`（0600，人可读可手改；删掉即重新生成）。
  字段：`name`（角色自己的名字）/ `persona`（性格 2~4 句）/ `speech`（口癖 + 怎么称呼主人）/
  `likes` `hates` / `moods`（常用情绪）/ `fav_expr`（偏好表情，**校验过必须是该模型真实存在的表情**；
  水印开关类表情被排除——用了它每次露面都会把水印打开）/ `by`（llm | heuristic）/ `at`。
- **生成时机**：内置 agent 启动 / 换模型（`model_changed`）时读缓存；没有就生成一次（LLM，失败重试 1 次）；
  没 API Key 或生成失败 → **本地启发式兜底**（按自带道具的气质线索 + 指纹选原型：猫系/傲娇/表演型/甜黏系/
  理性/软萌…）——所以离线状态下各模型仍然是不同性格；之后配好 Key 会自动升级成 LLM 版。
- **注入两处提示词**：① 段落生成（回应/闲时）的系统提示里加【你是谁】段（名字/性格/说话风格/喜欢不喜欢/
  常用情绪/偏好表情 + 不许跳出角色）；② 即时台词库的"角色"描述换成性格版（点/摸时的离线台词也带口癖）。
- **面板**：模型页「性格」卡显示全文 + 来源（LLM 按能力解算 / 本地兜底）+ 「重新生成性格」按钮
  （发 `persona regen`，完成后 `persona` 事件 → toast）。
- **实测**（DeepSeek）：yumi → 柚咪（嘴硬心软傲娇猫，偏好 猫猫嘴/泪汪汪/漂浮小狗/爱心眼）；
  舒芙蕾 → 舒芙蕾（软乎乎小甜点，"戳脸就鼓腮说再戳就漏奶油啦"，偏好 脸红/猫猫手/爱心/哭哭）；
  林翩翩 → 翩翩（娇气大小姐，只有 1 个可用表情 → 偏好 'exp:动作'）。
  点同一处（摸头）三只的台词明显不同：柚咪"哼，轻一下重一下的，主人到底想干嘛呀。"、
  舒芙蕾"呜哇～又戳脸！再戳真的要漏气啦…"、翩翩"别弄乱头发啦"。

### 问题库：提问不再只有"在干什么"（2026-10-10）

原来是这么问的：提示词里只有一句含糊引导（"称呼/作息/喜好之类"，**且只在还不了解主人时给**），
其它时候 LLM 自由发挥 → 每次都落回最笼统的"你在干什么呀？"；重复抑制也只列"已答过"的题面。
现在：

- **题库文件**：`~/.config/l2dpet/questions/<模型指纹>.json`（0600，人可读可手改；删掉即重新生成）。
  每条：`topic`（题材）/ `q`（她口吻的问题）/ `options`（2~3 个可点短选项）/ `asked`（问过几次）/ `answer`（主人答的）/ `at`。
- **生成**：LLM 按**题材清单**一次出 12 题（称呼/作息/工作学习/吃/爱好/心情/家人朋友/过去回忆/未来打算/
  和她的关系/她自己的小请求/随口闲聊），要求分散、不重复、不出是非题；已知（记忆 + 题库历史）作为输入喂回去避免重复。
  重新生成时机：没有题库 / 超过 24h / 没答过的题 <3 / **答够 5 题**（该换新题材了）；启动与换模型时检查，运行中每 5 分钟体检一次。
- **提问**：闲时段只要冷却到了（提问冷却 5 分钟）就把题库里 2~3 个**没问过**的候选写进提示词，让 LLM 挑最贴合气氛的
  （可微调措辞）；问过就记 `asked++`（LLM 自创的问题也收进库里），所以同一个问题不会反复问。
  启动后 1 分钟内不提问（每次重启都被问一句会烦）。
- **答案的去向**（"不限于对话"）：
  1. 写进题库（换批生成时作为"已知"喂回，不会重复问）；
  2. 进跨会话记忆（`ChoiceMade` → 习惯统计里的"最近问答" + 归纳进 `owner.md`）；
  3. **立刻重写即时台词库**（不等常规 15 分钟节流）→ 点/摸的离线台词马上能带上"新知道的事"；
  4. 段落生成的记忆块里明确要求：称呼/作息/口味/爱好要体现在**所有交互**里（台词/动作/表情/视线选择均可照顾）。

### 互动小功能：笑话 / 猜拳 / 工作时长 / 天气（2026-10-10）

右键菜单里多了三项，配置里多了两个键（面板"行为 → 互动小功能"）：

| 互动 | 协议 | 谁来做 |
|---|---|---|
| 讲笑话 | `{"cmd":"joke"}` | **本地**：先念 agent 定期补货推下来的**笑话池**（`{"cmd":"jokes","list":[…]}`, 轮转不重复）→ 没池子才用台词库 `life.joke` 组 → 内置兜底；讲完推 `joke_told` 让 agent 记账 |
| 猜拳 | `{"cmd":"game","kind":"rps"}` | **本地全流程**：气泡"石头/剪刀/布"三选项 → 点选后本地随机出拳+判定 → 结果气泡（`life.rps.win/lose/draw` 组）+ 反应（赢跳/输歪头/平点头）→ 推 `game` 事件给 agent 接着玩 |
| 聊天气 | `{"cmd":"weather"}` | **交给 LLM**：转成 `weather_ask` 事件让 agent 现编一段（带上 `weather_loc`）。**没有实时天气数据**，提示词要求用"关心/提问"式，不许编具体温度；开着 `weather` 时闲时也会偶尔主动聊一句 |
| 工作时长 | （无命令，自动） | 桌宠按"鼠标一直在动"估算连续使用时长（离开 >5 分钟清零），到 `work_remind` 分钟推 `{"event":"work_long","min":N}` → agent 出【关心】段（提醒休息 + 伸展动作）|

- 猜拳是本地判定的（公平 + 秒回），且**校验选项标签**：气泡过期后换了别的选项（LLM 提问）不会把那次点选当成出拳；猜拳的 `choice` **不发**普通 choice 事件（否则 agent 以为答了问题，会污染题库的问答记录）。
- 猜拳结果与工作时长都会进记忆（`Note`），闲聊时可自然呼应。
- 天气位置是可选的**字符串配置**：`{"cmd":"config","setstr":{"weather_loc":"北京"}}`（空串 = 删）；存在 `config.json` 的 `"str"` 段，`state.config.str` 可读。

**笑话池**（`~/.config/l2dpet/jokes/<模型指纹>.json`，0600、可手改）：agent **定期增量补货**——"没讲过的"少于 2 条且距上次生成 ≥30 分钟
（`L2D_JOKE_GAP_SEC` 可覆盖，测试用）就让 LLM 生成 6 条新的（已有的作为"不要重复"喂回去），追加进池子（上限 40，超了丢最久没讲的已讲条目），
生成完推给桌宠；讲笑话**纯本地秒回**（轮转；收到新货时先讲新的），讲完推 `joke_told` → agent 记 `used`。
所以她睡醒时她在睡、池子空了自动补，醒来后讲话就多起来了；**睡着时不补货**（见下）。

**睡着不调 LLM**：她处于 `asleep` 时，agent **一切 LLM 调用暂停**——闲时段本来就跳过（原有规则），现在**记忆归纳、
台词库刷新、问题库体检、笑话补货也全停**，只在本地跑心跳/状态机；醒来（mood 事件）自动恢复并立刻把欠的活补上
（实测：睡下 100 秒零调用 → 醒来立刻换了一批新题 + 补了 6 条笑话）。此时点"讲个笑话"她会用梦话体回一句（不吵醒她）。

### 记忆压缩 / 备份导出导入 / 文件选择器（2026-10-10）

**记忆压缩（每天一次 + 面板手动）**
- `PetMemory::Compress`：把《关于主人》与《我和主人之间》**合并去重**（同一件事反复记的必须并成一条）、
  旧事折进 `## 更早`、删一次性小细节，并把 `episodes.jsonl` 归档一并收编后**截断到最近 60 条**；**不新增事实**。
  另有一条硬约束：压缩结果**不允许比原来长**（超过原量 5% 就保留原文——LLM 偶尔会写得更啰嗦，再被截断成半句）。
- 触发：agent 空闲循环里每 10 分钟体检一次，`NeedsCompress(24h)` 为真且**醒着**且配了 Key → 压缩；
  面板「记忆」页「立即压缩记忆」= `{"cmd":"mem_compress"}`（桌宠转成 agent 事件）。压缩时间记在
  `memory/chars/<指纹>.habits.json` 的 `compressT`，面板显示"上次压缩：…"（`state.memory.compressed`）。
- 结果由 agent 用 `{"cmd":"mem_notify",...}` 回报 → 桌宠广播 `mem_compress_done` 事件 → 面板 toast。
  **命名注意**：广播事件名不能和 agent 监听的触发名相同（`mem_compress`）——所有桌宠事件都会喂给 agent，
  同名会让它把自己的回报当成新请求来回打转（踩过，见 HANDOFF）。

**备份导出 / 导入**（`{"cmd":"export","path":…}` / `{"cmd":"import","path":…}`）
- 打包 `~/.config/l2dpet` 下的**小文本文件**成一个 JSON：`memory/*` + `memory/chars/*`（记忆/习惯/待整理/归档）
  + `personas/*.json` + `questions/*.json` + `jokes/*.json` + `config.json`。**不含模型文件**；
  **不含 `llm.json`**（API Key 有意不导出——备份常被发出去/传网盘，带 Key 是最常见的泄漏方式）。
- 导出：读文件 → UTF-8 清洗（历史文件里可能有坏字节，带坏字节的包严格解析器直接读不了）→ 写 0600。
- 导入：校验 `app:"l2dpet"` → **只写白名单路径**（`config.json` / `memory/…` / `personas/` / `questions/` / `jokes/`，
  拒绝 `..` 与超长名）→ 完成后通知 agent `profile_reload`（重读记忆/性格/题库/笑话 + 重写台词库 + 刷新面板）。
- 事件：`mem_export` / `mem_import`（`ok`/`files`/`msg`）→ 面板 toast。

**文件选择器（面板，Linux）**：记忆页「导出…/导入…」与模型页「选 zip…/选目录…」打开**原生对话框**
（优先 `zenity`，退回 `kdialog`；Win/Mac 暂返回"不可用"）。实现要点：
- 用 `posix_spawn` + 管道取 stdout（不经 shell，路径里有引号/空格/分号都安全）；
- **子进程环境要收窄**：只给 `DISPLAY/XAUTHORITY/XDG_RUNTIME_DIR/DBUS_SESSION_BUS_ADDRESS/HOME` + `PATH` +
  `LC_ALL=C.UTF-8`（实测带着终端里的完整环境变量时 zenity 立刻退出、渲染不出来；同样给了 `env -i` 的最小集合才能弹窗）；
- 没有选择器时返回"不可用"，面板**回退**成"用输入框里手填的路径"（输入框保留，两边都能用）。



### 配置键（`~/.config/l2dpet/config.json`）

（键表见 `l2dtest.cpp` 的 `CfgDefs`；改键 = `{"cmd":"config","set":{"key":值}}`）

- 窗口类（port 自己应用）：`zoom` 大小 / `opacity` 不透明度 / `top` 总在最前 / `passthrough` 鼠标穿透 /
  **`lock` 锁定大小和位置**——开启后**角色窗口不再被拖动**（X11/GLFW 两个端口都拦，按下抬起仍算一次点击）
  且**滚轮不再缩放**（主循环拦）；面板上的滑条仍可改（那是有意为之，不属于误操作）。
  面板"窗口"页与右键菜单里都能开关。
- 互动类：`work_remind` 连续工作多久提醒休息（分钟，<1 分钟 = 关，默认 60）；`weather` 天气闲聊（0/1，默认关）。
  **回执与存盘都是合法 JSON**（2026-10-10 修：数值段为空时曾生成 `{,"str":{…}}`，全新安装只设过字符串键时必现——
  agent 解析不到 config，启动就一直按默认语言生成）。**`str` 段的存在 = "配置读好了"**：桌宠第一帧后 1.5s 才读配置，
  之前回执里不带 `str`（存盘时一定带，免得 `setstr` 抢在 `CfgLoad` 前把 lang 存丢）；agent 启动靠这个信号等（≤8s），
  配置读好后桌宠还会补推一条 `config` 事件。
  字符串键在 `"str"` 段（`{"cmd":"config","setstr":{"键":"值"}}`，空串 = 删）：
  - `weather_loc` 位置（可选，如"北京"）；
  - **`theme` 面板主题**：`light`（默认）/ `dark` / `auto`（19:00–7:00 深色）；
  - **`lang` 面板语言**：`zh`（默认）/ `en` / `ja` / `auto`（按系统 locale 猜，认 zh/ja，其余按英文）。
    面板"外观"页与右键菜单/托盘的「外观」子菜单都能改；**`lang=en` 时内置 agent 的提示词会加一句"说英语"**
    （`speak` 台词 / 台词库 / 笑话 / 提问与选项——见 §7 语言）。
    **改 `lang` 还会触发一次"存量内容换语言"**：桌宠在写配置处比前后值，变了就给内置 agent 推
    `{"event":"lang_changed","lang":"<新值>"}` → agent 用新语言重写**记忆展示**（LLM 原样改写：结构/条数/时间戳不变）
    并**清掉重生成**性格/台词库/题库/笑话池；她睡着时排队、醒来补做。记忆归纳等内部文本仍是中文。
    **桌宠自己说/写的文案也跟语言**（`player/src/PetI18n.cpp`）：气泡兜底台词、触摸兜底反应、
    `panel_state.life.feed` 的动态文案（`摸`+`了`+`头部` → `なでた（頭）`）、状态行（`犯困（指令）` → `眠そう（コマンド）`）。
    协议里的**结构化字段不变**（`state`/`kind`/部位名仍是中文键，面板自己翻），只有给人看的整句跟语言。

### 气泡与节制（2026-10-09）

- **样式**：思考气泡——白色圆鼓椭圆（轻微蛋形）、深青粗描边、下半部一弯浅蓝、右上两道高光，
  两个由大到小的小圆点指向角色头部；略透明（0.94）。选项 = 白底深青描边胶囊，悬停浅蓝。
  独立的 override-redirect ARGB 小窗（软件 SDF 抗锯齿 + Xft 文字），弹在头部左上/右上（看哪边屏幕空间大）。
- **位置固定**：弹出时按当时的头部位置定一次，之后相对桌宠窗口固定——**不随模型晃动**，只随窗口拖动。
- **排版**：避头点（，。！？等不落行首）；一行短句也是饱满的泡（高 ≥ 0.55 宽）。
- **待选提问不被冲掉**：气泡里有未选的选项时，后续普通台词只动嘴不换气泡。
- **三层交互**（LLM 一次调用要秒级，做不了"当下"反应，所以分层）：

  | 层 | 谁做 | 时机 | 内容 |
  |---|---|---|---|
  | 即时 | 桌宠本地 | 每次点/搓，0 延迟 | 看向触点 + 部位动作/表情 + **一句短台词气泡**（本地台词库，≥1.5 s 一句） |
  | 回应 | LLM | 用户**停手 2 s** 后（两次间隔 ≥10 s） | `mode:"replace"` 打断闲时段；≤6 s、2~3 步，第一步就开口，不加填充动作 |
  | 闲时 | LLM | 没人互动时，平均 60 s（+0~50%） | 8~15 s 小日常，台词可无 |

- **本地台词库**：`{"cmd":"reactions","lines":{"head.click":["…"],"head.rub":["…"],…,"angry":["…"]}}`
  （整体替换；每组 ≤8 句、每句 ≤60 字节）→ `{"ok":true,"type":"reactions","groups":N,"lines":M}`。
  键 = `<区域>.click` / `<区域>.rub` / `angry`（同区连点 ≥3 次）；缺 `.rub` 用 `.click`；没下发时用内置兜底台词。
  Agent 启动时让 LLM 按人设一次性写好下发，之后点/搓不再等 LLM。待选提问的气泡不会被即时台词冲掉。
- **Agent 节制**（`pet_agent.py`，代码强制）：每段 ≤2 句台词、≤2 次换表情（道具/装扮类表情提示词里禁用）；
  提问 ≥5 分钟一次、每段 ≤1 个，问出后等用户选或 25 s；hover 不打日志、只作下一段的弱上下文。
- **跨会话记忆 v2**（`pet_agent.py --llm` + `tools/pet_memory.py`；详见 `live2d/DESIGN-PET-LIFE.md` §10）：
  `~/.config/l2dpet/memory/owner.md`（关于主人，**所有角色共享**）+ `chars/<指纹>.md`（我和主人之间，**每角色一份**）
  + `habits.json`（触摸/时段/见面/选项统计，程序算）+ `pending.jsonl`（待整理，随时落盘）+ `episodes.jsonl`（归档，按相关度召回）；
  全部 0600。戳/摸只计数不进归纳；主人的选项回答、生闷气/被哄好/被吵醒记为重要要点；权重攒够 `--memory-every`（6）
  或每 `--memory-idle`（1200s）归纳一次（`--memory-tidy` 按当前规则重整）。久别重逢（启动时离开 >30min，或桌宠推
  `mood why:"你回来了（离开 N 分钟）"`）→【重逢】段结合记忆叙旧；对主人了解不足 → 闲时用选项问一个了解主人的小问题。
  旧 `memory.md` 自动迁移（原文件留作 `memory.md.migrated`）。记忆更新后（≥`--lines-refresh` 秒）带记忆重生成即时台词库。
  **在桌宠里看**：面板 行为 → 记忆 → 记忆本页（`{"cmd":"memory","text":…,"at":…}` 由 agent 上报，桌宠只存显示，
  上限 12KB 且按 UTF-8 边界截断）。


- 透明、无边框、置顶、不占任务栏的窗口，只显示角色（GNOME/X11 合成器下背景透明；点到透明处穿透到桌面）；
- 左键拖动 = 移动；左键单击角色 = 点击反应 + 推送 `touch` 事件；**滚轮 = 缩放整个视窗**（窗口跟着变大变小、
  模型永远完整不裁剪；**底边中点不动**，屏幕边缘自动钳制；范围 0.5~2.0，存进配置 `zoom` 下次启动保持）；
  气泡（含字号与位置）随缩放**同步**重排跟随；视线跟随全局鼠标；右键双击 / `Esc` = 退出；
- 启动（构建：`live2d/build-player.sh`）：

```bash
~/l2d-build/build-host/l2dtest <模型>.model3.json 0 0.0333 --pet --ctl 23333 --size 360x640
```

（shuflle 要加 `--order static`：它自带 5.x"参数驱动绘制顺序"，动态序下呼吸/右上注视会把胸口
皮肤层排到裙子上面（板上桌面已按 Kconfig 清单自动传 static；本机 `--pet` 默认 dynamic 会露，
证据 `live2d/img/shuflle-order-static-vs-dynamic-host.png`，详见 HANDOFF-DESKTOP §3）。）

- 调试客户端 `live2d/tools/pet_client.py`：`query [what…]` / `status` / `send '<json>'` / `play <时间线.json>` / `events`（自动补 model 特征值）；
- 示例 Agent `live2d/tools/pet_agent.py`：`--demo`（不需要 LLM，按能力表随机组合，定期下发）／`--llm`（OpenAI 兼容接口，
  `LLM_API_KEY` / `LLM_BASE_URL`（默认 DeepSeek）/ `LLM_MODEL`）；启动时下发即时台词库；用户停手后插一段【回应】，回执警告带进下一轮。

### LLM 接入配置：多供应商 + 多模型（2026-10-10）

存 `~/.config/l2dpet/llm.json`（0600，**v2 格式**；旧的单条 `{api_key,model,base_url}` 读取时自动迁移成
一个名为"默认"的供应商，不改文件、下次保存时写回 v2）：

```json
{"v":2,"active":"<供应商 id>","providers":[
  {"id":"...","name":"DeepSeek","base_url":"https://api.deepseek.com","api_key":"sk-…",
   "model":"deepseek-chat","models":["deepseek-chat","deepseek-reasoner"]}]}
```

- **同一时间只有一个激活供应商**（`active`）；它的 `model` 是使用的模型（`models` 只是候选列表）。
- 环境变量 `LLM_API_KEY` / `LLM_BASE_URL` / `LLM_MODEL` 仍**优先级最高**（临时覆盖，不改文件）。
- 协议命令：
  - `{"cmd":"llm_config"}` 读（**不含密钥**）：返回顶部兼容字段（= 激活供应商的 `key_set`/`key_tail`/`model`/
    `base_url`/`provider`）+ 全量 `providers`（每个带 `key_set`/`key_tail`/`models`）与 `active`。
  - `{"cmd":"llm_config","api_key":…,"llm_model":…,"base_url":…}` 改**激活供应商**的字段并存盘（老客户端的用法）。
  - `{"cmd":"llm_providers","active":"id","providers":[…]}` **全量保存**（面板"对话"页用）；
    某项 `api_key` 为空/缺省 = **保留该 id 原来的密钥**（面板不持有明文密钥）。
- 任一保存成功都会推 `llm_config` 事件（订阅方据此重读）；`agent_info` / `llm_stats` 报的都是激活供应商。
- 本机桌宠的"对话"页就是这个编辑器：供应商 chips（● = 激活）+ 名称/接口地址/API Key/模型列表（选用/删除/添加）。
- 调试：`L2D_PET_DUMP=<路径>` 把第 60 帧原样（含 alpha）存成 PAM。

### 内在状态与连续情绪（2026-10-09）

> 设计、公式、阈值与**边界/已知限制**的完整版：[`live2d/DESIGN-PET-LIFE.md`](live2d/DESIGN-PET-LIFE.md)。

桌宠不靠 LLM 也"活着"（`l2dtest.cpp` PetLifeTick / EmotionTick），LLM 只负责"说什么、怎么演"：

- **内在状态**：`energy` 精力（醒着慢慢降、夜里更快；睡觉回升）、`affection` 亲密度（摸头/牵手涨、乱戳降；
  **存盘** `~/.local/state/l2dpet/<指纹>.json`，下次启动按离开多久打招呼）、`annoy` 恼火（戳涨、自然消）、
  `lonely` 寂寞（没人理就涨）。状态机：`awake` → `drowsy`（精力低或长时间无人理；打哈欠、半闭眼点头）
  → `asleep`（闭眼低头、慢呼吸、冒 Zzz/梦话；摸头=睡梦里笑，戳一下=迷糊，再戳=被吵醒有起床气；睡饱自己醒）；
  `annoy ≥ 0.7` → `sulk`（扭头背过去、皱眉撇嘴、不跟鼠标；戳=更不理，摸头=慢慢哄好，气消一半会偷瞄你）。
  另有：持续摸头进入"享受"（眯眼、嘴角上扬、往手那边蹭，不重播动作）；你离开 5 分钟后回来会打招呼；
  寂寞时主动看向鼠标挥手搭话；你在忙时偶尔瞄一眼鼠标。状态变化推 `{"event":"mood",…}`（另每 20s 一次 `why:"tick"`）。
- **连续情绪**（参考 soullink-emotion-sdk）：VAD 三轴（愉悦/唤醒/支配，-1..1）。事件只**推目标**
  （`emotion` 命令、区域反应、状态变化），当前值指数逼近目标，保持 4~16s 后向**性格基线**回落
  （基线随亲密度/精力/状态变）；外加小幅环境漂移。每帧把 VAD 映射成**叠加层**：嘴角、眉高/眉形、眼开度、
  头俯仰/歪头、脸红（`ParamCheek`）、身体微挺——加在动作与眨眼之上，不抢通道；VAD 突变时给头部一个
  0.7s 微动脉冲。强度 > 0.3 时借模型自带表情做峰值（开心/害羞/生气/惊讶/难过；只清自己设的那个）。
  情绪名：happy excited shy affectionate curious confused tired sad anxious angry surprised calm sulky
  （低强度时报 content/uneasy/low 等"淡"名）。`status.mood.emotion = {name,intensity,v,a,d}`。

### 右键菜单与"状态与设置"窗口（参考 BongoCat）

- 右键角色 → 自绘菜单：状态与设置… / 摸摸头 / 哄她睡觉（睡着时为"叫醒她"）/ 表情 › / 动作 › /
  总在最前 / 鼠标穿透 / 大小 › / 不透明度 › / 退出（原"右键双击退出"取消；Esc 仍退出）。
- 设置窗口五页：**状态**（心情名 + 强度 + VAD 平面、精力/亲密/恼火/寂寞、陪伴统计、快捷互动按钮）/
  **行为**（即时台词、借用模型表情、情绪敏感度、表情幅度、会犯困、犯困等待、主动找你、自主小动作）/
  **窗口**（大小、不透明度、置顶、穿透）/ **对话**（LLM 连接状态、闲时小剧场开关与间隔、允许提问）/ **关于**。
- 面板上的操作全部变成本协议的命令行（`config`/`touch`/`mood`/`emotion`/`expression`/`action`）交给主循环执行，
  和 LLM 走同一条路；agent 订阅 `config` 事件实时遵守"对话"页的设置。
- 鼠标穿透启动时不继承（否则点不到也打不开菜单）；从菜单开启穿透会同时打开设置窗口作为回头路。

## 8. 实施状态（本机）

| 步 | 内容 | 状态 / 证据 |
|---|---|---|
| L1 | 桌宠窗口（`src/PortX11Pet.cpp`）| ✅ 透明置顶、拖动/点击/滚轮/鼠标视线；帧 alpha 全部合法（PAM 检查）|
| L2 | 控制端口（`src/CtlServer.cpp`，主循环执行，播放器单线程）| ✅ `pet_client.py status` 有回执 |
| L3 | 能力自动发现 + `query` | ✅ 10 个本机模型分级见 §3.2 表；Natori 自动识别结果比手写 profile 更全（手指/前臂/手型）|
| L4 | 控制命令 + `sequence` + 回执/警告 + 事件 | ✅ Natori 时间线：挥手 → 扶眼镜+抚胸 → 换手型举手 → 收回；不存在的 fv 逐条警告 |
| L5 | 示例 Agent | ✅ `--demo` 连续驱动（表情/动作/姿势套轮换）；`--llm`（DeepSeek）实跑：Natori 75s/5 段；**shuflle 8min/27 段 392 步**，仅 2 条 fv 警告（引用能力表外动作）且下一轮自纠正（证据 `live2d/img/demo-shuflle-pet-llm.png`，22 帧时间线）|
| L7 | **三层交互 + 本地即时台词库**（2026-10-09）| ✅ 模拟点脸×2/大腿×3：每下即时台词；停手 3s 出【回应】段（原 20~30s）；台词库 LLM 生成 43 组 130 句 |
| L8 | **内在状态 + 连续情绪（VAD）**| ✅ 生闷气→哄好、享受、睡着→迷糊→被吵醒、开场按存档招呼；姿态截图 `live2d/img/pet-life-awake-sulk-sleep.png`；LLM 实跑自发用 `emotion` 步 |
| L9 | **右键菜单 + 状态与设置窗口**| ✅ `live2d/img/pet-context-menu.png`、`live2d/img/pet-panel-status.png`；面板操作全部经协议命令执行 |
| L6 | **16 区域 + 指针手势 + 说话气泡**（2026-10-09 晚间）| ✅ 区域判定 `[zone]` 调试口径实测（颈部/右手肘/肚子/胯部等逐点核对）；`click`（连点合并 n=3）/`hover`/`rub`（count 计数）/`choice` 事件全通；气泡 Xft 渲染（长文折行/选项并排/点选回 label）——**LLM 实跑自发用 options 提问**（3 段带选项），用户点选后 LLM 续戏正确（证据 `live2d/img/demo-pet-zones-bubble.png`；裸名 fv 宽容回退：`"星星"`≡`"exp:星星"`/`"shy"`≡`"act:shy"`）|

不发指令时输出与改动前逐字节一致（shuflle/Haru 离线渲染对拍）。

**板卡移植**：L2~L4 在播放器里，与窗口无关。板卡侧让桌面进程（`l2d_ctrl.c`）把以 `{"cmd":` 开头的行
原样转给播放器、回复区从 256 B 扩到 8 KB（能力表约 7 KB）即可；指令环与转发通道已有。
