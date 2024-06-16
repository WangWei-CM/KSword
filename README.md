# KSword
Strong Shell tool,for both 5-shift on Windows or others.

正常使用：
1 将kali-sword直接使用dev-c++编译，编译后更改文件名为sethc.exe替换到c:\windows\system32下。需要手动操作，先更改原来的sethc.exe的安全选项，管理员取得所有权再赋予管理员更改权，然后替换掉。替换以后就可以被锁屏的时候按下win+l再连续按下5shift启动了。（system权限）
2 编译前查找Ready函数，里面有用户名和密码，由0~2越往后权限越低。usrlist[n][0]是第n用户的用户名，usrlist[n][1]则是该用户的密码。

机房专版使用：
1 准备一个自己的U盘，盘符为X，U盘中新建目录x:\kswordrc\class\，其中下载好除了kali-sword.exe之外的所有文件。
2 将kali-sword直接使用dev-c++编译，编译后更改文件名为sethc.exe替换到c:\windows\system32下。需要手动操作，先更改原来的sethc.exe的安全选项，管理员取得所有权再赋予管理员更改权，然后替换掉。替换以后就可以被锁屏的时候按下win+l再连续按下5shift启动了。

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
