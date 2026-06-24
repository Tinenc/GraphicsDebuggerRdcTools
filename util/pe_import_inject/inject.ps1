<#
.SYNOPSIS
    Add a new IMAGE_IMPORT_DESCRIPTOR (so the OS PE loader loads an
    extra DLL) into a target Windows PE32+ (x64) image, by appending a
    fresh section and rewriting the Import Directory.

.DESCRIPTION
    Use case: drop a hook DLL alongside an exe / DLL on disk and patch
    the PE so the standard NT loader (LdrInitializeThunk) picks it up
    BEFORE any user-mode initialisation in the target -- in particular
    before any anti-cheat that's loaded as a delay-load dependency or
    via a later LoadLibrary in user code. The loaded DLL is then in
    PEB.Ldr exactly like any normal dependency.

    What it does, in order:
      1. Open <InputFile> and validate it's a PE32+ image.
      2. Parse the existing IMAGE_IMPORT_DESCRIPTOR table.
      3. Build a new ".tnc" section containing:
           * The full IDT (existing descriptors copied verbatim + one
             new descriptor pointing at our injected DLL + a null
             terminator descriptor).
           * The injected dll name as a NUL-terminated ASCII string.
           * One IMAGE_THUNK_DATA64 + a null terminator (used by both
             OriginalFirstThunk and FirstThunk).
           * One IMAGE_IMPORT_BY_NAME for the import (hint=0,
             func name = -ImportSymbol).
      4. Append the section header (consumes 40 bytes inside
         SizeOfHeaders).
      5. Update the optional header:
           * SizeOfImage += aligned new section virtual size
           * NumberOfSections += 1
           * DataDirectory[IMPORT].VirtualAddress -> new IDT RVA
           * DataDirectory[IMPORT].Size           -> total IDT bytes
           * CheckSum cleared (user-mode loaders don't enforce it)
      6. Write <OutputFile>.

    The loader DLL must:
      * Be x64.
      * Export exactly one symbol matching -ImportSymbol (default
        "TnT_Entry"; pick whatever the supplied DLL actually exports).
      * Live next to <OutputFile> on disk (PE search order: app dir
        first), or anywhere on %PATH%.

.PARAMETER InputFile
    Source PE32+ image (.exe or .dll) to patch. NOT modified in place.

.PARAMETER OutputFile
    Destination path for the patched image. If omitted, defaults to
    "<InputFile>.patched.<ext>" next to the source.

.PARAMETER InjectDll
    The DLL name the loader should pull in. Use just the basename
    (e.g. "TinecmaTool_PoC.dll"); the loader resolves it via the
    normal PE search order.

.PARAMETER ImportSymbol
    Exported function name in -InjectDll to wire the IAT thunk to.
    Default "TnT_Entry". The function does not need to do anything; it
    just has to exist in the export table of the injected DLL so the
    loader doesn't reject the descriptor.

.PARAMETER SectionName
    Name (max 8 chars) of the appended section. Default ".tnc".

.EXAMPLE
    .\inject.ps1 -InputFile 'D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64\Client-Win64-Shipping.exe' `
                 -OutputFile 'D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64\Client-Win64-Shipping.patched.exe' `
                 -InjectDll  'TinecmaTool_PoC.dll'

.NOTES
    * x64 only (validates IMAGE_FILE_MACHINE_AMD64 + PE32+ optional
      header magic). 32-bit support would need a different
      IMAGE_THUNK_DATA layout.
    * Does NOT touch the original on-disk file. You stage the patched
      file under a different name and rename / copy yourself; this
      keeps the original recoverable.
    * Anti-cheat / anti-tamper that checks the image hash will catch
      this. There is nothing this script can do about that -- the
      check is by design.
    * Does NOT update the legacy IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT
      data directory; we clear it instead. Some old bound-import-using
      executables may need to rebind their first import sweep, but
      every Win10+ Visual C++ build we care about leaves it empty
      anyway.
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$InputFile,
    [string]$OutputFile,
    [Parameter(Mandatory = $true)][string]$InjectDll,
    [string]$ImportSymbol = 'TnT_Entry',
    [string]$SectionName = '.tnc'
)

$ErrorActionPreference = 'Stop'

if(-not (Test-Path -LiteralPath $InputFile)) {
    throw "Input file not found: $InputFile"
}
if(-not $OutputFile) {
    $ext = [System.IO.Path]::GetExtension($InputFile)
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($InputFile)
    $dir  = [System.IO.Path]::GetDirectoryName($InputFile)
    $OutputFile = Join-Path $dir ("{0}.patched{1}" -f $stem, $ext)
}
if($SectionName.Length -gt 8) {
    throw "SectionName must be <= 8 chars, got '$SectionName'"
}

