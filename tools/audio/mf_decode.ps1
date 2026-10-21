# Decode any song Windows can play (MP3, M4A / AAC, FLAC, WMA, WAV ...) to a 16-bit PCM WAV with Windows' own Media
# Foundation transcoder (WinRT MediaTranscoder, in every Windows 10 / 11 with Windows PowerShell 5.1: nothing to install).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File mf_decode.ps1 <in> <out.wav> [encode]
#   encode = mp3 | m4a | flac | wma : the other way round (WAV -> that format; the self-tests make their sample songs so).
# Exit 0 = written; exit 2 = Windows cannot read this file (message on stderr); 1 = other error.
param([Parameter(Mandatory = $true)][string]$In, [Parameter(Mandatory = $true)][string]$Out, [string]$Encode = '')
$ErrorActionPreference = 'Stop'
try {
  Add-Type -AssemblyName System.Runtime.WindowsRuntime
  $null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
  $null = [Windows.Storage.StorageFolder, Windows.Storage, ContentType = WindowsRuntime]
  $null = [Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime]
  $null = [Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime]
  $ext = [System.WindowsRuntimeSystemExtensions].GetMethods()
  $opT = ($ext | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
  $actP = ($ext | Where-Object { $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncActionWithProgress`1' })[0]
  function Await($op, [type]$t) { $task = $opT.MakeGenericMethod($t).Invoke($null, @($op)); $null = $task.Wait(-1); $task.Result }
  function AwaitProgress($op) { $task = $actP.MakeGenericMethod([double]).Invoke($null, @($op)); $null = $task.Wait(-1) }

  $inPath = [System.IO.Path]::GetFullPath($In); $outPath = [System.IO.Path]::GetFullPath($Out)
  $src = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($inPath)) ([Windows.Storage.StorageFile])
  $dir = Await ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync([System.IO.Path]::GetDirectoryName($outPath))) ([Windows.Storage.StorageFolder])
  $dst = Await ($dir.CreateFileAsync([System.IO.Path]::GetFileName($outPath), [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])
  $q = [Windows.Media.MediaProperties.AudioEncodingQuality]::High
  $P = [Windows.Media.MediaProperties.MediaEncodingProfile]
  $prof = switch ($Encode.ToLower()) { 'mp3' { $P::CreateMp3($q) } 'm4a' { $P::CreateM4a($q) } 'flac' { $P::CreateFlac($q) } 'wma' { $P::CreateWma($q) } default { $P::CreateWav($q) } }
  if (-not $Encode) { $prof.Audio.BitsPerSample = 16 }   # keep the source's rate / channels where Windows allows; 16-bit PCM
  $tc = New-Object Windows.Media.Transcoding.MediaTranscoder
  $prep = Await ($tc.PrepareFileTranscodeAsync($src, $dst, $prof)) ([Windows.Media.Transcoding.PrepareTranscodeResult])
  if (-not $prep.CanTranscode) {
    Remove-Item -LiteralPath $outPath -ErrorAction SilentlyContinue
    [Console]::Error.WriteLine("Windows cannot read this file ($($prep.FailureReason))"); exit 2
  }
  AwaitProgress ($prep.TranscodeAsync())
  exit 0
} catch {
  Remove-Item -LiteralPath $Out -ErrorAction SilentlyContinue
  $e = $_.Exception; while ($e.InnerException) { $e = $e.InnerException }
  [Console]::Error.WriteLine("mf_decode: $($e.Message)"); exit 1
}
