/*
 * L2dCmd.h — 宿主 → 播放器的**指令环**契约（C 与 C++ 共用）
 *
 * 宿主把**低频事件**（表情/动作/手势/演出脚本的 JSON）以**文本行**送进播放器，
 * 一条一行，不覆盖、按序执行。JSON 的解析只在播放器里做一份（宿主保持"哑"）。
 *
 * 结构：单生产者（宿主）/ 单消费者（播放器）的字节环，'\n' 为行界。
 *   · 宿主写：把行复制到 data[head % SIZE]（可跨环尾），再把 head 原子前推
 *     （release 语义）；环满/行过长 → 丢行并把 dropped 计数 +1（不阻塞宿主）。
 *   · 播放器读：从 tail 追着 head 取字节，攒行、执行，把 tail 原子前推（release）。
 * 计数都是 uint32 累计量（自然回绕），模运算下差值即有效字节数——不用 sem/futex，
 * 只靠原子读写。
 *
 * caps 区：播放器装载模型后写入能力表（表情清单/动作组/手势表，见下），供宿主
 * 应答 LIST 查询——宿主（纯 C）因此**不用解析 model3.json**。
 */
#ifndef L2D_CMD_H
#define L2D_CMD_H

#include <stdint.h>
#include <stddef.h>

#include "L2dAtomicCompat.h" /* __atomic_* 内建在 MSVC 下的等价实现（Linux 侧为空头） */

#define L2D_CMD_MAGIC 0x4c324443u /* "L2DC" */

#define L2D_CMD_RING_SIZE 65536u  /* 指令环容量（字节；行内 JSON 可到几 KB） */
#define L2D_CMD_CAPS_SIZE 12288u  /* 能力表容量（文本，见下；舒芙蕾的 PARAMGROUPS 很长） */
#define L2D_CMD_PARAMS_SIZE 32768u /* 参数清单容量（全量参数表；见下）*/
#define L2D_CMD_REPLY_SIZE 256u   /* 单条指令处理结果（子写父读，见下）*/

/** 控制头 + 数据环 + 能力表；父进程与子进程各自 mmap 同一 memfd */
typedef struct
{
    uint32_t magic;      /* L2D_CMD_MAGIC */
    volatile uint32_t head;    /* 生产游标（父写；累计字节数） */
    volatile uint32_t tail;    /* 消费游标（子写；累计字节数） */
    volatile uint32_t lines;   /* 子进程已执行的行数（诊断/心跳） */
    volatile uint32_t dropped; /* 父进程因环满丢弃的行数（诊断） */
    volatile uint32_t capsReady; /* 子进程写好 caps 后置 1 */
    volatile uint32_t replySeq; /* 子进程每执行一条指令 +1（回复就绪信号） */
    char fingerprint[24]; /* 子进程写：模型指纹 "0x%016llx"（caps 内的 FP= 同值） */
    char reply[L2D_CMD_REPLY_SIZE]; /* 子进程写：最近一条指令的结果（单行；可空） */
    char caps[L2D_CMD_CAPS_SIZE]; /* 能力表（UTF-8 文本；见下） */
    char params[L2D_CMD_PARAMS_SIZE]; /* 参数清单（全量参数表；PARAMS 查询用；见下）*/
    char data[L2D_CMD_RING_SIZE]; /* SPSC 文本环（'\n' 分行） */
} l2d_cmd_ring_t;

/** 整区字节数（父进程 ftruncate / mmap 用） */
static inline size_t l2d_cmd_size(void)
{
    return sizeof(l2d_cmd_ring_t);
}

/** 环内有效字节数（父/子都可用；只在各自窗口内准确） */
static inline uint32_t l2d_cmd_pending(const l2d_cmd_ring_t* r)
{
    return __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)
         - __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
}

