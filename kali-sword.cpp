/*
#include <bits/stdc++.h>
#include <cctype>
#include <conio.h>
#include <cstring>
#include <iostream>
#include <lmcons.h>
#include <stdlib.h>
#include <time.h>
#include <userenv.h>
#include <windows.h>
*/
#include "./libs/init.h"
#include "./libs/total.h"

using namespace std;

int main()
{
    wintop();
    //	system("mode con cols=140 lines=40");
    ready();

    int tmp;
    tmp = pw();
    if (tmp != 100)
        ;
    else
        exit(0);
    usrname = usrlist[tmp][0];
    doname = usrlist[tmp][0];
    donumber = tmp;
    usrnumber = tmp;
    system("cls");

    show_logo();
    //	options();
    loadcmd();
    shell();
    return 0;
}

int shell()
{
shellstart:
    strtochar();
    getpcname(lchostname, lcusrname);
    if (doname != usrname)
    {
        cout << "��";
        cout << "[";
        cprint(charusrname, 70);
        cout << "=>";
        cprint(chardoname, 60);
        cout << "@";
        cout << lchostname;
        cout << "]";
    }
    else
    {
        cout << "��";
        cout << "[";
        cprint(charusrname, 70);
        cout << "@";
        cout << lchostname;
        cout << "]";
    }

    cout << "~[";
    if (lcusrname == "SYSTEM")
        cprint("System", 70);
    else
        cout << lcusrname;
    cout << '@' << lchostname << ']';
    cout << endl;
    cout << "��";
    for (int i = 0; i <= cmdpthtop; i++)
    {
        cout << cmdpth[i];
        cout << "\\";
    }
    cout << ">";
    getline(cin, nowcmd);
    size_t spacePos = nowcmd.find(' ');
    if (spacePos != std::string::npos)
    {
        cmdpara = nowcmd.substr(spacePos + 1);
        nowcmd = nowcmd.substr(0, spacePos);
    }
    toLowerCase(nowcmd, cmdpara);
    if (nowcmd == "cd..")
    {
        if (cmdpthtop >= 1)
        {
            cmdpth[cmdpthtop] = "";
            cmdpthtop--;
        }
        else
        {
            cprint("[ x ]", 4);
            cout << "Already reach root!" << endl;
        }
    }
    else if (nowcmd == cmd[1])
    {
        if (usraut[donumber] >= 6)
        {
            cprint("[ ! ]", 6);
            cout << "Insufficient authority" << endl;
            cprint("[ x ]", 4);
            cout << "The operation did not complete successfully!" << endl;
        }
        cprint("[ ! ]", 6);
        cout << "type exit to quit." << endl;
        system("c:\\windows\\system32\\cmd.exe");
    }
    else if (nowcmd == cmd[2])
    {
        if (usraut[donumber] >= 6)
        {
            cprint("[ ! ]", 6);
            cout << "Insufficient authority" << endl;
            cprint("[ x ]", 4);
            cout << "The operation did not complete successfully!" << endl;
        }
        system("c:\\windows\\system32\\taskmgr.exe");
    }
    else if (nowcmd == cmd[3])
    {
        cout << "Help:all the commands are as following:";
        for (int i = 0; i <= cmdnumber; i++)
            cout << cmd[i] << ' ';
        cout << "." << endl;
        cout << "Enter the command name that you want to learn about:(c for cancel)";
        string helpcmd;
        getline(cin, helpcmd);
        if (helpcmd == cmd[1])
        {
            cout << "Enter cmd so that we'll start a cmd.exe for you with system privileges." << endl;
        }
        else if (helpcmd == cmd[2])
        {
            cout << "Enter to run taskmgr.exe." << endl;
        }
        else if (helpcmd == cmd[3])
        {
            cout << "How can you even don't know how to use help command?" << endl;
        }
        else if (helpcmd == cmd[4])
        {
            cout << "This command has been deprecated, and typing this command now gives the console program execution "
                    "permission."
                 << endl;
        }
        else if (helpcmd == cmd[5])
        {
            cout << "Just to get the time." << endl;
        }
        else if (helpcmd == cmd[6])
        {
            cout << "Switch user of Kali-sword.This requires that user's password.You can try runas if you don't have "
                    "theirs."
                 << endl;
        }
        else if (helpcmd == cmd[7])
        {
            cout << "A user with higher permissions can operate as a lower user without entering a password. The "
                    "reverse is required. This changes the recorded results in the log."
                 << endl;
        }
        else if (helpcmd == cmd[8])
        {
            cout << "Replace the sethc.exe(shift*5) with Kali-sword.This command may doesn't work." << endl;
        }
        else if (helpcmd == cmd[9])
        {
            cout << "Use it like on Windows cmd or linux shell.We support \"cd..\"and\"cd <dir>" << endl;
        }
        else if (helpcmd == cmd[10])
        {
            cout << "Untop your window." << endl;
        }
        else if (helpcmd == cmd[11])
        {
            cout << "Clean all output." << endl;
        }
        else if (helpcmd == cmd[12])
        {
            cout << "Run user script.You can copy your scripts(only *.bat supported) to C:\\script(Create one if "
                    "there's not one) and renamed it with the number 1~10."
                 << endl;
        }
        else if (helpcmd == cmd[13])
        {
            cout << "Show something about the Developer." << endl;
        }
        else if (helpcmd == cmd[14])
        {
            cout << "set cmd path.If you enter a command that is not included in Kali-sword,we will use cmd to run "
                    "it.And it will reset the cmd path to C:\\Windows\\system32 every time when you enter a new "
                    "command.This will change it.Make sure the path you enter is available!"
                 << endl;
        }
        else if (helpcmd == cmd[15])
        {
            cout << "Log out and stop Kali-sword." << endl;
        }
        else if (helpcmd == cmd[16])
        {
            cout << "top your window." << endl;
        }
        else if (helpcmd == "c")
            ;
        else
        {
            cprint("[ x ]", 4);
            cout << "command not found!" << endl;
        }
    }
    //		else if (nowcmd==cmd[4])
    //		{
    //			cprint("[ ! ]",6);
    //			cout<<"You are "<<usrname<<"@";
    //			cprint(hostname,4);
    //			cout<<endl;
    //			continue;
    //		}
    else if (nowcmd == cmd[5])
    {
        time_t timep;
        time(&timep);
        printf("%s", ctime(&timep));
    }
    else if (nowcmd == cmd[6])
    {
        int tmp;
        tmp = pw();
        if (tmp != 100)
        {
            usrname = usrlist[tmp][0];
            doname = usrlist[tmp][0];
            donumber = tmp;
            usrnumber = tmp;
            system("cls");
        }
        else
        {
            cprint("[ ! ]", 6);
            cout << "password incorrect!" << endl;
            cprint("[ x ]", 4);
            cout << "login failed!" << endl;
        }
    }
    else if (nowcmd == cmd[7])
    {
        int tmp, tmp1;
        cprint("[ ! ]", 6);
        cout << "choose a user(number):" << endl;
        for (int i = 0; i <= usrlisttop; i++)
            cout << usrlist[i][0] << endl;
        tmp = fread();
        if (tmp > usrlisttop)
        {
            cprint("[ x ]", 4);
            cout << "user not found!" << endl;
        }
        else if (usraut[tmp] < usraut[usrnumber])
        {
            cprint("[ ! ]", 6);
            cout << "Migration to higher level permissions is not allowed,but you can change it by entering their "
                    "passwords."
                 << endl;
            tmp1 = pw();
            cout << endl;
            if (tmp1 != 100)
            {
                doname = usrlist[tmp1][0];
                donumber = tmp1;
            }
            else
            {
                cprint("[ x ]", 4);
                cout << "Password incorrect!" << endl;
            }
        }
        else
        {
            doname = usrlist[tmp][0];
            donumber = tmp;
            cprint("[ * ]", 9);
            cout << "Operation completed successfully!" << endl;
        }
    }
    else if (nowcmd == cmd[8])
    {
        int tmp;
        cprint("[ ! ]", 6);
        cout << "This command is broken.Continur?(0/1):";
        cin >> tmp;
        cout << endl;
        if (tmp == 0)
            ;
        else if (tmp != 1)
        {
            cprint("[ ! ]", 6);
            cout << "Wrong input!please try this command later." << endl;
        }
        cprint("[ ! ]", 6);
        cout << "This will distroy original sethc.exe!Continue?(0/1):";
        cin >> tmp;
        cout << endl;
        if (tmp == 0)
            ;
        else if (tmp != 1)
        {
            cprint("[ ! ]", 6);
            cout << "Wrong input!please try this command later." << endl;
        }
        strre = getCmdResult(strcmd[1]);
        cout << strre << endl;
        strre = "";
    }
    else if (nowcmd == cmd[9])
    {
        int tmp;
        string newDir;
        for (int i = 0; i <= cmdpthtop; i++)
        {
            newDir += cmdpth[i] + "\\";
        }
        newDir += cmdpara;
        if (directoryExists(newDir))
        {
            if (cmdpara.find('\\') != std::string::npos)
            {
                size_t startPos = 0;
                size_t endPos = cmdpara.find_first_of('\\');
                while (endPos != std::string::npos)
                {
                    cmdpth[++cmdpthtop] = cmdpara.substr(startPos, endPos - startPos);
                    startPos = endPos + 1;
                    endPos = cmdpara.find_first_of('\\', startPos);
                }
                cmdpth[++cmdpthtop] = cmdpara.substr(startPos);
            }
            else
            {
                cmdpth[++cmdpthtop] += cmdpara;
            }
        }
        else
            cout << "ϵͳ�Ҳ���ָ�����ļ���·����" << endl;
    }
    else if (nowcmd == cmd[10])
    {
        winuntop();
    }
    else if (nowcmd == cmd[11])
    {
        system("cls");
    }
    else if (nowcmd == cmd[12])
    {
        if (usraut[donumber] >= 6)
        {
            cprint("[ ! ]", 6);
            cout << "Insufficient authority" << endl;
            cprint("[ x ]", 4);
            cout << "The operation did not complete successfully!" << endl;
        }
        int tmp;
        cprint("[ ! ]", 6);
        cout << "Enter your script number:";
        cin >> tmp;
        strre = getCmdResult(strcmd[tmp + 3]);
        cout << strre << endl;
        strre = "";
    }
    else if (nowcmd == cmd[13])
    {
        cprint("This Program is KALI_SWORD", 113);
        cout << endl;
        cprint("Developed by WangWei_CM.", 23);
        cout << endl;
        cprint("Welcome to visit my website:159.75.66.16", 56);
        cout << endl;
        cprint("Kali-Weidows ICS is available!", 74);
        cout << endl;
        system("start https://ylhcwwtx.icoc.vc");
    }
    else if (nowcmd == cmd[14])
    {
        string tmp;
        cprint("[ ! ]", 6);
        cout << "Enter default cmd path:";
        getline(cin, tmp);
        cutpth(tmp, '\\');
    }
    else if (nowcmd == cmd[16])
    {
        wintop();
    }
    else if (nowcmd == "")
    {
        goto shellstart;
    }
    else if (nowcmd != cmd[15])
    {
        string cmdcmd = "cd ";
        for (int i = 0; i <= cmdpthtop; i++)
        {
            cmdcmd += cmdpth[i];
            cmdcmd += "\\";
        }
        cmdcmd = cmdcmd + " && " + nowcmd + ' ' + cmdpara;
        strre = getCmdResult(cmdcmd);
        cout << strre << endl;
    }
    else if (nowcmd == cmd[15])
    {
        cprint("[ ! ]", 6);
        cout << "System is about to close..." << endl;
        cprint("[ * ]", 9);
        cout << "Logging out..." << endl;
        usrname = "";
        memset(charusrname, '\0', sizeof(charusrname));
        usrname = "";
        memset(charusrname, '\0', sizeof(charusrname));
        system("cls");
        return 0;
    }
    goto shellstart;
}