# ---- helpers --------------------------------------------------------------

function Align([uint64]$v, [uint64]$a) {
    if($a -le 1) { return $v }
    return [uint64](([uint64]$v + $a - 1) -band -bnot ([uint64]$a - 1))
}

function WriteU16($buf, $off, $val) {
    $bytes = [BitConverter]::GetBytes([uint16]$val)
    [Array]::Copy($bytes, 0, $buf, $off, 2)
}
function WriteU32($buf, $off, $val) {
    $bytes = [BitConverter]::GetBytes([uint32]$val)
    [Array]::Copy($bytes, 0, $buf, $off, 4)
}
function WriteU64($buf, $off, $val) {
    $bytes = [BitConverter]::GetBytes([uint64]$val)
    [Array]::Copy($bytes, 0, $buf, $off, 8)
}
function ReadU16($buf, $off) { return [BitConverter]::ToUInt16($buf, $off) }
function ReadU32($buf, $off) { return [BitConverter]::ToUInt32($buf, $off) }

# ---- load & validate ------------------------------------------------------

Write-Host "Reading $InputFile ..."
$bytes = [System.IO.File]::ReadAllBytes($InputFile)
if($bytes.Length -lt 0x80) { throw "File too small to be a PE." }

if($bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) { throw "Not MZ." }
$e_lfanew = ReadU32 $bytes 0x3C
if($e_lfanew -le 0 -or $e_lfanew + 264 -gt $bytes.Length) { throw "Bad e_lfanew." }

$peSig = ReadU32 $bytes $e_lfanew
if($peSig -ne 0x00004550) { throw "Bad PE signature." }

$fhOff = $e_lfanew + 4
$machine     = ReadU16 $bytes ($fhOff + 0)
$secCount    = ReadU16 $bytes ($fhOff + 2)
$optSize     = ReadU16 $bytes ($fhOff + 16)
$characteristics = ReadU16 $bytes ($fhOff + 18)

if($machine -ne 0x8664) { throw ("Not AMD64 (machine=0x{0:X4}); 32-bit not supported." -f $machine) }

$ohOff = $fhOff + 20
$magic = ReadU16 $bytes $ohOff
if($magic -ne 0x20B) { throw ("Not PE32+ (magic=0x{0:X4})." -f $magic) }
if($optSize -lt 240) { throw "OptionalHeader too small to be PE32+." }

# PE32+ Optional Header field offsets (relative to ohOff):
#  0   Magic                       2
#  ... LinkerVersion etc           ..
# 16   SizeOfCode                  4
# 20   SizeOfInitializedData       4
# 24   SizeOfUninitializedData     4
# 28   AddressOfEntryPoint         4
# 32   BaseOfCode                  4
# 36   ImageBase                   8
# 44   SectionAlignment            4
# 48   FileAlignment               4
# 52   OS / image / subsystem version ...
# 56   SizeOfImage                 4 (at ohOff+56)
# 60   SizeOfHeaders               4 (at ohOff+60)
# 64   CheckSum                    4
# ...
# 112  NumberOfRvaAndSizes         4 (at ohOff+108? -- careful, PE32+ has +16 shifts)

# Build offsets cleanly for PE32+:
# PE32+ OptionalHeader field offsets relative to $ohOff (per Microsoft PE
# spec; PE32+ skips PE32's BaseOfData(+24) and widens several fields to 8B).
$O_ImageBase        = $ohOff + 24
$O_SectionAlignment = $ohOff + 32
$O_FileAlignment    = $ohOff + 36
$O_SizeOfImage      = $ohOff + 56
$O_SizeOfHeaders    = $ohOff + 60
$O_CheckSum         = $ohOff + 64
$O_NumberOfRva      = $ohOff + 108
$O_DataDirectories  = $ohOff + 112

$sectionAlignment = ReadU32 $bytes $O_SectionAlignment
$fileAlignment    = ReadU32 $bytes $O_FileAlignment
$sizeOfImage      = ReadU32 $bytes $O_SizeOfImage
$sizeOfHeaders    = ReadU32 $bytes $O_SizeOfHeaders
$numberOfRva      = ReadU32 $bytes $O_NumberOfRva

