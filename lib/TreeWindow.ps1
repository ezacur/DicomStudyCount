# The tree window of explorer4dicoms.ps1 (DICOM > Show tree): dot-sourced by it for -ShowTree, with
# $mainScript, its path, set. Uses its functions (the reports, the quarantine, the comments).

# Window with the folder tree and the comment of every folder next to it. Reads desktop.ini only:
# it neither reads DICOM files nor writes anything.
function Show-DicomTree([string]$root) {
    Add-Type -AssemblyName System.Windows.Forms, System.Drawing
    [void][DicomStudyCount.Native]::SetProcessDPIAware()
    [Windows.Forms.Application]::EnableVisualStyles()

    $form = New-Object Windows.Forms.Form
    $form.Text = "DICOM tree - $root"
    $form.Size = New-Object Drawing.Size(1250, 800)
    $form.StartPosition = 'CenterScreen'
    $form.Font = New-Object Drawing.Font('Segoe UI', 9)
    $form.KeyPreview = $true

    $tree = New-Object Windows.Forms.TreeView
    $tree.Dock = 'Fill'
    $tree.DrawMode = 'OwnerDrawText'
    $tree.HideSelection = $false
    $tree.ShowNodeToolTips = $true
    $tree.ItemHeight = [int]($form.Font.Height * 1.6)

    # Folder icons: the same ones Explorer shows (study colors), at the size of the screen's DPI
    $dpiGraphics = $tree.CreateGraphics()
    $script:iconSize = [int][Math]::Round(16 * $dpiGraphics.DpiX / 96)
    $dpiGraphics.Dispose()
    $images = New-Object Windows.Forms.ImageList
    $images.ColorDepth = 'Depth32Bit'
    $images.ImageSize = New-Object Drawing.Size($script:iconSize, $script:iconSize)
    try { $images.Images.Add('folder', [DicomStudyCount.FolderIcons]::SystemFolder($script:iconSize)) } catch { }
    $tree.ImageList = $images
    $tree.ImageKey = 'folder'
    $tree.SelectedImageKey = 'folder'

    $toolbar = New-Object Windows.Forms.FlowLayoutPanel
    $toolbar.Dock = 'Top'
    $toolbar.AutoSize = $true
    $toolbar.Padding = New-Object Windows.Forms.Padding(4)
    $script:scriptPath = $mainScript                  # (explorer4dicoms.ps1: its Recount runs it)
    $countRoot = { Start-Process conhost.exe -ArgumentList ('--headless powershell.exe -NoProfile -ExecutionPolicy Bypass -File "{0}" -Path "{1}" -WriteComments' -f $script:scriptPath, $root) }
    $expandToStudies = {
        if (-not $tree.Nodes.Count) { return }
        $tree.BeginUpdate()
        Expand-ToStudies $tree.Nodes[0]
        $tree.EndUpdate()
        $tree.Nodes[0].EnsureVisible()
    }
    foreach ($b in @(@('Recount', $countRoot),
                     @('Expand to studies', $expandToStudies),
                     @('Expand all', { $tree.ExpandAll(); if ($tree.Nodes.Count) { $tree.Nodes[0].EnsureVisible() } }),
                     @('Collapse all', { $tree.CollapseAll(); if ($tree.Nodes.Count) { $tree.Nodes[0].Expand() } }),
                     @('Refresh (F5)', { Update-Tree }),
                     @('Problems', { Show-Problems }))) {
        $button = New-Object Windows.Forms.Button
        $button.Text = $b[0]
        $button.AutoSize = $true
        $button.add_Click($b[1])
        $toolbar.Controls.Add($button)
        if ($b[0] -eq 'Problems') { $problemsButton = $button }
    }

    # Updates the Problems button with the number of problems in the latest report
    function Update-ProblemsButton {
        $report = Get-ProblemsReport $root
        $count = if ($report) { $report.Splits.Count + $report.Duplicates.Count + $report.Incomplete.Count + $report.External.Count } else { 0 }
        $problemsButton.Text = if ($count) { "Problems ($count)" } else { 'Problems' }
        $problemsButton.ForeColor = if ($count) { [Drawing.Color]::FromArgb(180, 0, 0) } else { [Drawing.SystemColors]::ControlText }
    }

    # The quarantine of identical duplicates (Invoke-Quarantine / Restore-Quarantine) is implemented but
    # its interface is disabled for now: the Problems window only reports. Set to $true to show the
    # quarantine buttons and the right-click "Keep the copies in this folder".
    $script:quarantineEnabled = $false

    # Folder nodes of a duplicate set: the folder suggested to keep the copies is marked in bold green
    function Update-SetNode($setNode) {
        $set = $setNode.Tag
        $mark = if ($script:quarantineEnabled) { '(keep)' } else { '(suggested keep)' }
        foreach ($child in $setNode.Nodes) {
            if ($child.Tag -eq $set.Keep) {
                $child.Text = "$mark  $($child.Tag)"
                $child.NodeFont = New-Object Drawing.Font($tree.Font, [Drawing.FontStyle]::Bold)
                $child.ForeColor = [Drawing.Color]::FromArgb(0, 120, 40)
            } else {
                $child.Text = $child.Tag
                $child.NodeFont = $null
                $child.ForeColor = [Drawing.Color]::Empty
            }
        }
    }

    $quarantineClick = {
        $report = $script:problemsReport
        $toMove = 0
        foreach ($set in $report.Duplicates) {
            foreach ($g in $set.Groups) { if ($g.Identical) { $toMove += @($g.Files).Count - 1 } }
        }
        if (-not $toMove) {
            [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, 'There are no identical copies to quarantine.', 'Quarantine')
            return
        }
        $message = "Move $toMove identical copies to:`n$(Get-QuarantineBase $root)\<date_time>`n`n" +
                   "One copy of each image stays, in the folders marked (keep).`n" +
                   "Files with the same SOPInstanceUID but different contents are not touched.`n" +
                   "Nothing is deleted: 'Undo last quarantine' moves them back."
        $answer = [Windows.Forms.MessageBox]::Show($script:problemsDialog, $message, 'Quarantine identical duplicates', 'YesNo', 'Warning')
        if ($answer -ne 'Yes') { return }
        $script:problemsDialog.Cursor = 'WaitCursor'
        $r = Invoke-Quarantine $root $report
        $script:problemsDialog.Cursor = 'Default'
        [void][Windows.Forms.MessageBox]::Show($script:problemsDialog,
            ("Moved: {0}`nSkipped (changed since the count, or outside this folder): {1}`nFailed: {2}`n`nQuarantine folder:`n{3}`n`nThe folder is counted again now to update the comments." -f $r.Moved, $r.Skipped, $r.Failed, $r.Batch),
            'Quarantine')
        $script:problemsDialog.Close()
        & $countRoot
    }

    $undoClick = {
        $last = Get-LastQuarantine $root
        if (-not $last) {
            [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, 'There is no quarantine to undo for this folder.', 'Undo')
            return
        }
        $answer = [Windows.Forms.MessageBox]::Show($script:problemsDialog,
            "Move back the files quarantined in:`n$($last.FullName)?", 'Undo last quarantine', 'YesNo', 'Question')
        if ($answer -ne 'Yes') { return }
        $r = Restore-Quarantine $last.FullName
        [void][Windows.Forms.MessageBox]::Show($script:problemsDialog,
            ("Restored: {0}`nSkipped (already back or missing): {1}`n`nThe folder is counted again now to update the comments." -f $r.Restored, $r.Skipped),
            'Undo last quarantine')
        $script:problemsDialog.Close()
        & $countRoot
    }

    # Window with the latest problems report: studies split across folders and duplicated files,
    # with the quarantine of identical copies. Clicking a folder selects it in the tree,
    # double-clicking opens it in Explorer, right-clicking a folder of a duplicate set chooses it as
    # the one that keeps the copies. (Modeless: what its events need is kept in script variables.)
    function Show-Problems {
        $report = Get-ProblemsReport $root
        if (-not $report) {
            [void][Windows.Forms.MessageBox]::Show($form, 'There is no report for this folder yet: click "Recount" first.', 'Problems')
            return
        }
        if ($script:problemsDialog -and -not $script:problemsDialog.IsDisposed) { $script:problemsDialog.Close() }
        $script:problemsReport = $report

        $dialog = New-Object Windows.Forms.Form
        $dialog.Text = "Problems - $root  (counted $($report.ScannedAt -replace 'T', ' '))"
        $dialog.Size = New-Object Drawing.Size(1150, 700)
        $dialog.StartPosition = 'CenterParent'
        $dialog.Font = $form.Font
        $script:problemsDialog = $dialog

        $bar = New-Object Windows.Forms.FlowLayoutPanel
        $bar.Dock = 'Top'
        $bar.AutoSize = $true
        $bar.Padding = New-Object Windows.Forms.Padding(4)
        foreach ($b in @(@('Quarantine identical duplicates...', $quarantineClick),
                         @('Undo last quarantine', $undoClick),
                         @('Open quarantine folder', {
                             $base = Get-QuarantineBase $root
                             if (Test-Path -LiteralPath $base) { Start-Process explorer.exe ('"{0}"' -f $base) }
                             else { [void][Windows.Forms.MessageBox]::Show($script:problemsDialog, "Nothing quarantined yet.`n($base)", 'Quarantine') }
                         }))) {
            $button = New-Object Windows.Forms.Button
            $button.Text = $b[0]
            $button.AutoSize = $true
            $button.add_Click($b[1])
            $bar.Controls.Add($button)
        }
        $bar.Visible = $script:quarantineEnabled

        $list = New-Object Windows.Forms.TreeView
        $list.Dock = 'Fill'
        $list.HideSelection = $false
        $list.ItemHeight = $tree.ItemHeight

        $splits = $list.Nodes.Add("Studies split across folders ($($report.Splits.Count))")
        foreach ($s in $report.Splits) {
            $sn = $splits.Nodes.Add("[$($s.Tag)] $($s.StudyInstanceUID)  -  in $(@($s.Folders).Count) folders")
            foreach ($folder in $s.Folders) { $fn = $sn.Nodes.Add($folder); $fn.Tag = $folder }
        }
        $dupFiles = [int]($report.Duplicates | Measure-Object -Property Files -Sum).Sum
        $dupsLabel = "Duplicated files, same StudyInstanceUID and SOPInstanceUID ($dupFiles files in $($report.Duplicates.Count) folder sets)"
        if ($script:quarantineEnabled) { $dupsLabel += '  -  right-click a folder to keep the copies there' }
        $dups = $list.Nodes.Add($dupsLabel)
        foreach ($d in $report.Duplicates) {
            $where = if (@($d.Folders).Count -eq 1) { 'inside one folder' } else { "shared by $(@($d.Folders).Count) folders" }
            $dn = $dups.Nodes.Add("$($d.Files) files $where  -  $($d.Identical) byte-identical, $($d.Different) with different contents")
            if ($d.Different) { $dn.ForeColor = [Drawing.Color]::FromArgb(180, 0, 0) }
            $dn.Tag = $d
            foreach ($folder in $d.Folders) { $fn = $dn.Nodes.Add($folder); $fn.Tag = $folder }
            Update-SetNode $dn
        }
        $incomplete = $list.Nodes.Add("Incomplete series ($($report.Incomplete.Count))  -  InstanceNumbers missing in a folder, compared with the whole tree")
        foreach ($s in $report.Incomplete) {
            $sn = $incomplete.Nodes.Add("[$($s.Tag)] $($s.Series)  -  $($s.Have) of $($s.Expected), missing $($s.Missing)")
            $fn = $sn.Nodes.Add($s.Folder); $fn.Tag = $s.Folder
        }
        $externalFiles = [int]($report.External | Measure-Object -Property Files -Sum).Sum
        $external = $list.Nodes.Add("Also in other counted locations ($externalFiles files in $($report.External.Count) folder pairs)")
        foreach ($x in $report.External) {
            $xn = $external.Nodes.Add("$($x.Files) files  -  $($x.Identical) byte-identical, $($x.Different) with different contents")
            if ($x.Different) { $xn.ForeColor = [Drawing.Color]::FromArgb(180, 0, 0) }
            $fn = $xn.Nodes.Add($x.Folder); $fn.Tag = $x.Folder
            $on = $xn.Nodes.Add("elsewhere:  $($x.OtherFolder)"); $on.Tag = $x.OtherFolder
        }
        $splits.ExpandAll()
        $dups.ExpandAll()
        $incomplete.ExpandAll()
        $external.ExpandAll()
        if ($list.Nodes.Count) { $list.Nodes[0].EnsureVisible() }

        $menu = New-Object Windows.Forms.ContextMenuStrip
        $keepItem = $menu.Items.Add('Keep the copies in this folder')
        $keepItem.add_Click({
            $node = $script:menuNode
            $node.Parent.Tag.Keep = $node.Tag
            Update-SetNode $node.Parent
        })
        $list.add_NodeMouseClick({
            param($sender, $e)
            if ($e.Button -ne 'Right' -or -not $script:quarantineEnabled) { return }
            $sender.SelectedNode = $e.Node
            # Only folders of a duplicate set (their parent holds the set, which has Groups)
            if ($e.Node.Tag -is [string] -and $e.Node.Parent -and $e.Node.Parent.Tag -and $e.Node.Parent.Tag.PSObject.Properties['Groups']) {
                $script:menuNode = $e.Node
                $script:problemsMenu.Show($sender, $e.Location)
            }
        })
        $script:problemsMenu = $menu
        $list.add_AfterSelect({
            param($sender, $e)
            $node = if ($e.Node.Tag -is [string]) { $script:nodeByPath[$e.Node.Tag] } else { $null }
            if ($node) { $tree.SelectedNode = $node; $node.EnsureVisible() }
        })
        $list.add_NodeMouseDoubleClick({
            param($sender, $e)
            if ($e.Node.Tag -is [string]) { Start-Process explorer.exe ('"{0}"' -f $e.Node.Tag) }
        })
        $dialog.Controls.Add($list)
        $dialog.Controls.Add($bar)
        $dialog.Show($form)
    }

    $statusStrip = New-Object Windows.Forms.StatusStrip
    $statusLabel = New-Object Windows.Forms.ToolStripStatusLabel
    [void]$statusStrip.Items.Add($statusLabel)

    $form.Controls.Add($tree)
    $form.Controls.Add($toolbar)
    $form.Controls.Add($statusStrip)

    # Natural order: "2" before "10"
    $naturalKey = { [regex]::Replace([IO.Path]::GetFileName($_), '\d+', { $args[0].Value.PadLeft(12, '0') }) }

    function Get-FolderComment([string]$dir) {
        $ini = [IO.Path]::Combine($dir, 'desktop.ini')
        if (-not [IO.File]::Exists($ini)) { return '' }
        $text = [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo', 'InfoTip')
        # Non-ASCII text (such as the warning sign) is also stored UTF-7 encoded in .ShellClassInfo.W,
        # valid only while the .ShellClassInfo.A copy matches the main text (same rule as Windows)
        $unicode = [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo.W', 'InfoTip')
        if ($unicode -and [DicomStudyCount.Native]::ReadIni($ini, '.ShellClassInfo.A', 'InfoTip') -eq $text) {
            return [Text.Encoding]::UTF7.GetString([Text.Encoding]::ASCII.GetBytes($unicode))
        }
        $text
    }

    # Shows the folder icon written by the count (rebuilt if it was lost), or the plain folder
    function Set-NodeIcon($node) {
        $key = 'folder'
        try {
            $icon = [DicomStudyCount.FolderIcons]::OfFolder($node.Tag.Path)
            if ($icon) {
                if (-not $images.Images.ContainsKey($icon)) { $images.Images.Add($icon, [DicomStudyCount.FolderIcons]::Load($icon, $script:iconSize)) }
                $key = $icon
            }
        }
        catch { }
        if ($node.ImageKey -ne $key) {
            $node.ImageKey = $key
            $node.SelectedImageKey = $key
        }
    }

    # Files directly in the folder (the desktop.ini written by this script does not count)
    function Get-DirectFileCount([string]$dir) {
        try { $n = [IO.Directory]::GetFiles($dir).Length } catch { return 0 }
        if ([IO.File]::Exists([IO.Path]::Combine($dir, 'desktop.ini'))) { $n-- }
        $n
    }

    # Adds the folder and its whole subtree and returns its node; $index inserts it at a
    # position instead of at the end. TotalFiles counts the files of the whole subtree.
    function Add-FolderNode($nodes, [string]$dir, [string]$name, [int]$level, [int]$index = -1) {
        $comment = Get-FolderComment $dir
        $direct = Get-DirectFileCount $dir
        $node = New-Object Windows.Forms.TreeNode($name)
        if ($index -ge 0) { $nodes.Insert($index, $node) } else { [void]$nodes.Add($node) }
        $node.Tag = [pscustomobject]@{ Path = $dir; Comment = $comment; StudyUid = (Get-StudyUid $comment); DirectFiles = $direct; TotalFiles = $direct }
        $node.ToolTipText = $comment
        Set-NodeIcon $node
        $script:nodeByPath[$dir] = $node
        $width = ($level + 1) * $tree.Indent + $script:iconSize + 4 + [Windows.Forms.TextRenderer]::MeasureText($name, $tree.Font).Width
        if ($width -gt $script:nameWidth) { $script:nameWidth = $width }
        $subs = @()
        try { $subs = [IO.Directory]::GetDirectories($dir) | Sort-Object $naturalKey } catch { }
        foreach ($sub in $subs) {
            $child = Add-FolderNode $node.Nodes $sub ([IO.Path]::GetFileName($sub)) ($level + 1)
            $node.Tag.TotalFiles += $child.Tag.TotalFiles
        }
        $node
    }

    function Remove-FromIndex($node) {
        $script:nodeByPath.Remove($node.Tag.Path)
        foreach ($child in $node.Nodes) { Remove-FromIndex $child }
    }

    function Repaint-Row($node) {
        if ($node.IsVisible) {
            $b = $node.Bounds
            $tree.Invalidate((New-Object Drawing.Rectangle(0, $b.Y, $tree.ClientSize.Width, $b.Height)))
        }
    }

    # Adds a change in the number of files to the folder and all its ancestors
    function Add-FileDelta($node, [int]$delta) {
        if ($delta -eq 0) { return }
        for ($n = $node; $n; $n = $n.Parent) {
            $n.Tag.TotalFiles += $delta
            Repaint-Row $n
        }
    }

    # Makes the children of a node match the subfolders currently on disk (same natural order)
    function Sync-Children($node) {
        $subs = @()
        try { $subs = @([IO.Directory]::GetDirectories($node.Tag.Path) | Sort-Object $naturalKey) } catch { }
        $onDisk = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        foreach ($s in $subs) { [void]$onDisk.Add($s) }
        foreach ($child in @($node.Nodes)) {
            if (-not $onDisk.Contains($child.Tag.Path)) {
                Remove-FromIndex $child
                $child.Remove()
                Add-FileDelta $node (-$child.Tag.TotalFiles)
            }
        }
        for ($i = 0; $i -lt $subs.Count; $i++) {
            if ($i -lt $node.Nodes.Count -and $node.Nodes[$i].Tag.Path -eq $subs[$i]) { continue }
            $added = Add-FolderNode $node.Nodes $subs[$i] ([IO.Path]::GetFileName($subs[$i])) ($node.Level + 1) $i
            Add-FileDelta $node $added.Tag.TotalFiles
        }
    }

    # Single-study folders have comments starting with their study tag, "[CT-xxxxx] dd/MMM/yyyy ...";
    # the tag identifies the study in the tree (colors, "Expand to studies")
    function Get-StudyUid([string]$comment) {
        # 6 characters; 5 in the comments written by earlier versions, until counted again
        if ($comment -cmatch '^\[([A-Z0-9]+-[0-9A-Za-z]{5,6})\] ') { return $Matches[1] }
        $null
    }

    # Soft pastel color for the i-th study: the hue advances by the golden angle, so consecutive
    # studies always get clearly different hues; lightness is high enough for black text.
    function Get-PastelColor([int]$index) {
        $h = (($index * 137.508) % 360) / 360.0
        $s = 0.70; $l = 0.86
        $q = $l + $s - $l * $s
        $p = 2 * $l - $q
        $rgb = foreach ($t in ($h + 1 / 3), $h, ($h - 1 / 3)) {
            if ($t -lt 0) { $t += 1 }
            if ($t -gt 1) { $t -= 1 }
            $v = if ($t -lt 1 / 6) { $p + ($q - $p) * 6 * $t }
                 elseif ($t -lt 1 / 2) { $q }
                 elseif ($t -lt 2 / 3) { $p + ($q - $p) * (2 / 3 - $t) * 6 }
                 else { $p }
            [int][Math]::Round($v * 255)
        }
        [Drawing.Color]::FromArgb($rgb[0], $rgb[1], $rgb[2])
    }

    # Assigns a color to every study in tree order (pre-order, natural sort), so the same folder
    # always gets the same colors; repaints only if the assignment changed
    function Update-StudyColors {
        $order = New-Object Collections.Generic.List[string]
        $seen = New-Object 'System.Collections.Generic.HashSet[string]'
        $stack = New-Object System.Collections.Stack
        for ($i = $tree.Nodes.Count - 1; $i -ge 0; $i--) { $stack.Push($tree.Nodes[$i]) }
        while ($stack.Count) {
            $n = $stack.Pop()
            $uid = $n.Tag.StudyUid
            if ($uid -and $seen.Add($uid)) { $order.Add($uid) }
            for ($i = $n.Nodes.Count - 1; $i -ge 0; $i--) { $stack.Push($n.Nodes[$i]) }
        }
        $current = if ($script:studyOrder) { $script:studyOrder -join '|' } else { '' }
        if (($order -join '|') -eq $current) { return }
        $script:studyOrder = $order.ToArray()
        if ($script:studyBrushes) { foreach ($b in $script:studyBrushes.Values) { $b.Dispose() } }
        $script:studyBrushes = New-Object Collections.Hashtable     # case-sensitive: tags differ by case
        for ($i = 0; $i -lt $order.Count; $i++) {
            $script:studyBrushes[$order[$i]] = New-Object Drawing.SolidBrush (Get-PastelColor $i)
        }
        $tree.Invalidate()
    }

    # Expands folders holding several studies and stops at the first single-study level;
    # folders without DICOM stay collapsed
    function Expand-ToStudies($node) {
        if ($node.Tag.StudyUid -or $node.Tag.Comment -notmatch '^\d+ DICOM studies') {
            $node.Collapse()
            return
        }
        $node.Expand()
        foreach ($child in $node.Nodes) { Expand-ToStudies $child }
    }

    function Update-Status {
        $commented = 0
        foreach ($n in $script:nodeByPath.Values) { if ($n.Tag.Comment) { $commented++ } }
        $rootComment = if ($tree.Nodes.Count) { $tree.Nodes[0].Tag.Comment } else { '' }
        $files = if ($tree.Nodes.Count) { '{0:N0} files  -  ' -f $tree.Nodes[0].Tag.TotalFiles } else { '' }
        if ($script:studyOrder -and $script:studyOrder.Count) { $files += '{0} studies  -  ' -f $script:studyOrder.Count }
        $statusLabel.Text = $files + $(if ($rootComment -like 'Counting*') {
            "Counting... the tree updates as each folder is finished"
        } elseif ($commented) {
            "$commented folders with comments  -  double-click: open in Explorer  -  F5: refresh"
        } else {
            'No comments found: click "Recount" to count this folder'
        })
    }

    # Rebuilds the tree keeping the expanded folders and the selection
    function Update-Tree {
        $expanded = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        $stack = New-Object System.Collections.Stack
        foreach ($n in $tree.Nodes) { $stack.Push($n) }
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($n.IsExpanded) { [void]$expanded.Add($n.Tag.Path) }
            foreach ($c in $n.Nodes) { $stack.Push($c) }
        }
        $selected = if ($tree.SelectedNode) { $tree.SelectedNode.Tag.Path } else { $null }

        $form.Cursor = 'WaitCursor'
        $tree.BeginUpdate()
        $tree.Nodes.Clear()
        $script:nodeByPath = New-Object Collections.Hashtable ([StringComparer]::OrdinalIgnoreCase)
        $script:nameWidth = 0
        [void](Add-FolderNode $tree.Nodes $root $root 0)
        # Common columns right after the widest folder name: number of files, then the comment
        $script:filesX = [Math]::Min($script:nameWidth + 30, 560)
        $script:filesW = [Windows.Forms.TextRenderer]::MeasureText('9,999,999 files', $tree.Font).Width
        $script:commentX = $script:filesX + $script:filesW + 24

        $stack.Clear()
        foreach ($n in $tree.Nodes) { $stack.Push($n) }
        while ($stack.Count) {
            $n = $stack.Pop()
            if ($expanded.Contains($n.Tag.Path)) { $n.Expand() }
            if ($selected -and $n.Tag.Path -eq $selected) { $tree.SelectedNode = $n }
            foreach ($c in $n.Nodes) { $stack.Push($c) }
        }
        if ($expanded.Count -eq 0) { $tree.Nodes[0].Expand() }
        $tree.EndUpdate()
        $form.Cursor = 'Default'
        Update-StudyColors
        Update-Status
        Update-ProblemsButton
    }

    $tree.add_DrawNode({
        param($sender, $e)
        if ($e.Bounds.IsEmpty) { return }
        $selected = ($e.State -band [Windows.Forms.TreeNodeStates]::Selected) -ne 0
        $flags = [Windows.Forms.TextFormatFlags]'VerticalCenter, Left, NoPrefix, SingleLine'
        $nameColor = $tree.ForeColor
        # Single-study rows: pastel background with the color of their study, from the name to the edge
        $uid = $e.Node.Tag.StudyUid
        $studyBrush = if ($uid -and $script:studyBrushes) { $script:studyBrushes[$uid] } else { $null }
        if ($studyBrush) {
            $e.Graphics.FillRectangle($studyBrush, $e.Bounds.X, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $e.Bounds.X), $e.Bounds.Height)
        }
        if ($selected) {
            $e.Graphics.FillRectangle([Drawing.SystemBrushes]::Highlight, $e.Bounds)
            $nameColor = [Drawing.SystemColors]::HighlightText
        }
        [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $e.Node.Text, $tree.Font, $e.Bounds, $nameColor, $flags)

        # Number of files, right-aligned in its own column
        $total = $e.Node.Tag.TotalFiles
        $filesText = if ($total -eq 1) { '1 file' } else { '{0:N0} files' -f $total }
        $filesRect = New-Object Drawing.Rectangle([Math]::Max($script:filesX, $e.Bounds.Right + 8), $e.Bounds.Y, $script:filesW, $e.Bounds.Height)
        $filesFlags = [Windows.Forms.TextFormatFlags]'VerticalCenter, Right, NoPrefix, SingleLine'
        $filesColor = if ($studyBrush) { [Drawing.Color]::FromArgb(60, 60, 60) } else { [Drawing.Color]::DimGray }
        [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $filesText, $tree.Font, $filesRect, $filesColor, $filesFlags)

        $comment = $e.Node.Tag.Comment
        if ($comment) {
            $x = [Math]::Max($script:commentX, $filesRect.Right + 24)
            # Warnings ("also in", "duplicated") are drawn in red: a red mark first, so it is visible
            # even when the text does not fit, and the warning text after the comment
            $warnAt = $comment.IndexOf([char]0x26A0)
            $main = if ($warnAt -gt 0) { $comment.Substring(0, $warnAt).TrimEnd() } else { $comment }
            $red = [Drawing.Color]::FromArgb(190, 0, 0)
            if ($warnAt -gt 0) {
                $markRect = New-Object Drawing.Rectangle(($x - 20), $e.Bounds.Y, 20, $e.Bounds.Height)
                [Windows.Forms.TextRenderer]::DrawText($e.Graphics, [string][char]0x26A0, $tree.Font, $markRect, $red, $flags)
            }
            $rect = New-Object Drawing.Rectangle($x, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $x), $e.Bounds.Height)
            $color = if ($studyBrush) { [Drawing.Color]::Black }
                     elseif ($comment -like 'Counting*' -or $comment -like '(0 DICOMs*') { [Drawing.Color]::Gray }
                     else { [Drawing.Color]::FromArgb(0, 95, 170) }
            [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $main, $tree.Font, $rect, $color, $flags -bor [Windows.Forms.TextFormatFlags]::EndEllipsis)
            if ($warnAt -gt 0) {
                $mainWidth = [Windows.Forms.TextRenderer]::MeasureText($e.Graphics, $main, $tree.Font, $rect.Size, $flags).Width
                $wx = $x + $mainWidth + 6
                $warnRect = New-Object Drawing.Rectangle($wx, $e.Bounds.Y, [Math]::Max(0, $tree.ClientSize.Width - $wx), $e.Bounds.Height)
                [Windows.Forms.TextRenderer]::DrawText($e.Graphics, $comment.Substring($warnAt), $tree.Font, $warnRect, $red, $flags -bor [Windows.Forms.TextFormatFlags]::EndEllipsis)
            }
        }
    })
    $tree.add_NodeMouseDoubleClick({
        param($sender, $e)
        Start-Process explorer.exe ('"{0}"' -f $e.Node.Tag.Path)
    })
    $form.add_KeyDown({
        param($sender, $e)
        if ($e.KeyCode -eq 'F5') { Update-Tree; $e.Handled = $true }
    })
    $form.add_Shown({ $form.Activate() })

    # Live updates: a count writes desktop.ini folder by folder, and folders can be created,
    # deleted or renamed meanwhile. The watchers only collect what changed (SynchronizingObject
    # delivers their events on the window thread, as PowerShell requires); the timer applies it
    # in batches, since each comment is written in several steps, and repaints only those rows.
    $script:dirty = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:dirtyParents = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:dirtyFiles = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    $script:needsRebuild = $false

    # Files created, deleted or renamed: that folder is recounted
    $fileWatcher = New-Object IO.FileSystemWatcher($root)
    $fileWatcher.IncludeSubdirectories = $true
    $fileWatcher.NotifyFilter = [IO.NotifyFilters]::FileName
    $fileWatcher.InternalBufferSize = 65536
    $fileWatcher.SynchronizingObject = $form
    $onFileChange = {
        param($sender, $e)
        if ([IO.Path]::GetFileName($e.FullPath) -ne 'desktop.ini') {
            [void]$script:dirtyFiles.Add([IO.Path]::GetDirectoryName($e.FullPath))
        }
    }
    $fileWatcher.add_Created($onFileChange)
    $fileWatcher.add_Deleted($onFileChange)
    $fileWatcher.add_Renamed($onFileChange)
    $fileWatcher.add_Error({ $script:needsRebuild = $true })

    $dirWatcher = New-Object IO.FileSystemWatcher($root)
    $dirWatcher.IncludeSubdirectories = $true
    $dirWatcher.NotifyFilter = [IO.NotifyFilters]::DirectoryName
    $dirWatcher.InternalBufferSize = 65536
    $dirWatcher.SynchronizingObject = $form
    $onDirChange = {
        param($sender, $e)
        [void]$script:dirtyParents.Add([IO.Path]::GetDirectoryName($e.FullPath))
        if ($e -is [IO.RenamedEventArgs]) { [void]$script:dirtyParents.Add([IO.Path]::GetDirectoryName($e.OldFullPath)) }
    }
    $dirWatcher.add_Created($onDirChange)
    $dirWatcher.add_Deleted($onDirChange)
    $dirWatcher.add_Renamed($onDirChange)
    # Buffer overflow (too many changes at once): rebuild the whole tree
    $dirWatcher.add_Error({ $script:needsRebuild = $true })

    $watcher = New-Object IO.FileSystemWatcher($root, 'desktop.ini')
    $watcher.IncludeSubdirectories = $true
    $watcher.NotifyFilter = [IO.NotifyFilters]'FileName, LastWrite, Size'
    $watcher.InternalBufferSize = 65536
    $watcher.SynchronizingObject = $form
    $onChange = { param($sender, $e) [void]$script:dirty.Add([IO.Path]::GetDirectoryName($e.FullPath)) }
    $watcher.add_Changed($onChange)
    $watcher.add_Created($onChange)
    $watcher.add_Deleted($onChange)
    $watcher.add_Renamed($onChange)
    # Buffer overflow (too many changes at once): re-read every folder
    $watcher.add_Error({ foreach ($p in @($script:nodeByPath.Keys)) { [void]$script:dirty.Add($p) } })

    $timer = New-Object Windows.Forms.Timer
    $timer.Interval = 300
    $timer.add_Tick({
        if ($script:problemsCheckAt -and (Get-Date) -gt $script:problemsCheckAt) {
            $script:problemsCheckAt = $null
            Update-ProblemsButton
        }
        if ($script:needsRebuild) {
            $script:needsRebuild = $false
            $script:dirtyParents.Clear()
            $script:dirtyFiles.Clear()
            Update-Tree
        }
        if ($script:dirtyParents.Count) {
            # Parents first, so a new folder inside another new folder already has its place
            $parents = @($script:dirtyParents) | Sort-Object Length
            $script:dirtyParents.Clear()
            $tree.BeginUpdate()
            foreach ($p in $parents) {
                $node = $script:nodeByPath[$p]
                if ($node) { Sync-Children $node }
            }
            $tree.EndUpdate()
            Update-StudyColors
            Update-Status
        }
        if ($script:dirtyFiles.Count) {
            $dirs = @($script:dirtyFiles)
            $script:dirtyFiles.Clear()
            foreach ($d in $dirs) {
                $node = $script:nodeByPath[$d]
                if (-not $node) { continue }     # new folder: counted when its node is added
                $direct = Get-DirectFileCount $d
                $delta = $direct - $node.Tag.DirectFiles
                $node.Tag.DirectFiles = $direct
                Add-FileDelta $node $delta
            }
            Update-Status
        }
        if ($script:dirty.Count -eq 0) { return }
        $changed = @($script:dirty)
        $script:dirty.Clear()
        foreach ($dir in $changed) {
            $node = $script:nodeByPath[$dir]
            if (-not $node) { continue }
            $comment = Get-FolderComment $dir
            if ($comment -ne $node.Tag.Comment) {
                $node.Tag.Comment = $comment
                $node.Tag.StudyUid = Get-StudyUid $comment
                $node.ToolTipText = $comment
                Repaint-Row $node
            }
            Set-NodeIcon $node
        }
        Update-StudyColors
        Update-Status
        # The root is the last folder written by a count; its report is saved right after,
        # so the Problems button is refreshed a moment later
        if ($changed -contains $root) { $script:problemsCheckAt = (Get-Date).AddSeconds(2) }
    })

    Update-Tree
    $watcher.EnableRaisingEvents = $true
    $dirWatcher.EnableRaisingEvents = $true
    $fileWatcher.EnableRaisingEvents = $true
    $timer.Start()
    [void]$form.ShowDialog()
    $timer.Stop()
    $watcher.Dispose()
    $dirWatcher.Dispose()
    $fileWatcher.Dispose()
}
