# KSword
> Strong Shell tool,for both 5-shift on Windows or others.

## Kali-Sword 开发规范

1. 加载流程：`ready()` 函数加载外置命令到数组，`loadcmd` 加载 `cmd` 指令到数组。

2. `shell` 函数中采用了大量 `if else` 结构。我们建议你在 `nowcmd!=cmd[15]` 那个判断条件之前添加更多的 `cmd` 指令。别忘了写 `help`！

3. 提示请尽可能采用下面规范：
```cpp
cprint("[ x ]",4);
cprint("[ ! ]",6);
cprint("[ * ]",9);
```
再在后面添加内容。
使用 `cprint（char,int）` 可以输出彩色的文字。

4. 对于所有用户输入，我们以第一个空格分割，前面的内容赋值给 `string` 类的 `nowcmd` 变量，后面的全部赋值给 `cmdpara` 变量。

如：输入：`apt-get install g++`，`nowcmd`中内容：`apt-get`;

`cmdpara` 中内容：`install g++`

需要自行处理 `cmdpara` 中的内容。
