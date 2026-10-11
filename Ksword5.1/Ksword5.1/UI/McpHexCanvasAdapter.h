#pragma once

namespace ks::ui
{
    class McpLocalBridge;
    // 在主程序启动桥时注册 HexCanvas 只读适配；不连接驱动或发起页面读取。
    void registerMcpHexCanvasAdapter(McpLocalBridge* bridge);
}
