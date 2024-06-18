#ifndef _TOTAL_H_

#define _TOTAL_H_

#include <iostream>
#include <cstring>
#include <windows.h>
#include <time.h>
#include <conio.h>
#include <stdlib.h>
#include <userenv.h>
#include <lmcons.h>
#include <cctype>

#endif // !_TOTAL_H_

int pw();
int options();
int show_logo();
int shell();
int loadcmd();
int wintop(); // no input and output,put window to top
int winuntop();
int fread();      // fast read,int i;i=fread();
void strtochar(); // copy: charusrname,usrname;chardoname,doname
void cprint(const char *,
            int); // colorful print not c++ print,only print char,you can change it but i suggest dont do that;
void ready();     // some loading process;
void getpcname(std::string &, std::string &);
void toLowerCase(std::string &, std::string &);
void cutpth(string, const char);           // has something with cmd"cd" and "cd..".It is already shit so dont touch it.
bool directoryExists(const std::string &); // input a output a bool.

string getCmdResult(const string &); // get cmd result.Need a command in string'