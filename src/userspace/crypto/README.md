# ESP32-S31 OpenSSL provider

The `s31-crypto` Buildroot package installs `s31.so` in the standard OpenSSL
provider directory and installs a configuration at
`/usr/share/s31-crypto/openssl.cnf`. It uses the matching S31 Linux crypto driver.

Implemented operations:

- SHA-1, SHA-224/256/384/512 and SHA-512/224, SHA-512/256, including streamed
  input and independent copies of an unfinished digest.
- HMAC with SHA-1, SHA-224/256/384/512, streamed input and copied contexts.
- `S31-HMAC`: HMAC-SHA256 using an existing HMAC_UP eFuse slot through an
  opaque handle. It accepts `s31-slot` (unsigned index 0..4), without key bytes.
- AES-128 and AES-256 in ECB, CBC and CTR modes, including streamed input,
  PKCS#7 block padding, independent context copies and in-place operation.
- RSA public/private operations up to 4096 bits, PKCS#1 v1.5 and PSS signatures,
  and raw, PKCS#1 v1.5 and OAEP encryption/decryption.
- RSA signatures using existing encrypted DS parameters and a downstream-HMAC
  eFuse key, loaded through the `s31-ds:` OpenSSL STORE URI.
- Raw ECDH shared-secret derivation with P-256 or P-384, imported or generated
  keys, compressed or uncompressed peer public keys and normal PEM interchange.
- ECDSA verification with P-192/P-256/P-384, and protected signatures using
  existing signing slots through the `s31-ecdsa:` OpenSSL STORE URI.

AES, digest and HMAC operations bind the named S31 AF_ALG drivers. RSA and
private ECDH and ECDSA operations use `/dev/s31-crypto`. The default OpenSSL provider supplies key
encoding, validation and generation; a child library context explicitly selects
it for those functions. A claimed hardware operation that fails is reported as
an error. Each key owns its native file and transforms until its last reference
is released. Imported private scalars and temporary parameter buffers are cleared.
Sleepable native requests execute in the calling task. Atomic callers use the
CryptoAPI queues and softirq completion. Both paths retain the same engine
arbitration, runtime power references, result validation and key cleanup.

RSA uses OpenSSL's padding and private-operation blinding around native MPI
exponentiation, checks each private result with the public exponent, and retains
OpenSSL's implicit rejection for PKCS#1 v1.5 decryption. RSA key generation runs
in software and imports the resulting key into the native driver.

Use the installed configuration for an application:

```sh
export OPENSSL_CONF=/usr/share/s31-crypto/openssl.cnf
openssl dgst -sha256 -propquery provider=s31 input.bin
openssl mac -propquery provider=s31 -digest SHA256 -macopt hexkey:00112233 HMAC
openssl pkeyutl -derive -inkey private.pem -peerkey peer.pem \
    -propquery '?provider=s31' -out shared-secret.bin
openssl dgst -sha256 -sign rsa-private.pem -propquery '?provider=s31' \
    -out signature.bin input.bin
```

The optional property in the PEM example lets the standard file loader and
encoders remain available while selecting S31's asymmetric operation. EVP applications
can fetch the same algorithms with `provider=s31`. Loading this configuration
does not globally replace unrelated algorithms. The asymmetric device is
root-only by default; ordinary AF_ALG hash operations use normal Linux socket
permissions.

ECDH derivation supports P-256 and P-384. EC key management also accepts P-192
for ECDSA verification, but rejects P-192 ECDH. Other curves can explicitly
select the default provider. X9.63 ECDH KDF parameters and
TLS-record HMAC parameters are rejected rather than silently ignored. Use the
standard separate KDF API after deriving a raw shared secret.

AES-192 and AEAD modes are not advertised. AES context copies keep independent
IVs, partial blocks and CTR keystream state. CBC/ECB input must be block aligned
when padding is disabled. TLS-specific cipher parameters and RSA premaster-secret
parameters are rejected; this is not a TLS-record offload provider. Restricted
RSA-PSS key management is not advertised; ordinary RSA keys support PSS signatures.

`S31-HMAC` uses `/dev/s31-crypto` instead of AF_ALG, whose ordinary key input
does not represent eFuse handles. For example, on hardware already provisioned
with an HMAC_UP key in slot 1:

