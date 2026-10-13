# npm / PyPI 可信发布

包名均为 `ksword-mcp`。独立发布仓库为 `KSwordDEV/MCP`，发布工作流固定为 `.github/workflows/release.yml`，版本标签为 `v<version>`。工作流只在 GitHub 托管 runner 上发布，发布 job 各自拥有 `id-token: write`，构建和测试 job 只有读取权限；不需要仓库保存 npm/PyPI 长期令牌。

## 首次配置

使用包所有者的官方网页账户配置以下信任关系。GitHub 登录不代替 npm/PyPI 登录：

| 字段 | npm | PyPI |
| --- | --- | --- |
| 包名 | `ksword-mcp` | `ksword-mcp` |
| GitHub owner | `KSwordDEV` | `KSwordDEV` |
| Repository | `MCP` | `MCP` |
| Workflow filename | `release.yml` | `release.yml` |
| Environment | `npm` | `pypi` |

npm 在包设置中的 Trusted publishing 配置 GitHub Actions，并允许直接 `npm publish`。PyPI 尚无项目时，在账户的 [Publishing](https://pypi.org/manage/account/publishing/) 添加 pending publisher；首次 OIDC 发布会创建项目。PyPI pending 配置不会预留包名。已有包则在项目设置中添加 publisher。

配置前确认对应 registry 账户有包所有权，且仓库、工作流、环境名称与上表逐字一致。npm 的包设置需要先存在包；新包的首次创建若要求交互登录/二次验证，应由包所有者通过官方流程完成，再配置可信发布，不将个人令牌复制到聊天或仓库。npm 的新可信关系需在配置后两天内完成首次成功发布；见 [npm 官方文档](https://docs.npmjs.com/trusted-publishers/)。PyPI 首次配置见 [官方 pending publisher 指南](https://docs.pypi.org/trusted-publishers/creating-a-project-through-oidc/)。

## 构建和发布

1. 同步 `package.json`、`pyproject.toml`、`ksword_mcp/__init__.py` 的版本，更新安装示例中的固定 npm 版本。
2. 合并代码并确认独立仓库 CI 通过。CI 验证 Windows/Python 3.10 与3.13及 Linux 打包/stdio，不进行产品 GUI 或驱动操作。
3. 创建并推送对应 `v<version>` 标签，在 Actions 的 `Publish npm and PyPI` 选择该标签及 `registry=all`。`release.yml` 核验版本一致，测试后构建 wheel、sdist 与 npm tgz，分别发布两个 registry。
4. 某一 registry 的配置较晚完成时，从同一标签运行 `release.yml`，仅选择尚未发布的 `pypi` 或 `npm`；选择 `none` 只构建。拒绝从分支或版本不匹配的标签发布；已交互创建的 npm 首版不重复发布，后续新版本再使用 OIDC。
5. 核验 registry 实际版本、发行文件 SHA256 和从公开 registry 安装后的版本/stdio 工具目录。工具发现、CLI help 和 GUI 业务结果分别判断。

npm 发布 runner 使用 Node.js 24 和支持 OIDC 的 npm；公开包启用 provenance。PyPI 使用官方 `pypa/gh-action-pypi-publish` 上传构建产物并生成证明。版本不可覆盖；配置失败时修复真实信任字段，不回退到泄漏长期凭据。
