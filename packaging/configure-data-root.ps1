param(
    [Parameter(Mandatory = $true)]
    [string]$DataRoot,
    [string]$OperatorAccount = [Security.Principal.WindowsIdentity]::GetCurrent().Name
)

$ErrorActionPreference = "Stop"
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Configuring the NSTU data root requires Administrator privileges."
}

$resolved = [IO.Path]::GetFullPath($DataRoot)
$root = [IO.Path]::GetPathRoot($resolved)
if ([string]::IsNullOrWhiteSpace($root) -or $resolved.TrimEnd('\') -eq $root.TrimEnd('\')) {
    throw "The NSTU data root must be an absolute subdirectory, not a drive root."
}

New-Item -ItemType Directory -Path $resolved -Force | Out-Null
$acl = [Security.AccessControl.DirectorySecurity]::new()
$acl.SetAccessRuleProtection($true, $false)
$inheritance = [Security.AccessControl.InheritanceFlags]::ContainerInherit -bor
    [Security.AccessControl.InheritanceFlags]::ObjectInherit
$propagation = [Security.AccessControl.PropagationFlags]::None
$full = [Security.AccessControl.FileSystemRights]::FullControl
$modify = [Security.AccessControl.FileSystemRights]::Modify
foreach ($account in @("NT AUTHORITY\SYSTEM", "BUILTIN\Administrators")) {
    $rule = [Security.AccessControl.FileSystemAccessRule]::new(
        $account, $full, $inheritance, $propagation,
        [Security.AccessControl.AccessControlType]::Allow)
    $acl.AddAccessRule($rule)
}
$operatorRule = [Security.AccessControl.FileSystemAccessRule]::new(
    $OperatorAccount, $modify, $inheritance, $propagation,
    [Security.AccessControl.AccessControlType]::Allow)
$acl.AddAccessRule($operatorRule)
Set-Acl -LiteralPath $resolved -AclObject $acl

$registryPath = "HKLM:\SOFTWARE\NSTU"
New-Item -Path $registryPath -Force | Out-Null
Set-ItemProperty -Path $registryPath -Name DataRoot -Type String -Value $resolved
Write-Host "NSTU data root configured: $resolved"
