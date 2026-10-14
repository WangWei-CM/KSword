"""抽取生产图表的时间窗口与命中方法，验证全量缩放和最近窗口滚动。"""
from pathlib import Path
import os
import subprocess

from async_operation_tests import extract_method


def main() -> None:
    """使用已有 output 和 HostX64 工具链编译真实坐标方法，不创建替代 GUI。"""
    root = Path(__file__).resolve().parents[1]
    output = root / "output"
    source = (root / "Ksword5.1/Ksword5.1/ProcessDock/ProcessDock.cpp").read_text(encoding="utf-8-sig")
    chart = source[source.index("class ProcessActivityChartWidget final") :]
    signatures = ("void freezeVisibleTimelineWindow()", "bool hasFrozenTimelineWindow() const",
                  "double windowDurationMs() const", "double windowEndMs() const",
                  "int sampleIndexAtX(", "double sampleIndexToX(")
    methods = "\n".join(extract_method(chart, signature) for signature in signatures)
    template = (root / "tools/process_activity_timeline_tests.template.cpp").read_text(encoding="utf-8")
    generated = output / "process_activity_timeline_fixture.cpp"
    generated.write_text(template.replace("__PRODUCTION_METHODS__", methods), encoding="utf-8")

    # 工具和系统头固定使用仓库现有验证环境，所有生成物留在已有 output。
    vc = Path("C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207")
    sdk = Path("C:/Program Files (x86)/Windows Kits/10")
    qt = root / ".deps/Qt/6.9.3/msvc2022_64"
    compiler = vc / "bin/Hostx64/x64/cl.exe"
    flags = ["/nologo", "/std:c++20", "/utf-8", "/EHsc", "/MD", "/W4", "/WX",
             "/Zc:__cplusplus", "/permissive-", "/external:W0", "/I" + str(vc / "include")]
    flags += ["/external:I" + str(sdk / "Include/10.0.26100.0" / part) for part in ("ucrt", "shared", "um")]
    flags += ["/external:I" + str(qt / "include" / part) for part in ("", "QtCore")]
    executable = output / "process_activity_timeline_tests.exe"
    libraries = ["/LIBPATH:" + str(vc / "lib/x64")]
    libraries += ["/LIBPATH:" + str(sdk / "Lib/10.0.26100.0" / part / "x64") for part in ("ucrt", "um")]
    subprocess.run([str(compiler), *flags, str(generated), "/Fo" + str(output / "process_activity_timeline.obj"),
                    "/Fe" + str(executable), "/link", *libraries, str(qt / "lib/Qt6Core.lib")],
                   cwd=output, check=True)
    environment = dict(os.environ)
    environment["PATH"] = str(qt / "bin") + os.pathsep + environment.get("PATH", "")
    subprocess.run([str(executable)], cwd=output, env=environment, check=True)


if __name__ == "__main__":
    main()
