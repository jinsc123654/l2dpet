# AppImage 依赖打包回归

Ubuntu 24.04 用户报告 `libssl.so.3` 需要 `OPENSSL_3.2.0/3.3.0/3.4.0`，但实际加载的是宿主的 `libcrypto.so.3`。原因是原排除规则 `libcrypt*` 同时误排除了 OpenSSL 的 Crypto 库。随后在 Ubuntu 24.04 容器发现，规则 `librt*` 也误排除了 libcurl 使用的 `librtmp.so.1`；本机有这些库，掩盖了漏打包问题。

修复后的 glibc 排除规则使用库名边界，例如 `libcrypt.so*`、`librt.so*`。OpenSSL 的 SSL/Crypto 和网络库的实际依赖整套随包提供。GLFW 通过 dlopen 加载的 Wayland 库也收集其传递依赖。

打包流程现在包含：

1. `check_appimage_deps.py` 扫描两个实际可执行文件及包内共享库的 ELF NEEDED，拒绝未明确交给宿主的漏打包依赖。
2. 在包内库路径下对两个真实程序执行 `ldd -r`，检查库、版本和符号解析。
3. 原有启动器、安装路径和 AppImage 集成检查。

此前的 `status` 自检只运行 shell 启动器，不会加载播放器或面板的 ELF 依赖，不能发现这类错误。

回归测试：

```sh
python3 scripts/test_packaging.py
bash scripts/test_appimage_ubuntu24.sh
```

后者需要 Docker，使用官方 Ubuntu 24.04 镜像，只读挂载包，安装标准桌面通常已有的编译器运行库/图形库。在 Ubuntu 的宿主 libc 和 libstdc++ 下验证两个程序的库与符号，并确认 SSL 和 Crypto 同时从 AppImage 加载；实际运行播放器，加载 Hiyori，软件渲染一帧。它不验证图形桌面的完整交互。

库前缀回归使用真实生成的 ELF 库模拟 SSL→Crypto 和 curl→RTMP 依赖，分别删除 Crypto/RTMP 后必须拒绝该打包树。
