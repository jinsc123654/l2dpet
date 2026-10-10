/*
 * JsonLite.h — 够用且健壮的小 JSON 解析器（播放器内部：profile 与演出脚本）
 *
 * 为什么不直接用 SDK 自带的 Utils::CubismJson：实测它对**手写/紧凑格式**有解析 bug
 *（同一内容：`indent=1` 多行能过、单行紧凑失败；`[[0,0],[1,0]]` 这种紧凑嵌套数组
 * 直接解析失败）。官方 SDK 只在编辑器序列化出来的文件上用它（model3/exp3），恰好
 * 绕开了那些路径。本工程的 profile 是手写的、演出脚本是 **LLM 生成的（格式不可控）**，
 * 必须用一个按 JSON 规范写全的解析器。
 *
 * 支持：对象/数组/字符串（含 \" \\ \/ \b \f \n \r \t \uXXXX 转义）/数字（含指数）/
 * true/false/null；任意空白；UTF-8 原样透传。单头文件、无依赖、纯 C++98 子集可用。
 */
#ifndef L2D_JSON_LITE_H
#define L2D_JSON_LITE_H

#include <string>
#include <utility>
#include <vector>

namespace jsonlite
{

struct Value
{
    enum Type
    {
        NUL,
        BOOL,
        NUM,
        STR,
        ARR,
        OBJ
    };

    Type type;
    bool b;
    double num;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value> > obj;

    Value() : type(NUL), b(false), num(0.0) {}

    /** 构造用（解析器只解析不构建；内置 agent 等需要拼 JSON 时用） */
    void setType(Type t) { type = t; }

    bool isNull() const { return type == NUL; }
    bool isBool() const { return type == BOOL; }
    bool isNum() const { return type == NUM; }
    bool isStr() const { return type == STR; }
    bool isArr() const { return type == ARR; }
    bool isObj() const { return type == OBJ; }

    /** 成员查找（对象）；找不到返回内置空值 */
    const Value &get(const char *key) const
    {
        for (size_t i = 0; i < obj.size(); ++i)
        {
            if (obj[i].first == key)
            {
                return obj[i].second;
            }
        }
        static const Value kNull;
        return kNull;
    }

    bool has(const char *key) const
    {
        for (size_t i = 0; i < obj.size(); ++i)
        {
            if (obj[i].first == key)
            {
                return true;
            }
        }
        return false;
    }

    double numOr(double d) const { return type == NUM ? num : d; }

    /** 取字符串；非字符串返回 fallback */
    const char *strOr(const char *fallback) const
    {
        return type == STR ? str.c_str() : fallback;
    }
};

namespace detail
{

struct Parser
{
    const char *p;
    const char *end;
    std::string err;

    Parser(const char *s, size_t n) : p(s), end(s + n) {}

    // 嵌套深度上限：递归下降解析，"[[[[…" 6 万层就会把栈打爆（实测段错误）——协议行、LLM 回复、
    // 备份文件都经这里，都可能是外来数据。正常数据嵌套不超过十几层。
    static const int kMaxDepth = 64;
    int depth = 0;

    void skipWs()
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        {
            ++p;
        }
    }

