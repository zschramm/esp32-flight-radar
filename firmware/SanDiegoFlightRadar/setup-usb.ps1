param([string]$Port='COM6')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

function Read-RadarLine($Serial,[string]$Prefix) {
    $deadline=[DateTime]::UtcNow.AddSeconds(40)
    $pending=''
    while([DateTime]::UtcNow -lt $deadline) {
        $pending += $Serial.ReadExisting()
        while($pending.Contains("`n")) {
            $end=$pending.IndexOf("`n")
            $line=$pending.Substring(0,$end).Trim()
            $pending=$pending.Substring($end+1)
            if($line.StartsWith($Prefix)){return $line}
        }
        [Windows.Forms.Application]::DoEvents()
        Start-Sleep -Milliseconds 30
    }
    throw 'The display did not respond. Check the USB cable, selected port, and that no serial monitor is open.'
}
function Send-RadarCommand([string]$Command,[string]$Prefix) {
    $serial=[IO.Ports.SerialPort]::new($ports.Text,921600,[IO.Ports.Parity]::None,8,[IO.Ports.StopBits]::One)
    $serial.Encoding=[Text.UTF8Encoding]::new($false)
    $serial.DtrEnable=$false;$serial.RtsEnable=$false;$serial.WriteTimeout=3000
    try {
        $serial.Open();$serial.DiscardInBuffer();$serial.Write("STATUS`n")
        $identity=Read-RadarLine $serial 'RADAR: mode='
        if($identity -notmatch 'version=1\.[2-9]'){throw 'This setup tool requires the updated flight radar firmware.'}
        $serial.Write($Command+"`n")
        return Read-RadarLine $serial $Prefix
    } finally {if($serial.IsOpen){$serial.Close()};$serial.Dispose()}
}
$form=[Windows.Forms.Form]::new()
$form.Text='Flight Radar Setup';$form.ClientSize=[Drawing.Size]::new(500,530)
$form.StartPosition='CenterScreen';$form.FormBorderStyle='FixedDialog';$form.MaximizeBox=$false
$form.Font=[Drawing.Font]::new('Segoe UI',10)
function Add-Label([string]$Value,[int]$Y) {
    $l=[Windows.Forms.Label]::new();$l.Text=$Value;$l.Location=[Drawing.Point]::new(24,$Y);$l.Size=[Drawing.Size]::new(450,25);$form.Controls.Add($l)
}
function Add-Input([int]$Y,[string]$Value) {
    $t=[Windows.Forms.TextBox]::new();$t.Location=[Drawing.Point]::new(24,$Y);$t.Size=[Drawing.Size]::new(450,28);$t.Text=$Value;$form.Controls.Add($t);return $t
}
Add-Label 'Change the saved Wi-Fi or radar center over USB.' 18
Add-Label 'USB port' 53
$ports=[Windows.Forms.ComboBox]::new();$ports.Location=[Drawing.Point]::new(24,78);$ports.Size=[Drawing.Size]::new(150,28)
$ports.Items.AddRange([string[]][IO.Ports.SerialPort]::GetPortNames());$ports.Text=$Port;$form.Controls.Add($ports)
Add-Label 'Home Wi-Fi network (2.4 GHz)' 116
$ssid=Add-Input 142 ''
Add-Label 'Home Wi-Fi password (re-enter only when changing settings)' 181
$password=Add-Input 207 '';$password.UseSystemPasswordChar=$true
Add-Label 'Radar latitude' 246
$latitude=Add-Input 272 '32.7157'
Add-Label 'Radar longitude' 311
$longitude=Add-Input 337 '-117.1611'
$status=[Windows.Forms.Label]::new();$status.Location=[Drawing.Point]::new(24,440);$status.Size=[Drawing.Size]::new(450,70)
$status.Text='GUI updates retain saved settings. You do not need this form after an update.';$form.Controls.Add($status)
$load=[Windows.Forms.Button]::new();$load.Text='Read saved settings';$load.Location=[Drawing.Point]::new(24,386);$load.Size=[Drawing.Size]::new(205,36);$form.Controls.Add($load)
$save=[Windows.Forms.Button]::new();$save.Text='Save and reconnect';$save.Location=[Drawing.Point]::new(245,386);$save.Size=[Drawing.Size]::new(229,36);$form.Controls.Add($save)
$load.Add_Click({
    $load.Enabled=$false;$save.Enabled=$false;$status.Text='Reading the display...'
    try {
        $line=Send-RadarCommand 'SETTINGS' 'SETTINGS '
        $settings=$line.Substring(9)|ConvertFrom-Json
        $ssid.Text=$settings.ssid
        $latitude.Text=$settings.lat.ToString([Globalization.CultureInfo]::InvariantCulture)
        $longitude.Text=$settings.lon.ToString([Globalization.CultureInfo]::InvariantCulture)
        $status.Text='Saved network and location loaded. Passwords are never read back from the display.'
    }catch{$status.Text=$_.Exception.Message}finally{$load.Enabled=$true;$save.Enabled=$true}
})
$save.Add_Click({
    $load.Enabled=$false;$save.Enabled=$false
    try {
        $latitudeValue=[double]::Parse($latitude.Text,[Globalization.CultureInfo]::InvariantCulture)
        $longitudeValue=[double]::Parse($longitude.Text,[Globalization.CultureInfo]::InvariantCulture)
        if([double]::IsNaN($latitudeValue)-or [double]::IsNaN($longitudeValue)-or [Math]::Abs($latitudeValue)-gt 90-or [Math]::Abs($longitudeValue)-gt 180){throw 'Check the latitude and longitude.'}
        $networkBytes=[Text.Encoding]::UTF8.GetByteCount($ssid.Text);$passwordBytes=[Text.Encoding]::UTF8.GetByteCount($password.Text)
        if($networkBytes -lt 1 -or $networkBytes -gt 32){throw 'Enter a Wi-Fi network name of 1-32 bytes.'}
        if($passwordBytes -gt 0 -and ($passwordBytes -lt 8 -or $passwordBytes -gt 63) -and $password.Text -notmatch '^[0-9a-fA-F]{64}$'){throw 'Enter your existing Wi-Fi password (8-63 bytes, or a 64-digit hexadecimal key).'}
        $payload=@{ssid=$ssid.Text;password=$password.Text;lat=$latitudeValue;lon=$longitudeValue}|ConvertTo-Json -Compress
        $status.Text='Saving to the display...'
        $reply=Send-RadarCommand ('CONFIG '+$payload) 'CONFIG_'
        if(-not $reply.StartsWith('CONFIG_OK')){throw $reply}
        $password.Clear();$payload=$null;$status.Text='Saved. The display is restarting and reconnecting automatically.'
    }catch{$status.Text=$_.Exception.Message}finally{$load.Enabled=$true;$save.Enabled=$true}
})
[void]$form.ShowDialog()
$password.Clear();$form.Dispose()
