
$projectPath = "Ksword5.1.vcxproj"
$namespace = ""

[xml]$projXml = Get-Content $projectPath

foreach ($item in $projXml.Project.ItemGroup.File) {
    if ($item.Include -like "*.h") {
        $customBuild = $projXml.CreateElement("CustomBuild", $projXml.Project.XmlNamespaceURI)
        
        $command = $projXml.CreateElement("Command", $projXml.Project.XmlNamespaceURI)
        $command.InnerText = """$(QTDIR)\bin\moc.exe"" ""%(FullPath)"" -o "".\GeneratedFiles\moc_%(Filename).cpp"" -f""%(FullPath)"""
        $customBuild.AppendChild($command) | Out-Null
        
        $description = $projXml.CreateElement("Description", $projXml.Project.XmlNamespaceURI)
        $description.InnerText = "MOC %(Identity)"
        $customBuild.AppendChild($description) | Out-Null
        
        $outputs = $projXml.CreateElement("Outputs", $projXml.Project.XmlNamespaceURI)
        $outputs.InnerText = ".\GeneratedFiles\moc_%(Filename).cpp"
        $customBuild.AppendChild($outputs) | Out-Null
        
        # 将CustomBuildTool节点添加到项目文件
        $itemGroup = $projXml.SelectSingleNode("//ItemGroup[not(CustomBuildTool)]")
        if (-not $itemGroup) {
            $itemGroup = $projXml.CreateElement("ItemGroup", $projXml.Project.XmlNamespaceURI)
            $projXml.Project.AppendChild($itemGroup) | Out-Null
        }
        $itemGroup.AppendChild($customBuild) | Out-Null
    }
}

# 保存修改后的项目文件
$projXml.Save($projectPath)
Write-Host "批量配置完成，请清理并重新生成项目。"