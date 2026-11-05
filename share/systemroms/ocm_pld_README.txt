OCM-PLD IPL files used by the 1chipMSX machine (share/machines/1chipMSX.xml)
============================================================================

ocm_pld_iplrom_304k.bin
    IPL-ROM (preloader) of the FPGA, 282 bytes padded to 512 bytes with FFh.
    Built from esemsx3/src/peripheral/iplrom_304k.v.

ocm_pld_iplloader_304k_rev300.bin
    IPL-ROM v3.00 loader, stored in the EPCS serial flash at 33400h.
    Same as esemsx3/iplrom_304k.hex (hex_iplrom_304k_rev300.bin).

ocm_pld_ig1_init_profile.bin
    Default (empty) IGX smart profile, stored in the EPCS at 33F80h.

Source: OCM-PLD firmware, https://github.com/ToughkidDev/ocm-pld-dev
(esemsx3/src/peripheral/iplrom3_work).

The BIOS itself is not included: like on the real machine, it is loaded
from OCM-BIOS.DAT on the SD card (FAT12/FAT16, first partition).

Copyright (c) 2020 Caro
Copyright (c) 2021-2025 Takayuki Hara
Copyright (c) 2022-2025 KdL
All rights reserved.

Redistribution and use of this source code or any derivative works, are
permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Redistributions may not be sold, nor may they be used in a commercial
   product or activity without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