```sh
openssl mac -propquery provider=s31 -macopt s31-slot:1 S31-HMAC < input.bin
```

The driver checks the slot's purpose at selection and execution. It rejects
unprovisioned slots and keys assigned to downstream HMAC, signing or other
engines. Only SHA256 is accepted, and ordinary key, TLS and unrelated MAC
parameters are rejected. Context copies have independent native files and
message buffers. Messages are limited to 65536 bytes: updates buffer input and
final submits one bounded hardware operation, so no hardware lock remains held
between application calls or across suspend. This limit is specific to
`S31-HMAC`; ordinary HMAC continues to stream through AF_ALG.

`ESP32S31_CRYPTO_IOC_SLOT` exposes public key-purpose/read-protection metadata,
never key bytes. A purpose-tagged slot is not necessarily read-protected;
applications can inspect `ESP32S31_CRYPTO_SLOT_READ_PROTECTED`. No operation
programs eFuses, accepts plaintext hardware-root keys, issues JTAG authorization commands or
enables secure boot or memory encryption. Existing protected ECDSA signing
slots are described below. Other implemented keys are ordinary
imported/generated application keys.

For protected RSA signing, `s31-ds-pack` combines an **existing** S31 IDF
`esp_ds_data_t` binary, its matching public RSA PEM file and the eFuse slot index:

```sh
s31-ds-pack 1 encrypted-idf-ds.bin rsa-public.pem key.ds
openssl dgst -sha256 -propquery '?provider=s31' \
    -sign s31-ds:/path/to/key.ds -out signature.bin input.bin
openssl dgst -sha256 -verify rsa-public.pem -signature signature.bin input.bin
```

The converter creates a new regular bundle file with mode 0600 and refuses to
overwrite an existing file. It neither encrypts private parameters nor creates
or provisions a key. S31's IDF encrypted input has a 32-bit `rsa_length`, 16-byte
IV and 1584-byte encrypted `c` field. The native bundle replaces `rsa_length`
with a header containing slot, RSA bit width, public DER length and a zero
reserved word, followed by IV, encrypted `c` and PKCS#1 DER public key. Integer
fields use the S31 little-endian ABI. The native UAPI header defines this layout.

Only existing HMAC_DOWN_DS or HMAC_DOWN_ALL slots can select a DS bundle. The
driver checks the purpose again at execution, verifies the hardware's wrapped
parameter integrity result and checks each private result with the supplied
public exponent. The OpenSSL backend keeps signing blinding and its additional
private-result check; the private exponent is never imported into Linux or
exported by the provider. Private key copies re-import the encrypted bundle.
Full private-key validation performs a hardware-backed challenge operation.
The backend supports PKCS#1 v1.5, PSS and raw RSA signatures and public RSA
operations, with 1024/2048/3072/4096-bit DS widths. Applications retain their
normal security policy for key sizes. DS private decryption is rejected because
OpenSSL's implicit-rejection contract requires a private exponent in software.

The STORE loader accepts bounded regular files only and uses the device's
ordinary access permissions. An unavailable key, failed hardware operation,
invalid wrapped parameters or mismatched public key is an error; no software
private-key substitute is used.

## PKCS #11 application module

`/usr/lib/pkcs11/s31-pkcs11.so` exposes a Cryptoki 2.40 function table for native
PKCS #11 clients. It uses a private OpenSSL library context and explicitly selects
the S31 backend for the advertised crypto operations. Load this module directly;
it does not require a token daemon or globally replace another module. Slot 1 is
the logical `S31 Linux crypto` token; its serial identifies this software token,
not a unique chip identity.

Set `S31_PKCS11_KEYS` before initializing the module to a semicolon-separated list
of up to eight existing RSA PEM files or `s31-ds:` bundle URIs. PEM files must be
unencrypted regular files, at most 16 KiB, containing ordinary RSA public or
private keys. No password prompt is issued. Each file supplies a public object
and, when available, a private signing object. Their binary IDs are 01 through 08
in list order and their labels are the file basenames. An explicitly requested
file that cannot be loaded makes initialization fail. A key list is not required
for digest operations or volatile session-key imports.

For example, using a standard OpenSC client:

