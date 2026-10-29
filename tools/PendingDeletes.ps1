<#
.SYNOPSIS
    Deletions that Windows does at the next restart (MoveFileEx with MOVEFILE_DELAY_UNTIL_REBOOT, kept in
    HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\PendingFileRenameOperations): dot-source it.
      Get-PendingDeletes [-Under dirs]       the files and folders to delete, all or those under some folders
      Add-PendingDeletes -Under dirs         every file and folder under those folders, deleted at the restart
      Remove-PendingDeletes -Under dirs      those under some folders, no longer deleted
    Reading needs nothing; adding and removing need administrator rights.
.NOTES
    The value is a list of pairs (source, destination; an empty destination: delete), with empty strings
    in it, which .NET would lose: it is read and written as its bytes.
#>
if (-not ('PendingDeletes.Native' -as [type])) {
Add-Type -Namespace PendingDeletes -Name Native -MemberDefinition @'
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileExW(string from, string to, int flags);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    public static extern int RegOpenKeyExW(UIntPtr key, string sub, int options, int sam, out UIntPtr result);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    public static extern int RegQueryValueExW(UIntPtr key, string name, IntPtr reserved, out int type, byte[] data, ref int size);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    public static extern int RegSetValueExW(UIntPtr key, string name, int reserved, int type, byte[] data, int size);
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode)]
    public static extern int RegDeleteValueW(UIntPtr key, string name);
    [DllImport("advapi32.dll")]
    public static extern int RegCloseKey(UIntPtr key);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetLongPathNameW(string shortPath, System.Text.StringBuilder longPath, int size);
'@
}
$script:pendingKey = 'SYSTEM\CurrentControlSet\Control\Session Manager'
$script:pendingValue = 'PendingFileRenameOperations'
# The start of a path there: "\??\", or "*1\??\" for a deletion (as Windows 11 writes it)
$script:pendingPrefix = '^(\*\d+)?\\\?\?\\'

# The list as its strings, the empty ones included
function Read-PendingList([UIntPtr]$key) {
    $type = 0; $size = 0
    if ([PendingDeletes.Native]::RegQueryValueExW($key, $script:pendingValue, [IntPtr]::Zero, [ref]$type, $null, [ref]$size) -ne 0 -or $size -lt 2) { return ,@() }
    $data = New-Object byte[] $size
    [void][PendingDeletes.Native]::RegQueryValueExW($key, $script:pendingValue, [IntPtr]::Zero, [ref]$type, $data, [ref]$size)
    $text = [Text.Encoding]::Unicode.GetString($data, 0, $size)
    # (every string ends in a null, and the list in one more)
    if ($text.EndsWith("`0`0")) { $text = $text.Substring(0, $text.Length - 2) } elseif ($text.EndsWith("`0")) { $text = $text.Substring(0, $text.Length - 1) }
    ,($text -split "`0")
}

function Open-PendingKey([int]$access) {
    $key = [UIntPtr]::Zero
    $hklm = [UIntPtr]::new([uint64]2147483650)   # HKEY_LOCAL_MACHINE (0x80000002)
    if ([PendingDeletes.Native]::RegOpenKeyExW($hklm, $script:pendingKey, 0, $access, [ref]$key) -ne 0) { return [UIntPtr]::Zero }
    $key
}

# The long form of a path (Windows keeps them so: "C:\Users\JOHNDO~1" is "C:\Users\JohnDoe")
function Get-LongPath([string]$path) {
    $b = New-Object Text.StringBuilder 1024
    if ([PendingDeletes.Native]::GetLongPathNameW($path, $b, 1024) -gt 0) { return $b.ToString() }
    $path
}

function Test-Under([string]$path, [string[]]$dirs) {
    $p = $path -replace $script:pendingPrefix, ''
    foreach ($d in $dirs) {
        foreach ($form in $d.TrimEnd('\'), (Get-LongPath $d).TrimEnd('\')) {
            if ($p -eq $form -or $p.StartsWith($form + '\', 'OrdinalIgnoreCase')) { return $true }
        }
    }
    $false
}

function Get-PendingDeletes([string[]]$Under) {
    $key = Open-PendingKey 0x20019   # KEY_READ
    if ($key -eq [UIntPtr]::Zero) { return }
    try { $list = Read-PendingList $key } finally { [void][PendingDeletes.Native]::RegCloseKey($key) }
    for ($i = 0; $i + 1 -lt $list.Count; $i += 2) {
        if ($list[$i + 1] -ne '') { continue }       # (a rename, not a deletion)
        $path = $list[$i] -replace $script:pendingPrefix, ''
        if (-not $Under -or (Test-Under $path $Under)) { $path }
    }
}

function Add-PendingDeletes([string[]]$Under) {
    $n = 0
    foreach ($dir in $Under) {
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        # The files, then the folders from the deepest (a folder goes only once empty)
        $items = @(Get-ChildItem -LiteralPath $dir -Recurse -Force -File -ErrorAction SilentlyContinue) +
                 @(Get-ChildItem -LiteralPath $dir -Recurse -Force -Directory -ErrorAction SilentlyContinue | Sort-Object { $_.FullName.Length } -Descending) +
                 @(Get-Item -LiteralPath $dir -Force)
        # (MOVEFILE_DELAY_UNTIL_REBOOT, to nothing: [NullString], as $null would go as an empty string)
        foreach ($i in $items) { if ([PendingDeletes.Native]::MoveFileExW($i.FullName, [NullString]::Value, 4)) { $n++ } }
    }
    $n
}

function Remove-PendingDeletes([string[]]$Under) {
    $key = Open-PendingKey 0x2001F   # KEY_READ | KEY_WRITE
    if ($key -eq [UIntPtr]::Zero) { return 0 }
    try {
        $list = Read-PendingList $key
        $kept = New-Object Collections.Generic.List[string]; $removed = 0
        for ($i = 0; $i + 1 -lt $list.Count; $i += 2) {
            if ($list[$i + 1] -eq '' -and (Test-Under $list[$i] $Under)) { $removed++; continue }
            $kept.Add($list[$i]); $kept.Add($list[$i + 1])
        }
        if ($removed) {
            if ($kept.Count) {
                $bytes = [Text.Encoding]::Unicode.GetBytes(($kept -join "`0") + "`0`0")
                [void][PendingDeletes.Native]::RegSetValueExW($key, $script:pendingValue, 0, 7, $bytes, $bytes.Length)   # REG_MULTI_SZ
            }
            else { [void][PendingDeletes.Native]::RegDeleteValueW($key, $script:pendingValue) }
        }
        $removed
    }
    finally { [void][PendingDeletes.Native]::RegCloseKey($key) }
}
