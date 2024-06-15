# KSword
Strong Shell tool,for both 5-shift on Windows or others.

Kali-Sword 开发规范
1 加载流程：ready函数加载外置命令到数组，loadcmd加载cmd指令到数组。
2 shell函数中采用了大量if else结构。我们建议你在nowcmd!=cmd[15]那个判断条件之前添加更多的cmd指令。别忘了写help！
3 提示请尽可能采用下面规范：
cprint("[ x ]",4);
cprint("[ ! ]",6);
cprint("[ * ]",9);
再在后面添加内容。
使用cprint（char,int）可以输出彩色的文字。
4 对于所有用户输入，我们以第一个空格分割，前面的内容赋值给string类的nowcmd变量，后面的全部赋值给cmdpara变量。
如：输入：apt-get install g++
nowcmd中内容：apt-get
cmdpara中内容：install g++
需要自行处理cmdpara中的内容。
