"""离线执行生产模板选择函数；不下载资产、不修改发行版、不运行程序。"""

from pathlib import Path
import subprocess
import unittest

from test_ci_driver_package import bash_path


SCRIPT = Path(__file__).resolve().parent.parent / ".github/workflows/scripts/publish-ci-prerelease.sh"


def production_selector():
    """抽取生产函数原文，避免测试另写一套筛选逻辑。"""
    source = SCRIPT.read_text(encoding="utf-8-sig")
    start = source.index("# BEGIN manual template selector:")
    end = source.index("# END manual template selector.", start)
    return source[start:end]


class ReleaseTemplateTests(unittest.TestCase):
    def select(self, records):
        """只把候选数据送入 stdin；Bash 代码来自生产文件，不拼接资产名称。"""
        command = production_selector() + "\nselect_manual_release_template_record\n"
        completed = subprocess.run(
            [bash_path(), "-c", "set -euo pipefail\n" + command],
            input="\n".join(records) + "\n", encoding="utf-8",
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True,
        )
        return completed.stdout.strip()

    def test_newer_pdb_asset_does_not_replace_full_package(self):
        """实际故障：同一手工发行的 PDB asset id 比完整包大。"""
        full = "2026-10-13T02:11:57Z\tmcp-preview-20261012\t635598205\tKswordARK-MCP-Preview-261012.7z"
        pdb = "2026-10-13T02:11:57Z\tmcp-preview-20261012\t635598210\tKswordARK-MCP-Preview-261012-PDB.7z"
        self.assertEqual(self.select([full, pdb]), full)

    def test_symbols_only_returns_no_template(self):
        """符号包不能充当运行时模板，名称判断须大小写无关。"""
        records = [
            "2026-10-13T00:00:00Z\tpreview\t9\tKSword.PdB.7z",
            "2026-10-13T00:00:00Z\tpreview\t8\tKSword-SYMBOLS.7z",
        ]
        self.assertEqual(self.select(records), "")

    def test_newest_full_package_preserves_record(self):
        """跳过最新符号附件后，完整保留最新完整包的时间/tag/id/中文名称。"""
        newest = "2026-10-12T00:00:00Z\tpreview\t8\tKsword评估版本.7z"
        older = "2026-10-11T00:00:00Z\tstable\t7\tKsword.7z"
        symbol = "2026-10-13T00:00:00Z\tdebug\t9\tKsword-symbol.7z"
        self.assertEqual(self.select([older, newest, symbol]), newest)

    def test_pdb_feature_in_full_package_name_is_retained(self):
        """完整包的功能描述含 PDB 时仍可选，只有符号附件后缀被排除。"""
        full = "2026-10-13T00:00:00Z\tpreview\t10\tKsword-PDB-增强.7z"
        self.assertEqual(self.select([full]), full)

    def test_large_candidate_list_is_consumed_without_pipe_failure(self):
        """提前停止读取会使 pipefail 下的 sort 被 SIGPIPE 中断。"""
        full = "2026-10-13T00:00:00Z\tpreview\t9\tKsword.7z"
        records = [full] + [f"2026-10-12T00:00:00Z\told\t{number}\tKsword-PDB.7z" for number in range(5000)]
        self.assertEqual(self.select(records), full)


if __name__ == "__main__":
    unittest.main()
