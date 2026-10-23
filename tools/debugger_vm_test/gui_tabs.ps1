# Run in the guest interactive session with both production PluginHost Tabs open.
# Only helpers belonging to OwnedMainPid are selected. No process is terminated.
param(
    [string]$Root='C:\ksword\debugger-tests\20260930',
    [int]$OwnedMainPid=0,
    [switch]$AllowSavedOptions,
    [switch]$VerifyFreshSessions,
    [switch]$KeepTestPreferences,
    [string]$CeTempDirectory=$env:TEMP,
    [string]$CeSettingsFile=(Join-Path $env:LOCALAPPDATA 'KSword\ce-backend-options.txt')
)
$ErrorActionPreference='Stop'
$Root=(Resolve-Path -LiteralPath $Root).Path.TrimEnd('\')
$report=Join-Path $Root 'gui-tabs.log'
$script:contexts=@(); $script:freshContexts=@(); $script:startedHelpers=@(); $script:preferences=@{}; $script:outcome='FAIL'
Set-Content -LiteralPath $report -Value ''
function Check($Value,[string]$Message) { if(!$Value){throw $Message}; Add-Content -LiteralPath $report -Value "PASS $Message" }
function Note([string]$Message) { Add-Content -LiteralPath $report -Value "INFO $Message" }
function UnderRoot([string]$Path) { $Path -and $Path.StartsWith($Root+'\',[StringComparison]::OrdinalIgnoreCase) }
function RandomizedPayload([string]$Path,[string]$Kind) {
    if(!$Path){return $false}
    $sessions=Join-Path $env:LOCALAPPDATA 'KSword\sessions'
    $parent=Split-Path -Parent $Path
    $session=Split-Path -Leaf $parent
    if(![string]::Equals((Split-Path -Parent $parent),$sessions,[StringComparison]::OrdinalIgnoreCase) -or
       $session -cnotmatch '^x-[0-9A-F]{16}$'){return $false}
    $name=Split-Path -Leaf $Path
    if($name -cmatch '^[0-9A-F]{32}\.exe$'){return $true}
    if($Kind -eq 'CE'){return $name -cmatch '^cheatengine-x86_64-[0-9A-F]{8}\.exe$'}
    return $name -cmatch '^x64dbg-[0-9A-F]{8}\.exe$'
}
try {
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes
if(-not ('GuestPolicyWindows' -as [type])) { Add-Type -TypeDefinition @'
using System; using System.Text; using System.IO; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class GuestPolicyWindows {
 public delegate bool EnumProc(IntPtr h, IntPtr p);
 [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr h, EnumProc f, IntPtr p);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr h,uint f);
 [DllImport("user32.dll")] public static extern IntPtr GetParent(IntPtr h);
 [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr h,int i);
 [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int command);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr SetActiveWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint first,uint second,bool attach);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 public struct Point { public int X; public int Y; }
 [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(Point point);
 [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint flags,uint x,uint y,uint data,UIntPtr extra);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageW")] public static extern IntPtr SetText(IntPtr h,uint m,IntPtr n,string s);
 [DllImport("user32.dll",CharSet=CharSet.Unicode,EntryPoint="SendMessageW")] public static extern IntPtr ReadText(IntPtr h,uint m,IntPtr n,StringBuilder s);
 [DllImport("gdi32.dll",CharSet=CharSet.Unicode,EntryPoint="GetObjectW")] public static extern int GetObject(IntPtr h,int n,byte[] data);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)] public static extern bool MoveFileEx(string a,string b,uint flags);
 public static IntPtr[] Children(IntPtr h) { var r=new List<IntPtr>();EnumChildWindows(h,(c,p)=>{r.Add(c);return true;},IntPtr.Zero);return r.ToArray(); }
 public static string Class(IntPtr h) { var s=new StringBuilder(256);GetClassName(h,s,256);return s.ToString(); }
 public static string Text(IntPtr h) { var s=new StringBuilder(65536);ReadText(h,13,(IntPtr)65536,s);return s.ToString(); }
 public static string Font(IntPtr h) { var f=SendMessage(h,49,IntPtr.Zero,IntPtr.Zero);var d=new byte[92];if(GetObject(f,92,d)!=92)return "";return Encoding.Unicode.GetString(d,28,64).TrimEnd('\0'); }
 public static string Packet(string path) { try { using(var f=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.ReadWrite|FileShare.Delete)) { if(f.Length>4096)return "";using(var r=new StreamReader(f,Encoding.UTF8))return r.ReadToEnd().Trim(); } } catch(IOException) { return ""; } }
}
'@ }
if($OwnedMainPid -eq 0) { $OwnedMainPid=[int](Get-Content -LiteralPath (Join-Path $Root 'gui.pid') -Raw).Trim() }
$gui=Get-Process -Id $OwnedMainPid
Check ((UnderRoot $gui.Path) -and $gui.MainWindowHandle -ne 0) "owned KSword PID $OwnedMainPid has a real window under the explicit guest root"
$script:mainWindow=$gui.MainWindowHandle
function OwnedChildren([int]$HelperId) {
    @([GuestPolicyWindows]::Children($script:mainWindow) | Where-Object {
        $owner=[uint32]0; [GuestPolicyWindows]::GetWindowThreadProcessId($_,[ref]$owner)|Out-Null; $owner -eq $HelperId
    })
}
function Control($Context,[int]$Id) {
    $found=@(OwnedChildren $Context.HelperId | Where-Object {[GuestPolicyWindows]::GetDlgCtrlID($_) -eq $Id})
    if($found.Count -ne 1){throw "$($Context.Kind) needs one owned control HWND for ID $Id"}; $found[0]
}
function ActivateTab($Context) {
    $currentThread=[GuestPolicyWindows]::GetCurrentThreadId(); $attached=@()
    try {
        # BM_CLICK does not reliably notify an inactive native button. Join the
        # owned host/helper input queues only while activating their real page.
        foreach($window in @($script:mainWindow,$Context.Surface)) {
            $owner=[uint32]0; $thread=[GuestPolicyWindows]::GetWindowThreadProcessId($window,[ref]$owner)
            if($owner -notin @($OwnedMainPid,$Context.HelperId)){throw 'Activation HWND ownership changed'}
            if($thread -ne $currentThread -and $thread -notin $attached -and [GuestPolicyWindows]::AttachThreadInput($currentThread,$thread,$true)){$attached+=,$thread}
        }
        if([GuestPolicyWindows]::IsIconic($script:mainWindow)){[GuestPolicyWindows]::ShowWindow($script:mainWindow,9)|Out-Null}
        [GuestPolicyWindows]::SetForegroundWindow($script:mainWindow)|Out-Null
        [GuestPolicyWindows]::SetActiveWindow($script:mainWindow)|Out-Null
        if(!$Context.IsFresh) {
            # Keep selectors ASCII: Windows PowerShell 5 reads BOM-less UTF-8
            # scripts as the guest ANSI code page. Localized suffixes may vary.
            $prefix=if($Context.Kind -eq 'CE'){'Cheat Engine '}else{'x96dbg '}
            $main=Automation $script:mainWindow $OwnedMainPid
            $condition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::TabItem)
            $all=$main.FindAll([System.Windows.Automation.TreeScope]::Descendants,$condition)
            $tabs=@($all | Where-Object {$_.Current.ProcessId -eq $OwnedMainPid -and $_.Current.Name.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)})
            if($tabs.Count -ne 1) {
                $names=@($all | ForEach-Object {$_.Current.Name}) -join ', '
                throw "Cannot identify one owned Qt TabItem prefix '$prefix'; observed: $names"
            }
            $tab=$tabs[0]; $pattern=$null
            if($tab.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern,[ref]$pattern)) {
                $pattern.Select()
                Check $pattern.Current.IsSelected "$($Context.Kind) Qt TabItem is actually selected"
            } elseif($tab.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$pattern)) {$pattern.Invoke()}
            else {throw "Owned Qt TabItem prefix '$prefix' exposes neither SelectionItem nor Invoke"}
        }
        $end=(Get-Date).AddSeconds(5)
        while(![GuestPolicyWindows]::IsWindowVisible($Context.Surface) -and (Get-Date) -lt $end){Start-Sleep -Milliseconds 100}
        Check ([GuestPolicyWindows]::IsWindowVisible($Context.Surface)) "$($Context.Kind) owned log/control page is visible after activation"
    } finally {foreach($thread in $attached){[GuestPolicyWindows]::AttachThreadInput($currentThread,$thread,$false)|Out-Null}}
}
function WaitEnabled($Context,[int]$Id) {
    ActivateTab $Context
    $hwnd=Control $Context $Id; $end=(Get-Date).AddSeconds(4)
    while(![GuestPolicyWindows]::IsWindowEnabled($hwnd) -and (Get-Date) -lt $end){Start-Sleep -Milliseconds 100}
    Check ([GuestPolicyWindows]::IsWindowVisible($hwnd)) "$($Context.Kind) control $Id is visible on the selected page"
    Check ([GuestPolicyWindows]::IsWindowEnabled($hwnd)) "$($Context.Kind) control $Id applied the actual ACK and is enabled"; $hwnd
}
function Automation([IntPtr]$Window,[int]$ExpectedPid) {
    $element=[System.Windows.Automation.AutomationElement]::FromHandle($Window)
    if($element.Current.ProcessId -ne $ExpectedPid){throw 'UIA HWND process identity changed'}; $element
}
function PhysicalClick($Context,[IntPtr]$Window) {
    $element=Automation $Window $Context.HelperId; $rectangle=$element.Current.BoundingRectangle
    if($rectangle.IsEmpty -or $rectangle.Width -le 0 -or $rectangle.Height -le 0){throw 'Physical fallback requires a real owned UIA rectangle'}
    $point=New-Object GuestPolicyWindows+Point
    $point.X=[int]($rectangle.Left+$rectangle.Width/2); $point.Y=[int]($rectangle.Top+$rectangle.Height/2)
    $hit=[GuestPolicyWindows]::WindowFromPoint($point); $owner=[uint32]0
    [GuestPolicyWindows]::GetWindowThreadProcessId($hit,[ref]$owner)|Out-Null
    Check ($hit -eq $Window -and $owner -eq $Context.HelperId -and [GuestPolicyWindows]::IsWindowVisible($Window) -and [GuestPolicyWindows]::IsWindowEnabled($Window)) "$($Context.Kind) physical fallback point hits the exact owned, visible, enabled control"
    $previous=New-Object GuestPolicyWindows+Point; $restore=[GuestPolicyWindows]::GetCursorPos([ref]$previous)
    try {
        if(![GuestPolicyWindows]::SetCursorPos($point.X,$point.Y)){throw 'Cannot position cursor over validated control'}
        [GuestPolicyWindows]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
        [GuestPolicyWindows]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
    } finally {if($restore){[GuestPolicyWindows]::SetCursorPos($previous.X,$previous.Y)|Out-Null}}
    Note "$($Context.Kind) used a physical click at the verified owned UIA rectangle"
}
function ObserveUiRequest($Context,$Before,[string]$OldPacket,[IntPtr]$Window,[switch]$PhysicalFallback) {
    $changed=$false; $packet=''; $end=(Get-Date).AddSeconds(2)
    do {
        $packet=[GuestPolicyWindows]::Packet($Context.Control)
        if($packet -and $packet -ne $OldPacket){$changed=$true;break}
        Start-Sleep -Milliseconds 50
    } while((Get-Date) -lt $end)
    if(!$changed -and $PhysicalFallback) {
        Note "$($Context.Kind) no outgoing packet after native/UIA click; trying one validated physical click"
        PhysicalClick $Context $Window
        $end=(Get-Date).AddSeconds(2)
        do {
            $packet=[GuestPolicyWindows]::Packet($Context.Control)
            if($packet -and $packet -ne $OldPacket){$changed=$true;break}
            Start-Sleep -Milliseconds 50
        } while((Get-Date) -lt $end)
    }
    Note "$($Context.Kind) outgoing UI request before ACK wait: '$packet'; previous actual ACK=$($Before.Revision)"
    Check $changed "$($Context.Kind) real UI action wrote a changed control packet"
    $fields=$packet -split '\s+'; $revision=[uint64]0
    $valid=$fields.Count -eq 10 -and [uint64]::TryParse($fields[1],[ref]$revision)
    if($Context.Kind -eq 'CE'){$valid=$valid -and $fields[0] -eq 'CE2'}else{$valid=$valid -and $fields[0] -eq $Context.Session -and $fields[3] -eq 'v2'}
    Check ($valid -and $revision -gt $Before.Revision) "$($Context.Kind) outgoing UI revision $revision is newer than confirmed ACK $($Before.Revision), not a duplicate"
}
function Click($Context,[int]$Id) {
    $hwnd=WaitEnabled $Context $Id; $element=Automation $hwnd $Context.HelperId; $pattern=$null
    $before=State $Context; $oldPacket=[GuestPolicyWindows]::Packet($Context.Control)
    $expectRequest=$Context.Kind -eq 'X96' -or $Id -in @(7101,7110,7111)
    if($Context.Kind -eq 'CE' -and $Id -eq 7110) {
        $limit=[uint32]0; $valid=[uint32]::TryParse([GuestPolicyWindows]::Text((Control $Context 7112)),[ref]$limit)
        $expectRequest=$valid -and $limit -ge 1 -and $limit -le 32
    }
    $invoked=$false
    try {$element.SetFocus()} catch {Note "$($Context.Kind) $Id native UIA focus unavailable: $($_.Exception.Message)"}
    if($element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$pattern)) {
        try {$pattern.Invoke(); Note "$($Context.Kind) $Id used UIA Invoke"; $invoked=$true}
        catch {Note "$($Context.Kind) $Id UIA Invoke unavailable: $($_.Exception.Message)"}
    }
    # Owner-drawn controls may omit InvokePattern. Native click still enters the real UI handler.
    if(!$invoked) {
        [GuestPolicyWindows]::SendMessage($hwnd,245,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
        Note "$($Context.Kind) $Id used BM_CLICK on a validated owned HWND"
    }
    if($expectRequest){ObserveUiRequest $Context $before $oldPacket $hwnd -PhysicalFallback}
}
function Combo($Context,[int]$Id,[int]$Index) {
    $hwnd=WaitEnabled $Context $Id; $null=Automation $hwnd $Context.HelperId
    $before=State $Context; $oldPacket=[GuestPolicyWindows]::Packet($Context.Control)
    $selected=[GuestPolicyWindows]::SendMessage($hwnd,334,[IntPtr]$Index,[IntPtr]::Zero).ToInt32()
    if($selected -ne $Index){throw 'Cannot select native combo option'}
    [GuestPolicyWindows]::SendMessage([GuestPolicyWindows]::GetParent($hwnd),273,[IntPtr](65536+$Id),$hwnd)|Out-Null
    Note "$($Context.Kind) $Id sent real CBN_SELCHANGE, index $Index"
    ObserveUiRequest $Context $before $oldPacket $hwnd
}
function Limit($Context,[string]$Value) {
    $hwnd=WaitEnabled $Context 7112; $element=Automation $hwnd $Context.HelperId; $pattern=$null
    if($element.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern,[ref]$pattern)){$pattern.SetValue($Value)}
    else {[GuestPolicyWindows]::SetText($hwnd,12,[IntPtr]::Zero,$Value)|Out-Null}
}
function State($Context) {
    $packet=[GuestPolicyWindows]::Packet($Context.State); if(!$packet){return $null}; $fields=$packet -split '\s+'
    if($Context.Kind -eq 'CE') {
        if($fields.Count -ne 22 -or $fields[6] -ne 'CE2' -or $fields[8] -ne '1'){return $null}
        $map=@{Revision=0;Error=1;Selected=2;OptionStart=9;Path=15;Pages=16;Bindings=17;CanChange=18;FallbackCount=19;FallbackError=20;PersistError=21}
    } else {
        if($fields.Count -ne 20 -or $fields[0] -ne $Context.Session -or $fields[7] -ne 'v2'){return $null}
        $map=@{Revision=1;Error=2;Selected=3;OptionStart=8;Path=14;Bindings=15;Pages=16;CanChange=17;FallbackCount=18;FallbackError=19}
    }
    foreach($index in $map.Values){if($fields[$index] -notmatch '^\d+$'){return $null}}
    $start=[int]$map.OptionStart; $options=@($fields[$start..($start+5)] | ForEach-Object {[int]$_})
    if($options[0] -gt 1 -or @($options[1..4] | Where-Object {$_ -gt 1}).Count -ne 0 -or $options[5] -lt 1 -or $options[5] -gt 32){return $null}
    $actual=[ordered]@{Options=$options;Key=($options -join ' ');Packet=$packet}
    foreach($name in $map.Keys){if($name -ne 'OptionStart'){$actual[$name]=[uint64]$fields[$map[$name]]}}
    if($actual.Selected -gt 1 -or $actual.Path -gt 2 -or $actual.CanChange -gt 1){return $null}; [pscustomobject]$actual
}
function Await($Context,[decimal]$After=-1,[string]$Options='',[int]$Selected=-1,[int]$ErrorCode=0) {
    $end=(Get-Date).AddSeconds(10); $actual=$null; $matched=$false
    do {
        $actual=State $Context
        $matched=$null -ne $actual -and [decimal]$actual.Revision -gt $After -and $actual.Error -eq $ErrorCode -and (!$Options -or $actual.Key -eq $Options) -and ($Selected -lt 0 -or $actual.Selected -eq $Selected)
        if($matched){break}; Start-Sleep -Milliseconds 100
    } while((Get-Date) -lt $end)
    Check $matched "$($Context.Kind) actual ACK after ${After}: options=$Options HVM=$Selected error=$ErrorCode; actual=$($actual.Packet)"
    if($Context.ExternalRevision -ne 0 -and $actual.Revision -ge $Context.ExternalRevision) {
        # Source timers: CE 250 ms, X96 200 ms. Controls may remain enabled
        # from an older ACK, so IsWindowEnabled alone cannot prove UI revision
        # adoption after a directly injected packet. Allow three CE ticks.
        Start-Sleep -Milliseconds 750
        Note "$($Context.Kind) settled frontend poll timers after external ACK revision $($actual.Revision) before the next real UI action"
        $Context.ExternalRevision=[uint64]0
    }
    Note "$($Context.Kind) actual path=$($actual.Path) bindings=$($actual.Bindings) Shadow write pages=$($actual.Pages) fallback=$($actual.FallbackCount)/$($actual.FallbackError)"; $actual
}
function WriteRequest($Context,[string]$Packet) {
    $temporary=$Context.Control+'.gui-test.new'
    [IO.File]::WriteAllText($temporary,$Packet+[char]10,(New-Object Text.UTF8Encoding($false)))
    if(![GuestPolicyWindows]::MoveFileEx($temporary,$Context.Control,9)){throw "Cannot atomically replace owned control file, error $([Runtime.InteropServices.Marshal]::GetLastWin32Error())"}
    $Context.ExternalRevision=[uint64](($Packet -split '\s+')[1])
    Note "$($Context.Kind) injected test control packet: '$Packet'"
}
function ValidRequest($Context,[uint64]$Revision,[string]$Options,[int]$Selected=0,[int]$Action=1) {
    if($Context.Kind -eq 'CE'){"CE2 $Revision $Action $Selected $Options"}else{"$($Context.Session) $Revision $Selected v2 $Options"}
}
function ResetNormalPolicy($Context) {
    $before=State $Context
    if($null -eq $before){throw "$($Context.Kind) cannot acknowledge final policy cleanup"}
    # CE action 1 changes options only; its HVM field is not a selection request.
    # Turn off a saved HVM selection explicitly before resetting the policy.
    if($Context.Kind -eq 'CE' -and $before.Selected -ne 0) {
        WriteRequest $Context (ValidRequest $Context ($before.Revision+1) $before.Key 0 0)
        $before=Await $Context $before.Revision $before.Key 0
    }
    WriteRequest $Context (ValidRequest $Context ($before.Revision+1) '0 0 1 1 1 32')
    Await $Context $before.Revision '0 0 1 1 1 32' 0
}
function FinishPolicy($Context) {
    $null=ResetNormalPolicy $Context
    $null=WaitEnabled $Context $(if($Context.Kind -eq 'CE'){7102}else{4103})
}
function CloseFreshSurface($Context) {
    $owner=[uint32]0; [GuestPolicyWindows]::GetWindowThreadProcessId($Context.Surface,[ref]$owner)|Out-Null
    if($owner -eq $Context.HelperId){[GuestPolicyWindows]::PostMessage($Context.Surface,16,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null}
}
function NewContext([int]$HelperId,[switch]$Fresh) {
    $helper=Get-Process -Id $HelperId
    if(!(UnderRoot $helper.Path)){throw 'Helper executable escaped explicit guest root'}
    $kind=if($helper.ProcessName -eq 'KswordCheatEngineLauncher'){'CE'}elseif($helper.ProcessName -eq 'KswordX96dbgLauncher'){'X96'}else{throw 'Unexpected helper identity'}
    $end=(Get-Date).AddSeconds(20); $client=$null; $owned=@()
    do {
        $expected=if($kind -eq 'CE'){'cheatengine-x86_64.exe'}else{'x64dbg.exe'}
        $clients=@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$HelperId" | Where-Object {
            (($_.Name -eq $expected) -and (UnderRoot $_.ExecutablePath)) -or
            (RandomizedPayload $_.ExecutablePath $kind)
        })
        $owned=@(OwnedChildren $HelperId)
        if($clients.Count -eq 1 -and $owned.Count -ge 4){$client=Get-Process -Id $clients[0].ProcessId; $client.Refresh(); if($client.MainWindowHandle -ne 0){break}}
        Start-Sleep -Milliseconds 150
    } while((Get-Date) -lt $end)
    Check ($null -ne $client -and $client.MainWindowHandle -ne 0 -and $owned.Count -ge 4) "$kind helper $HelperId owns a real payload and embedded controls"
    Check (([GuestPolicyWindows]::GetWindowLong($client.MainWindowHandle,-16) -band 0x40000000) -eq 0 -and [GuestPolicyWindows]::GetAncestor($client.MainWindowHandle,2) -ne $script:mainWindow) "$kind debugger PID $($client.Id) remains an independent top-level window"
    $editors=@($owned | Where-Object {[GuestPolicyWindows]::Class($_) -eq 'Edit' -and ([GuestPolicyWindows]::GetWindowLong($_,-16) -band 4) -ne 0})
    Check ($editors.Count -eq 1 -and [GuestPolicyWindows]::Font($editors[0]) -eq 'Consolas') "$kind multiline log uses Consolas"
    $session=''; $preferences=Join-Path (Split-Path -Parent $helper.Path) 'x96dbg-options.ini'
    if($kind -eq 'CE'){$state=Join-Path $CeTempDirectory "ksword-ce-bridge-$HelperId.status.log.state"; $preferences=$CeSettingsFile}
    else {
        $command=$clients[0].CommandLine
        if($command -notmatch '(?i)(?:^|\s)-userdir\s+(?:"([^"]+)"|(\S+))'){throw 'Real x64dbg command line has no session userdir'}
        $sessionDirectory=if($Matches[1]){$Matches[1]}else{$Matches[2]}
        Check (UnderRoot $sessionDirectory) 'X96 actual debugger userdir stays inside the explicit guest root'
        $session=Split-Path -Leaf $sessionDirectory; $state=Join-Path $sessionDirectory 'backend.log.state'
    }
    $context=[pscustomobject]@{Kind=$kind;HelperId=$HelperId;HelperPath=$helper.Path;DebuggerId=$client.Id;Surface=[GuestPolicyWindows]::GetParent($editors[0]);Log=$editors[0];State=$state;Control=($state -replace '\.state$','.control');Session=$session;Preferences=$preferences;IsFresh=[bool]$Fresh;ExternalRevision=[uint64]0}
    $script:contexts+=,$context
    $actual=Await $context
    Check ($actual.CanChange -eq 1 -and $actual.Bindings -eq 0 -and $actual.Pages -eq 0) "$kind starts without active target bindings or memory patches"
    $null=WaitEnabled $context $(if($kind -eq 'CE'){7101}else{4101})
    Check ([GuestPolicyWindows]::Text($editors[0]) -match 'backend|\u540e\u7aef') "$kind forwards real backend startup logs"
    $context
}
$helpers=@(Get-CimInstance Win32_Process | Where-Object {
    $_.Name -in @('KswordCheatEngineLauncher.exe','KswordX96dbgLauncher.exe') -and (UnderRoot $_.ExecutablePath) -and
    ($_.ParentProcessId -eq $OwnedMainPid -or $_.CommandLine -match ("(?:^|\s)--host-pid\s+"+$OwnedMainPid+"(?:\s|$)"))
})
Check ($helpers.Count -eq 2) 'exactly two production PluginHost helpers belong to the selected KSword PID'
$ce=NewContext ([int]($helpers | Where-Object Name -eq 'KswordCheatEngineLauncher.exe').ProcessId)
$x96=NewContext ([int]($helpers | Where-Object Name -eq 'KswordX96dbgLauncher.exe').ProcessId)
foreach($context in @($ce,$x96)) {
    $exists=Test-Path -LiteralPath $context.Preferences -PathType Leaf
    $script:preferences[$context.Preferences]=[pscustomobject]@{Exists=$exists;Bytes=$(if($exists){[IO.File]::ReadAllBytes($context.Preferences)}else{[byte[]]@()})}
}
foreach($context in @($ce,$x96)) {
    $before=State $context
    if(!$AllowSavedOptions){
        Check ($before.Selected -eq 0) "$($context.Kind) initially acknowledges HVM off"
        Check ($before.Key -eq '0 0 1 1 1 32') "$($context.Kind) documented normal defaults, Shadow memory writes off"
    }
    else {
        $before=ResetNormalPolicy $context
        Note "$($context.Kind) explicit acknowledged reset of saved HVM selection and options; startup-default assertion skipped"
    }
    $toggle=if($context.Kind -eq 'CE'){7101}else{4101}
    # Preserve the original repeated real-driver HVM switch acceptance checks.
    foreach($expected in 1,0,1,0){Click $context $toggle; $before=Await $context $before.Revision '' $expected; $null=WaitEnabled $context $toggle}
}
# CE drafts have no backend effect before Apply.
$before=State $ce
Click $ce 7102; Click $ce 7103; Click $ce 7105; Click $ce 7106; Limit $ce '8'
Start-Sleep -Milliseconds 400; $draft=State $ce
Check ($draft.Revision -eq $before.Revision -and $draft.Key -eq $before.Key) 'CE drafts retain the previous actual ACK before Apply'
Click $ce 7110; $actual=Await $ce $before.Revision '0 1 0 0 0 8' 0
$null=WaitEnabled $ce 7112
Check ([GuestPolicyWindows]::Text((Control $ce 7112)) -eq '8') 'CE displays the acknowledged custom page limit'
Limit $ce '33'; Click $ce 7110; Start-Sleep -Milliseconds 400; $invalid=State $ce
Check ($invalid.Revision -eq $actual.Revision -and $invalid.Key -eq $actual.Key) 'CE rejects invalid local page limits before sending any backend request'
Limit $ce '8'; Click $ce 7104; Click $ce 7110; $actual=Await $ce $actual.Revision '1 1 0 0 0 8' 0
$null=WaitEnabled $ce 7111
Check (![GuestPolicyWindows]::IsWindowEnabled((Control $ce 7102))) 'CE stealth forces effective Shadow and disables its toggle'
Click $ce 7111; $actual=Await $ce $actual.Revision '1 1 0 0 0 8' 0
Check ($actual.Pages -eq 0) 'CE Restore control received an actual backend ACK with no outstanding test memory patches'
Note 'This Restore command ACK is not proof of a live patch restore; policy_live.py tests actual code patches separately'
# X96 controls apply immediately, with confirmation after each real UI action.
$before=State $x96; Click $x96 4103; $actual=Await $x96 $before.Revision '0 1 1 1 1 32' 0
Combo $x96 4102 1; $actual=Await $x96 $actual.Revision '1 1 0 1 1 32' 0
$null=WaitEnabled $x96 4102
Check (![GuestPolicyWindows]::IsWindowEnabled((Control $x96 4103))) 'X96 stealth forces effective Shadow and disables its toggle'
Click $x96 4104; $actual=Await $x96 $actual.Revision '1 1 1 1 1 32' 0
Click $x96 4105; $actual=Await $x96 $actual.Revision '1 1 1 0 1 32' 0
Click $x96 4106; $actual=Await $x96 $actual.Revision '1 1 1 0 0 32' 0
Combo $x96 4107 7; $actual=Await $x96 $actual.Revision '1 1 1 0 0 8' 0
Combo $x96 4102 0; $actual=Await $x96 $actual.Revision '0 1 1 0 0 8' 0
$null=WaitEnabled $x96 4103
Check ([GuestPolicyWindows]::IsWindowEnabled((Control $x96 4103))) 'X96 normal mode restores the user normal Shadow choice and enables its toggle'
Combo $x96 4102 1; $actual=Await $x96 $actual.Revision '1 1 0 0 0 8' 0
$null=WaitEnabled $x96 4102
Check ([GuestPolicyWindows]::SendMessage((Control $x96 4102),327,[IntPtr]::Zero,[IntPtr]::Zero).ToInt32() -eq 1) 'X96 mode combo displays actual acknowledged stealth'
# Invalid identified packets must ACK failure and preserve actual/saved options.
foreach($context in @($ce,$x96)) {
    $before=State $context; $saved=[GuestPolicyWindows]::Packet($context.Preferences)
    Check ($saved -ne '') "$($context.Kind) successful actual options were persisted"
    $bad=$before.Options.Clone(); $bad[5]=33
    WriteRequest $context (ValidRequest $context ($before.Revision+1) ($bad -join ' '))
    $failed=Await $context $before.Revision $before.Key 0 87
    Check ([GuestPolicyWindows]::Packet($context.Preferences) -eq $saved) "$($context.Kind) rejected options retain the last saved actual configuration"
    WriteRequest $context (ValidRequest $context ($failed.Revision+1) $before.Key)
    $actual=Await $context $failed.Revision $before.Key 0
    $null=WaitEnabled $context $(if($context.Kind -eq 'CE'){7101}else{4101})
    $expected=if($context.Kind -eq 'CE'){'CE2 0 1 1 0 0 0 8'}else{'ksword-x96dbg-options/1 1 1 0 0 0 8'}
    Check ([GuestPolicyWindows]::Packet($context.Preferences) -eq $expected) "$($context.Kind) saved custom policy is confirmed and HVM remains off"
    if($context.Kind -eq 'CE'){Check ($actual.PersistError -eq 0) 'CE ACK reports successful persistence'}
}
if($VerifyFreshSessions) {
    foreach($original in @($ce,$x96)) {
        $name=$original.Kind.ToLowerInvariant()
        $start=Start-Process -FilePath $original.HelperPath -ArgumentList @('--ksword-plugin','tab','--parent-hwnd',$script:mainWindow.ToInt64().ToString(),'--host-pid',$OwnedMainPid.ToString()) -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $Root "gui-policy-fresh-$name.jsonl") -RedirectStandardError (Join-Path $Root "gui-policy-fresh-$name.stderr.log")
        $script:startedHelpers+=,$start.Id
        $fresh=NewContext $start.Id -Fresh; $script:freshContexts+=,$fresh; $actual=State $fresh
        Check ($actual.Key -eq '1 1 0 0 0 8' -and $actual.Selected -eq 0) "$($fresh.Kind) a fresh real debugger loaded saved custom policy with HVM off"
        Check (![GuestPolicyWindows]::IsWindowEnabled((Control $fresh $(if($fresh.Kind -eq 'CE'){7102}else{4103})))) "$($fresh.Kind) fresh stealth keeps forced Shadow disabled"
        FinishPolicy $fresh; CloseFreshSurface $fresh
        Note "$($fresh.Kind) closed only the additional owned log surface; debugger PID $($fresh.DebuggerId) remains for owner cleanup"
    }
} else {Note 'Fresh real-session persistence reload skipped; enable -VerifyFreshSessions to exercise it'}
foreach($context in @($ce,$x96)){FinishPolicy $context}
$script:outcome='PASS'
Add-Content -LiteralPath $report -Value 'FINAL PASS owned real PluginHost Tabs, standalone debuggers, actual policy ACKs and invalid-option rollback'
} catch {Add-Content -LiteralPath $report -Value ('FAIL '+($_|Out-String))}
finally {
    # On failure make the remaining test-owned sessions normal/HVM-off as well.
    if($script:outcome -ne 'PASS') {
        foreach($context in $script:contexts){try {if(State $context){FinishPolicy $context}}catch {Note "Cleanup not acknowledged for helper $($context.HelperId): $($_.Exception.Message)"}}
    }
    foreach($context in $script:freshContexts){CloseFreshSurface $context}
    if(!$KeepTestPreferences) {
        foreach($path in $script:preferences.Keys) {
            try {
                $saved=$script:preferences[$path]
                if($saved.Exists) {
                    $temporary=$path+'.gui-test.restore'; [IO.File]::WriteAllBytes($temporary,[byte[]]$saved.Bytes)
                    if(![GuestPolicyWindows]::MoveFileEx($temporary,$path,9)){throw 'Atomic preference restore failed'}
                } else {[IO.File]::Delete($path)}
                Note "Restored the original preference file state: $path"
            } catch {$script:outcome='FAIL'; Add-Content -LiteralPath $report -Value ('FAIL preference restore: '+$_.Exception.Message)}
        }
    }
    [pscustomobject]@{Outcome=$script:outcome;OwnedMainPid=$OwnedMainPid;Root=$Root;FreshSessionReload=[bool]$VerifyFreshSessions;OriginalPreferencesRestored=(!$KeepTestPreferences);StartedHelpers=$script:startedHelpers;Sessions=@($script:contexts | Select-Object Kind,HelperId,DebuggerId,State,Preferences)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $Root 'gui-tabs-owned-processes.json') -Encoding UTF8
}
if($script:outcome -ne 'PASS'){exit 1}
