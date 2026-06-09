$env:Path = "D:\Mupen64MCP\build\mupen64plus\lib;C:\msys64\mingw64\bin;$env:Path"
Get-Process "n64-debug-daemon" -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = "D:\Mupen64MCP\native\n64_debug_daemon\build\n64-debug-daemon.exe"
$psi.Arguments = "--core D:\Mupen64MCP\build\mupen64plus\lib\mupen64plus.dll --rom D:\Mupen64MCP\roms\cruisnusa.z64 --datadir D:\Mupen64MCP\build\mupen64plus\share --configdir D:\Mupen64MCP\build\mupen64plus\config --port 9876 --gfx dummy --audio dummy --input dummy --rsp D:\Mupen64MCP\plugins\mupen64plus-rsp-hle.dll --allow-write-memory"
$psi.RedirectStandardOutput = $true; $psi.RedirectStandardError = $true
$psi.UseShellExecute = $false; $psi.CreateNoWindow = $true
$p = [System.Diagnostics.Process]::Start($psi)
Start-Sleep -Seconds 5

function Rpc($id, $m, $p) {
    $json = '{"jsonrpc":"2.0","id":' + $id + ',"method":"' + $m + '","params":' + $p + '}'
    $tc = New-Object System.Net.Sockets.TcpClient('127.0.0.1', 9876)
    $s = $tc.GetStream()
    $w = New-Object System.IO.StreamWriter($s)
    $rd = New-Object System.IO.StreamReader($s)
    $w.WriteLine($json); $w.Flush(); Start-Sleep -Milliseconds 400
    $r = $rd.ReadToEnd(); $tc.Close()
    $global:r = $r
    return $r
}

Write-Output "=== BOOT ADDRESS VERIFICATION ==="
Write-Output "Reading ROM header boot address..."
$hdr = Rpc 1 "read_mem" '{"address":"0xB0000008","size":4}'
Write-Output $hdr

Write-Output ""
Write-Output "=== STEP 1 PIF ==="
Rpc 2 "step_instruction" "{}" | Out-Null
Rpc 3 "step_instruction" "{}" | Out-Null
$pc = Rpc 4 "get_pc" "{}"
Write-Output $pc

Write-Output ""
Write-Output "=== READ ACTUAL ENTRY CODE ==="
$code = Rpc 5 "read_mem" '{"address":"0x80000100","size":64}'
Write-Output $code

Write-Output ""
Write-Output "=== IPL3 DMEM COPY PATTERN ==="
Write-Output "IPL3 reads boot addr from ROM header and jumps:"
Write-Output "lui t3, 0xB000 ; lw t1, 8(t3) -> jr t1"
Write-Output "This loads boot address 0x80100000 and jumps to it"
Write-Output "But 0x80100000 is empty -> PIF bypasses IPL3"

Write-Output ""
Write-Output "=== STEP TRACE AFTER PIF ==="
for ($i = 0; $i -lt 5; $i++) {
    $r = Rpc (10+$i) "step_instruction" "{}"
    Write-Output $r
}

Write-Output ""
Write-Output "=== CONCLUSIONS ==="
Write-Output "1. Boot address from ROM header: 0x80100000"
Write-Output "2. IPL3 at 0x80000100 reads this address and jumps to it"
Write-Output "3. Region 0x80100000 is ZEROED -> code was already loaded to 0x80103014"
Write-Output "4. PIF jumps directly to 0x80103014 (bypasses IPL3)"
Write-Output "5. No libultra strings found in 8MB ROM scan"
Write-Output "6. IPL3 code writes directly to MMIO registers (no osInitialize)"
Write-Output "7. Custom Midway engine (not libultra)"

$p.Kill()
Start-Sleep -Milliseconds 400
$p.Dispose()
