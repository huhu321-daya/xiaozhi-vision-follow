# AAS3-CAM 独立节点带进度条构建脚本
# 用法:
#   .\build_node.ps1 build     # 编译（ninja 打印 [N/M] 进度）
#   .\build_node.ps1 flash     # 烧录 COM4（esptool 百分比进度条）
#   .\build_node.ps1 monitor   # 监控 COM4
param(
    [string]$Action = 'build'
)

$env:IDF_PATH = 'D:\S3\v5.5.4\esp-idf'
$env:PATH = 'C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.9;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;' + $env:PATH

$py  = 'C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe'
$idf = 'D:\S3\v5.5.4\esp-idf\tools\idf.py'

Write-Host "== AAS3-CAM node: $Action ==" -ForegroundColor Cyan
switch ($Action.ToLower()) {
    'build'   { & $py $idf build }
    'flash'   { & $py $idf -p COM4 flash }
    'monitor' { & $py $idf -p COM4 monitor }
}
exit $LASTEXITCODE