## HyrbridCPUWin10

Hybrid CPU scheduler kernel driver for Windows 10 on Intel 12th gen and newer processors.

### Features
- Detects P-cores and E-cores via CPUID on load
- Routes High/Realtime priority processes to P-cores automatically
- Routes Normal/Idle/Below Normal processes to E-cores automatically
- 500ms background scan detects priority changes on existing processes
- Process creation callback catches new processes instantly
- Tested on Intel Core i9-14900K (8P + 16E, 32 logical processors)
- Tested on Windows 10 22H2 (19045)

### Installation
1. Enable test signing:
bcdedit /set testsigning on
2. Reboot
3. Copy to C:\Windows\System32\drivers
4. Run as Administrator:
sc create HybridCPU type= kernel start= system binPath= "C:\Windows\System32\drivers\HybridCPU.sys"
sc start HybridCPU

### Files
- `HybridCPU.sys` - Kernel mode driver
- `HybridCPU.cer` - Test signing certificate

### Requirements
- Windows 10 (tested on 22H2 19045)
- Intel 12th gen or newer (Alder Lake, Raptor Lake, etc.)
- Test signing enabled

### Notes
- Does not support AMD (no hybrid topology to detect, safe no-op)
- EPROCESS offsets verified for Windows 10 19045 only
- Not WHQL signed, requires test signing mode
