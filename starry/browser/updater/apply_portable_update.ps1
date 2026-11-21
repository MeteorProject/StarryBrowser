# Copyright 2026 The Starry Browser Authors

# portable.zip stores the runtime in a versioned directory. Switch the main
# launcher last, keeping its old runtime available until the new one is ready.
$ErrorActionPreference = 'Stop'
function Initialize-PortableFileOperations {
  Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using Microsoft.Win32.SafeHandles;
public static class PortableFiles {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileEx(string source, string destination, uint flags);

    public static void PublishFile(string source, string destination) {
        // Same-volume rename with REPLACE_EXISTING | WRITE_THROUGH.
        const uint replaceExisting = 1;
        const uint writeThrough = 8;
        RetryLockedFiles(delegate {
            if (!MoveFileEx(source, destination, replaceExisting | writeThrough))
                throw new Win32Exception();
        });
    }

    [DllImport("KtmW32.dll", SetLastError = true)]
    static extern SafeFileHandle CreateTransaction(IntPtr attributes, IntPtr id,
        uint options, uint level, uint flags, uint timeout, IntPtr description);
    [DllImport("KtmW32.dll", SetLastError = true)]
    static extern bool CommitTransaction(SafeFileHandle transaction);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool MoveFileTransacted(string source, string destination,
        IntPtr progress, IntPtr data, uint flags, SafeFileHandle transaction);

    static void MoveInTransaction(SafeFileHandle tx, string source,
                                  string destination, uint flags) {
        if (!MoveFileTransacted(source, destination, IntPtr.Zero, IntPtr.Zero, flags, tx))
            throw new Win32Exception();
    }

    public static void PublishTransaction(string runtime, string installed,
                                          string backup, string[] launchers) {
        RetryLockedFiles(delegate { SwitchRuntime(runtime, installed, backup, launchers); });
    }

    static void RetryLockedFiles(Action publish) {
        for (int attempt = 1; ; ++attempt) {
            try {
                publish();
                return;
            } catch (Win32Exception error) {
                // Access denied, sharing violation, or lock violation while a
                // child process or a virus scanner still holds the old image.
                int code = error.NativeErrorCode;
                if (attempt == 10 || (code != 5 && code != 32 && code != 33))
                    throw;
                Thread.Sleep(500);
            }
        }
    }

    static void SwitchRuntime(string runtime, string installed, string backup,
                              string[] launchers) {
        using (var tx = CreateTransaction(IntPtr.Zero, IntPtr.Zero, 0, 0, 0, 0,
                                          IntPtr.Zero)) {
            if (tx.IsInvalid)
                throw new Win32Exception();
            MoveInTransaction(tx, installed, backup, 0);
            MoveInTransaction(tx, runtime, installed, 0);
            string targetDir = Path.GetDirectoryName(installed);
            foreach (string launcher in launchers) {
                string target = Path.Combine(targetDir, Path.GetFileName(launcher));
                MoveInTransaction(tx, launcher, target, 1); // REPLACE_EXISTING
            }
            if (!CommitTransaction(tx))
                throw new Win32Exception();
        }
    }
}
'@
}

function Get-BinaryVersion($path) {
  $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
  return [version]::new($info.FileMajorPart, $info.FileMinorPart,
                        $info.FileBuildPart, $info.FilePrivatePart)
}

function Publish-Portable($work) {
  $exeName = [IO.Path]::GetFileName($targetExe)
  $newExe = [IO.Path]::Combine($work, $exeName)
  $oldVersion = Get-BinaryVersion $targetExe
  $newVersion = Get-BinaryVersion $newExe
  if ($newVersion -lt $oldVersion) { throw 'A newer version is installed' }
  $versionName = $newVersion.ToString()
  $runtime = [IO.Path]::Combine($work, $versionName)
  $installed = [IO.Path]::Combine($targetDir, $versionName)
  $dll = [IO.Path]::ChangeExtension($exeName, 'dll')
  if (![IO.File]::Exists([IO.Path]::Combine($runtime, $dll))) {
    throw 'Missing portable runtime'
  }

  $files = @([IO.Directory]::GetFiles($work) | Where-Object { $_ -ne $newExe })
  $files += $newExe
  if ($newVersion -eq $oldVersion) {
    # Prereleases can share a Windows assembly version. Both the runtime and
    # its launchers must switch together. Closing an uncommitted transaction
    # rolls it back, including when the helper process is killed.
    $backup = [IO.Path]::Combine($work, 'previous-runtime')
    [PortableFiles]::PublishTransaction($runtime, $installed, $backup, $files)
    return
  }

  if ([IO.Directory]::Exists($installed)) {
    # A previous helper may have stopped after publishing this directory.
    # Reuse it only when its files match the freshly staged runtime.
    $expectedFiles = [IO.Directory]::GetFiles($runtime, '*', 'AllDirectories')
    if ([IO.Directory]::GetFiles($installed, '*', 'AllDirectories').Length -ne
        $expectedFiles.Length) { throw 'Runtime mismatch' }
    foreach ($file in $expectedFiles) {
      $existing = $installed + $file.Substring($runtime.Length)
      if ((Get-FileHash -LiteralPath $existing).Hash -ne
          (Get-FileHash -LiteralPath $file).Hash) { throw 'Runtime mismatch' }
    }
  } else {
    [IO.Directory]::Move($runtime, $installed)
  }
  foreach ($file in $files) {
    $destination = [IO.Path]::Combine($targetDir, [IO.Path]::GetFileName($file))
    [PortableFiles]::PublishFile($file, $destination)
  }
}

$lock = $null
$work = [IO.Path]::Combine($targetDir, '.starry-update-' + [guid]::NewGuid())
try {
  if (Get-Process -Id $parentPid -ErrorAction SilentlyContinue) {
    throw 'Browser is still running'
  }
  $lock = [IO.FileStream]::new([IO.Path]::Combine($targetDir, '.starry-update.lock'),
      'OpenOrCreate', 'ReadWrite', 'None', 4096, 'DeleteOnClose')
  Initialize-PortableFileOperations
  Copy-Item -LiteralPath $staged -Destination $work -Recurse -Force
  Publish-Portable $work
  Remove-Item -LiteralPath $stagingDir -Recurse -Force -ErrorAction SilentlyContinue
} catch {
  $relaunch = $false
  Write-Warning $_
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
  if ($lock) { $lock.Dispose() }
}