```sh
export S31_PKCS11_KEYS=/path/to/rsa-private.pem
pkcs11-tool --module /usr/lib/pkcs11/s31-pkcs11.so --list-mechanisms
pkcs11-tool --module /usr/lib/pkcs11/s31-pkcs11.so --login --pin ' ' \
    --sign --mechanism SHA256-RSA-PKCS --id 01 \
    --input-file input.bin --output-file signature.bin
openssl dgst -sha256 -verify rsa-public.pem -signature signature.bin input.bin
```

Access to `/dev/s31-crypto` and the configured key files is the authorization
boundary. Login accepts an empty PIN or **one ASCII space** as a public activation
marker; it checks Linux device permissions and makes private objects available
to sessions in that process. The single-space form works with clients such as
OpenSC that reject an empty PIN before calling the module. Neither form is a
password or a hardware authentication mechanism. The module does not advertise
a protected authentication path, store a PIN or change device permissions.
Other PIN values are rejected. Do not grant the device or private key files to
untrusted applications. Ordinary PEM key material remains in application memory;
DS bundles, eFuse handles and Key Manager recovery handles are opaque hardware keys.

The module provides SHA-1/224/256/384/512 and SHA-512/224, SHA-512/256 digests;
SHA-1/224/256/384/512 HMAC; raw, PKCS #1 v1.5 and PSS RSA signatures;
RSA raw/OAEP encryption and decryption; and public PKCS #1 v1.5 encryption.
RSA key sizes are 1024 through 4096 bits, subject to application policy.
Hash, HMAC and hashed RSA signatures support multipart input. RSA encryption,
decryption and un-hashed RSA signatures are single-part operations. PSS and OAEP
parameters are validated, including the hash, MGF and explicit PSS salt length.
Private PKCS #1 v1.5 decryption is intentionally unavailable: its PKCS #11
padding-error contract must not silently disable OpenSSL's implicit rejection.
AES and ECDH remain available through the OpenSSL provider and native Linux APIs;
this module does not advertise their PKCS #11 mechanisms.

Existing purpose-8 HMAC_UP eFuse slots appear as nonextractable SHA256 HMAC
objects with IDs `80` followed by the slot index and labels `efuse-hmac-N`.
Their input limit is 65536 bytes. An existing DS bundle supplies signing-only
private key operations and exportable public components. The module neither
provisions slots nor generates or stores persistent objects. Configured token
objects are immutable. `C_CreateObject` can import ordinary RSA or HMAC keys as
volatile session objects; closing their owning session destroys them. Logout
destroys private session objects and cancels private operations. Sensitive and
nonextractable attributes cannot be relaxed after import.

There are at most 16 sessions and 64 objects, including configured keys. Each
session serializes its own calls; independent sessions can execute concurrently.
Finalization, logout and session closure drain admitted calls before releasing
their resources. No hardware lease is held between application update calls.
After a successful ordinary digest, a session retains one completed provider
context to avoid reopening AF_ALG sockets for each message. This can retain two
file descriptors per session until closure; it does not keep an operation active
or hold a hardware lease. Failed operations and digests containing `C_DigestKey`
input release their contexts instead of caching them.
POSIX thread cancellation is deferred while a call owns library state. Pending
deferred cancellation is honored after its locks and references are released;
the operation may have completed before cancellation is delivered. A caller's
already-disabled cancellation state is preserved between calls.
A child process must initialize again after `fork()`; parent handles are invalid
in the child. The shared object remains mapped after `dlclose()` so its registered
OpenSSL/fork cleanup callbacks cannot point into unmapped code. Unsupported
functions return explicit errors rather than null function-table entries. Key
generation, wrapping, derivation, persistent object writes and token/PIN setup
are not provided. Random generation uses OpenSSL's default DRBG and Linux entropy;
it is not advertised as a direct hardware RNG operation.
# ECDSA signatures and existing signing slots

The native signature provider supports P-192, P-256 and P-384 verification.
The kernel exports the standard Signature API names `ecdsa-nist-p192`,
`ecdsa-nist-p256` and `ecdsa-nist-p384`, including the usual public-key
verification templates. Process-context verification uses the accelerator;
softirq callers use the preallocated generic verifier without sleeping.

Signing requires an existing eFuse key. Use `s31-ecdsa:256:0` for P-256 in
logical KEY0, or `s31-ecdsa:384:1:2` for P-384 with its low and high halves
in KEY1 and KEY2. P-192 uses `s31-ecdsa:192:0`. Slot numbers range from zero
through four. The driver checks each slot's purpose and uses only volatile
key selection and a read reload. These interfaces neither create keys nor
program fuses. They do not read private key shadows.

