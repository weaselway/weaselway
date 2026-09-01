<#
.SYNOPSIS
    Sets up a GPU-accelerated GNOME session under WSL2, end to end, from Windows.

.DESCRIPTION
    Runs every step of the README from a Windows shell: creates the Ubuntu
    distro, installs the build dependencies, clones this repo inside it, and
    drives the install-*.sh scripts. The Windows-side pieces the README leaves
    manual -- the `systemDistro` line in .wslconfig and the `wsl --shutdown`
    after it -- are done here too.

    Safe to re-run: each step checks whether it is already done.

.PARAMETER Distro
    Name of the WSL distro to create/use. Default "Gnome".

.PARAMETER BuildPackages
    Build the patched mesa and mutter locally (README step 5) instead of
    pulling them from the PPA. Takes hours; the PPA carries the same packages.

.PARAMETER Adapter
    Pin the GPU d3d12 renders on, for machines where the first adapter Windows
    lists is not the one you want -- "intel", "nvidia" and so on, matched
    against a substring of the adapter description. Left unset, nothing is
    pinned and d3d12 picks.

.PARAMETER Browsers
    Install Firefox and Chromium from the xtradeb PPA. Without it the script
    asks, once, interactively and defaults to yes. Pass -Browsers:$false to
    answer "no" without being asked.

.PARAMETER SkipDistroInstall
    Use an existing distro of that name as-is, rather than trying to create it.

.PARAMETER PasswordlessSudo
    Give the distro user passwordless sudo, so the rest of the run does not
    stop for a password. Without it the script asks, once, interactively.
    Pass -PasswordlessSudo:$false to answer "no" without being asked.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\install.ps1 -Distro Weasel -BuildPackages
#>

[CmdletBinding()]
param(
    [string] $Distro = "Gnome",
    [switch] $BuildPackages,
    [string] $Adapter,
    [switch] $Browsers,
    [switch] $SkipDistroInstall,
    [switch] $PasswordlessSudo
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# wsl.exe writes UTF-16LE unless this is set, which makes every output check
# below (and `wsl -l -q` in particular) unparsable.
$env:WSL_UTF8 = "1"

$RepoUrl = "https://github.com/weaselway/weaselway.git"
$WeaselDir = "C:\Weaselway"

# Where the scratch scripts handed to the distro get written. $PSScriptRoot is
# empty when this file is piped into powershell rather than run with -File.
$ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }

