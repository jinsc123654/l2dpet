# macOS 测试包

CI 手动执行或标签触发时生成 `l2dpet.app`（ZIP）和 `.dmg`，文件名含 `untested`。

**未经 macOS 实机验证。** CI 只验证构建、签名结构与 headless 真实模型渲染；窗口行为、托盘、显示缩放、权限、开机启动和系统安全提示尚未实机验证。

构建使用当前 runner 架构，不是 universal 包。仅进行 ad-hoc 签名，没有 Apple 开发者证书签名或公证，Gatekeeper 可能阻止打开。

打包入口：`bash scripts/macos_build.sh`。需要 macOS、完整 Cubism Native SDK、CMake 和项目构建依赖。产物位于 `dist/macos-<arch>/`。
