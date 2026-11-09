param([Parameter(Mandatory=$true)][string]$Path)
$ErrorActionPreference = 'Stop'
# Preserve PiPLtool's counted byte strings across compiler code pages.
$bytes = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $Path))
$result = New-Object Text.StringBuilder
foreach ($value in $bytes) {
    if ($value -ge 128) {
        [void]$result.Append('\' + [Convert]::ToString($value, 8).PadLeft(3, '0'))
    } else {
        [void]$result.Append([char]$value)
    }
}
[IO.File]::WriteAllText((Resolve-Path -LiteralPath $Path), $result.ToString(), [Text.Encoding]::ASCII)