function Write-Step([string] $Message) {
    Write-Host ""
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Write-Note([string] $Message) {
    Write-Host "    $Message" -ForegroundColor DarkGray
}

function Invoke-Native([scriptblock] $Block) {
    # With $ErrorActionPreference = "Stop", anything a native command writes to
    # stderr is turned into a terminating NativeCommandError -- and the scripts
    # here run under `set -x`, so they write to stderr constantly even when they
    # succeed. Exit codes are what we actually judge them by, so native calls go
    # through here, where stderr is just output again.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try { & $Block } finally { $ErrorActionPreference = $previous }
}

function ConvertTo-WslPath([string] $Path) {
    $full = [System.IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') {
        throw "cannot map '$full' to a /mnt path; run this script from a local drive."
    }
    return "/mnt/" + $Matches[1].ToLower() + "/" + ($Matches[2] -replace '\\', '/')
}

function Invoke-WslCommand {
    # Runs a bash script inside the distro, by way of a file.
    #
    # Not `bash -lc <script>`: the command line PowerShell builds for wsl.exe is
    # re-split on the other side, so a script containing double quotes gets
    # re-parsed and falls apart, and newlines do not survive at all. Dropping the
    # script next to this file and passing only its (quote-free, space-free) path
    # sidesteps both -- the repo is on a Windows drive, so the distro can read it
    # under /mnt.
    param(
        [Parameter(Mandatory)] [string] $Command,
        [string] $User,
        [string] $Description,
        [switch] $Quiet
    )

    $tempDir = Join-Path $ScriptDir ".weaselway-tmp"
    if (-not (Test-Path $tempDir)) {
        New-Item -ItemType Directory -Path $tempDir -Force | Out-Null
    }
    $file = Join-Path $tempDir ("step-" + [guid]::NewGuid().ToString("N") + ".sh")

    try {
        # LF endings and no BOM: bash chokes on both.
        $text = ($Command -replace "`r`n", "`n").TrimEnd() + "`n"
        [System.IO.File]::WriteAllText($file, $text, (New-Object System.Text.UTF8Encoding $false))

        $wslArgs = @("-d", $Distro)
        if ($User) { $wslArgs += @("-u", $User) }
        # --cd fixes the working directory even when the distro's default differs.
        $wslArgs += @("--cd", "~", "--", "bash", (ConvertTo-WslPath $file))

        # Out-Host, not bare output: anything a function leaves in the pipeline
        # becomes part of its return value, and the exit code is all we want back.
        if ($Quiet) {
            Invoke-Native { & wsl.exe @wslArgs 2>&1 | Out-Null }
        } else {
            Invoke-Native { & wsl.exe @wslArgs | Out-Host }
        }
        return [int] $LASTEXITCODE
    } finally {
        Remove-Item -LiteralPath $file -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-Wsl {
    # Runs a bash script inside the distro and fails the script if it does.
    param(
        [Parameter(Mandatory)] [string] $Command,
        [string] $User,
        [string] $Description
    )

    $code = Invoke-WslCommand -Command $Command -User $User
    if ($code -ne 0) {
        $what = if ($Description) { $Description } else { $Command }
        throw "failed inside ${Distro}: $what (exit $code)"
    }
}

function Test-WslCommand([string] $Command) {
    # Same, but reports success as a boolean rather than throwing. Output is
    # discarded -- this is only ever used for "is this already done?" probes.
    return (Invoke-WslCommand -Command $Command -Quiet) -eq 0
}

# --- 0. WSL itself ----------------------------------------------------------

Write-Step "Checking WSL"

if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    throw "wsl.exe not found. Install WSL from the Microsoft Store first."
}

Invoke-Native { & wsl.exe --update }
# --update exits non-zero when there is nothing to update on some builds, so
# its status is not worth checking; the version check below is the real gate.

$versionOutput = (Invoke-Native { & wsl.exe --version }) -join "`n"
$match = [regex]::Match($versionOutput, '(?m)^\s*WSL\D*:\s*(\d+)\.(\d+)')
if (-not $match.Success) {
    throw @"
Could not read a WSL version from ``wsl --version``. That flag only exists on
the Store build of WSL -- if it is unrecognised you are on the in-box Windows
component. Install WSL from the Microsoft Store and re-run this script.
"@
}

$wslVersion = [version]("{0}.{1}" -f $match.Groups[1].Value, $match.Groups[2].Value)
if ($wslVersion -lt [version]"2.7") {
    throw "WSL $wslVersion is too old; 2.7 or newer is required."
}
Write-Note "WSL $wslVersion"

# --- 0b. The distro ---------------------------------------------------------

$existing = @(Invoke-Native { & wsl.exe --list --quiet } | ForEach-Object { $_.Trim() } | Where-Object { $_ })

if ($existing -contains $Distro) {
    Write-Note "distro '$Distro' already exists"
} elseif ($SkipDistroInstall) {
    throw "distro '$Distro' does not exist and -SkipDistroInstall was given."
} else {
    Write-Step "Creating the distro '$Distro'"
    Write-Host "    This opens the distro's first-run setup: pick a username and password."
    Write-Host "    It then drops you at a shell inside the distro -- type 'exit' there to"
    Write-Host "    hand control back and let this script continue."
    Invoke-Native { & wsl.exe --install Ubuntu-26.04 --name $Distro }
    if ($LASTEXITCODE -ne 0) {
        throw "wsl --install failed (exit $LASTEXITCODE). It may need an elevated shell, or a reboot."
    }
}

if ($existing.Count -gt 1 -or ($existing.Count -eq 1 -and $existing[0] -ne $Distro)) {
    Write-Note "other distros are present: '$Distro' must be the first one started after"
    Write-Note "a 'wsl --shutdown', or WSL will not wire up /run/user/1000 for it."
}

# The default user, needed for the repo path and for the docker group.
$distroUser = Invoke-Native { & wsl.exe -d $Distro -- bash -lc 'printf %s "$(whoami)"' }
if ($LASTEXITCODE -ne 0 -or -not $distroUser) {
    throw "could not talk to distro '$Distro'."
}
if ($distroUser -eq "root") {
    throw "distro '$Distro' logs in as root; create a normal user first (the build steps refuse to run as root)."
}
Write-Note "user: $distroUser"

# --- 0c. Passwordless sudo (optional) --------------------------------------

# Most of the install-*.sh scripts call sudo themselves, so an unattended run
# needs this; otherwise the run parks on a password prompt partway through.
Write-Step "Passwordless sudo"

if (Test-WslCommand "sudo -n true") {
    Write-Note "$distroUser already has passwordless sudo"
} else {
    $wantIt = $false
    if ($PSBoundParameters.ContainsKey('PasswordlessSudo')) {
        $wantIt = [bool] $PasswordlessSudo
        Write-Note ("-PasswordlessSudo: {0}" -f $wantIt)
    } elseif ([Environment]::UserInteractive) {
        Write-Host "    The remaining steps use sudo and will each ask for $distroUser's password."
        Write-Host "    Enabling passwordless sudo writes /etc/sudoers.d/weaselway and lets the"
        Write-Host "    rest of the run go through unattended. It is a lasting change to the"
        Write-Host "    distro: anything running as $distroUser can then become root without a"
        Write-Host "    password. Delete that file to undo it."
        $answer = Read-Host "    Enable passwordless sudo? [y/N]"
        $wantIt = $answer -match '^\s*(y|yes)\s*$'
    } else {
        Write-Note "not interactive and -PasswordlessSudo not given; leaving sudo as it is"
    }

    if ($wantIt) {
        # Written via a temp file and checked with visudo before it is put in
        # place -- a malformed drop-in locks sudo out of the distro entirely.
        Invoke-Wsl -User root -Description "enable passwordless sudo" -Command @"
set -eu
tmp=`$(mktemp)
trap 'rm -f "`$tmp"' EXIT
printf '%s ALL=(ALL) NOPASSWD:ALL\n' '$distroUser' > "`$tmp"
visudo -cqf "`$tmp"
install -m 0440 -o root -g root "`$tmp" /etc/sudoers.d/weaselway
"@
        Write-Note "wrote /etc/sudoers.d/weaselway"
    } else {
        Write-Note "leaving sudo as it is -- expect password prompts below"
    }
}

# --- 0d. Dependencies and the repo -----------------------------------------

Write-Step "Installing dependencies inside $Distro"

Invoke-Wsl -User root -Description "apt install" -Command @'
set -eu
export DEBIAN_FRONTEND=noninteractive
apt -y update
apt -y upgrade
apt -y install git curl unzip docker.io gnome-session ubuntu-session winpr-utils ptyxis
'@

Invoke-Wsl -User root -Description "usermod -aG docker" -Command "usermod -aG docker '$distroUser'"

# Group membership only takes effect in a fresh login session, and the distro
# keeps one alive between `wsl` invocations. Terminating it is how the docker
# group reaches the build step later on.
Write-Note "restarting the distro so the docker group takes effect"
Invoke-Native { & wsl.exe --terminate $Distro } | Out-Null

$repo = "/home/$distroUser/weaselway"

Write-Step "Cloning the repo"
if (Test-WslCommand "test -d '$repo/.git'") {
    Write-Note "already cloned; pulling"
    Invoke-Wsl -Description "git pull" -Command "cd '$repo' && git pull --ff-only"
} else {
    Invoke-Wsl -Description "git clone" -Command "git clone '$RepoUrl' '$repo'"
}

# --- 1. FreeRDP -------------------------------------------------------------

if (-not (Test-WslCommand "sudo -n true")) {
    Write-Note "the steps below run as $distroUser and use sudo -- expect a password prompt"
}

Write-Step "1. Installing the FreeRDP client into $WeaselDir"
Invoke-Wsl -Description "install-freerdp.sh" -Command "cd '$repo' && ./install-freerdp.sh"

# --- 2. System distro image -------------------------------------------------

Write-Step "2. Installing the system distro image"

$vhd = Get-ChildItem -Path $WeaselDir -Filter "system_x64-*.vhd" -ErrorAction SilentlyContinue |
       Sort-Object LastWriteTime -Descending | Select-Object -First 1

if ($vhd) {
    Write-Note "already present: $($vhd.Name)"
} else {
    Invoke-Wsl -Description "install-system-image.sh" -Command "cd '$repo' && ./install-system-image.sh"
    $vhd = Get-ChildItem -Path $WeaselDir -Filter "system_x64-*.vhd" -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $vhd) {
        throw "install-system-image.sh did not leave a system_x64-*.vhd in $WeaselDir."
    }
}

# The README's manual step: point .wslconfig at that image. The value is read
# as an INI string, so the backslashes have to be doubled.
Write-Step "Pointing .wslconfig at $($vhd.Name)"

$wslConfig = Join-Path $env:USERPROFILE ".wslconfig"
$wanted = "systemDistro=" + ($vhd.FullName -replace '\\', '\\')

$lines = if (Test-Path $wslConfig) { @(Get-Content -LiteralPath $wslConfig) } else { @() }

if ($lines -contains $wanted) {
    Write-Note "already set"
    $configChanged = $false
} else {
    # Drop any previous systemDistro line, wherever it sits, then put ours
    # directly under [wsl2] -- creating that section if the file lacks one.
    $lines = @($lines | Where-Object { $_ -notmatch '^\s*systemDistro\s*=' })

    $wsl2Index = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[wsl2\]\s*$') { $wsl2Index = $i; break }
    }

    if ($wsl2Index -ge 0) {
        $rebuilt = @()
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $rebuilt += $lines[$i]
            if ($i -eq $wsl2Index) { $rebuilt += $wanted }
        }
        $lines = $rebuilt
    } else {
        if ($lines.Count -gt 0) { $lines += "" }
        $lines += @("[wsl2]", $wanted)
    }

    if (Test-Path $wslConfig) {
        Copy-Item -LiteralPath $wslConfig -Destination "$wslConfig.weaselway.bak" -Force
        Write-Note "backed up the old file to $wslConfig.weaselway.bak"
    }
    Set-Content -LiteralPath $wslConfig -Value $lines -Encoding UTF8
    Write-Note "wrote $wslConfig"
    $configChanged = $true
}