Write-Host ("PE32+ x64, sections={0}, SecAlign=0x{1:X}, FileAlign=0x{2:X}, SizeOfImage=0x{3:X}, SizeOfHeaders=0x{4:X}, NumRva={5}" -f `
    $secCount, $sectionAlignment, $fileAlignment, $sizeOfImage, $sizeOfHeaders, $numberOfRva)

if($numberOfRva -lt 2) { throw "NumberOfRvaAndSizes < 2 -- no Import directory slot." }

$D_IMPORT_RVA  = $O_DataDirectories + 1 * 8 + 0
$D_IMPORT_SIZE = $O_DataDirectories + 1 * 8 + 4
$D_BOUND_RVA   = $O_DataDirectories + 11 * 8 + 0
$D_BOUND_SIZE  = $O_DataDirectories + 11 * 8 + 4
$D_IAT_RVA     = $O_DataDirectories + 12 * 8 + 0
$D_IAT_SIZE    = $O_DataDirectories + 12 * 8 + 4

$oldImportRva  = ReadU32 $bytes $D_IMPORT_RVA
$oldImportSize = ReadU32 $bytes $D_IMPORT_SIZE

Write-Host ("Existing IMPORT directory: RVA=0x{0:X8} Size=0x{1:X8}" -f $oldImportRva, $oldImportSize)

# ---- section table --------------------------------------------------------

$secOff = $ohOff + $optSize    # IMAGE_FIRST_SECTION
$sections = @()
for($i = 0; $i -lt $secCount; $i++) {
    $sh = $secOff + $i * 40
    $name = [System.Text.Encoding]::ASCII.GetString($bytes, $sh, 8).TrimEnd([char]0)
    $entry = [pscustomobject]@{
        Name             = $name
        VirtualSize      = ReadU32 $bytes ($sh + 8)
        VirtualAddress   = ReadU32 $bytes ($sh + 12)
        SizeOfRawData    = ReadU32 $bytes ($sh + 16)
        PointerToRawData = ReadU32 $bytes ($sh + 20)
        Characteristics  = ReadU32 $bytes ($sh + 36)
        HeaderOffset     = $sh
    }
    $sections += $entry
}
Write-Host "Sections:"
foreach($s in $sections) {
    Write-Host ("  {0,-8} VA=0x{1:X8} VS=0x{2:X8} Raw=0x{3:X8} @ 0x{4:X8} chars=0x{5:X8}" -f `
        $s.Name, $s.VirtualAddress, $s.VirtualSize, $s.SizeOfRawData, $s.PointerToRawData, $s.Characteristics)
}

# Read existing IDT (so we can copy descriptors verbatim).
function RvaToFileOffset($rva) {
    foreach($s in $sections) {
        if($rva -ge $s.VirtualAddress -and $rva -lt $s.VirtualAddress + [Math]::Max($s.VirtualSize, $s.SizeOfRawData)) {
            return $s.PointerToRawData + ($rva - $s.VirtualAddress)
        }
    }
    return 0
}

$IDT_ENTRY = 20    # IMAGE_IMPORT_DESCRIPTOR size
$existingDescriptors = New-Object byte[] 0
if($oldImportRva -ne 0) {
    $impOff = RvaToFileOffset $oldImportRva
    if($impOff -eq 0) { throw "Couldn't map existing IMPORT RVA to a file offset." }
    $count = 0
    while($true) {
        $entryOff = $impOff + $count * $IDT_ENTRY
        if($entryOff + $IDT_ENTRY -gt $bytes.Length) { break }
        $oft    = ReadU32 $bytes ($entryOff + 0)
        $tds    = ReadU32 $bytes ($entryOff + 4)
        $fchain = ReadU32 $bytes ($entryOff + 8)
        $name   = ReadU32 $bytes ($entryOff + 12)
        $first  = ReadU32 $bytes ($entryOff + 16)
        if($oft -eq 0 -and $tds -eq 0 -and $fchain -eq 0 -and $name -eq 0 -and $first -eq 0) { break }
        $count++
        if($count -gt 1024) { throw "Sanity: existing IDT has >1024 entries." }
    }
    Write-Host ("Existing IDT has {0} descriptor(s) plus null terminator." -f $count)
    $existingDescriptors = New-Object byte[] ($count * $IDT_ENTRY)
    [Array]::Copy($bytes, $impOff, $existingDescriptors, 0, $count * $IDT_ENTRY)
}

# ---- build the new section payload ----------------------------------------
#
# Layout inside the section (offsets relative to section start):
#   [IDT]               existing descriptors copied verbatim
#                       + 1 new descriptor (ours)
#                       + 1 null-terminator descriptor (zeros)
#   [DllName]           ASCII NUL-terminated "InjectDll"
#   [Hint+Name]         IMAGE_IMPORT_BY_NAME { hint=0, name = ImportSymbol\0 }
#   [Thunk]             IMAGE_THUNK_DATA64 = RVA of Hint+Name
#   [ThunkNull]         IMAGE_THUNK_DATA64 = 0
#
# IMAGE_THUNK_DATA64 = 8 bytes. We use the same thunk array for both
# OriginalFirstThunk and FirstThunk (loader patches FirstThunk in place
# in the loaded image; on disk both can legitimately point at the same
# array).

