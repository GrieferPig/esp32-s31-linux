# Taste
- Prefers fully autonomous, end-to-end execution: agent should diagnose, fix, debug (using tools like IDF OpenOCD when needed), test, and optimize without stopping for manual approval until quantitative goals are met. Confidence: 0.95
- Expects hardware-level debugging via ESP-IDF OpenOCD when diagnosing low-level/SMP issues. Confidence: 0.9
- Requires performance validation via benchmarks (e.g., CoreMark on all harts/cores) against an explicit target score, with iterative optimization if below target. Confidence: 0.88
- Constrain optimizations to preserve radio functionality: do not disable WiFi/BT to gain performance. Confidence: 0.92
- Prefers long-duration shell commands to be run with nohup/background execution (e.g., `nohup ... &`) to avoid blocking and timeouts. Confidence: 0.9
- When debugging PIE/custom ISA faults (e.g., HWLoop/XEspV on ESP32-S31 hart0), prefers checking toolchain source in ../crosstool-NG (especially packages/musl) and temporarily disabling musl PIE/ vector optimizations as a reversible isolation step before permanent fixes. Confidence: 0.85
- Expects agent to immediately respect pause/stop commands and provide a concise structured progress summary covering completed fixes, reproduced issues, root causes, and next steps when asked for current progress. Confidence: 0.88
