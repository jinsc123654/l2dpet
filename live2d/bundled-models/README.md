# 随包示例模型（bundled-models）

打包时（`scripts/linux_build.sh`）整个目录拷进安装包的 `models/`；启动器 `l2dpet` 发现用户模型库
（`~/.local/share/l2dpet/models/`）是空的，就把这里的模型拷进去再启动——保证**没有任何模型的机器也能直接跑起来**。
拷进用户目录（而不是直接读 AppImage 的只读挂载）是为了让桌宠能在模型旁写能力缓存/探测结果，面板"模型"页也能照常管理它。

## Hiyori（桃瀬ひより）

- 来源：Live2D 官方示例 <https://github.com/Live2D/CubismWebSamples/tree/develop/Samples/Resources/Hiyori>（原样，未修改）
- 版权：© Live2D Inc.
- 许可：**Live2D Free Material License Agreement**（Live2D 无偿提供素材使用许诺契约）
  - 英文：<https://www.live2d.com/eula/live2d-free-material-license-agreement_en.html>
  - 日文：<https://www.live2d.com/eula/live2d-free-material-license-agreement_jp.html>
- 要点（以官方条款原文为准）：个人与小规模企业可在作品/应用中使用并随之分发；超过官方规定营收规模的企业需另与
  Live2D 签约。**不得把素材本身单独再分发**（只能作为本应用的一部分）。
- 换掉它：往 `~/.local/share/l2dpet/models/` 放任意模型即可（模型库不再为空时不会再拷）；不想要就在面板里删。
