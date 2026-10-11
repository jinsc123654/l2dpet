#include "../src/LlmClient.h"
#include <cassert>
#include <iostream>

// Run against the local fixture in token_usage_test.py; no real provider or credentials.
int main(int argc, char** argv)
{
    assert(argc == 2);
    l2d::LlmSettings cfg;
    cfg.baseUrl = argv[1];
    cfg.key = "test-only";
    cfg.maxReplyTokens = 768;
    std::vector<l2d::LlmMsg> messages(1);
    messages[0].role = "user";
    messages[0].content = "test";
    std::string content, error;
    assert(l2d::LlmChat(cfg, messages, 0.3f, true, &content, &error, NULL));
    assert(content == "{\"steps\":[]}");
    auto stats = l2d::LlmGetTokenStats();
    assert(stats.total.tokIn == 100 && stats.total.tokOut == 20);
    // Maintenance has no cap; truncated output still counts, but is rejected.
    cfg.maxReplyTokens = 0;
    l2d::LlmUsage usage;
    assert(!l2d::LlmChat(cfg, messages, 0.3f, true, &content, &error, &usage));
    assert(error.find("输出上限") != std::string::npos);
    stats = l2d::LlmGetTokenStats();
    assert(stats.last.tokIn == 150 && stats.last.tokOut == 30);
    assert(stats.total.tokIn == 250 && stats.total.tokOut == 50);
    assert(usage.tokIn == 150 && usage.tokOut == 30);
    std::cout << "PASS null-usage maintenance accounting and truncated-output rejection\n";
}
