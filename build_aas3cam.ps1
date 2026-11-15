# AAS3-CAM 独立监控节点专用编译/烧录/监控脚本
# 用法:
#   .\build_aas3cam.ps1 build          # 编译（独立 build 目录 build_aas3cam）
#   .\build_aas3cam.ps1 flash          # 烧录（默认 COM4，摄像头板）
#   .\build_aas3cam.ps1 -p COMx build  # 指定串口
# 说明: 与 AHUHU 的 build/（COM3）完全隔离，互不影响。

param(
    [Alias('p')]
    [string]$Port = 'COM4',
    [Parameter(Position = 0)]
    [string]$Action = 'build'
)

$env:IDF_PATH = 'D:\S3\v5.5.4\esp-idf'
$env:PATH = 'C:\Espressif\tools\cmake\3.30.2\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.9;C:\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;' + $env:PATH
$env:SDKCONFIG = 'sdkconfig.aas3cam'
$env:SDKCONFIG_DEFAULTS = 'sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.aas3cam.defaults'

$py = 'C:\Espressif\tools\python\v5.5.4\venv\Scripts\python.exe'
$idf = 'D:\S3\v5.5.4\esp-idf\tools\idf.py'

switch ($Action) {
    'flash' {
        & $py $idf -B build_aas3cam -p $Port flash
        if ($LASTEXITCODE -eq 0) { Write-Host "AAS3-CAM flash OK ($Port)" -ForegroundColor Green }
    }
    'monitor' {
        & $py $idf -B build_aas3cam -p $Port monitor
    }
    default {
        & $py $idf -B build_aas3cam -p $Port $Action
    }
}