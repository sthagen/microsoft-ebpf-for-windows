# Copyright (c) eBPF for Windows contributors
# SPDX-License-Identifier: MIT

param([Parameter(Mandatory=$True)][string]$FileName,
      [Parameter(Mandatory=$True)][string]$FilePath,
      [Parameter(Mandatory=$True)][string]$Platform,
      [Parameter(Mandatory=$True)][string]$Configuration,
      [Parameter(Mandatory=$True)][string]$KernelConfiguration,
      [Parameter(Mandatory=$True)][string]$IncludePath,
      [Parameter(Mandatory=$True)][string]$Packages)

Push-Location $FilePath

$ProgramType = ""

if ($FileName -eq "bpf")
{
    $ProgramType = "bind"
}

.\Convert-BpfToNative.ps1 -FileName $Filename -Type $ProgramType -IncludeDir $IncludePath -Platform $Platform -Configuration $KernelConfiguration -KernelMode $True -Packages $Packages
.\Convert-BpfToNative.ps1 -FileName $Filename -Type $ProgramType -IncludeDir $IncludePath -Platform $Platform -Configuration $Configuration -KernelMode $False -Packages $Packages


Pop-Location
