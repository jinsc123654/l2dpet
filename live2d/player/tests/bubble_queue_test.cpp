#include "../src/PetBubbleQueue.h"
#include <cassert>
#include <iostream>

int main()
{
    PetBubbleQueue queue;
    PetBubbleQueue::Bubble old, next, question, out;
    old.text = "正在查看代码";
    next.text = "构建通过";
    question.text = "你喜欢哪个？";
    question.shown = question.options = {"甲", "乙"};
    question.talkDur = 4.0f;   // speak_dur 之类的显式口型时长要随气泡一起排队到上屏那一刻
    assert(queue.Offer(old, 10, 3));
    assert(queue.PendingCount() == 0);
    assert(!queue.Offer(next, 10.1, 3));
    assert(queue.PendingCount() == 1);
    assert(!queue.Offer(question, 10.2, 3));
    assert(queue.PendingCount() == 2);
    assert(queue.HasQuestion());
    assert(!queue.Take(12.99, 3, false, &out));
    assert(queue.Take(13, 3, false, &out) && out.text == next.text);
    assert(!queue.Take(15.99, 3, false, &out));
    assert(!queue.Take(16, 3, true, &out));
    assert(queue.Take(16, 3, false, &out) && out.options == question.options && out.talkDur == 4.0f);
    assert(!queue.HasQuestion());
    assert(!queue.Offer(old, 16.1, 3));
    queue.Clear();
    assert(!queue.Take(100, 3, false, &out));
    assert(queue.Offer(next, 16.2, 3));

    // 长气泡（LLM 回复最长 16 秒）按自己的 duration 占满时间：最短时间一到，新台词也只能排队，不许顶掉它
    PetBubbleQueue longq;
    PetBubbleQueue::Bubble reply, line, after;
    reply.text = "这段回复要说满十六秒";
    reply.duration = 16.0f;
    line.text = "嗯？";
    line.duration = 2.6f;
    assert(longq.Offer(reply, 0, 3));                              // 空闲：立即显示，按住 16 秒
    assert(!longq.Offer(line, 3.5, 3));                            // 3.5 秒时新台词到（已过最短 3 秒）：排队
    assert(!longq.Take(3.5, 3, false, &out));                      // 每帧轮询也不能提前换掉
    assert(!longq.Take(15.9, 3, false, &out));
    assert(longq.Take(16, 3, false, &out) && out.text == line.text);   // 说满时长后才轮到新台词
    after.text = "再说一句";
    assert(!longq.Offer(after, 17, 3));
    assert(!longq.Take(18.9, 3, false, &out));                     // 短气泡也至少占满最短时间 3 秒（2.6 → 3）
    assert(longq.Take(19, 3, false, &out) && out.text == after.text);

    // 最短时间只兜底、不削短：duration 更长就用 duration，调大配置也不缩气泡
    assert(PetBubbleQueue::Hold(reply, 3) == 16);
    assert(PetBubbleQueue::Hold(line, 3) == 3);
    assert(PetBubbleQueue::Hold(line, 5) == 5);

    std::cout << "PASS minimum duration, long-bubble no-interrupt, message ordering, question payload, choice protection and reset\n";
}