# Need to know where in the file we'll put the section so we can compute
# RVAs. Choose the next file-aligned offset after the current end of the
# file (we just append).

$newSectionFileOff = [int](Align $bytes.Length $fileAlignment)

# Highest current RVA so we can place ours after it.
$highestEnd = 0
foreach($s in $sections) {
    $end = $s.VirtualAddress + [Math]::Max($s.VirtualSize, $s.SizeOfRawData)
    if($end -gt $highestEnd) { $highestEnd = $end }
}
$newSectionRva = [uint32](Align $highestEnd $sectionAlignment)

# Compose payload (we don't yet know the final size, build in memory).
# IMAGE_IMPORT_BY_NAME = u16 Hint (=0) then ASCIIZ Name.
$enc = [System.Text.Encoding]::ASCII
function MakeAsciiZ([string]$s) {
    $src = $enc.GetBytes($s)
    $dst = New-Object byte[] ($src.Length + 1)
    [Array]::Copy($src, 0, $dst, 0, $src.Length)
    return ,$dst    # return as a single object (byte[]) not unrolled
}
function MakeHintName([string]$s) {
    $src = $enc.GetBytes($s)
    $dst = New-Object byte[] ($src.Length + 3)   # Hint(2) + name + NUL(1)
    [Array]::Copy($src, 0, $dst, 2, $src.Length)
    return ,$dst
}
$dllNameBytes  = MakeAsciiZ $InjectDll
$hintNameBytes = MakeHintName $ImportSymbol

# Compute layout / offsets within section. Each block is placed *after*
# the previous one and then aligned, so blocks never overlap.
$off_IDT      = 0
$idtBytesLen  = $existingDescriptors.Length + $IDT_ENTRY * 2   # +1 our descriptor +1 null terminator
$off_DllName  = $off_IDT + $idtBytesLen
# Place Hint/Name *after* the DLL name string, 2-byte aligned.
$off_HintName = [int](Align ($off_DllName + $dllNameBytes.Length) 2)
# Thunk array goes after Hint/Name, 8-byte aligned.
$off_Thunk    = [int](Align ($off_HintName + $hintNameBytes.Length) 8)
$thunkBytesLen = 16  # one thunk + null terminator (8 bytes each)
$sectionVirtualSize = $off_Thunk + $thunkBytesLen
$sectionRawSize     = [int](Align $sectionVirtualSize $fileAlignment)

$payload = New-Object byte[] $sectionRawSize

# Copy existing descriptors verbatim.
if($existingDescriptors.Length -gt 0) {
    [Array]::Copy($existingDescriptors, 0, $payload, 0, $existingDescriptors.Length)
}

# Build our IMAGE_IMPORT_DESCRIPTOR:
#   OriginalFirstThunk   = RVA(off_Thunk)
#   TimeDateStamp        = 0
#   ForwarderChain       = 0
#   Name                 = RVA(off_DllName)
#   FirstThunk           = RVA(off_Thunk)    -- loader patches in place
$ourDescOff = $existingDescriptors.Length
$thunkRva  = [uint32]($newSectionRva + $off_Thunk)
$nameRva   = [uint32]($newSectionRva + $off_DllName)
WriteU32 $payload ($ourDescOff + 0)  $thunkRva
WriteU32 $payload ($ourDescOff + 4)  0
WriteU32 $payload ($ourDescOff + 8)  0
WriteU32 $payload ($ourDescOff + 12) $nameRva
WriteU32 $payload ($ourDescOff + 16) $thunkRva
# Trailing null terminator descriptor is left as zeros (payload starts zeroed).

# DLL name
[Array]::Copy($dllNameBytes, 0, $payload, $off_DllName, $dllNameBytes.Length)

# Hint/Name
[Array]::Copy($hintNameBytes, 0, $payload, $off_HintName, $hintNameBytes.Length)

# Thunk[0] = RVA of hint/name struct. Bit 63 = 0 (not by-ordinal).
$hintNameRva = [uint64]($newSectionRva + $off_HintName)
WriteU64 $payload $off_Thunk $hintNameRva
# Thunk[1] = null terminator (already zero).

# ---- compose new file -----------------------------------------------------

