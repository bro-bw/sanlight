param([Parameter(Mandatory=$true)][string]$Path)
$ErrorActionPreference = 'Stop'
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class SLResourceCheck {
[DllImport("kernel32",CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibraryEx(string p,IntPtr f,uint flags);
[DllImport("kernel32",CharSet=CharSet.Unicode)] public static extern IntPtr FindResource(IntPtr h,IntPtr n,string t);
[DllImport("kernel32")] public static extern IntPtr LoadResource(IntPtr h,IntPtr r);
[DllImport("kernel32")] public static extern IntPtr LockResource(IntPtr r);
[DllImport("kernel32")] public static extern uint SizeofResource(IntPtr h,IntPtr r);
[DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr h);
}
'@
$h = [SLResourceCheck]::LoadLibraryEx((Resolve-Path -LiteralPath $Path), [IntPtr]::Zero, 2)
if ($h -eq [IntPtr]::Zero) { throw 'Cannot read AEX' }
try {
    $r = [SLResourceCheck]::FindResource($h, [IntPtr]16000, 'PiPL')
    if ($r -eq [IntPtr]::Zero) { throw 'Missing PiPL' }
    $b = New-Object byte[] ([SLResourceCheck]::SizeofResource($h, $r))
    [Runtime.InteropServices.Marshal]::Copy([SLResourceCheck]::LockResource([SLResourceCheck]::LoadResource($h, $r)), $b, 0, $b.Length)
    if ($b.Length -lt 10) { throw 'Truncated PiPL' }
    $offset = 10
    $count = [BitConverter]::ToUInt32($b, 6)
    for ($i = 0; $i -lt $count; $i++) {
        if ($offset + 16 -gt $b.Length) { throw "Truncated property $i" }
        $vendor = [Text.Encoding]::ASCII.GetString($b, $offset, 4)
        $key = [Text.Encoding]::ASCII.GetString($b, $offset + 4, 4)
        $length = [BitConverter]::ToUInt32($b, $offset + 12)
        if ($vendor -ne 'MIB8' -or $offset + 16 + [long]$length -gt $b.Length) { throw "Invalid property $i at $offset ($key)" }
        if ($key -in @('eman', 'gtac', 'ANMe')) {
            if ($length -lt 1 -or $b[$offset + 16] + 1 -gt $length) { throw "Invalid string: $key" }
        }
        $offset += 16 + [int](4 * [Math]::Ceiling($length / 4.0))
    }
    if ($offset -ne $b.Length) { throw "Unexpected trailing bytes: $($b.Length - $offset)" }
    "$Path : $count PiPL properties validated ($offset bytes)"
} finally { [void][SLResourceCheck]::FreeLibrary($h) }