With the packaged provider configuration active, these commands export a
public key and sign with its existing protected key:

```sh
openssl pkey -in s31-ecdsa:256:0 -propquery '?provider=s31' -pubout
openssl dgst -sha256 -sign s31-ecdsa:256:0 -propquery '?provider=s31' \
    -out signature.der message
```

The optional property preference permits the default provider's file/PEM
loader while preferring S31 operations. A strict `provider=s31` query excludes
that file loader. Signatures use DER encoding in OpenSSL. The device uses
fixed-width big-endian `r || s`; the provider converts between the encodings.
Hardware signing uses its deterministic nonce generator, and the kernel
checks every signature with an independent software verifier before release.

Ordinary scalar EC keys in this provider support P-256/P-384 ECDH.
ECDSA private signing in this provider requires an existing slot handle;
use OpenSSL's default provider for software signing with ordinary EC keys.
Public EC parameters and keys remain interchangeable with the default provider.
An opaque signing handle cannot be exported as a private scalar or used for ECDH.

The PKCS#11 module accepts the same signing URIs in the semicolon-separated
`S31_PKCS11_KEYS` setting, alongside RSA key files and DS bundles. It exposes
EC public/private objects, `CKA_EC_PARAMS`, public `CKA_EC_POINT`, `CKM_ECDSA`
and the SHA-1/224/256/384/512 ECDSA mechanisms. PKCS#11 signatures use the
standard fixed-width `r || s` encoding. Applications may import public EC
objects for verification; private EC objects come from existing slot handles.
The module never provisions a signing key. Missing or incompatible slots
are errors and do not select an alternative key.

A peripheral reset failure quarantines its affected hardware domains. Native
requests and SDK leases then fail with `EIO`; unrelated engines remain usable.
Runtime and system resume preserve that quarantine. The driver holds failed
engines in reset with their clocks gated because operand erasure cannot be
established for unresponsive hardware. Recovery requires closing old transforms
and files, then a successful driver reprobe and its hardware self-tests. The
radio's managed supplier link reprobes it after the crypto supplier returns.

Key Manager keys can be used without programming an eFuse. The native interface
accepts only HMAC, DS and ECDSA deployment purposes, never flash/PSRAM encryption
or hardware policy-lock programming. `s31-km-deploy AES_REQUEST NEW_RECOVERY_FILE`
deploys a software-init-key AES request and writes an SDK-compatible 824-byte
chip-bound recovery record, mode 0600. The request layout is
`esp32s31_crypto_km_deploy` in the installed UAPI; `k2_info` and encrypted key
bytes follow the pinned SDK's AES deployment format. HMAC/DS use type 2/3 and
length 3. ECDSA uses type 0 and length 0/1/2 for P192/P256/P384. P384 supplies
the high stage first and low stage second. With `KM_REUSE_HUK`, supply the HUK
record from an earlier deployment. Otherwise the provider reuses its current
volatile root, creating one on the first deployment after probe; active
encrypted-memory keys are refused. No private hardware key is
exported, and no hardware lease spans application calls or system suspend.

- `S31-HMAC` accepts `s31-km-file` (UTF-8 recovery-file path), or `s31-km-key`
  (recovery-record octets). Both select HMAC-SHA256 without a raw EVP key.
- `s31-ecdsa:km:/absolute/recovery-file` loads a protected ECDSA key through
  OpenSSL STORE; its public point is exported and validated before use.
- `s31-ds-pack km:/absolute/recovery-file IDF_DS_DATA RSA_PUBLIC_PEM NEW_BUNDLE`
  packages DS parameters encrypted with the deployed DS AES key. The resulting
  file loads through the existing `s31-ds:` STORE URI.
- `S31_PKCS11_KEYS` accepts these DS/ECDSA URIs and `s31-hmac:/absolute/recovery-file`
  entries, separated by semicolons. Protected objects remain nonextractable.

Recovery records contain public helper material and CRCs, not a file signature.
Changing key information and recomputing its CRC can select a different HMAC
key. Protect record integrity and access like key handles. Copying a recovery record
to another chip does not transfer the underlying hardware root. Applications
must still authenticate the expected public key or verify a known HMAC result.
