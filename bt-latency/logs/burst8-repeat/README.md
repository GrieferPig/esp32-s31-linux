# Invalid burst-8 baseline repeat
Same flashed image 94fb9aabbbf452d9. Restarted BTstack at burst 8 / PHY default
1M / PAIRABLE=0 / DLE enabled; exact restart in restart-command.txt.
Host warmed scan cache, reused bond (AlreadyExists), connected in 0.6 s.
Capture is complete: 12227 bytes, 121 records, 0 parse errors.
Only 17 notifications (8619 ATT bytes) in 0.252044 s, followed by HCI
Disconnection Complete reason 0x08 (supervision timeout) at 2720.071991.
Connection began at 2717.237241 with interval 36 (45 ms).
This is NOT a sustained throughput result and not a fair burst comparison.
A subsequent fresh-pair retry without adapter power cycle failed with
AuthenticationCanceled; its raw host transcript is in ../burst8-fresh.
Host adapter power cycle + fresh pairing succeeded for ../phy2-confirm.
No causal gain from changing burst depth is established by this failed run.
Pull:
python3 bt-latency/pull_pklg_chunk.py /tmp/mission8.pklg bt-latency/logs/burst8-repeat/capture.pklg 0 12
python3 bt-latency/analyze_pklg.py bt-latency/logs/burst8-repeat/capture.pklg
