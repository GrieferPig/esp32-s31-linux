# Released S31 firmware

The S31 defconfig pins the firmware archive URL and SHA-256. Buildroot
verifies the archive before extracting it. The package installs the
S31 SPL and U-Boot FIT into the image directory, and installs LP firmware
into the root filesystem.

The Linux extension imports the radio symbols before Kbuild. S31 image
assembly links the radio payload against that same build's kernel and
then assembles the NOR image with genimage.
