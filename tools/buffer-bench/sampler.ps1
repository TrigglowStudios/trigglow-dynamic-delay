# Every 10 s until the stop file exists: physical disk byte counters (raw, cumulative),
# the system's modified (dirty) page list and file cache, and the spill files in the test folders.
param([string]$Out, [string]$Stop)
"time,disk,write_bytes,read_bytes,modified_mb,cache_mb,standby_mb,spill_files_mb" | Out-File -Encoding ascii $Out
while (-not (Test-Path $Stop)) {
  $t = Get-Date -Format HH:mm:ss
  $m = Get-CimInstance Win32_PerfRawData_PerfOS_Memory
  $sb = ([double]$m.StandbyCacheNormalPriorityBytes + [double]$m.StandbyCacheReserveBytes + [double]$m.StandbyCacheCoreBytes)/1MB
  $sp = 0
  foreach ($d in 'C:\dd-bench-tmp','E:\dd-bench-tmp') { Get-ChildItem $d -Filter 'trigglow-dd-spill-*' -ErrorAction SilentlyContinue | ForEach-Object { $sp += $_.Length } }
  foreach ($d in Get-CimInstance Win32_PerfRawData_PerfDisk_PhysicalDisk | Where-Object Name -ne '_Total') {
    "$t,$($d.Name),$($d.DiskWriteBytesPersec),$($d.DiskReadBytesPersec),$([math]::Round($m.ModifiedPageListBytes/1MB)),$([math]::Round($m.CacheBytes/1MB)),$([math]::Round($sb)),$([math]::Round($sp/1MB))" | Out-File -Append -Encoding ascii $Out
  }
  Start-Sleep -Seconds 10
}
