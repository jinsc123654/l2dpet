# 外部助手接入

桌宠运行后自动维护 ~/.config/l2dpet/integration/。Windows 默认位于 C:/Users/<用户名>/.config/l2dpet/integration/；以桌宠外部接入页显示的路径为准。

## 给编程助手的任务

读取 integration.json 和本说明；选择自己的稳定 ID，在 clients/<id>/registration.json 登记，然后在同一目录生成适合自己宿主的 Hook、MCP 配置或转发脚本。按宿主要求安装或引用这些配置；仅把配置放进目录不会让宿主自动加载。保留已有通知链与其他 MCP 配置。宿主要求审核 Hook 时由用户审核，不自行写入信任记录。

不必改动桌宠安装目录。生成的配置使用固定位置的 pet_bridge.py 或直接访问 TCP 接口；不要把当前模型指纹作为助手 ID，也不要在切换模型时重新注册。不要把密钥、完整对话、原始工具输出或长期记忆写入接入文件。

## 自动维护的文件

- integration.json：schema_version、host、实际活动端口 port、已保存端口 saved_port、模型标识/名字/性格、动作与表情能力、支持的陪伴事件、注册目录和适配器路径。只读，由桌宠更新。改端口需重启，发送方应重新读取活动端口。
- README.md：本说明，由桌宠从应用包发布。
- pet_bridge.py：可选 Python 3.9+ 适配器，固定路径，由桌宠从应用包发布。
- clients/<id>/：助手自己的注册与配置目录。id 只允许 1–64 个小写英文字母、数字、连字符和下划线。
- preferences/<id>.json：桌宠保存的通知开关，助手不得改动。重新注册不会恢复被用户关闭的通知。

## 注册句柄

创建 clients/codex/registration.json，例如：

```json
{"schema_version":1,"id":"codex","name":"Codex","configuration_files":["mcp.toml","hooks.json"]}
```

目录名与 id 必须一致。name 是显示名称，configuration_files 可自行扩展；桌宠不执行这些文件。多个同类助手或项目需要独立开关时使用不同 ID，如 codex-work、codex-personal。更新注册内容不要覆盖 preferences。

也可调用固定适配器：python <pet_bridge.py> --client-id codex --name Codex register。该命令离线也能创建注册文件，不改动宿主设置。

## 发送陪伴事件

读取 integration.json 的 host 和 port，建立本机 TCP 连接；发送一行 UTF-8 JSON，再读取一行响应。这不是 HTTP 服务。示例：

连接后先认证：读取本用户 `~/.config/l2dpet/control-token-<活动端口>`，发送 `{"cmd":"auth","token":"<文件内容>"}` 并确认 `ok=true`，然后发送业务命令或订阅事件。Token 每次启动更新；不得放入注册文件、日志、提示词或聊天。它是本机控制凭据，不是 LLM API Key。随包面板和 `pet_bridge.py` 自动处理认证；旧客户端必须升级。服务拒绝未认证连接，监听范围仍为回环地址。

```json
{"cmd":"companion_event","client_id":"codex","kind":"progress","message":"构建通过，正在检查安装包","event_id":"session/turn/build-passed"}
```

kind 可用 started、working、progress、waiting、failed、completed、interrupted。completed 只代表本轮结束，不表示任务成功。message 最多 600 字；event_id 最多 200 字，同一真实事件应使用相同 ID，未提供 ID 时仅短时间去重。

必须使用已注册的 client_id。显示来源由注册名称决定，不能通过 source 字段改变。关闭通知时返回 ok=true、shown=false、reason=client_disabled；调用方无需报错或反复重试。也可能因等待选项、限频或全局陪伴关闭而不弹气泡。只转发有意义的进度，不发送每个工具调用。桌宠自行选择文案和动作。

## Hook 与 MCP 配置由助手生成

可选适配器运行参数：python <固定适配器路径> --client-id <id> mcp（stdio）。Codex 的完成 notify 使用相同参数加 notify；生命周期观察 Hook 使用 hook，并从 stdin 读取宿主事件。该适配器的 Hook 事件映射针对 Codex，其他助手自行按自己的生命周期把事件转换为上述 JSON。

Hook 和 MCP 必须使用同一个 client_id 才能统一开关。默认不指定 --port，让适配器每次调用读取活动端口；需要指定时由助手管理配置同步。助手根据自己的宿主版本决定实际配置格式和加载位置，不要求所有客户端使用同一种配置。

旧版无 client_id 的陪伴事件继续兼容：按 source 自动登记兼容句柄（例如 Codex → codex）。新接入请显式注册和传 client_id。注册 ID 是本机通知路由标识，不是认证凭证；该服务面向本机使用。
