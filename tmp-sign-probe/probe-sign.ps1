$ErrorActionPreference = 'Stop'
$signTool = 'E:\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
$target = 'H:\Project\Ksword5.1\tmp-sign-probe\KswordARK-probe.sys'
$pfx = 'H:\Project\Ksword5.1\.cert\KswordARK-TestSigning.pfx'
$pwd = 'KswordARK-TestSigning-LocalOnly'
& $signTool sign /debug /v /fd SHA256 /f $pfx /p $pwd $target
Write-Host "signtool exit=$LASTEXITCODE"
Get-AuthenticodeSignature $target | Format-List Status,StatusMessage,SignerCertificate
