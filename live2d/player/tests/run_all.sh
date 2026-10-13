#!/usr/bin/env bash
# run_all.sh — 编译并运行 live2d/player/tests 下的全部单测（不需要 Cubism SDK，不碰用户目录）
#
# 用法：bash live2d/player/tests/run_all.sh [构建目录]
#   构建目录默认 $L2D_TEST_BUILD，再默认临时目录（每次新建，跑完不清理，失败时好翻产物）。
#
# 覆盖：15 个独立 C++ 测试 + token_usage / control_auth 两个 Python 驱动的协议测试。
# 不含 linux_window_test.cpp —— 它要真 X11 会话 + 窗口管理器，走 scripts/test_linux_windows.sh。
#
# 每个测试都在临时 HOME 里跑：好几个测试会往 HOME 写缓存/配置，用真实 HOME 会互相污染，
# 也会留下垃圾。
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)   # live2d/player
TESTS=$ROOT/tests
BUILD=${1:-${L2D_TEST_BUILD:-$(mktemp -d "${TMPDIR:-/tmp}/l2dpet-tests.XXXXXX")}}
mkdir -p "$BUILD"

CXX=${CXX:-}
if [ -z "$CXX" ]; then
    for c in g++ clang++ c++; do
        if command -v "$c" >/dev/null 2>&1; then CXX=$c; break; fi
    done
fi
[ -n "$CXX" ] || { echo '[x] 找不到 C++ 编译器（g++/clang++），可用 CXX= 指定' >&2; exit 1; }
CXXFLAGS=(-std=c++17 -O0 -pthread -I "$ROOT/src")

# timeout 是 GNU coreutils 的；macOS 上是 gtimeout（brew coreutils）或干脆没有
TIMEOUT=""
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT=gtimeout
fi

run_limited() {  # 有 timeout 就加上限，没有就裸跑（macOS 上 bash 3.2：别用数组展开，set -u 会炸）
    if [ -n "$TIMEOUT" ]; then
        "$TIMEOUT" 300 "$@"
    else
        "$@"
    fi
}

PLATFORM="$ROOT/src/Platform.cpp"
# 整片 agent 依赖：测试 #include 了 PetAgent.cpp 时，它会调用下面这些模块的符号。
# 注意测试自己 #include 的 .cpp 不要重复链接（重复符号，链接直接失败）。
AGENT_SET=(
    "$ROOT/src/PetQuestions.cpp" "$ROOT/src/PetJokes.cpp" "$ROOT/src/PetWeather.cpp"
    "$ROOT/src/PetPersona.cpp" "$ROOT/src/LlmClient.cpp" "$ROOT/src/PetMemory.cpp"
    "$ROOT/src/PetI18n.cpp" "$PLATFORM"
)

build() {  # build <名字> <测试名> [额外源文件/参数…]
    local name=$1 test=$2; shift 2
    printf '\n\033[1m== 编译 %s\033[0m\n' "$name"
    "$CXX" "${CXXFLAGS[@]}" "$TESTS/$test.cpp" "$@" -o "$BUILD/$name"
}

run() {  # run <名字>
    local home
    home=$(mktemp -d)
    printf '\033[1m-- 运行 %s\033[0m\n' "$1"
    if ! run_limited env HOME="$home" "$BUILD/$1"; then
        rm -rf "$home"
        printf '[x] %s 失败\n' "$1" >&2
        exit 1
    fi
    rm -rf "$home"
}

run_py() {  # run_py <名字> <python 脚本> [参数…]
    local name=$1 home
    shift
    home=$(mktemp -d)
    printf '\033[1m-- 运行 %s\033[0m\n' "$name"
    if ! run_limited env HOME="$home" python3 "$@"; then
        rm -rf "$home"
        printf '[x] %s 失败\n' "$name" >&2
        exit 1
    fi
    rm -rf "$home"
}

# --- 头文件自带实现：只编测试文件本身 ---
for t in bubble_queue_test companion_feedback_test life_balance_test \
         watermark_match_test weather_cadence_test; do
    build "${t%_test}" "$t"
    run   "${t%_test}"
done

# --- 需要平台层（时间/路径/子进程）---
for t in child_timeout_test integration_registry_test; do
    build "${t%_test}" "$t" "$PLATFORM"
    run   "${t%_test}"
done

# --- #include PetAgent.cpp：要整片 agent 依赖 ---
for t in context_isolation_test question_dedup_test bubble_questions_test; do
    build "${t%_test}" "$t" "${AGENT_SET[@]}"
    run   "${t%_test}"
done

# --- 单独点名依赖的几个 ---
build memory_isolation    memory_isolation_test "$ROOT/src/PetMemory.cpp" "$ROOT/src/PetI18n.cpp" "$PLATFORM"
run   memory_isolation
build persona_guidance    persona_guidance_test "$ROOT/src/PetPersona.cpp" "$PLATFORM"
run   persona_guidance
build weather_openmeteo   weather_openmeteo_test "$ROOT/src/PetWeather.cpp" "$PLATFORM"
run   weather_openmeteo
build weather_search      weather_search_test    "$ROOT/src/PetWeather.cpp" "$PLATFORM"
run   weather_search
# token_budget_test 自己 #include 了 LlmClient.cpp：不能再链 LlmClient.cpp（重复符号）
build token_budget        token_budget_test      "$ROOT/src/PetWeather.cpp" "$PLATFORM"
run   token_budget

# --- 两个 Python 驱动的协议测试（本地假 LLM / 控制端口，不需要联网或 Key）---
printf '\n\033[1m== 编译 token_usage\033[0m\n'
"$CXX" "${CXXFLAGS[@]}" -DL2D_WITH_CURL=1 "$TESTS/token_usage_test.cpp" \
    "$ROOT/src/LlmClient.cpp" "$ROOT/src/PetWeather.cpp" "$PLATFORM" -lcurl -o "$BUILD/token_usage"
run_py token_usage_test.py "$TESTS/token_usage_test.py" "$BUILD/token_usage"

printf '\n\033[1m== 编译 control_auth\033[0m\n'
build control_auth control_auth_server "$ROOT/src/CtlServer.cpp" "$PLATFORM"
run_py control_auth_test.py "$TESTS/control_auth_test.py" "$BUILD/control_auth"

printf '\n\033[1m全部通过：15 个独立测试 + 2 个协议测试（构建目录 %s）\033[0m\n' "$BUILD"
