 @echo off
 for /r %%i in (*.sdf,*.ncb,*.suo,*.exp,*.user,*.aps,*.idb,*.dep,*.opt,*.plg,*.bsc,*.ilk,*.trg) do del /s /f /q "%%i"
 pause