if ($configChanged) {
    Write-Step "Restarting WSL so the new system distro is picked up"
    Invoke-Native { & wsl.exe --shutdown }
    Start-Sleep -Seconds 3
}

# --- 3. Kernel module -------------------------------------------------------

Write-Step "3. Building the dxgdrm kernel module (this takes a while)"
Invoke-Wsl -Description "install-kernel-module.sh" -Command "cd '$repo' && ./install-kernel-module.sh"

# --- 5/6. Patched mesa and mutter ------------------------------------------

if ($BuildPackages) {
    Write-Step "5. Building the patched mesa and mutter (this takes hours)"
    Invoke-Wsl -Description "build-packages.sh" -Command "cd '$repo' && ./build-packages.sh"

    Write-Step "6. Installing the built packages"
    Invoke-Wsl -User root -Description "apt install packages" -Command @"
set -eu
export DEBIAN_FRONTEND=noninteractive
cd '$repo'
apt install -y ./ubuntu/resolute/packages/*/*.deb
"@
} else {
    Write-Step "5/6. Installing the patched mesa and mutter from the PPA"
    Invoke-Wsl -Description "install-ppa-packages.sh" -Command "cd '$repo' && ./install-ppa-packages.sh"
}

# --- 7. Session units -------------------------------------------------------

Write-Step "7. Installing the session units"
$unitsArgs = ""
if ($PSBoundParameters.ContainsKey('Adapter')) {
    $unitsArgs = " --adapter '$Adapter'"
    Write-Note "pinning the d3d12 adapter to '$Adapter'"
}
Invoke-Wsl -Description "install-units.sh" -Command "cd '$repo' && ./install-units.sh$unitsArgs"

# --- 8. Audio bridge --------------------------------------------------------

Write-Step "8. Installing the audio bridge"
Invoke-Wsl -Description "install-audio.sh" -Command "cd '$repo' && ./install-audio.sh"

# --- Browsers (optional) ----------------------------------------------------

Write-Step "Firefox and Chromium"

if (Test-WslCommand "test -f /etc/apt/preferences.d/weaselway-xtradeb") {
    Write-Note "the xtradeb PPA is already set up"
} else {
    $wantBrowsers = $true
    if ($PSBoundParameters.ContainsKey('Browsers')) {
        $wantBrowsers = [bool] $Browsers
        Write-Note ("-Browsers: {0}" -f $wantBrowsers)
    } elseif ([Environment]::UserInteractive) {
        Write-Host "    Ubuntu ships both as snaps, and a browser snap bundles its own mesa --"
        Write-Host "    not the patched one, so it will not accelerate. This installs them as"
        Write-Host "    ordinary .debs from the xtradeb PPA instead. Nothing about the session"
        Write-Host "    depends on it; you can run ./install-xtradeb.sh later just as well."
        $answer = Read-Host "    Install Firefox and Chromium? [Y/n]"
        $wantBrowsers = $answer -notmatch '^\s*(n|no)\s*$'
    } else {
        Write-Note "not interactive; taking the default and installing them"
    }

    if ($wantBrowsers) {
        Invoke-Wsl -Description "install-xtradeb.sh" -Command "cd '$repo' && ./install-xtradeb.sh"
    } else {
        Write-Note "skipped -- run ./install-xtradeb.sh inside the distro to do it later"
    }
}

# --- Done -------------------------------------------------------------------

Write-Step "Done"
Write-Host @"

Start the session and connect to it, from inside the distro:

    wsl -d $Distro --cd ~/weaselway
    ./start-gnome-shell.sh
    ./start-viewer.sh

To stop it again:

    systemctl --user start gnome-session-shutdown.target

"@
