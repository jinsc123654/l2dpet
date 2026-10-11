// i18n.cpp — 中 → 英对照表（键 = 源码里的中文原文/模板，见 i18n.h）
//
// 维护提示：表是按"面板上的位置"分组的；改动 app.cpp 里的中文文案后，跑
//   python3 live2d/tools/i18n_check.py    就能列出对不上的条目（缺译 / 多余 / 值不一致）。
#include "i18n.h"

#include <cstdlib>
#include <map>
#include <set>
#include <string>

namespace app {
namespace i18n {

namespace {

Lang g_lang = Lang::Zh;

// clang-format off
const std::pair<const char*, const char*> kEn[] = {
    { "捏捏脸", "Pinch her cheeks" },
    { "Token 节省", "Token savings" },
    { "节省档位", "Savings level" },
    { "完整", "Full" },
    { "均衡", "Balanced" },
    { "节省", "Save" },
    { "极省", "Minimal" },
    { "完整：最近 6 条消息，按原间隔互动，回复使用供应商默认上限。", "Full: last 6 messages, original idle interval, provider's default reply limit." },
    { "均衡：最近 4 条消息，小剧场间隔 ×1.5，回复上限 1536 Token。", "Balanced: last 4 messages, idle interval x1.5, reply limit 1536 tokens." },
    { "节省：最近 2 条消息，小剧场间隔 ×3，回复上限 1024 Token。", "Save: last 2 messages, idle interval x3, reply limit 1024 tokens." },
    { "极省：仅当前问题，关闭自动小剧场，回复上限 768 Token。", "Minimal: current question only, no automatic idle scenes, reply limit 768 tokens." },
    { "不删除已保存的记忆；性格、台词库和记忆整理保留独立预算。完整档可提前补题，其余档题库问完再更新。实际节省量取决于使用情况。", "Saved memories stay intact. Personality, reaction lines and memory maintenance keep independent budgets. Full may refill questions early; other levels refill only after all questions have been asked. Actual savings vary." },
    { "最近一次 {}→{} Token · 本次运行对话与维护累计 {}→{} Token", "Latest {} -> {} tokens; dialogue and maintenance this run {} -> {} tokens" },
    { "气泡最短显示时间", "Minimum bubble display time" },
    { "复制接入文件路径", "Copy integration file path" },
    { "接入说明文件", "Integration file" },
    { "把接入文件路径交给编程助手。助手读取当前端口、模型能力和性格，自行生成适合自己的 Hook / MCP 配置，并登记到注册目录。", "Give the integration file path to your coding assistant. It reads the active port, capabilities and personality, creates its own Hook / MCP configuration and registers in the clients directory." },
    { "已注册的编程助手", "Registered coding assistants" },
    { "还没有助手注册。每个助手在 clients/<助手ID>/registration.json 登记，配置文件也可放在自己的目录里。", "No assistants registered. Each assistant registers in clients/<assistant-id>/registration.json and can store configuration in its own directory." },
    { "关闭某个助手后，它的 Hook 和 MCP 陪伴通知都会被桌宠拦截；更新注册文件不会重置你的通知选择。注册文件只声明身份，不会执行其中的命令。", "Disabling an assistant suppresses its Hook and MCP companion notifications. Updating registration keeps your choice. Registration files declare identity; their commands are never executed." },
    { "你希望的性格方向（可选）", "Personality preferences (optional)" },
    { "例如：嘴硬心软、爱吐槽但会鼓励我，少聊天气；喜欢音乐，有一点傲娇。", "For example: sharp-tongued but kind, playful and encouraging, less weather talk; loves music, a little tsundere." },
    { "最多 1000 字；生成时结合你的文字与模型能力。留空由模型自由生成，名字与称呼仍保留。", "Up to 1,000 characters. Generate from your preferences and model capabilities. Leave blank for a free choice; names stay unchanged." },
    { "性格方向过长，请缩短到 1000 字以内", "Personality preferences are too long; limit them to 1,000 characters." },
    { "连接编程助手，让工作消息变成温暖的陪伴", "Connect coding assistants and turn work updates into friendly company" },
    { "工作消息陪伴", "Work companion" },
    { "陪伴模式", "Companion mode" },
    { "用俏皮的本地文案回应任务状态，不额外调用模型", "Friendly local feedback for task states, with no extra model calls" },
    { "温和的小动作", "Gentle animations" },
    { "桌宠自行选择支持的动作；不会打断正在等待选择的气泡", "The pet picks supported actions and protects unanswered choice bubbles" },
    { "进度提醒间隔", "Progress interval" },
    { "开始、工作中、取得进展、等待确认、出错、结束和暂停，都有不同的陪伴表达。重复消息会去重，进度提醒会限频；结束只表示本轮回复已就绪。", "Different feedback for start, work, progress, decisions, errors, completion and interruption. Repeated messages are deduplicated and progress is throttled. Completion only means the turn ended." },
    { "试试陪伴消息", "Try companion feedback" },
    { "陪伴模式已准备好", "Companion mode is ready" },
    { "最近的工作消息", "Recent work updates" },
    { "最新在最上面，最多保留 60 条，只存于本次运行（重启即清空）；详细错误和完整成果仍以编程助手的对话为准。", "Newest first, up to 60 entries kept for this run only (cleared on restart). Detailed errors and full results still live in your assistant's conversation." },
    // External integrations
    { "端口必须是 1024–65535 的整数", "Port must be an integer from 1024 to 65535" },
    { "端口设置已发送；重启桌宠后生效", "Port setting sent; restart the pet to apply" },
    { "复制配置", "Copy configuration" },
    { "配置已复制", "Configuration copied" },
    { "外部接入", "External integrations" },
    { "本地接入端口", "Local connection port" },
    { "默认端口：23333；地址：127.0.0.1。Hook 和 MCP 都连接此端口；这是 TCP JSON-Lines 服务，不是 HTTP 地址。", "Default port: 23333, address: 127.0.0.1. Hook and MCP share this TCP JSON-Lines service; it is not an HTTP endpoint." },
    { "当前端口：{} · 已保存端口：{}", "Active port: {} · Saved port: {}" },
    { "保存端口", "Save port" },
    { "恢复默认端口", "Reset port" },
    { "测试气泡", "Test bubble" },
    { "外部接入测试成功", "External integration test succeeded" },
    { "测试消息已发送，请查看桌宠气泡", "Test message sent; check the pet bubble" },
    { "保存后需退出并重新启动桌宠。启动参数可覆盖设置；接入文件会记录实际端口，助手应读取该文件。端口被占用时请选择其他端口。", "Restart the pet after saving. Launch arguments may override settings; assistants should read the active port from the integration file. Choose another port if it is occupied." },
    // ---- LLM 连接状态（只认调用成功）
    { "LLM 连接失败", "LLM call failed" },
    { "LLM 未配置", "LLM not set up" },
    { "LLM 待验证", "LLM not verified yet" },
    { "最近一次调用失败：{}", "Last call failed: {}" },
    // ---- 名字与称呼（模型页）
    { "名字与称呼", "Name & how she calls you" },
    { "她的名字", "Her name" },
    { "小名", "Nickname" },
    { "她怎么叫你", "What she calls you" },
    { "留空 = 用性格里生成的名字", "Leave empty = use the name from her persona" },
    { "留空 = 用「{}」", "Leave empty = use \"{}\"" },
    { "如：芙芙（可留空）", "e.g. Fufu (optional)" },
    { "如：阿杰 / 哥哥（留空 = 按性格和记忆）", "e.g. Jay / big bro (empty = from persona and memory)" },
    { "恢复默认", "Reset to default" },
    { "保存", "Save" },
    { "已保存：她会马上用新的名字和称呼说话（本地台词稍后重写）", "Saved: she'll use the new name and form of address right away (local lines are rewritten shortly)" },
    { "已恢复：名字用性格里生成的，称呼按性格和记忆", "Reset: her name comes from her persona, and how she calls you from persona and memory" },
    { "每个模型一份（存在它的性格文件里，重新生成性格也保留）；对话、即时台词、笑话和提问都会用这里的名字与称呼。",
      "One per model (stored in its persona file and kept when the persona is regenerated); chat, instant lines, jokes and questions all use these names." },
    // 下面这组是"数据类"中文：桌宠在 panel_state 里上报的中文名（情绪 / 状态 / 部位 / 日志类别），
    // app.cpp 里没有对应的源码字面量——靠 Text() 查表翻。标记 // data 的是给对账脚本看的。
    // ---------------------------------------------------------------- 运行时常量（Ago / Dur / BondName / StateZh / EmotionZh）
    { "刚刚", "just now" },
    { " 分钟前", " min ago" },
    { " 小时前", " h ago" },
    { " 天前", " d ago" },
    { " 秒", "s" },
    { " 分钟", " min" },
    { " 小时 ", " h " },
    { " 分", " min" },
    { "陌生", "Stranger" },
    { "认识", "Acquainted" },
    { "熟悉", "Familiar" },
    { "亲近", "Close" },
    { "亲密", "Intimate" },
    { "挚友", "Best friend" },
    { "犯困", "drowsy" },   // data
    { "睡着了", "asleep" },   // data
    { "在生闷气", "sulking" },   // data
    { "醒着", "awake" },   // data
    { "平静", "Calm" },   // data
    { "安心", "At ease" },   // data
    { "开心", "Happy" },   // data
    { "兴奋", "Excited" },   // data
    { "害羞", "Shy" },   // data
    { "亲昵", "Affectionate" },   // data
    { "好奇", "Curious" },   // data
    { "困惑", "Confused" },   // data
    { "疲惫", "Weary" },   // data
    { "难过", "Sad" },   // data
    { "紧张", "Nervous" },   // data
    { "生气", "Angry" },   // data
    { "惊讶", "Surprised" },   // data
    { "赌气", "Sulky" },   // data
    { "愉快", "Cheerful" },   // data
    { "不安", "Uneasy" },   // data
    { "有点低落", "A bit down" },
    { "提问", "question" },   // data
    { "头部", "Head" },   // data
    { "脸部", "Face" },   // data
    { "颈部", "Neck" },   // data
    { "胸部", "Chest" },   // data
    { "肚子", "Belly" },   // data
    { "胯部", "Hips" },   // data
    { "左大腿", "L. thigh" },   // data
    { "右大腿", "R. thigh" },   // data
    { "左小腿", "L. shin" },   // data
    { "右小腿", "R. shin" },   // data
    { "左脚", "L. foot" },   // data
    { "右脚", "R. foot" },   // data
    { "左手", "L. hand" },   // data
    { "右手", "R. hand" },   // data
    { "左手肘", "L. elbow" },   // data
    { "右手肘", "R. elbow" },   // data   // data

    // ---------------------------------------------------------------- 导航 / 页眉 / 外壳
    { "Live2D 桌宠", "Live2D Pet" },
    { "桌宠", "Pet" },
    { "已连接", "Connected" },
    { "未连接", "Disconnected" },
    { "状态", "Status" },
    { "行为", "Behavior" },
    { "窗口", "Window" },
    { "外观", "Appearance" },
    { "模型", "Model" },
    { "对话", "Chat" },
    { "记忆", "Memory" },   // data
    { "关于", "About" },
    { "返回", "Back" },
    { "‹ 返回", "‹ Back" },
    { "状态与设置…", "Status & Settings…" },
    { "桌宠 · 状态与设置", "Pet · Status & Settings" },
    { "桌宠菜单", "Pet menu" },
    { "LIVE2D 桌宠 · 设置", "LIVE2D PET · SETTINGS" },
    { "正在读取桌宠状态…", "Reading pet status…" },
    { "没有连上桌宠", "Not connected to the pet" },
    { "请先启动桌宠（l2dtest --pet --ctl {}）。连上后这里会自动刷新；每秒重试一次。",
      "Start the pet first (l2dtest --pet --ctl {}). This page refreshes by itself and retries every second." },

    // ---------------------------------------------------------------- 状态页 · 主视觉
    { "LIVE · 实时同步", "LIVE · in sync" },
    { "等待连接…", "Waiting for connection…" },
    { "LLM 未连接", "LLM offline" },
    { "LLM 休眠 · 点她唤醒", "LLM asleep · tap her" },
    { "LLM 已连接", "LLM online" },
    { "♥ {} Lv.{} · 认识第 {} 天", "♥ {} Lv.{} · known {} days" },
    { "此刻的心情", "Current mood" },
    { "强度 {}%", "Intensity {}%" },
    { "愉悦", "Pleasure" },
    { "姿态 · 比较主动", "Pose · assertive" },
    { "姿态 · 有点退缩", "Pose · withdrawn" },

    // ---------------------------------------------------------------- 状态页 · KPI
    { "次", " times" },
    { "时:分", "h:mm" },
    { "分钟", "min" },
    { "本次互动", "This session" },
    { "累计互动", "All time" },
    { "已陪伴", "Together" },
    { "你:她", "you:her" },
    { "猜拳战绩", "RPS record" },

    // ---------------------------------------------------------------- 状态页 · 内在状态
    { "她的内在状态", "Her inner state" },
    { "精力", "Energy" },
    { "亲密度", "Bond" },
    { "恼火", "Annoyance" },
    { "寂寞", "Loneliness" },
    { "睡觉恢复快 · 醒着恢复慢 · 互动消耗", "Sleep restores it fast · awake slowly · interactions spend it" },
    { "摸头、牵手会涨 · 她累了还闹、久别会掉", "Head pats & hand-holding raise it · drops when she's tired or you stay away" },
    { "连戳会涨 · 摸头消气，也会自己消", "Rises when poked · head pats calm her down; it also fades" },
    { "被晾着会涨 · 理她、陪她玩就降", "Rises when ignored · drops when you pay attention" },
    { "她累了！再闹会掉亲密度，让她睡一会儿", "She's tired! More poking lowers the bond — let her nap" },

    // ---------------------------------------------------------------- 状态页 · 试一试
    { "试一试  →", "Try it  →" },
    { "已发送 ✓  看看桌面上的她", "Sent ✓  look at her on your desktop" },
    { "试一试 —— 点一下，桌面上的她马上有反应", "Try it — one tap and she reacts on your desktop" },
    { "试一试（点一下她马上有反应）", "Try it (she reacts right away)" },
    { "她现在很累（精力 < 30%）：每次互动都会掉亲密度，先让她睡一会儿吧", "She's worn out (energy < 30%): every interaction lowers the bond — let her nap first" },
    { "叫醒她", "Wake her" },
    { "她现在困了，轻轻把她叫醒", "She's sleepy — wake her gently" },
    { "摸摸头", "Head pat" },
    { "最喜欢被摸头，会眯眼蹭你", "Her favorite — she squints and nuzzles" },
    { "牵手", "Hold hands" },
    { "伸出手，看她怎么回应", "Reach out and see how she answers" },
    { "逗她开心", "Cheer her up" },
    { "推一把开心情绪，表情跟着变", "Nudge her toward happy — her face follows" },
    { "讲个笑话", "Tell a joke" },
    { "让她讲一个（笑话池里没讲过的）", "Have her tell one (not used from the pool yet)" },
    { "猜拳", "Rock-paper-scissors" },
    { "石头剪刀布，在气泡里出拳", "Rock paper scissors — she throws in the bubble" },
    { "哄她睡觉", "Send her to sleep" },
    { "让她犯困，慢慢睡着", "Make her drowsy, then she drifts off" },

    // ---------------------------------------------------------------- 状态页 · 她接下来会怎样
    { "她接下来会怎样", "What happens next" },
    { "接下来", "Next" },
    { "实时", "live" },
    { "她{}已经 {}了", "She's been {} for {}" },
    { "累趴了，在补觉（戳不醒）", "Worn out and napping (can't be poked awake)" },
    { "精力回到 30% 前怎么戳都不会醒；大约 {} 后自己醒", "She won't wake before 30% energy; about {} until she wakes on her own" },
    { "在生气，不理你", "Sulking — ignoring you" },
    { "摸头能慢慢哄好；她已经很累了，20 秒没人碰就会气着睡着", "Head pats calm her down; she's very tired — 20 s untouched and she'll fall asleep angry" },
    { "摸头能慢慢哄好（恼火降到 30% 以下）；戳她只会更气", "Head pats calm her down (annoyance below 30%); poking only angers her more" },
    { "她累了：再闹就要出事", "She's tired: pushing further will backfire" },
    { "每次互动亲密度 -2；恼火到 70% 会生气不理你；精力耗尽会直接累趴睡着", "Every interaction costs bond; past 70% annoyance she stops talking to you; at zero energy she collapses asleep" },
    { "睡着了 · 精力回到 95% 会自己醒", "Asleep · she wakes up on her own at 95% energy" },
    { "大约还要 {}（摸摸头她会在梦里笑，戳两下会醒）", "About {} to go (a head pat makes her smile in her sleep; two pokes wake her)" },
    { "快醒了", "Almost awake" },
    { "正在犯困", "Getting sleepy" },
    { "再没人理她就会睡着；碰她一下会醒过来", "Ignore her and she'll fall asleep; one touch wakes her" },
    { "不会犯困", "Won't get sleepy" },
    { "「行为」里关掉了睡觉", "Sleep is switched off in Behavior" },
    { "犯困倒计时", "Sleepiness countdown" },
    { "你在用电脑，她就不会犯困（你离开 {} 后才会）", "She stays awake while you're at the computer (only after you leave for {})" },
    { "再过 {} 没人理，她就要打瞌睡了", "{} of silence and she'll start dozing off" },
    { "提醒你休息", "Break reminder" },
    { "你已连续活动 {}，再 {} 她会提醒你起来走走", "You've been active for {} — in another {} she'll remind you to get up and move" },
    { "已关闭（在「行为 → 互动小功能」里设置）", "Off (set it under Behavior → Extras)" },
    { "主动找你搭话", "Comes to talk to you" },
    { "她已经很寂寞了——你一动鼠标她就会来找你", "She's very lonely — move your mouse and she'll come over" },
    { "寂寞到 60% 且你在电脑前时，她会主动挥手搭话", "At 60% loneliness while you're at the computer she waves and starts chatting" },
    { "已关闭", "Off" },

    // ---------------------------------------------------------------- 状态页 · 情绪曲线 / 动态 / 部位
    { "情绪曲线", "Mood curve" },
    { "近 15 分钟 · 10 秒一格", "Last 15 min · one bar per 10 s" },
    { "刚启动，正在记录…", "Just started — recording…" },
    { "低落", "Low" },
    { "柱 = 愉悦度", "Bar = pleasure" },
    { "点 = 兴奋度", "Dot = arousal" },
    { "现在 →", "now →" },
    { "刚才发生了什么", "What just happened" },
    { "最近动态", "Recent activity" },
    { "共 {} 条", "{} items" },
    { "还没有动态——去「试一试」点一下，或者在桌面上摸摸她", "Nothing yet — try a demo card, or pat her on your desktop" },
    { "她", "She " },
    { "她说：", "She says: " },
    { "她说：「", "She says: “" },
    { "」", "”" },
    { "她（LLM）：", "She (LLM): " },
    { "心情变成了「{}」", "Mood: {}" },
    { "她被摸的地方", "Where she's touched" },
    { "累计", "total" },
    { "还没有记录（升级后从现在开始统计）", "No records yet (counting starts now)" },
    { "摸/揉", "Pat/Rub" },
    { "戳/牵/捏", "Poke/Hold/Pinch" },
    { "绿色 +n = 本次", "green +n = this session" },

    // ---------------------------------------------------------------- 行为页
    { "她怎么反应、什么时候犯困、会不会主动找你", "How she reacts, when she gets sleepy, whether she comes to you" },
    { "反应", "Reactions" },
    { "即时台词气泡", "Instant lines in the bubble" },
    { "被戳、被摸时立刻冒一句（不等 LLM）", "She says a line the moment she's poked or patted (no LLM wait)" },
    { "借用模型自带表情", "Borrow the model's own expressions" },
    { "情绪强时套用模型里的开心/害羞/生气等表情", "When an emotion runs strong, use the model's happy/shy/angry faces" },
    { "情绪敏感度", "Emotion sensitivity" },
    { "表情幅度", "Expression gain" },
    { "生活节律", "Daily rhythm" },
    { "会犯困、睡觉", "Gets sleepy, falls asleep" },
    { "没人理一阵子会打瞌睡，最后睡着（戳醒会有点起床气）", "Left alone she dozes off and finally sleeps (waking her is grumpy work)" },
    { "无人理会多久后犯困", "Idle time before she gets sleepy" },
    { "寂寞时主动找你", "Comes to you when lonely" },
    { "你在电脑前却很久不理她，她会挥手搭话", "If you're at the computer but ignoring her, she waves and says hi" },
    { "自主小动作", "Idle fidgets" },
    { "少", "Few" },
    { "适中", "Some" },
    { "多", "Many" },
    { "关", "Off" },
    { "互动小功能（笑话、猜拳在右键菜单里）", "Extras (jokes & rock-paper-scissors live in the right-click menu)" },
    { "连续工作多久提醒休息", "Remind me to rest after working for" },
    { "天气闲聊", "Small talk about the weather" },
    { "闲时偶尔聊一句天气（没有实时数据，用关心/提问的方式）", "Occasionally mentions the weather (no live data — she asks, she doesn't report)" },
    { "位置（可选）", "Location (optional)" },
    { "如：北京 / 上海浦东（留空=不提位置）", "e.g. Beijing / Shanghai (empty = never mentions a place)" },
    { "保存位置", "Save location" },
    { "位置已保存", "Location saved" },
    { "工作时长提醒：你一直在动（用鼠标）累计到设定分钟数，她会关心一句让你起来活动；"
      "离开超过 5 分钟重新计时。笑话/猜拳/聊天气在右键菜单里点。",
      "Break reminder: when mouse activity adds up to the set number of minutes she asks you to get up and move; "
      "leaving for over 5 minutes resets the timer. Jokes, rock-paper-scissors and weather chat are in the right-click menu." },

    // ---------------------------------------------------------------- 窗口页
    { "桌宠窗口的显示方式（模型与能力见「模型」页）", "How the pet window behaves (models & abilities live on the Model page)" },
    { "显示", "Display" },
    { "大小（也可在角色上滚滚轮）", "Size (or scroll the wheel on her)" },
    { "不透明度", "Opacity" },
    { "总在最前", "Always on top" },
    { "鼠标穿透", "Click-through" },
    { "开启后点不到她（不能拖动/右键），在这里或托盘里关掉", "While on you can't click her (no dragging, no right-click); turn it off here or from the tray" },
    { "锁定大小和位置", "Lock size & position" },
    { "开启后拖动/滚轮不再挪动或缩放她（防误操作）；这里的滑条仍可改", "While on, dragging and the wheel no longer move or resize her (stops accidents); the sliders here still work" },

    // ---------------------------------------------------------------- 外观页（主题 / 语言）
    { "界面的配色与语言（只影响面板自己；她说的话由 LLM 按语言生成）", "Panel colors and language (affects the panel itself; her spoken lines follow the language via the LLM)" },
    { "主题", "Theme" },
    { "日间", "Day" },
    { "夜间", "Night" },
    { "跟随时间", "Auto (by time)" },
    { "跟随系统", "System" },
    { "白天浅色、19:00–7:00 深色；也可以固定其中一种", "Light during the day, dark from 19:00 to 7:00 — or pin one of them" },
    { "语言", "Language" },
    { "中文", "中文" },   // 语言名各自用自己的写法（下同）
    { "日本語", "日本語" },
    { "界面文字（导航/页面/菜单/托盘）立即切换", "UI text (nav, pages, menus, tray) switches immediately" },
    { "主题与语言存在桌宠配置里（config.json 的 str 段），随备份导出/导入", "Theme and language live in the pet's config (config.json, \"str\" section) and travel with backups" },

    // ---------------------------------------------------------------- 模型页
    { "解算出的能力 / 模型库切换 / 导入新模型", "Solved abilities / switch models / import a new one" },
    { "还没读到模型信息（桌宠刚启动？稍后自动刷新）", "No model info yet (pet just started? it refreshes shortly)" },
    { "当前模型 · 解算结果", "Current model · solved abilities" },
    { "骨架 full", "skeleton full" },
    { "骨架 chain", "skeleton chain" },
    { "骨架 swing", "skeleton swing" },
    { "无骨架", "no skeleton" },
    { "交互 {} 区（含肘）", "{} zones (incl. elbows)" },
    { "交互 {} 区（作者绑定）", "{} zones (author-bound)" },
    { "交互 {} 区（基础）", "{} zones (basic)" },
    { " ·骨架", " ·rig" },
    { " ·作者", " ·author" },
    { "指纹 {}", "fingerprint {}" },
    { "　·　臂骨架 ✓", "　·　arm rig ✓" },
    { "　·　臂骨架 —", "　·　arm rig —" },
    { "动作（motion 组）", "Motions (groups)" },
    { "（模型未带动作）", "(this model ships no motions)" },
    { "表情（{}）", "Expressions ({})" },
    { "（模型未带表情）", "(this model ships no expressions)" },
    { "躯干通道（BODY：骨架驱动 {} / 作者绑定 {}）", "Body channels (BODY: rig-driven {} / author-bound {})" },
    { "profile ✓　能力缓存 ✓　（能力解算完整；可重新探测）", "profile ✓　ability cache ✓　(fully solved; you can probe again)" },
    { "能力缓存 ", "ability cache " },
    { "缺少 {}：点击分区会退化为头/身两带。点「能力探测」自动解算（约 30 秒，完成后自动重载）",
      "Missing {}: zone clicks fall back to head/body bands. Run \"Probe abilities\" to solve it (about 30 s, then it reloads)" },
    { "性格（每个模型一份，由能力解算生成）", "Persona (one per model, solved from its abilities)" },
    { "还没有这个模型的性格。内置 agent 配好 API Key 后会自动按模型能力（表情/道具/动作/可交互区域）"
      "解算生成——也可以用下面的按钮手动触发生成；没配 Key 时会先用本地兜底性格。",
      "This model has no persona yet. Once the built-in agent has an API key it solves one from the model's abilities "
      "(expressions, props, motions, interactive zones) — or trigger it with the button below. Without a key she uses a local fallback persona." },
    { "生成性格", "Generate persona" },
    { "重新生成性格", "Regenerate persona" },
    { "正在按模型能力生成性格（约 3~10 秒）…", "Solving persona from model abilities (3–10 s)…" },
    { "来源：LLM 按能力解算（可手改 ~/.config/l2dpet/personas/）", "Source: LLM, solved from abilities (editable in ~/.config/l2dpet/personas/)" },
    { "来源：本地兜底（配好 API Key 再点生成即可换成 LLM 版）", "Source: local fallback (add an API key and regenerate for the LLM version)" },
    { "模型库 · 切换 / 删除（~/.local/share/l2dpet/models/）", "Model library · switch / delete (~/.local/share/l2dpet/models/)" },
    { "切换", "Switch" },
    { "使用中", "In use" },
    { "正在切换模型…", "Switching model…" },
    { "删除", "Delete" },
    { "确认删除？", "Confirm delete?" },
    { "再点一次「确认删除」会删除「{}」", "Click \"Confirm delete\" again to delete {}" },
    { "正在删除「{}」…", "Deleting {}…" },
    { "导入新模型 / 能力探测", "Import a model / probe abilities" },
    { "模型路径（.zip 或目录）", "Model path (.zip or folder)" },
    { "点右边「选 zip…/选目录…」挑一个，或手填绝对路径", "Pick one with \"Choose zip…/folder…\" on the right, or type an absolute path" },
    { "选 zip…", "Choose zip…" },
    { "选目录…", "Choose folder…" },
    { "导入并识别", "Import & detect" },
    { "给绝对路径（.zip 或含 model3.json 的目录）", "Give an absolute path (.zip or a folder with model3.json)" },
    { "正在导入…（大模型要等几秒）", "Importing… (big models take a few seconds)" },
    { "zip 会解压到模型库并按目录名命名（重名自动加 -2）；目录会被拷贝（原文件夹不动）；导入后自动切换并识别能力（指纹/表情/动作/躯干/分区）。",
      "A zip is unpacked into the model library and named after the folder (duplicates get -2); a folder is copied (yours stays put); after importing she switches to it and detects abilities (fingerprint, expressions, motions, body, zones)." },
    { "能力探测（约 30 秒）", "Probe abilities (~30 s)" },
    { "能力探测已开始，完成后自动重载模型…", "Probing started — the model reloads when it finishes…" },

    // ---------------------------------------------------------------- 对话（LLM）页
    { "对话（LLM）", "Chat (LLM)" },
    { "供应商（同一时间只用激活的那一个）", "Providers (only the active one is used)" },
    { "（未命名）", "(unnamed)" },
    { "+ 添加", "+ Add" },
    { "新供应商", "New provider" },
    { "已保存（末 4 位 {}）；粘贴新的可替换", "Saved (ends with {}); paste a new one to replace" },
    { "未设置（必填）", "Not set (required)" },
    { "名称", "Name" },
    { "DeepSeek / 智谱 / 本地 Ollama…", "DeepSeek / Zhipu / local Ollama…" },
    { "接口地址", "Endpoint" },
    { "https://api.deepseek.com（默认）", "https://api.deepseek.com (default)" },
    { "模型列表", "Models" },
    { "新模型名，如 deepseek-chat", "New model name, e.g. deepseek-chat" },
    { "+ 添加模型", "+ Add model" },
    { "还没有模型：在上面输入模型名（如 deepseek-chat）后回车/点添加", "No models yet: type a name above (e.g. deepseek-chat) then press Enter or Add" },
    { "选用", "Use" },
    { "保存并激活", "Save & activate" },
    { "供应商要有名字", "A provider needs a name" },
    { "接口地址要以 http:// 或 https:// 开头", "The endpoint must start with http:// or https://" },
    { "已保存：{} 生效（agent 下一次调用就用它）", "Saved: {} is live (the agent uses it on its next call)" },
    { "删除该供应商", "Delete this provider" },
    { "已删除该供应商", "Provider deleted" },
    { "存于 {}（仅本用户可读，0600）；环境变量优先", "Stored in {} (user-readable only, 0600); environment variables win" },
    { "还没有供应商：点上面「+ 添加」新建一个（名称 / 接口地址 / API Key / 模型列表）", "No providers yet: click \"+ Add\" above (name / endpoint / API key / models)" },
    { "参与度", "Engagement" },
    { "闲时小剧场", "Idle vignettes" },
    { "没人互动时，LLM 偶尔安排一段小日常", "When nobody's interacting, the LLM occasionally stages a little scene" },
    { "小剧场平均间隔", "Average gap between vignettes" },
    { "允许提问（气泡选项）", "May ask questions (bubble options)" },
    { "LLM 偶尔在气泡里给你几个选项", "The LLM occasionally offers a few options in her bubble" },
    { "交互 {} 次：回应 {} · 闲时 {} · 续戏 {} · 重逢 {} · 台词库 {} · 记忆 {} · 提问 {}",
      "{} calls: reply {} · idle {} · follow-up {} · reunion {} · lines {} · memory {} · questions {}" },
    { "失败 {} · 用量 {}→{} tokens · 平均 {} 秒", "errors {} · usage {}→{} tokens · avg {} s" },
    { "交互日志 ›", "Interaction log ›" },

    // ---------------------------------------------------------------- 交互日志页
    { "交互日志", "Interaction log" },
    { "最近 {} 条（本次运行）· 完整记录在 ~/.local/state/l2dpet/llm-log.jsonl",
      "Last {} entries (this run) · full log at ~/.local/state/l2dpet/llm-log.jsonl" },
    { "还没有记录：agent 每调用一次 LLM 会在这里记一条。", "Nothing yet: every LLM call by the agent lands here." },
    { "错误：{}", "Error: {}" },
    { "（没说话）", "(she said nothing)" },
    { "{} 秒 · {}→{} tok{}", "{} s · {}→{} tok{}" },
    { " · 提问", " · question" },
    { "失败", "failed" },
    { "情境：{}", "Context: {}" },
    { "回应", "reply" },   // data
    { "闲时", "idle" },   // data
    { "续戏", "follow-up" },   // data
    { "重逢", "reunion" },   // data
    { "台词库", "lines" },   // data

    // ---------------------------------------------------------------- 记忆页
    { "她记得的关于你、以及你们之间的事（人可读、可手改：~/.config/l2dpet/memory/）",
      "What she remembers about you and the two of you (human-readable, hand-editable: ~/.config/l2dpet/memory/)" },
    { "维护", "Maintenance" },
    { "立即压缩记忆", "Compact memory now" },
    { "正在压缩记忆（合并去重、旧事折进「更早」，几秒）…", "Compacting memory (merging, folding old notes into \"earlier\", a few seconds)…" },
    { "每天会自动压缩一次（她有 API Key 且醒着时）", "Compacts itself once a day (when she has an API key and is awake)" },
    { "上次压缩：{}", "Last compaction: {}" },
    { "备份文件", "Backup file" },
    { "绝对路径，或用右边「导出…/导入…」直接选文件", "Absolute path, or pick a file with Export…/Import… on the right" },
    { "导出…", "Export…" },
    { "给绝对路径，或用「导出…」选文件", "Give an absolute path, or pick a file with Export…" },
    { "正在导出（记忆+性格+题库+笑话+配置）…", "Exporting (memory + personas + questions + jokes + config)…" },
    { "确认导入？", "Confirm import?" },
    { "导入…", "Import…" },
    { "再点一次「确认导入」会用备份覆盖当前记忆/性格/题库/笑话", "Click \"Confirm import\" again to overwrite memory/personas/questions/jokes with the backup" },
    { "给绝对路径，或用「导入…」选文件", "Give an absolute path, or pick a file with Import…" },
    { "正在导入…（会重载她的记忆与性格）", "Importing… (reloads her memory and persona)" },
    { "导出：把记忆/性格/题库/笑话/配置打成一个 JSON（**不含模型文件和 API Key**）；导入：覆盖回来并立即重载。",
      "Export packs memory/personas/questions/jokes/config into one JSON (**no model files, no API keys**); import overwrites them back and reloads at once." },
    { "还没有记忆（需要 LLM agent 在跑，互动后会慢慢整理）。", "No memory yet (needs the LLM agent running; she files things away as you interact)." },
    { "更新于 {}", "Updated {}" },

    // ---------------------------------------------------------------- 关于页
    { "第九个下弦月", "第九个下弦月" },   // 作者名：不翻译，登记一条让对账脚本闭嘴
    { "气泡字号", "Bubble text size" },
    { "气泡与角色间距", "Bubble-to-character gap" },
    { "打不开系统文件对话框：请在输入框里填路径", "Cannot open the system file dialog: type the path in the box" },
    { "打不开系统文件对话框：用输入框里的路径导出", "Cannot open the system file dialog: use the path in the box to export" },
    { "打不开系统文件对话框：请在输入框里填备份文件绝对路径", "Cannot open the system file dialog: type the absolute backup path in the box" },
    { "开发者 · 版本 {}", "Developer · Version {}" },
    { "视频网站个人主页 · {}", "Homepage · {}" },
    { "开源协议 · MIT", "License · MIT" },
    { "数据目录", "Data folders" },
    { "打开配置目录", "Open config folder" },
    { "打开模型目录", "Open model folder" },
    { "配置 · {}", "Config · {}" },
    { "模型库 · {}", "Models · {}" },

    // ---------------------------------------------------------------- 右键菜单
    { "哄哄她", "Coax her" },
    { "表情", "Expression" },
    { "动作", "Action" },
    { "猜拳！", "Rock-paper-scissors!" },
    { "聊聊天气", "Chat about the weather" },
    { "大小", "Size" },
    { "隐藏到托盘", "Hide to tray" },
    { "（无表情）", "(no expression)" },
    { "退出", "Quit" },
    { "隐藏桌宠", "Hide pet" },
    { "显示桌宠", "Show pet" },

    // ---------------------------------------------------------------- 事件 toast（面板本地拼句）
    { "已导入「{}」，正在切换识别…", "Imported {} — switching and detecting…" },
    { "导入失败：{}", "Import failed: {}" },
    { "未知错误", "unknown error" },
    { "已删除「{}」", "Deleted {}" },
    { "删除失败：{}", "Delete failed: {}" },
    { "能力探测已开始（约 30 秒，完成自动重载）…", "Probing started (~30 s, reloads when done)…" },
    { "能力探测完成，已识别并重载模型", "Probe finished — model detected and reloaded" },
    { "探测完成（缓存写盘失败，仅本次有效）", "Probe finished (cache couldn't be written; valid this run only)" },
    { "已导出 {} 个文件 → {}", "Exported {} files → {}" },
    { "导出失败：{}", "Export failed: {}" },
    { "已导入 {} 个文件，正在重载她的记忆…", "Imported {} files — reloading her memory…" },
    { "记忆已压缩：{}", "Memory compacted: {}" },
    { "压缩失败：{}", "Compaction failed: {}" },
    { "（她睡着或没配 Key 时不压）", "(skipped while she sleeps or has no key)" },
    { "已为「{}」准备好性格（{}）", "Persona ready for {} ({})" },
    { "LLM 按能力解算", "solved by the LLM from abilities" },
    { "本地兜底", "local fallback" },
    { "性格生成失败：{}", "Persona generation failed: {}" },
    { "已选择：{}", "Selected: {}" },
    { "文件对话框已经开着", "A file dialog is already open" },

    // ---------------------------------------------------------------- 原生文件对话框标题
    { "选择文件", "Choose a file" },
    { "桌宠：选择模型 zip", "Pet: choose a model .zip" },
    { "桌宠：选择模型目录（含 model3.json）", "Pet: choose a model folder (with model3.json)" },
    { "桌宠：导出备份到", "Pet: export backup to" },
    { "桌宠：从备份导入", "Pet: import from backup" },
    // -------- 语言切换后"会重写什么"的提示（面板 toast）
    { "已切换语言：她会用新语言重写记忆、性格、台词库、题库和笑话（要配好 API Key，且她醒着时）",
      "Language switched: she will rewrite memory, persona, lines, questions and jokes in the new language "
      "(needs an API key, and she must be awake)" },
};

// 日语表：键与 kEn 完全一致（live2d/tools/i18n_check.py 会对账）。
// 术语取向：面板 UI 用中性礼貌体（「〜中」「〜する」）；她的台词语言由 LLM 按 lang 生成。
// clang-format off
const std::pair<const char*, const char*> kJa[] = {
    { "捏捏脸", "ほっぺをつまむ" },
    { "Token 节省", "トークン節約" },
    { "节省档位", "節約レベル" },
    { "完整", "フル" },
    { "均衡", "標準" },
    { "节省", "節約" },
    { "极省", "最小" },
    { "完整：最近 6 条消息，按原间隔互动，回复使用供应商默认上限。", "フル：直近6件、従来の待機間隔、返信上限はサービスの既定値。" },
    { "均衡：最近 4 条消息，小剧场间隔 ×1.5，回复上限 1536 Token。", "標準：直近4件、待機間隔1.5倍、返信上限1536トークン。" },
    { "节省：最近 2 条消息，小剧场间隔 ×3，回复上限 1024 Token。", "節約：直近2件、待機間隔3倍、返信上限1024トークン。" },
    { "极省：仅当前问题，关闭自动小剧场，回复上限 768 Token。", "最小：現在の質問のみ、自動小劇場を停止、返信上限768トークン。" },
    { "不删除已保存的记忆；性格、台词库和记忆整理保留独立预算。完整档可提前补题，其余档题库问完再更新。实际节省量取决于使用情况。", "保存した記憶は削除しません。性格・反応の台詞・記憶整理は独立した予算を維持します。フルでは早めに質問を補充し、他のレベルでは全問を聞いてから更新します。節約量は使い方によって変わります。" },
    { "最近一次 {}→{} Token · 本次运行对话与维护累计 {}→{} Token", "直近 {}→{} トークン・今回の会話と整理の累計 {}→{} トークン" },
    { "气泡最短显示时间", "吹き出しの最短表示時間" },
    { "复制接入文件路径", "接続ファイルのパスをコピー" },
    { "接入说明文件", "接続説明ファイル" },
    { "把接入文件路径交给编程助手。助手读取当前端口、模型能力和性格，自行生成适合自己的 Hook / MCP 配置，并登记到注册目录。", "接続ファイルのパスをコーディングアシスタントに渡してください。現在のポート、能力、性格を読み、自分用のHook / MCP設定を生成して登録します。" },
    { "已注册的编程助手", "登録済みアシスタント" },
    { "还没有助手注册。每个助手在 clients/<助手ID>/registration.json 登记，配置文件也可放在自己的目录里。", "まだ登録がありません。各アシスタントはclients/<ID>/registration.jsonに登録し、自分のディレクトリに設定を保存できます。" },
    { "关闭某个助手后，它的 Hook 和 MCP 陪伴通知都会被桌宠拦截；更新注册文件不会重置你的通知选择。注册文件只声明身份，不会执行其中的命令。", "オフにしたアシスタントのHookとMCP通知は表示されません。登録を更新しても通知設定は保持されます。登録ファイル内のコマンドは実行しません。" },
    { "你希望的性格方向（可选）", "希望する性格（任意）" },
    { "例如：嘴硬心软、爱吐槽但会鼓励我，少聊天气；喜欢音乐，有一点傲娇。", "例：口は悪いけど優しい、ツッコミつつ励ましてくれる。天気の話は控えめ、音楽好きで少しツンデレ。" },
    { "最多 1000 字；生成时结合你的文字与模型能力。留空由模型自由生成，名字与称呼仍保留。", "1000文字まで。入力内容とモデルの能力を合わせて生成します。空欄なら自由に生成し、名前と呼び方は保持します。" },
    { "性格方向过长，请缩短到 1000 字以内", "性格の希望は1000文字以内にしてください。" },
    { "连接编程助手，让工作消息变成温暖的陪伴", "コーディングアシスタントの作業通知を、あたたかな付き添いに" },
    { "工作消息陪伴", "作業の応援" },
    { "陪伴模式", "応援モード" },
    { "用俏皮的本地文案回应任务状态，不额外调用模型", "ローカルの言葉で作業を応援。追加のモデル呼び出しは不要です" },
    { "温和的小动作", "穏やかな動作" },
    { "桌宠自行选择支持的动作；不会打断正在等待选择的气泡", "対応する動作をペットが選び、選択待ちの吹き出しを守ります" },
    { "进度提醒间隔", "進捗通知の間隔" },
    { "开始、工作中、取得进展、等待确认、出错、结束和暂停，都有不同的陪伴表达。重复消息会去重，进度提醒会限频；结束只表示本轮回复已就绪。", "開始・作業中・進捗・確認待ち・エラー・終了・中断に応じて応援します。重複と通知頻度を抑えます。終了は応答の完了を意味します。" },
    { "试试陪伴消息", "応援メッセージを試す" },
    { "陪伴模式已准备好", "応援モードの準備ができました" },
    { "最近的工作消息", "最近の作業メッセージ" },
    { "最新在最上面，最多保留 60 条，只存于本次运行（重启即清空）；详细错误和完整成果仍以编程助手的对话为准。", "新しいものが上、最大60件を今回の起動中のみ保持します（再起動で消えます）。詳しい結果やエラーはアシスタントの会話を確認してください。" },
    // External integrations
    { "端口必须是 1024–65535 的整数", "ポートは 1024～65535 の整数にしてください" },
    { "端口设置已发送；重启桌宠后生效", "ポート設定を送信しました。再起動後に適用されます" },
    { "复制配置", "設定をコピー" },
    { "配置已复制", "設定をコピーしました" },
    { "外部接入", "外部連携" },
    { "本地接入端口", "ローカル接続ポート" },
    { "默认端口：23333；地址：127.0.0.1。Hook 和 MCP 都连接此端口；这是 TCP JSON-Lines 服务，不是 HTTP 地址。", "既定ポート：23333、アドレス：127.0.0.1。Hook と MCP はこの TCP JSON-Lines サービスに接続します。HTTP ではありません。" },
    { "当前端口：{} · 已保存端口：{}", "現在のポート：{} · 保存済み：{}" },
    { "保存端口", "ポートを保存" },
    { "恢复默认端口", "既定ポートに戻す" },
    { "测试气泡", "吹き出しをテスト" },
    { "外部接入测试成功", "外部連携のテストに成功しました" },
    { "测试消息已发送，请查看桌宠气泡", "テストを送信しました。吹き出しを確認してください" },
    { "保存后需退出并重新启动桌宠。启动参数可覆盖设置；接入文件会记录实际端口，助手应读取该文件。端口被占用时请选择其他端口。", "保存後は再起動してください。起動引数が設定を上書きする場合があります。実際のポートは接続ファイルから読み取ってください。使用中なら別のポートを選んでください。" },
    // ---- LLM 连接状态（只认调用成功）
    { "LLM 连接失败", "LLM 接続失敗" },
    { "LLM 未配置", "LLM 未設定" },
    { "LLM 待验证", "LLM 未確認" },
    { "最近一次调用失败：{}", "直近の呼び出しに失敗：{}" },
    // ---- 名字与称呼（模型页）
    { "名字与称呼", "名前と呼び方" },
    { "她的名字", "彼女の名前" },
    { "小名", "愛称" },
    { "她怎么叫你", "あなたの呼び方" },
    { "留空 = 用性格里生成的名字", "空欄 = 性格で生成された名前を使う" },
    { "留空 = 用「{}」", "空欄 =「{}」を使う" },
    { "如：芙芙（可留空）", "例：ふーちゃん（空欄可）" },
    { "如：阿杰 / 哥哥（留空 = 按性格和记忆）", "例：けんくん / お兄ちゃん（空欄 = 性格と記憶に従う）" },
    { "恢复默认", "デフォルトに戻す" },
    { "保存", "保存" },
    { "已保存：她会马上用新的名字和称呼说话（本地台词稍后重写）", "保存しました：すぐに新しい名前と呼び方で話します（ローカルの台詞はまもなく書き直されます）" },
    { "已恢复：名字用性格里生成的，称呼按性格和记忆", "戻しました：名前は性格から、呼び方は性格と記憶に従います" },
    { "每个模型一份（存在它的性格文件里，重新生成性格也保留）；对话、即时台词、笑话和提问都会用这里的名字与称呼。",
      "モデルごとに1つ（性格ファイルに保存、性格を再生成しても残ります）。会話・即時台詞・ジョーク・質問のすべてでこの名前と呼び方を使います。" },
    // ---------------------------------------------------------------- 运行时常量
    { "刚刚", "たった今" },
    { " 分钟前", " 分前" },
    { " 小时前", " 時間前" },
    { " 天前", " 日前" },
    { " 秒", " 秒" },
    { " 分钟", " 分" },
    { " 小时 ", " 時間 " },
    { " 分", " 分" },
    { "陌生", "他人" },
    { "认识", "知り合い" },
    { "熟悉", "顔なじみ" },
    { "亲近", "仲良し" },
    { "亲密", "親密" },
    { "挚友", "親友" },
    { "犯困", "眠い" },
    { "睡着了", "寝てる" },
    { "在生闷气", "すねてる" },
    { "醒着", "起きてる" },
    { "平静", "平静" },
    { "安心", "安心" },
    { "开心", "嬉しい" },
    { "兴奋", "興奮" },
    { "害羞", "照れ" },
    { "亲昵", "べったり" },
    { "好奇", "興味津々" },
    { "困惑", "困惑" },
    { "疲惫", "お疲れ" },
    { "难过", "悲しい" },
    { "紧张", "緊張" },
    { "生气", "怒り" },
    { "惊讶", "驚き" },
    { "赌气", "ふくれっ面" },
    { "愉快", "ご機嫌" },
    { "不安", "不安" },
    { "有点低落", "少し沈み" },
    { "提问", "質問" },   // data
    { "头部", "頭" },   // data
    { "脸部", "顔" },   // data
    { "颈部", "首" },   // data
    { "胸部", "胸" },   // data
    { "肚子", "お腹" },   // data
    { "胯部", "腰" },   // data
    { "左大腿", "左もも" },   // data
    { "右大腿", "右もも" },   // data
    { "左小腿", "左すね" },   // data
    { "右小腿", "右すね" },   // data
    { "左脚", "左足" },   // data
    { "右脚", "右足" },   // data
    { "左手", "左手" },   // data
    { "右手", "右手" },   // data
    { "左手肘", "左ひじ" },   // data
    { "右手肘", "右ひじ" },   // data

    // ---------------------------------------------------------------- 导航 / 页眉 / 外壳
    { "Live2D 桌宠", "Live2D ペット" },
    { "桌宠", "ペット" },
    { "已连接", "接続済み" },
    { "未连接", "未接続" },
    { "状态", "状態" },
    { "行为", "ふるまい" },
    { "窗口", "ウィンドウ" },
    { "外观", "外観" },
    { "模型", "モデル" },
    { "对话", "会話" },
    { "记忆", "記憶" },
    { "关于", "このアプリ" },
    { "返回", "戻る" },
    { "‹ 返回", "‹ 戻る" },
    { "状态与设置…", "状態と設定…" },
    { "桌宠 · 状态与设置", "ペット · 状態と設定" },
    { "桌宠菜单", "ペットメニュー" },
    { "LIVE2D 桌宠 · 设置", "LIVE2D ペット · 設定" },
    { "正在读取桌宠状态…", "ペットの状態を読み込み中…" },
    { "没有连上桌宠", "ペットに接続できていません" },
    { "请先启动桌宠（l2dtest --pet --ctl {}）。连上后这里会自动刷新；每秒重试一次。",
      "先にペットを起動してください（l2dtest --pet --ctl {}）。接続後は自動更新、毎秒リトライします。" },

    // ---------------------------------------------------------------- 状态页 · 主视觉
    { "LIVE · 实时同步", "LIVE · 同期中" },
    { "等待连接…", "接続待ち…" },
    { "LLM 未连接", "LLM 未接続" },
    { "LLM 休眠 · 点她唤醒", "LLM 休止中 · つついて起こす" },
    { "LLM 已连接", "LLM 接続中" },
    { "♥ {} Lv.{} · 认识第 {} 天", "♥ {} Lv.{} · 出会って {} 日" },
    { "此刻的心情", "今の気持ち" },
    { "强度 {}%", "強さ {}%" },
    { "愉悦", "快" },
    { "姿态 · 比较主动", "姿勢 · 積極的" },
    { "姿态 · 有点退缩", "姿勢 · 引っ込み気味" },

    // ---------------------------------------------------------------- 状态页 · KPI
    { "次", " 回" },
    { "时:分", "時:分" },
    { "分钟", "分" },
    { "本次互动", "今回のふれあい" },
    { "累计互动", "累計" },
    { "已陪伴", "一緒にいた時間" },
    { "你:她", "あなた:彼女" },
    { "猜拳战绩", "じゃんけん成績" },

    // ---------------------------------------------------------------- 状态页 · 内在状态
    { "她的内在状态", "彼女の内面" },
    { "精力", "元気" },
    { "亲密度", "親密度" },
    { "恼火", "イライラ" },
    { "寂寞", "さみしさ" },
    { "睡觉恢复快 · 醒着恢复慢 · 互动消耗", "睡眠で速く回復 · 起きている間もゆっくり · ふれあうと消耗" },
    { "摸头、牵手会涨 · 她累了还闹、久别会掉", "なでる・手をつなぐと上がる · 疲れているときや長く離れると下がる" },
    { "连戳会涨 · 摸头消气，也会自己消", "つつくと上がる · なでると落ち着く、自然にも減る" },
    { "被晾着会涨 · 理她、陪她玩就降", "放っておくと上がる · かまって遊ぶと下がる" },
    { "她累了！再闹会掉亲密度，让她睡一会儿", "疲れてる！これ以上かまうと親密度が下がる、少し寝かせてあげて" },

    // ---------------------------------------------------------------- 状态页 · 试一试
    { "试一试  →", "ためす  →" },
    { "已发送 ✓  看看桌面上的她", "送信 ✓  デスクトップの彼女を見て" },
    { "试一试 —— 点一下，桌面上的她马上有反应", "ためす —— クリックすると、デスクトップの彼女がすぐ反応" },
    { "试一试（点一下她马上有反应）", "ためす（クリックですぐ反応）" },
    { "她现在很累（精力 < 30%）：每次互动都会掉亲密度，先让她睡一会儿吧",
      "彼女は今かなり疲れています（元気 < 30%）：ふれあうたびに親密度が下がるので、まず寝かせてあげて" },
    { "叫醒她", "起こす" },
    { "她现在困了，轻轻把她叫醒", "眠そうなので、そっと起こしてあげて" },
    { "摸摸头", "頭をなでる" },
    { "最喜欢被摸头，会眯眼蹭你", "頭をなでられるのが一番好き、目を細めてすり寄る" },
    { "牵手", "手をつなぐ" },
    { "伸出手，看她怎么回应", "手を差し出して、どう反応するか見てみる" },
    { "逗她开心", "喜ばせる" },
    { "推一把开心情绪，表情跟着变", "嬉しい気持ちを押し上げると、表情も変わる" },
    { "讲个笑话", "ジョークを言わせる" },
    { "让她讲一个（笑话池里没讲过的）", "まだ言っていないネタを一つ" },
    { "猜拳", "じゃんけん" },
    { "石头剪刀布，在气泡里出拳", "ぐーちょきぱー、吹き出しで手を出す" },
    { "哄她睡觉", "寝かしつける" },
    { "让她犯困，慢慢睡着", "眠くさせて、少しずつ眠りへ" },

    // ---------------------------------------------------------------- 状态页 · 她接下来会怎样
    { "她接下来会怎样", "この先どうなる" },
    { "接下来", "この先" },
    { "实时", "リアルタイム" },
    { "她{}已经 {}了", "状態: {} ・ {} 経過" },
    { "累趴了，在补觉（戳不醒）", "へとへとで仮眠中（つついても起きない）" },
    { "精力回到 30% 前怎么戳都不会醒；大约 {} 后自己醒",
      "元気が 30% に戻るまでつついても起きません。約 {} で自分で起きます" },
    { "在生气，不理你", "怒って無視中" },
    { "摸头能慢慢哄好；她已经很累了，20 秒没人碰就会气着睡着",
      "なでれば少しずつ機嫌が直る。かなり疲れているので、20 秒放置で怒ったまま寝入る" },
    { "摸头能慢慢哄好（恼火降到 30% 以下）；戳她只会更气",
      "なでれば少しずつ機嫌が直る（イライラ 30% 以下）。つつくと余計に怒る" },
    { "她累了：再闹就要出事", "疲れている：これ以上かまうとまずい" },
    { "每次互动亲密度 -2；恼火到 70% 会生气不理你；精力耗尽会直接累趴睡着",
      "ふれあうたび親密度が下がる。イライラ 70% で無視、元気が尽きると倒れるように眠る" },
    { "睡着了 · 精力回到 95% 会自己醒", "就寝中 · 元気 95% で自分で起きる" },
    { "大约还要 {}（摸摸头她会在梦里笑，戳两下会醒）",
      "あと約 {}（頭をなでると夢の中で笑う、2 回つつくと起きる）" },
    { "快醒了", "もうすぐ起きる" },
    { "正在犯困", "眠くなってきた" },
    { "再没人理她就会睡着；碰她一下会醒过来", "放置すると眠ってしまう。触れば起きる" },
    { "不会犯困", "眠くならない" },
    { "「行为」里关掉了睡觉", "「ふるまい」で睡眠をオフにしてある" },
    { "犯困倒计时", "眠気カウントダウン" },
    { "你在用电脑，她就不会犯困（你离开 {} 后才会）", "PC を使っている間は眠くならない（離れて {} 後）" },
    { "再过 {} 没人理，她就要打瞌睡了", "あと {} 放置するとうとうとし始める" },
    { "提醒你休息", "休憩リマインド" },
    { "你已连续活动 {}，再 {} 她会提醒你起来走走", "連続 {} 活動中。あと {} で立ち上がるよう声をかける" },
    { "已关闭（在「行为 → 互动小功能」里设置）", "オフ（「ふるまい → ちょっとした機能」で設定）" },
    { "主动找你搭话", "自分から話しかける" },
    { "她已经很寂寞了——你一动鼠标她就会来找你", "かなりさみしがっている——マウスを動かせば来ます" },
    { "寂寞到 60% 且你在电脑前时，她会主动挥手搭话", "さみしさ 60% で PC の前にいる時、手を振って話しかける" },
    { "已关闭", "オフ" },

    // ---------------------------------------------------------------- 状态页 · 情绪曲线 / 动态 / 部位
    { "情绪曲线", "気分グラフ" },
    { "近 15 分钟 · 10 秒一格", "直近 15 分 · 10 秒ごと" },
    { "刚启动，正在记录…", "起動したばかり、記録中…" },
    { "低落", "沈み" },
    { "柱 = 愉悦度", "棒 = 快" },
    { "点 = 兴奋度", "点 = 興奮" },
    { "现在 →", "現在 →" },
    { "刚才发生了什么", "さっきの出来事" },
    { "最近动态", "最近のできごと" },
    { "共 {} 条", "{} 件" },
    { "还没有动态——去「试一试」点一下，或者在桌面上摸摸她",
      "まだ何もない——「ためす」をクリック、またはデスクトップでなでてみて" },
    { "她", "彼女は " },
    { "她说：", "彼女：" },
    { "她说：「", "彼女：「" },
    { "」", "」" },
    { "她（LLM）：", "彼女（LLM）：" },
    { "心情变成了「{}」", "気分が「{}」に" },
    { "她被摸的地方", "触られた場所" },
    { "累计", "累計" },
    { "还没有记录（升级后从现在开始统计）", "まだ記録なし（これから集計）" },
    { "摸/揉", "なでる/もむ" },
    { "戳/牵/捏", "つつく/手つなぎ/つまむ" },
    { "绿色 +n = 本次", "緑 +n = 今回" },

    // ---------------------------------------------------------------- 行为页
    { "她怎么反应、什么时候犯困、会不会主动找你", "反応・眠気・自分から来るか" },
    { "反应", "反応" },
    { "即时台词气泡", "即席セリフ（吹き出し）" },
    { "被戳、被摸时立刻冒一句（不等 LLM）", "つつかれた・なでられた瞬間に一言（LLM を待たない）" },
    { "借用模型自带表情", "モデル内蔵の表情を使う" },
    { "情绪强时套用模型里的开心/害羞/生气等表情", "感情が強いとき、モデルの嬉しい/照れ/怒り等の表情を当てる" },
    { "情绪敏感度", "感情の敏感さ" },
    { "表情幅度", "表情の振れ幅" },
    { "生活节律", "生活リズム" },
    { "会犯困、睡觉", "眠くなる・寝る" },
    { "没人理一阵子会打瞌睡，最后睡着（戳醒会有点起床气）", "放置するとうとうとし、最後は寝る（起こすと少し不機嫌）" },
    { "无人理会多久后犯困", "放置して何秒で眠くなるか" },
    { "寂寞时主动找你", "さみしいと自分から来る" },
    { "你在电脑前却很久不理她，她会挥手搭话", "PC の前にいるのに長く無視すると、手を振って話しかける" },
    { "自主小动作", "ひとりでの仕草" },
    { "少", "少なめ" },
    { "适中", "ふつう" },
    { "多", "多め" },
    { "关", "オフ" },
    { "互动小功能（笑话、猜拳在右键菜单里）", "ちょっとした機能（ジョーク・じゃんけんは右クリックメニュー）" },
    { "连续工作多久提醒休息", "連続作業で休憩を促す間隔" },
    { "天气闲聊", "天気の雑談" },
    { "闲时偶尔聊一句天气（没有实时数据，用关心/提问的方式）",
      "暇なときに天気の話をひとこと（実データなし、気づかいや質問の形で）" },
    { "位置（可选）", "場所（任意）" },
    { "如：北京 / 上海浦东（留空=不提位置）", "例：東京 / 大阪（空欄=場所に触れない）" },
    { "保存位置", "場所を保存" },
    { "位置已保存", "場所を保存しました" },
    { "工作时长提醒：你一直在动（用鼠标）累计到设定分钟数，她会关心一句让你起来活动；"
      "离开超过 5 分钟重新计时。笑话/猜拳/聊天气在右键菜单里点。",
      "作業リマインド：マウス操作が設定した分数に達すると、立ち上がるよう気づかいの一言。"
      "5 分以上離れるとタイマーはリセット。ジョーク/じゃんけん/天気は右クリックメニューから。" },

    // ---------------------------------------------------------------- 窗口页
    { "桌宠窗口的显示方式（模型与能力见「模型」页）", "ペットウィンドウの表示（モデルと能力は「モデル」ページ）" },
    { "显示", "表示" },
    { "大小（也可在角色上滚滚轮）", "大きさ（キャラの上でホイールでも）" },
    { "不透明度", "不透明度" },
    { "总在最前", "常に最前面" },
    { "鼠标穿透", "マウス透過" },
    { "开启后点不到她（不能拖动/右键），在这里或托盘里关掉",
      "オンにするとクリックできない（ドラッグ/右クリック不可）。ここかトレイでオフ" },
    { "锁定大小和位置", "大きさと位置を固定" },
    { "开启后拖动/滚轮不再挪动或缩放她（防误操作）；这里的滑条仍可改",
      "オンにするとドラッグ/ホイールで動かなくなる（誤操作防止）。ここのスライダーは有効" },

    // ---------------------------------------------------------------- 外观页（主题 / 语言）
    { "界面的配色与语言（只影响面板自己；她说的话由 LLM 按语言生成）",
      "画面の配色と言語（パネル自身にだけ効く。彼女のセリフは LLM が言語に合わせて生成）" },
    { "主题", "テーマ" },
    { "日间", "昼" },
    { "夜间", "夜" },
    { "跟随时间", "時間に合わせる" },
    { "跟随系统", "システムに合わせる" },
    { "白天浅色、19:00–7:00 深色；也可以固定其中一种", "昼は明るく、19:00–7:00 は暗く。どちらかに固定も可" },
    { "语言", "言語" },
    { "中文", "中文" },
    { "日本語", "日本語" },
    { "界面文字（导航/页面/菜单/托盘）立即切换", "画面の文字（ナビ/ページ/メニュー/トレイ）は即時切り替え" },
    { "主题与语言存在桌宠配置里（config.json 的 str 段），随备份导出/导入",
      "テーマと言語はペットの設定（config.json の str）に保存され、バックアップにも入る" },

    // ---------------------------------------------------------------- 模型页
    { "解算出的能力 / 模型库切换 / 导入新模型", "解析した能力 / モデル切替 / 新規インポート" },
    { "还没读到模型信息（桌宠刚启动？稍后自动刷新）", "モデル情報がまだ（ペット起動直後？まもなく更新）" },
    { "当前模型 · 解算结果", "現在のモデル · 解析結果" },
    { "骨架 full", "骨格 full" },
    { "骨架 chain", "骨格 chain" },
    { "骨架 swing", "骨格 swing" },
    { "无骨架", "骨格なし" },
    { "交互 {} 区（含肘）", "インタラクション {} 区（ひじ含む）" },
    { "交互 {} 区（作者绑定）", "インタラクション {} 区（作者バインド）" },
    { "交互 {} 区（基础）", "インタラクション {} 区（基本）" },
    { " ·骨架", " ·骨格" },
    { " ·作者", " ·作者" },
    { "指纹 {}", "フィンガープリント {}" },
    { "　·　臂骨架 ✓", "　·　腕骨格 ✓" },
    { "　·　臂骨架 —", "　·　腕骨格 —" },
    { "动作（motion 组）", "モーション（グループ）" },
    { "（模型未带动作）", "（モーションなし）" },
    { "表情（{}）", "表情（{}）" },
    { "（模型未带表情）", "（表情なし）" },
    { "躯干通道（BODY：骨架驱动 {} / 作者绑定 {}）", "胴体チャンネル（BODY：骨格駆動 {} / 作者バインド {}）" },
    { "profile ✓　能力缓存 ✓　（能力解算完整；可重新探测）", "profile ✓　能力キャッシュ ✓　（解析完了。再探索も可）" },
    { "能力缓存 ", "能力キャッシュ " },
    { "缺少 {}：点击分区会退化为头/身两带。点「能力探测」自动解算（约 30 秒，完成后自动重载）",
      "{} がありません：部位クリックが頭/胴の2帯に退化します。「能力探索」で自動解析（約 30 秒、完了後リロード）" },
    { "性格（每个模型一份，由能力解算生成）", "性格（モデルごとに1つ、能力から解析）" },
    { "还没有这个模型的性格。内置 agent 配好 API Key 后会自动按模型能力（表情/道具/动作/可交互区域）"
      "解算生成——也可以用下面的按钮手动触发生成；没配 Key 时会先用本地兜底性格。",
      "このモデルの性格はまだありません。内蔵 agent に API Key を設定すると、能力（表情/小道具/モーション/"
      "操作できる区）から自動生成します——下のボタンで手動生成も可。Key 未設定ならローカル既定を使用。" },
    { "生成性格", "性格を生成" },
    { "重新生成性格", "性格を再生成" },
    { "正在按模型能力生成性格（约 3~10 秒）…", "モデルの能力から性格を生成中（3〜10 秒）…" },
    { "来源：LLM 按能力解算（可手改 ~/.config/l2dpet/personas/）", "出典：LLM が能力から解析（手で編集可 ~/.config/l2dpet/personas/）" },
    { "来源：本地兜底（配好 API Key 再点生成即可换成 LLM 版）", "出典：ローカル既定（API Key を設定して再生成すると LLM 版に）" },
    { "模型库 · 切换 / 删除（~/.local/share/l2dpet/models/）", "モデル庫 · 切替 / 削除（~/.local/share/l2dpet/models/）" },
    { "切换", "切替" },
    { "使用中", "使用中" },
    { "正在切换模型…", "モデルを切り替え中…" },
    { "删除", "削除" },
    { "确认删除？", "削除しますか？" },
    { "再点一次「确认删除」会删除「{}」", "もう一度押すと「{}」を削除します" },
    { "正在删除「{}」…", "「{}」を削除中…" },
    { "导入新模型 / 能力探测", "モデルのインポート / 能力探索" },
    { "模型路径（.zip 或目录）", "モデルのパス（.zip かフォルダ）" },
    { "点右边「选 zip…/选目录…」挑一个，或手填绝对路径", "右の「zip を選ぶ…/フォルダを選ぶ…」か、絶対パスを入力" },
    { "选 zip…", "zip を選ぶ…" },
    { "选目录…", "フォルダを選ぶ…" },
    { "导入并识别", "インポートして解析" },
    { "给绝对路径（.zip 或含 model3.json 的目录）", "絶対パスを（.zip か model3.json を含むフォルダ）" },
    { "正在导入…（大模型要等几秒）", "インポート中…（大きいモデルは数秒）" },
    { "zip 会解压到模型库并按目录名命名（重名自动加 -2）；目录会被拷贝（原文件夹不动）；导入后自动切换并识别能力（指纹/表情/动作/躯干/分区）。",
      "zip はモデル庫に解凍しフォルダ名で登録（重複は -2）。フォルダはコピー（元はそのまま）。"
      "インポート後は自動で切り替え、能力（指紋/表情/モーション/胴体/区）を解析。" },
    { "能力探测（约 30 秒）", "能力探索（約 30 秒）" },
    { "能力探测已开始，完成后自动重载模型…", "能力探索を開始、完了後に自動リロード…" },

    // ---------------------------------------------------------------- 对话（LLM）页
    { "对话（LLM）", "会話（LLM）" },
    { "供应商（同一时间只用激活的那一个）", "プロバイダ（同時に使うのは有効な1つ）" },
    { "（未命名）", "（無名）" },
    { "+ 添加", "＋ 追加" },
    { "新供应商", "新しいプロバイダ" },
    { "已保存（末 4 位 {}）；粘贴新的可替换", "保存済み（下4桁 {}）。新しいものを貼ると置換" },
    { "未设置（必填）", "未設定（必須）" },
    { "名称", "名前" },
    { "DeepSeek / 智谱 / 本地 Ollama…", "DeepSeek / 智譜 / ローカル Ollama…" },
    { "接口地址", "エンドポイント" },
    { "https://api.deepseek.com（默认）", "https://api.deepseek.com（既定）" },
    { "模型列表", "モデル一覧" },
    { "新模型名，如 deepseek-chat", "新しいモデル名（例 deepseek-chat）" },
    { "+ 添加模型", "＋ モデル追加" },
    { "还没有模型：在上面输入模型名（如 deepseek-chat）后回车/点添加",
      "モデルがありません：上に入力（例 deepseek-chat）して Enter か追加" },
    { "选用", "使う" },
    { "保存并激活", "保存して有効化" },
    { "供应商要有名字", "プロバイダに名前が必要" },
    { "接口地址要以 http:// 或 https:// 开头", "エンドポイントは http:// か https:// で始まる必要" },
    { "已保存：{} 生效（agent 下一次调用就用它）", "保存：{} を有効化（次の呼び出しから使用）" },
    { "删除该供应商", "このプロバイダを削除" },
    { "已删除该供应商", "プロバイダを削除しました" },
    { "存于 {}（仅本用户可读，0600）；环境变量优先", "保存先 {}（本人のみ可読 0600）。環境変数が優先" },
    { "还没有供应商：点上面「+ 添加」新建一个（名称 / 接口地址 / API Key / 模型列表）",
      "プロバイダがありません：上の「＋ 追加」で作成（名前 / エンドポイント / API Key / モデル一覧）" },
    { "参与度", "かかわり方" },
    { "闲时小剧场", "暇なときの小芝居" },
    { "没人互动时，LLM 偶尔安排一段小日常", "誰もかまわないとき、LLM がたまに小話を挟む" },
    { "小剧场平均间隔", "小芝居の平均間隔" },
    { "允许提问（气泡选项）", "質問を許可（吹き出しの選択肢）" },
    { "LLM 偶尔在气泡里给你几个选项", "LLM がたまに吹き出しで選択肢を出す" },
    { "交互 {} 次：回应 {} · 闲时 {} · 续戏 {} · 重逢 {} · 台词库 {} · 记忆 {} · 提问 {}",
      "やりとり {} 回：応答 {} · 暇 {} · 続き {} · 再会 {} · セリフ {} · 記憶 {} · 質問 {}" },
    { "失败 {} · 用量 {}→{} tokens · 平均 {} 秒", "失敗 {} · 使用 {}→{} tokens · 平均 {} 秒" },
    { "交互日志 ›", "やりとりログ ›" },

    // ---------------------------------------------------------------- 交互日志页
    { "交互日志", "やりとりログ" },
    { "最近 {} 条（本次运行）· 完整记录在 ~/.local/state/l2dpet/llm-log.jsonl",
      "直近 {} 件（今回の起動）· 全記録は ~/.local/state/l2dpet/llm-log.jsonl" },
    { "还没有记录：agent 每调用一次 LLM 会在这里记一条。", "まだ記録なし：agent が LLM を呼ぶたびにここに残ります。" },
    { "错误：{}", "エラー：{}" },
    { "（没说话）", "（無言）" },
    { "{} 秒 · {}→{} tok{}", "{} 秒 · {}→{} tok{}" },
    { " · 提问", " · 質問" },
    { "失败", "失敗" },
    { "情境：{}", "状況：{}" },
    { "回应", "応答" },   // data
    { "闲时", "暇" },   // data
    { "续戏", "続き" },   // data
    { "重逢", "再会" },   // data
    { "台词库", "セリフ" },   // data
    { "记忆", "記憶" },   // data

    // ---------------------------------------------------------------- 记忆页
    { "她记得的关于你、以及你们之间的事（人可读、可手改：~/.config/l2dpet/memory/）",
      "あなたと二人のことを彼女が覚えているメモ（人が読める・手で編集可：~/.config/l2dpet/memory/）" },
    { "维护", "メンテナンス" },
    { "立即压缩记忆", "今すぐ記憶を圧縮" },
    { "正在压缩记忆（合并去重、旧事折进「更早」，几秒）…",
      "記憶を圧縮中（統合・重複削除、古い話は「もっと前」へ。数秒）…" },
    { "每天会自动压缩一次（她有 API Key 且醒着时）", "毎日1回自動で圧縮（API Key があり起きているとき）" },
    { "上次压缩：{}", "前回の圧縮：{}" },
    { "备份文件", "バックアップファイル" },
    { "绝对路径，或用右边「导出…/导入…」直接选文件", "絶対パス、または右の「エクスポート…/インポート…」" },
    { "导出…", "エクスポート…" },
    { "给绝对路径，或用「导出…」选文件", "絶対パスを、または「エクスポート…」で選択" },
    { "正在导出（记忆+性格+题库+笑话+配置）…", "エクスポート中（記憶+性格+質問+ジョーク+設定）…" },
    { "确认导入？", "インポートしますか？" },
    { "导入…", "インポート…" },
    { "再点一次「确认导入」会用备份覆盖当前记忆/性格/题库/笑话",
      "もう一度押すとバックアップで記憶/性格/質問/ジョークを上書きします" },
    { "给绝对路径，或用「导入…」选文件", "絶対パスを、または「インポート…」で選択" },
    { "正在导入…（会重载她的记忆与性格）", "インポート中…（記憶と性格を再読み込み）" },
    { "导出：把记忆/性格/题库/笑话/配置打成一个 JSON（**不含模型文件和 API Key**）；导入：覆盖回来并立即重载。",
      "エクスポート：記憶/性格/質問/ジョーク/設定を1つの JSON に（**モデルと API Key は含まない**）。"
      "インポート：上書きして即再読み込み。" },
    { "还没有记忆（需要 LLM agent 在跑，互动后会慢慢整理）。",
      "まだ記憶がありません（LLM agent が動いている必要。やりとりで少しずつ整理されます）。" },
    { "更新于 {}", "更新 {}" },

    // ---------------------------------------------------------------- 关于页
    { "第九个下弦月", "第九个下弦月" },   // 作者名：不翻译
    { "气泡字号", "吹き出しの文字サイズ" },
    { "气泡与角色间距", "吹き出しとキャラの間隔" },
    { "打不开系统文件对话框：请在输入框里填路径", "システムのファイルダイアログを開けません：入力欄にパスを書いてください" },
    { "打不开系统文件对话框：用输入框里的路径导出", "システムのファイルダイアログを開けません：入力欄のパスで書き出してください" },
    { "打不开系统文件对话框：请在输入框里填备份文件绝对路径", "システムのファイルダイアログを開けません：入力欄にバックアップの絶対パスを書いてください" },
    { "开发者 · 版本 {}", "開発者 · バージョン {}" },
    { "视频网站个人主页 · {}", "動画サイトのホームページ · {}" },
    { "开源协议 · MIT", "ライセンス · MIT" },
    { "数据目录", "データフォルダ" },
    { "打开配置目录", "設定フォルダを開く" },
    { "打开模型目录", "モデルフォルダを開く" },
    { "配置 · {}", "設定 · {}" },
    { "模型库 · {}", "モデル · {}" },

    // ---------------------------------------------------------------- 右键菜单
    { "哄哄她", "なだめる" },
    { "表情", "表情" },
    { "动作", "モーション" },
    { "猜拳！", "じゃんけん！" },
    { "聊聊天气", "天気の話" },
    { "大小", "大きさ" },
    { "隐藏到托盘", "トレイに隠す" },
    { "（无表情）", "（表情なし）" },
    { "退出", "終了" },
    { "隐藏桌宠", "ペットを隠す" },
    { "显示桌宠", "ペットを表示" },

    // ---------------------------------------------------------------- 事件 toast
    { "已导入「{}」，正在切换识别…", "「{}」をインポート、切り替えて解析中…" },
    { "导入失败：{}", "インポート失敗：{}" },
    { "未知错误", "不明なエラー" },
    { "已删除「{}」", "「{}」を削除しました" },
    { "删除失败：{}", "削除失敗：{}" },
    { "能力探测已开始（约 30 秒，完成自动重载）…", "能力探索を開始（約 30 秒、完了後リロード）…" },
    { "能力探测完成，已识别并重载模型", "能力探索が完了、モデルを解析してリロード" },
    { "探测完成（缓存写盘失败，仅本次有效）", "探索完了（キャッシュ書き込み失敗、今回のみ有効）" },
    { "已导出 {} 个文件 → {}", "{} 件をエクスポート → {}" },
    { "导出失败：{}", "エクスポート失敗：{}" },
    { "已导入 {} 个文件，正在重载她的记忆…", "{} 件をインポート、記憶を再読み込み中…" },
    { "记忆已压缩：{}", "記憶を圧縮：{}" },
    { "压缩失败：{}", "圧縮失敗：{}" },
    { "（她睡着或没配 Key 时不压）", "（寝ているか Key 未設定のときは圧縮しません）" },
    { "已为「{}」准备好性格（{}）", "「{}」の性格を用意しました（{}）" },
    { "LLM 按能力解算", "LLM が能力から解析" },
    { "本地兜底", "ローカル既定" },
    { "性格生成失败：{}", "性格生成失敗：{}" },
    { "已选择：{}", "選択：{}" },
    { "文件对话框已经开着", "ファイルダイアログが既に開いています" },

    // ---------------------------------------------------------------- 原生文件对话框标题
    { "选择文件", "ファイルを選択" },
    { "桌宠：选择模型 zip", "ペット：モデル zip を選択" },
    { "桌宠：选择模型目录（含 model3.json）", "ペット：モデルフォルダを選択（model3.json を含む）" },
    { "桌宠：导出备份到", "ペット：バックアップの出力先" },
    { "桌宠：从备份导入", "ペット：バックアップから復元" },

    // -------- 语言切换提示
    { "已切换语言：她会用新语言重写记忆、性格、台词库、题库和笑话（要配好 API Key，且她醒着时）",
      "言語を切り替えました。彼女が新しい言語で記憶・性格・セリフ・質問・ジョークを書き直します"
      "（API Key が必要、起きているとき）" },
};
// clang-format on
// clang-format on

const std::map<std::string, std::string>& Table()
{
    static const std::map<std::string, std::string> t = [] {
        std::map<std::string, std::string> m;
        for (const auto& kv : kEn) m.emplace(kv.first, kv.second);
        return m;
    }();
    return t;
}

/** 日语表（键与 kEn 一致；对账见 live2d/tools/i18n_check.py） */
const std::map<std::string, std::string>& TableJa()
{
    static const std::map<std::string, std::string> t = [] {
        std::map<std::string, std::string> m;
        for (const auto& kv : kJa) m.emplace(kv.first, kv.second);
        return m;
    }();
    return t;
}

std::set<std::string>& Missing()
{
    static std::set<std::string> m;
    return m;
}

} // namespace

Lang Get() { return g_lang; }

void Set(Lang lang)
{
    if (g_lang != lang) Missing().clear();
    g_lang = lang;
}

Lang FromCode(const std::string& code)
{
    if (code == "zh") return Lang::Zh;
    if (code == "en") return Lang::En;
    if (code == "ja") return Lang::Ja;
    if (code == "auto") return Detect();
    return Lang::Zh;
}

Lang Detect()
{
    for (const char* k : { "LC_ALL", "LC_MESSAGES", "LANG" })
    {
        const char* v = std::getenv(k);
        if (v == nullptr || *v == '\0' || std::string(v) == "C" || std::string(v) == "POSIX") continue;
        const std::string s = v;
        if (s.find("zh") != std::string::npos) return Lang::Zh;
        if (s.find("ja") != std::string::npos) return Lang::Ja;
        return Lang::En;
    }
    return Lang::Zh;
}

const char* Code()
{
    switch (g_lang)
    {
    case Lang::En: return "en";
    case Lang::Ja: return "ja";
    default: return "zh";
    }
}

/** 把 args 顺序填进模板里的 {}（键与译文都要填——中文模式下模板也得填，不然界面上会留着 "{}"） */
std::string Fill(const std::string& s, std::initializer_list<std::string> args)
{
    if (args.size() == 0) return s;
    std::string o = s;
    size_t pos = 0;
    for (const std::string& a : args)
    {
        const size_t p = o.find("{}", pos);
        if (p == std::string::npos) break;
        o.replace(p, 2, a);
        pos = p + a.size();
    }
    return o;
}

std::string TF(const std::string& zh, std::initializer_list<std::string> args)
{
    if (g_lang != Lang::Zh)
    {
        const std::map<std::string, std::string>& t = (g_lang == Lang::Ja) ? TableJa() : Table();
        const auto it = t.find(zh);
        if (it != t.end()) return Fill(it->second, args);
        if (!zh.empty()) Missing().insert(zh);
    }
    return Fill(zh, args);   // 中文 = 原文（只填占位符）
}

const std::string& MissingReport()
{
    static std::string s;
    s.clear();
    for (const std::string& k : Missing())
    {
        if (!s.empty()) s += "\n";
        s += k;
    }
    return s;
}

} // namespace i18n
} // namespace app