/*
 * 能力表格式（caps 区文本，行尾 '\n'；名字为 UTF-8，协议命令一律用序号）：
 *   MODEL=<目录名>
 *   FP=0x<16位hex>   —— 模型指纹（参数表/drawable 规模派生，装载时子进程算）。
 *                      外部下发前先记下它，指令行首加 "@<fp> " 即可防空转/串扰：
 *                      指纹不匹配（切了模型/换了模型包）的指令会被拒绝执行。
 *   EXPR=<个数>:名字1,名字2,...
 *   MOTION=<组名>:<个数>;<组名>:<个数>;...
 *   GESTURE=名字1,名字2,...
 *   IDLE=<默认自主行为强度 0..3>
 *   PARAMGROUPS=<组名>:<参数id>,<参数id>;...   —— cdi3 的参数分组（身体/手/头发/裙…），
 *                                                 让外部（LLM）知道"这个模型能控制哪些部位"
 *
 * 参数清单（params 区文本，行尾 '\n'；PARAMS 查询用；来自 moc3 + cdi3 + 效果探测）：
 *   PARAMS=<个数>
 *   # eff: d=顶点直连 p=仅经物理链 a=换美术(不透明度) -=无效果 ?=未探测
 *   P <id> <默认值> <最小值> <最大值> <组名> <中文名> eff=<码>[:<顶点位移>]
 *   例：P ParamarmR1 0.000 -1.000 1.000 身体 手臂R1 eff=p:0.106
 *   —— eff 是"这个参数动了到底有没有用、靠哪条链生效"，来自播放器的效果探测
 *      （PROBE/PROBEPHYS），结果同时缓存到 <模型目录>/l2d.capability.json。
 *      外部（LLM）据此挑参数：d/p 走参数轨道直接驱动；a 类参数走的是"换美术"
 *      （眨眼/道具/黑脸这类），要在参数表里看到它是它、动的是不透明度。
 *
 * 指令行（data 环）：可选行首 "@<fp> " 前缀（指纹校验，见上）；
 *   其余同 DESIGN-LLM-CONTROL.md §5（该文档只在 main 分支；本分支桌面协议见 PROTOCOL-LLM.md）（EXPR/MOTION/GESTURE/LOOK/IDLE/RESET/TAP/
 *   TALK <秒|文本>/PROBE [ALL]/PROBEPHYS [ALL]/JSON 脚本）。
 *   TALK = 说话口型：给秒数，或给文本（按字数估时长）；口型写在轨道之后，
 *   叠在表情姿态上（"举着话筒说话"就是 mic_hold/麦克风 + TALK）。
 *   BODY = **躯体通道**（像控制身体一样控制模型）：`BODY head.x=20 arm.r=0.6` 批量设定、
 *   `BODY reset` 释放、`BODY`（无参）查询当前值。通道清单在 caps 的 BODYCHANNELS=。
 *   语义：**增量**（叠加在呼吸/视线之上，身体仍然"活着"），设定后每帧保持、一阶平滑
 *   过渡（看得到"动过去"的过程）；参数不存在的通道该模型自动没有（跨模型降级）。
 *   标准通道：head.x/y/z、body.x/y/z、gaze.x/y、eye.l/r、brow.l/r(.form)、
 *   mouth.open/form；模型专属通道由 profile 的 "body" 段定义（如舒芙蕾的 arm.l/arm.r）。
 *   演出脚本 JSON 里也可带可选字段："fp":"0x…" / "model":"<目录名>"——
 *   对不上就整段拒绝（防"给 A 编的脚本放到 B 上播"）。
 *   脚本 step 里的说话：{ "at": 0, "talk": 8 }（秒）／{ "talk": "你好呀" }（文本）／
 *   { "talk": { "dur": 8, "text": "…" } }；手势可带 "expr"（姿态类手势自带表情，
 *   如 profile 的 mic_hold=麦克风+举话筒幅度、paws_up=猫猫手）。
 *
 * 回复区（reply 区 + replySeq）：子进程每执行一条指令把结果写成一**单行**文本
 * （"OK …" / "REJECT …"），并把 replySeq +1；父进程按需读取（seq 变化 = 有新回复）。
 *   单行、可空；不保证只读一次——上一条的回复可能被下一条覆盖（按需轮询）。
 *
 * 演出脚本的 "params" 步（JSON steps 元素）：
 *   { "at": 2.0, "params": { "ParamarmR1": -0.7, "ParamarmR3": 0.4 }, "dur": 1.2 }
 *   —— 数值是**增量**（Add 语义，与手势/视线一致），dur 内缓动到位并**保持**到脚本结束
 *      （保持靠"轨道活到场景末尾"实现）；写到同一参数的后一步会覆盖前一步。
 */

#endif /* L2D_CMD_H */
