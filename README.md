# TODO LIST
- 添加全局滚动条
- 首页logo被焊死了
- 标题栏被拆下来下面就不刷新了
- 

# Ksword开发者文档
## 坚持使用UTF-8
我们使用了宏来确保这件事。对于任何需要在图形界面上显示的中文文本，请务必使用C()包裹。
```cpp
ImGui::Text(C("示例文本"));
```
本质上C被定义为GBK转UTF-8的函数。
## 创建日志记录
在ImGui完成初始化过后，可以使用以下代码添加日志记录：
```cpp
#define CURRENT_MODULE C("测试")
	enum LogLevel { Debug,Info, Warn, Err,Fatal };
    kLog.Add(Debug, C("测试消息类型Debug"), C(CURRENT_MODULE));
    //其中，Debug代表消息严重级别，然后第二个参数是消息内容，最后可选参数是当前模块名称。
    //如果不填当前模块，那么默认为Unknown
    //在最新的版本中，添加了如下选项：
    kLog.dbg(...);
    kLog.info(...);
    kLog.warn(...);
    kLog.err(...);
    kLog.fatal(...);
    //其他参数语法和Add无异。

#undef CURRENT_MODULE
```
## 创建带进度显示的任务
我们内部维护一个DWORD PID列表保存所有进度。**已经完成的事件不会被销毁，他会永远保留在事件列表中。不显示的原因只是我们把进度为100%的彻底隐藏了。
使用任务进度的方法如下。
```cpp
    int /*返回进程的唯一标识符。只有通过这个唯一标识符才能操控事件。
    我们并没有进行权限的划分。请注意不要使用错误的pid操作事件。你会获得RE。*/ Kpid = kItem.AddProcess
    (
    	C("正在进行的操作"),
        std::string(C("正在进行的步骤")),
        NULL,/*取消任务的变量。如果这个进程无法取消，那么传递NULL。*/
        0.98f/*进程完成百分比*/
    );
    kItem.UI(Kpid,C( "点击按钮以继续运行该进程"), 1);
    //该函数是阻塞的。调用它以在某个进程显示可以选择的选项。1代表有一个操作选项。返回值为从1开始的整数，表示选择的选项。如果用户没有点击，那么函数会无限等待。
    kItem.SetProcess(Kpid,"",1.00f);
    //在任何时候都不要忘了，在你丢失Kpid之前把任务进度设置为100%。否则你会获得一个始终无法结束的事件。
```
## 退出Ksword
很多时候，你可能遇到严重的问题以至于需要Ksword立刻退出。此时，为ksword_main_should_exit变量赋值为1会让Ksword在下一帧渲染前，本帧完成渲染后退出。