# Output buffer: start with the original bytes, padded to file alignment,
# then append our section payload.
$padded = $newSectionFileOff
if($padded -lt $bytes.Length) { throw "Internal: padded ($padded) < original len ($($bytes.Length))" }

$newSize = $padded + $sectionRawSize
$out = New-Object byte[] $newSize
[Array]::Copy($bytes, 0, $out, 0, $bytes.Length)
[Array]::Copy($payload, 0, $out, $padded, $sectionRawSize)

# Patch a new section header in.
$newSecHdrOff = $secOff + $secCount * 40
$headerLimit  = $sizeOfHeaders
if($newSecHdrOff + 40 -gt $headerLimit) {
    throw ("Not enough room in SizeOfHeaders (0x{0:X}) for a new section header at offset 0x{1:X}. " +
           "Would need to rebuild the headers, not yet supported." -f $headerLimit, $newSecHdrOff)
}

# Section name (zero-pad to 8 bytes).
$nameBuf = New-Object byte[] 8
$nameAscii = [System.Text.Encoding]::ASCII.GetBytes($SectionName)
[Array]::Copy($nameAscii, 0, $nameBuf, 0, [Math]::Min($nameAscii.Length, 8))
[Array]::Copy($nameBuf, 0, $out, $newSecHdrOff, 8)

WriteU32 $out ($newSecHdrOff + 8)  $sectionVirtualSize     # VirtualSize
WriteU32 $out ($newSecHdrOff + 12) $newSectionRva          # VirtualAddress
WriteU32 $out ($newSecHdrOff + 16) $sectionRawSize         # SizeOfRawData
WriteU32 $out ($newSecHdrOff + 20) $newSectionFileOff      # PointerToRawData
WriteU32 $out ($newSecHdrOff + 24) 0                       # PointerToRelocations
WriteU32 $out ($newSecHdrOff + 28) 0                       # PointerToLinenumbers
WriteU16 $out ($newSecHdrOff + 32) 0                       # NumberOfRelocations
WriteU16 $out ($newSecHdrOff + 34) 0                       # NumberOfLinenumbers
# Characteristics:
#   IMAGE_SCN_CNT_INITIALIZED_DATA (0x00000040)
#   IMAGE_SCN_MEM_READ             (0x40000000)
#   IMAGE_SCN_MEM_WRITE            (0x80000000)
# Write bit is *required* because ntdll loader writes the resolved function
# pointers back into FirstThunk (the IAT) of every import descriptor; we
# point FirstThunk into this section, so the section must be writable.
# Use [uint32] cast explicitly because PowerShell parses 0xC0000040 as a
# signed int32 that overflows during the implicit cast to uint32.
WriteU32 $out ($newSecHdrOff + 36) ([uint32]'0xC0000040')

# Update file header NumberOfSections.
WriteU16 $out ($fhOff + 2) ($secCount + 1)

# Update optional header SizeOfImage.
$newSizeOfImage = [uint32](Align ($newSectionRva + $sectionVirtualSize) $sectionAlignment)
WriteU32 $out $O_SizeOfImage $newSizeOfImage

# Update IMPORT directory.
$newImportRva  = [uint32]($newSectionRva + $off_IDT)
$newImportSize = [uint32]($idtBytesLen)
WriteU32 $out $D_IMPORT_RVA  $newImportRva
WriteU32 $out $D_IMPORT_SIZE $newImportSize

# Clear BOUND_IMPORT directory: any cached bindings refer to the old IDT.
WriteU32 $out $D_BOUND_RVA  0
WriteU32 $out $D_BOUND_SIZE 0

# Clear CheckSum (user-mode PE checksum is informational).
WriteU32 $out $O_CheckSum 0

# ---- write & summarise ----------------------------------------------------

[System.IO.File]::WriteAllBytes($OutputFile, $out)

Write-Host ""
Write-Host "Wrote $OutputFile ($($out.Length) bytes)."
Write-Host ("  New section '{0}' VA=0x{1:X8} VS=0x{2:X8} Raw=0x{3:X8} @ 0x{4:X8}" -f `
    $SectionName, $newSectionRva, $sectionVirtualSize, $sectionRawSize, $newSectionFileOff)
Write-Host ("  IMPORT directory rewritten to RVA=0x{0:X8} Size=0x{1:X8}" -f $newImportRva, $newImportSize)
Write-Host ("  SizeOfImage updated to 0x{0:X}" -f $newSizeOfImage)
Write-Host ""
Write-Host "Drop '$InjectDll' next to the patched file before launching."
Write-Host "Sanity: open the patched file in any PE viewer; the import table"
Write-Host "should list everything that was there before, with a trailing"
Write-Host "entry naming '$InjectDll'."
