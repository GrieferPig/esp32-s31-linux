# Fresh burst-8 comparison
Image 94fb9aabbbf452d9 unchanged; burst 8, default 1M, DLE enabled,
PAIRABLE=0, COEX=0. Same host adapter and board; host power cycle, scan cache,
fresh pair and four subscription attempts match the PHY=2/burst-24 run.
Fresh pairing succeeds; host-fd subscription remains InProgress.

Complete capture: 1021100 bytes, 2960 records, zero parse errors and zero
incomplete ACL messages. 1890 notifications x 507 ATT bytes = 958230 bytes
in 40.066848 s: 23.355256 KiB/s pump-to-controller. ATT values are 504 bytes.
10 s windows: 21.933691 / 25.795605 / 27.033398 / 18.764941 KiB/s.
LE interval 45 ms and DLE accepted; no end-to-end BLE result.

For comparison, burst-24/default 1M first 30 s was 27.429492 KiB/s.
Fresh burst-24/PHY=2-requested (actual 1M) was 28.32 KiB/s over ~40 s.
These are single trials with fluctuating windows. The PHY-request setting
and pairing histories differ for parts of this comparison; results suggest
a modest burst gain, not a controlled causal estimate or a path to 100 KiB/s.
The banked 19.26 KiB/s baseline is not reproduced exactly (fresh burst-8 is
faster), so comparing only against that banked figure overstates the gain.

Transfer used lossless board gzip in RAM, then the required dd/base64
transport in a single 70-block chunk. gzip CRC validated on decompression;
reconstructed byte count exactly matches the board's 1021100-byte file.
