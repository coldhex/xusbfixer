# Search for the entry containing the original INF filename
$targetInf = 'xusbfixer.inf'
$matchingEntries = Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match ".*\\$targetInf" }

# Check if matching entries are found
if ($matchingEntries.Count -gt 0) {
    # Iterate through matching entries
    foreach ($entry in $matchingEntries) {
        # Extract the OEM INF name from the entry
        $oemInf = $entry.Driver

        # Run pnputil to delete the driver with the corresponding OEM INF
        $deleteResult = pnputil /delete-driver $oemInf

        if ($deleteResult -match "deleted") {
            Write-Host "Driver package deleted successfully."
        } else {
            Write-Host "Failed to delete driver package. Ensure the script is run with administrative privileges."
        }
    }
} else {
    Write-Host "Driver package not found."
}


$driverFilePath = "$env:SystemRoot\System32\drivers\xusbfixer.sys"
try {
    $acl = Get-Acl -Path $driverFilePath
    $rule = New-Object System.Security.AccessControl.FileSystemAccessRule("Everyone", "FullControl", "Allow")
    $acl.SetAccessRule($rule)
    Set-Acl -Path $driverFilePath -AclObject $acl

    Remove-Item -Path $driverFilePath -Force
    Write-Host "Driver sys file deleted successfully."
} catch {
    Write-Host "Failed to delete the driver sys file. $_"
}


Remove-Item -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\xusbfixer' -Recurse -ErrorAction Ignore

$RegPath = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{d61ca365-5af4-4486-998b-9db4734c6ca3}'

foreach ($propertyName in @("UpperFilters", "LowerFilters")) {
    if (Get-ItemProperty -Path $RegPath -Name $propertyName -ErrorAction Ignore) {
        $Values = Get-ItemPropertyValue -Path $RegPath -Name $propertyName
        if ($Values) {
            $NewValues = @($Values | ? { $_ -ne 'xusbfixer' })
            if ($NewValues) {
                Set-ItemProperty -Path $RegPath -Name $propertyName -Value $NewValues
            } else {
                Remove-ItemProperty -Path $RegPath -Name $propertyName
            }
            Write-Host "Removed from $propertyName"
        } else {
            Write-Host "Property $propertyName did not have values."
        }
    } else {
        Write-Host "Property $propertyName did not exist."
    }
}