    bool parseValue(Value &v)
    {
        skipWs();
        if (p >= end)
        {
            err = "意外结束";
            return false;
        }
        switch (*p)
        {
        case '{':
        case '[':
        {
            if (++depth > kMaxDepth)
            {
                err = "嵌套太深";
                return false;
            }
            const bool ok = *p == '{' ? parseObject(v) : parseArray(v);
            --depth;
            return ok;
        }
        case '"':
            v.type = Value::STR;
            return parseString(v.str);
        case 't':
            if (end - p >= 4 && p[1] == 'r' && p[2] == 'u' && p[3] == 'e')
            {
                p += 4;
                v.type = Value::BOOL;
                v.b = true;
                return true;
            }
            err = "非法字面量";
            return false;
        case 'f':
            if (end - p >= 5 && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e')
            {
                p += 5;
                v.type = Value::BOOL;
                v.b = false;
                return true;
            }
            err = "非法字面量";
            return false;
        case 'n':
            if (end - p >= 4 && p[1] == 'u' && p[2] == 'l' && p[3] == 'l')
            {
                p += 4;
                v.type = Value::NUL;
                return true;
            }
            err = "非法字面量";
            return false;
        default:
            return parseNumber(v);
        }
    }

    bool parseObject(Value &v)
    {
        v.type = Value::OBJ;
        ++p; /* '{' */
        skipWs();
        if (p < end && *p == '}')
        {
            ++p;
            return true;
        }
        for (;;)
        {
            skipWs();
            std::string key;
            if (p >= end || *p != '"')
            {
                err = "对象键必须是字符串";
                return false;
            }
            if (!parseString(key))
            {
                return false;
            }
            skipWs();
            if (p >= end || *p != ':')
            {
                err = "对象缺冒号";
                return false;
            }
            ++p;
            Value child;
            if (!parseValue(child))
            {
                return false;
            }
            v.obj.push_back(std::make_pair(key, child));
            skipWs();
            if (p < end && *p == ',')
            {
                ++p;
                continue;
            }
            if (p < end && *p == '}')
            {
                ++p;
                return true;
            }
            err = "对象缺逗号或右括号";
            return false;
        }
    }

    bool parseArray(Value &v)
    {
        v.type = Value::ARR;
        ++p; /* '[' */
        skipWs();
        if (p < end && *p == ']')
        {
            ++p;
            return true;
        }
        for (;;)
        {
            Value child;
            if (!parseValue(child))
            {
                return false;
            }
            v.arr.push_back(child);
            skipWs();
            if (p < end && *p == ',')
            {
                ++p;
                continue;
            }
            if (p < end && *p == ']')
            {
                ++p;
                return true;
            }
            err = "数组缺逗号或右括号";
            return false;
        }
    }

    static void appendUtf8(std::string &out, unsigned cp)
    {
        if (cp < 0x80)
        {
            out += (char)cp;
        }
        else if (cp < 0x800)
        {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        else
        {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    static int hex4(const char *s)
    {
        int v = 0;

        for (int i = 0; i < 4; ++i)
        {
            const char c = s[i];

            v <<= 4;
            if (c >= '0' && c <= '9')
            {
                v |= c - '0';
            }
            else if (c >= 'a' && c <= 'f')
            {
                v |= c - 'a' + 10;
            }
            else if (c >= 'A' && c <= 'F')
            {
                v |= c - 'A' + 10;
            }
            else
            {
                return -1;
            }
        }
        return v;
    }

    bool parseString(std::string &out)
    {
        ++p; /* '"' */
        out.clear();
        while (p < end)
        {
            const unsigned char c = (unsigned char)*p;

            if (c == '"')
            {
                ++p;
                return true;
            }
            if (c == '\\')
            {
                ++p;
                if (p >= end)
                {
                    break;
                }
                switch (*p)
                {
                case '"': out += '"'; ++p; break;
                case '\\': out += '\\'; ++p; break;
                case '/': out += '/'; ++p; break;
                case 'b': out += '\b'; ++p; break;
                case 'f': out += '\f'; ++p; break;
                case 'n': out += '\n'; ++p; break;
                case 'r': out += '\r'; ++p; break;
                case 't': out += '\t'; ++p; break;
                case 'u':
                {
                    if (end - p < 5)
                    {
                        err = "\\u 转义不完整";
                        return false;
                    }
                    int cp = hex4(p + 1);

                    if (cp < 0)
                    {
                        err = "\\u 转义非法";
                        return false;
                    }
                    p += 5;
                    /* 代理对：高位后紧跟 \uDC00-\uDFFF */
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u')
                    {
                        const int lo = hex4(p + 2);

                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                        {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                    appendUtf8(out, (unsigned)cp);
                    break;
                }
                default:
                    err = "未知转义";
                    return false;
                }
                continue;
            }
            out += (char)c; /* UTF-8 原样透传 */
            ++p;
        }
        err = "字符串未闭合";
        return false;
    }

    bool parseNumber(Value &v)
    {
        const char *start = p;

        if (p < end && (*p == '-' || *p == '+'))
        {
            ++p;
        }
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' || *p == 'E' || *p == '+' || *p == '-'))
        {
            ++p;
        }
        if (p == start)
        {
            err = "非法字符";
            return false;
        }
        v.type = Value::NUM;
        v.num = atof(std::string(start, (size_t)(p - start)).c_str());
        return true;
    }
};

} // namespace detail

/** 解析一整份 JSON 文本；成功返回 true，失败把原因写进 err */
inline bool parse(const char *text, size_t len, Value &out, std::string *err)
{
    detail::Parser ps(text, len);

    if (!ps.parseValue(out))
    {
        if (err != NULL)
        {
            *err = ps.err;
        }
        return false;
    }
    ps.skipWs();
    if (ps.p != ps.end)
    {
        /* 允许结尾空白；有其它内容（如 OneLine 里塞了两份）算失败 */
        if (err != NULL)
        {
            *err = "多余内容";
        }
        return false;
    }
    return true;
}

} // namespace jsonlite

#endif /* L2D_JSON_LITE_H */
