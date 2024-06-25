#ifndef _INIT_H_

#define _INIT_H_

#include "total.h"

bool directoryExists(const std::string &path)
{
    DWORD attrib = GetFileAttributesA(path.c_str());
    return (attrib != INVALID_FILE_ATTRIBUTES && (attrib & FILE_ATTRIBUTE_DIRECTORY));
}

void ready()
{
    // usrlist[usrname][usrpassword],usraut[usrnumber] ,0~10=max~min.
    usrlist[0][0] = "Admin";
    usrlist[1][0] = "User 1";
    usrlist[2][0] = "User 2";
    usrlist[2][2] = "Pwd2";
    usrlist[0][1] = "Admin";
    usrlist[1][1] = "Pwd1";
    usraut[0] = 0;
    usraut[1] = 1;
    usraut[2] = 8;
    strcmd[1] = "icacls C:\\windows\\system32\\sethc.exe /setowner Administrator /c && icacls "
                "C:\\Windows\\System32\\sethc.exe /grant Administrator:F /t";
    strcmd[2] = "taskkill /f /t /im explorer.exe";
    strcmd[3] = "icacls C:\\Windows\\explorer.exe /grant Administrator:F /t && icacls "
                "C:\\Windows\\system32\\taskmgr.exe /grant Administrator:F /t && cd c:\\windows && ren explorer.exe "
                "explorer1.exe && cd c:\\windows\\system32 && ren taskmgr.exe taskmgr1.exe";
    strcmd[4] = "c:\\script\\1.bat";
    strcmd[5] = "c:\\script\\2.bat";
    strcmd[6] = "c:\\script\\3.bat";
    strcmd[7] = "c:\\script\\4.bat";
    strcmd[8] = "c:\\script\\5.bat";
    strcmd[9] = "c:\\script\\6.bat";
    strcmd[10] = "c:\\script\\7.bat";
    strcmd[11] = "c:\\script\\8.bat";
}
int pw()
{
    string password;
    char ch;
    printf("Enter password:");
    //	scanf("%s", password);

    int i = 0;
    bool flag = 1;
    while (flag)
    {
        ch = getch();
        switch ((int)ch)
        {
        case 8:
            if (password.empty())
                break;
            password.erase(password.end() - 1);
            putchar('\b');
            putchar(' ');
            putchar('\b');
            break;
        case 27:
            exit(0);
            break;
        case 13:
            flag = 0;
            break;
        default:
            password += ch;
            putchar('*');
            break;
        }
    }
    if (password == usrlist[1][1])
    {
        return 1;
    }
    if (password == usrlist[0][1])
    {
        return 0;
    }
    if (password == usrlist[2][1])
    {
        return 2;
    }
    else
        return 100;
}

int gethost()
{
    hostnamesize = sizeof(hostname);
    GetComputerName(hostname, &hostnamesize);
}

void cprint(const char *s, int color)
{
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | color);
    cout << s;
    SetConsoleTextAttribute(handle, FOREGROUND_INTENSITY | 7);
}

void getpcname(std::string &lchostname, std::string &lcusrname)
{
    TCHAR computerName[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD size = sizeof(computerName) / sizeof(computerName[0]);
    if (GetComputerName(computerName, &size))
    {
        lchostname = computerName;
    }
    else
    {
        lchostname = "Unknown";
    }

    TCHAR username[UNLEN + 1];
    size = sizeof(username) / sizeof(username[0]);
    if (GetUserName(username, &size))
    {
        lcusrname = username;
    }
    else
    {
        lcusrname = "Unknown";
    }
}

void strtochar()
{
    strcpy(charusrname, usrname.c_str());
    strcpy(chardoname, doname.c_str());
}

string getCmdResult(const string &strCmd)
{
    char buf[102400] = {0};
    FILE *pf = NULL;

    if ((pf = popen(strCmd.c_str(), "r")) == NULL)
    {
        return "";
    }

    string strResult;
    while (fgets(buf, sizeof buf, pf))
    {
        strResult += buf;
    }

    pclose(pf);

    unsigned int iSize = strResult.size();
    if (iSize > 0 && strResult[iSize - 1] == '\n')
    {
        strResult = strResult.substr(0, iSize - 1);
    }

    return strResult;
}

int loadcmd()
{
    cmd[1] = "cmd";
    cmd[2] = "taskmgr";
    cmd[3] = "help";
    cmd[4] = "whoami";
    cmd[5] = "time";
    cmd[6] = "switchuser";
    cmd[7] = "runas";
    cmd[8] = "sethc";
    cmd[9] = "cd";
    cmd[10] = "winuntop";
    cmd[11] = "clear";
    cmd[12] = "script";
    cmd[13] = "about";
    cmd[14] = "setpth";
    cmd[15] = "exit";
    cmd[16] = "top";
}

void toLowerCase(std::string &nowcmd, std::string &cmdpara)
{
    for (size_t i = 0; i < nowcmd.length(); ++i)
    {
        nowcmd[i] = std::tolower(static_cast<unsigned char>(nowcmd[i]));
    }
    for (size_t i = 0; i < cmdpara.length(); ++i)
    {
        cmdpara[i] = std::tolower(static_cast<unsigned char>(cmdpara[i]));
    }
}

void cutpth(string str, const char split)
{
    istringstream iss(str);
    string token;

    for (int i = 0; i <= cmdpthtop; i++)
    {
        cmdpth[i] = "";
    }
    cmdpthtop = 0;
    while (getline(iss, token, split))
    {
        cmdpth[cmdpthtop++] = token;
    }
    cmdpthtop--;
}

#endif
