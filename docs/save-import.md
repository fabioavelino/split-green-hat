# Importing a NAND or a save into the portable kit

The kit is portable: everything it reads and writes lives next to
`lichii.exe`, in the `user` folder.

```
my-build-fixed\
  lichii.exe
  user\
    config\        split.ini, qt-config.ini, ...
    nand\
      user\save\0000000000000000\<profile-uuid>\0400486024DEC000\
      system\save\8000000000000010\su\avators\profiles.dat
    load\0400486024DEC000\split-puppet\    (the puppet pack)
```

A save is not addressed by title ID alone. The game asks the account service
for the active profile, the service reads `profiles.dat`, and the file system
then opens
`user\save\0000000000000000\<profile-uuid>\0400486024DEC000`.
If the folder name and `profiles.dat` disagree, the Load menu is empty.

## Preferred: import the whole NAND

This keeps the save, the profile and `profiles.dat` consistent. Close Lichii,
then in PowerShell:

```powershell
$kit  = "C:\Users\fabio\Downloads\split-green-hat\my-build-fixed"
$zip  = "C:\Users\fabio\Downloads\zelda\nand.zip"
$tmp  = Join-Path $env:TEMP "split-nand"

Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive $zip -DestinationPath $tmp -Force

# The zip has a top-level "nand" folder.
Copy-Item (Join-Path $tmp "nand") (Join-Path $kit "user") -Recurse -Force
```

Start `lichii.exe`. The Load menu should now list the saves.

The NAND supplied for this project uses profile
`CE0C9E803E9F26D5349C94D2BC0BD82D`.

## Import only a save folder

If the kit already has the same profile, copy just the title folder:

```powershell
$src     = "C:\path\to\0400486024DEC000"
$kitUuid = "CE0C9E803E9F26D5349C94D2BC0BD82D"
$dst     = "C:\Users\fabio\Downloads\split-green-hat\my-build-fixed\user\nand\user\save\0000000000000000\$kitUuid"

New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item $src $dst -Recurse -Force
```

To find the profile the kit is using:

```powershell
Get-ChildItem "$kit\user\nand\user\save\0000000000000000" -Directory |
    Select-Object -ExpandProperty Name
```

If that lists a different UUID, either rename your save folder to that UUID
or copy `nand\system\save\8000000000000010\su\avators\profiles.dat` from the
NAND that produced the save, so the two agree.

## Check the config points at the kit

The portable build stores its settings in `user\config`. If the kit was
copied from another folder, an old `qt-config.ini` can still point at the old
`user` path. Search for the old name and replace it:

```powershell
$cfg = "$kit\user\config\qt-config.ini"
if (Test-Path $cfg) {
    (Get-Content $cfg) -replace 'my-build\\user', 'my-build-fixed\\user' |
        Set-Content $cfg
}
```

## What the log should say

After starting the game with `EDEN_NX2_DEBUG_SERVER=127.0.0.1:6545` set, the
M1a kit logs:

```
split: loaded ...
split: on, settings ...\user\config\split.ini
split: M1 debug puppet enabled, 3000 ms after the scene is known
split: M1 game build e0f3ccfaf50065ae, main at 0x...
split: diag scene 'Market_Adult', pos ..., debug true, targets 1
split: coop: puppet: P0: block at 0x...
split: coop: puppet: P0 CoopPuppet_Child spawned after ... ms
split: coop: puppet: P0 live actor ...
```

If `targets` stays at `0`, the `split: diag` line shows the scene and position
the planner received; send that line.
